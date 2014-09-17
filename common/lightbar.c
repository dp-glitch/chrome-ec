/*
 * Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * LED controls.
 */

#ifdef LIGHTBAR_SIMULATION
#include "simulation.h"
#else
#include "battery.h"
#include "charge_state.h"
#include "common.h"
#include "console.h"
#include "ec_commands.h"
#include "hooks.h"
#include "host_command.h"
#include "lb_common.h"
#include "lightbar.h"
#include "pwm.h"
#include "system.h"
#include "task.h"
#include "timer.h"
#include "util.h"
#endif

/*
 * The Link lightbar had no version command, so defaulted to zero. We have
 * added a couple of new commands, so we've updated the version. Any
 * optional features in the current version should be marked with flags.
 */
#define LIGHTBAR_IMPLEMENTATION_VERSION 1
#define LIGHTBAR_IMPLEMENTATION_FLAGS   0

/* Console output macros */
#define CPUTS(outstr) cputs(CC_LIGHTBAR, outstr)
#define CPRINTS(format, args...) cprints(CC_LIGHTBAR, format, ## args)

#define FP_SCALE 10000

/******************************************************************************/
/* Here's some state that we might want to maintain across sysjumps, just to
 * prevent the lightbar from flashing during normal boot as the EC jumps from
 * RO to RW. */
static struct p_state {
	/* What patterns are we showing? */
	enum lightbar_sequence cur_seq;
	enum lightbar_sequence prev_seq;

	/* Quantized battery charge level: 0=low 1=med 2=high 3=full. */
	int battery_level;
	int battery_percent;

	/* It's either charging or discharging. */
	int battery_is_charging;

	/* Pattern variables for state S0. */
	uint16_t w0;				/* primary phase */
	uint8_t ramp;				/* ramp-in for S3->S0 */

	uint8_t _pad0;				/* next item is __packed */

	/* Tweakable parameters. */
	struct lightbar_params_v1 p;
} st;

static const struct lightbar_params_v1 default_params = {
	.google_ramp_up = 2500,
	.google_ramp_down = 10000,
	.s3s0_ramp_up = 2000,
	.s0_tick_delay = { 45000, 30000 },	/* battery, AC */
	.s0a_tick_delay = { 5000, 3000 },	/* battery, AC */
	.s0s3_ramp_down = 2000,
	.s3_sleep_for = 5 * SECOND,		/* between checks */
	.s3_ramp_up = 2500,
	.s3_ramp_down = 10000,
	.tap_tick_delay = 5000,			/* oscillation step time */
	.tap_display_time = 3000000,		/* total sequence time */

	.tap_pct_red = 10,			/* below this is red */
	.tap_pct_green = 97,			/* above this is green */
	.tap_seg_min_on = 35,		        /* min intensity (%) for "on" */
	.tap_seg_max_on = 100,			/* max intensity (%) for "on" */
	.tap_seg_osc = 25,			/* amplitude for charging osc */
	.tap_idx = {5, 6, 7},			/* color [red, yellow, green] */

	.osc_min = { 0x60, 0x60 },		/* battery, AC */
	.osc_max = { 0xd0, 0xd0 },		/* battery, AC */
	.w_ofs = {24, 24},			/* phase offset, 256 == 2*PI */

	.bright_bl_off_fixed = {0xcc, 0xff},	/* backlight off: battery, AC */
	.bright_bl_on_min = {0xcc, 0xff},	/* backlight on: battery, AC */
	.bright_bl_on_max = {0xcc, 0xff},	/* backlight on: battery, AC */

	.battery_threshold = { 14, 40, 99 },	/* percent, lowest to highest */
	.s0_idx = {
		{ 5, 4, 4, 4 },		/* battery: 0 = red, other = blue */
		{ 4, 4, 4, 4 }		/* AC: always blue */
	},
	.s3_idx = {
		{ 5, 0xff, 0xff, 0xff },       /* battery: 0 = red, else off */
		{ 0xff, 0xff, 0xff, 0xff }     /* AC: do nothing */
	},
	.color = {
		{0x33, 0x69, 0xe8},		/* 0: Google blue */
		{0xd5, 0x0f, 0x25},		/* 1: Google red */
		{0xee, 0xb2, 0x11},		/* 2: Google yellow */
		{0x00, 0x99, 0x25},		/* 3: Google green */
		{0x00, 0x00, 0xff},		/* 4: full blue */
		{0xff, 0x00, 0x00},		/* 5: full red */
		{0xff, 0xff, 0x00},		/* 6: full yellow */
		{0x00, 0xff, 0x00},		/* 7: full green */
	},
};

#define LB_SYSJUMP_TAG 0x4c42			/* "LB" */
static void lightbar_preserve_state(void)
{
	system_add_jump_tag(LB_SYSJUMP_TAG, 0, sizeof(st), &st);
}
DECLARE_HOOK(HOOK_SYSJUMP, lightbar_preserve_state, HOOK_PRIO_DEFAULT);

static void lightbar_restore_state(void)
{
	const uint8_t *old_state = 0;
	int size;

	old_state = system_get_jump_tag(LB_SYSJUMP_TAG, 0, &size);
	if (old_state && size == sizeof(st)) {
		memcpy(&st, old_state, size);
		CPRINTS("LB state restored: %d %d - %d %d/%d",
			st.cur_seq, st.prev_seq,
			st.battery_is_charging,
			st.battery_percent,
			st.battery_level);
	} else {
		st.cur_seq = st.prev_seq = LIGHTBAR_S5;
		st.battery_percent = 100;
		st.battery_level = LB_BATTERY_LEVELS - 1;
		st.w0 = 0;
		st.ramp = 0;
		memcpy(&st.p, &default_params, sizeof(st.p));
		CPRINTS("LB state initialized");
	}
}

/******************************************************************************/
/* The patterns are generally dependent on the current battery level and AC
 * state. These functions obtain that information, generally by querying the
 * power manager task. In demo mode, the keyboard task forces changes to the
 * state by calling the demo_* functions directly. */
/******************************************************************************/

#ifdef CONFIG_PWM_KBLIGHT
static int last_backlight_level;
#endif

static int demo_mode = DEMO_MODE_DEFAULT;

static int quantize_battery_level(int pct)
{
	int i, bl = 0;
	for (i = 0; i < LB_BATTERY_LEVELS - 1; i++)
		if (pct >= st.p.battery_threshold[i])
			bl++;
	return bl;
}

/* Update the known state. */
static void get_battery_level(void)
{
	int pct = 0;
	int bl;

	if (demo_mode)
		return;

#ifdef HAS_TASK_CHARGER
	st.battery_percent = pct = charge_get_percent();
	st.battery_is_charging = (PWR_STATE_DISCHARGE != charge_get_state());
#endif

	/* Find the new battery level */
	bl = quantize_battery_level(pct);

	/* Use some hysteresis to avoid flickering */
	if (bl > st.battery_level
	    && pct >= (st.p.battery_threshold[bl-1] + 1))
		st.battery_level = bl;
	else if (bl < st.battery_level &&
		 pct <= (st.p.battery_threshold[bl] - 1))
		st.battery_level = bl;

#ifdef CONFIG_PWM_KBLIGHT
	/*
	 * With nothing else to go on, use the keyboard backlight level to *
	 * set the brightness. In general, if the keyboard backlight
	 * is OFF (which it is when ambient is bright), use max brightness for
	 * lightbar. If keyboard backlight is ON, use keyboard backlight
	 * brightness. That fails if the keyboard backlight is off because
	 * someone's watching a movie in the dark, of course. Ideally we should
	 * just let the AP control it directly.
	 */
	if (pwm_get_enabled(PWM_CH_KBLIGHT)) {
		pct = pwm_get_duty(PWM_CH_KBLIGHT);
		pct = (255 * pct) / 100;  /* 00 - FF */
		if (pct > st.p.bright_bl_on_max[st.battery_is_charging])
			pct = st.p.bright_bl_on_max[st.battery_is_charging];
		else if (pct < st.p.bright_bl_on_min[st.battery_is_charging])
			pct = st.p.bright_bl_on_min[st.battery_is_charging];
	} else
		pct = st.p.bright_bl_off_fixed[st.battery_is_charging];

	if (pct != last_backlight_level) {
		last_backlight_level = pct;
		lb_set_brightness(pct);
	}
#endif
}

/* Forcing functions for demo mode, called by the keyboard task. */

/* Up/Down keys */
#define DEMO_CHARGE_STEP 1
void demo_battery_level(int inc)
{
	if (!demo_mode)
		return;

	st.battery_percent += DEMO_CHARGE_STEP * inc;

	if (st.battery_percent > 100)
		st.battery_percent = 100;
	else if (st.battery_percent < 0)
		st.battery_percent = 0;

	st.battery_level = quantize_battery_level(st.battery_percent);

	CPRINTS("LB demo: battery_percent = %d%%, battery_level=%d",
		st.battery_percent, st.battery_level);
}

/* Left/Right keys */

void demo_is_charging(int ischarge)
{
	if (!demo_mode)
		return;

	st.battery_is_charging = ischarge;
	CPRINTS("LB demo: battery_is_charging=%d",
		st.battery_is_charging);
}

/* Bright/Dim keys */
void demo_brightness(int inc)
{
	int b;

	if (!demo_mode)
		return;

	b = lb_get_brightness() + (inc * 16);
	if (b > 0xff)
		b = 0xff;
	else if (b < 0)
		b = 0;
	lb_set_brightness(b);
}

/******************************************************************************/
/* Helper functions and data. */
/******************************************************************************/

#define F(x) (x * FP_SCALE)
static const int _ramp_table[] = {
	F(0.000000), F(0.000151), F(0.000602), F(0.001355), F(0.002408),
	F(0.003760), F(0.005412), F(0.007361), F(0.009607), F(0.012149),
	F(0.014984), F(0.018112), F(0.021530), F(0.025236), F(0.029228),
	F(0.033504), F(0.038060), F(0.042895), F(0.048005), F(0.053388),
	F(0.059039), F(0.064957), F(0.071136), F(0.077573), F(0.084265),
	F(0.091208), F(0.098396), F(0.105827), F(0.113495), F(0.121396),
	F(0.129524), F(0.137876), F(0.146447), F(0.155230), F(0.164221),
	F(0.173414), F(0.182803), F(0.192384), F(0.202150), F(0.212096),
	F(0.222215), F(0.232501), F(0.242949), F(0.253551), F(0.264302),
	F(0.275194), F(0.286222), F(0.297379), F(0.308658), F(0.320052),
	F(0.331555), F(0.343159), F(0.354858), F(0.366644), F(0.378510),
	F(0.390449), F(0.402455), F(0.414519), F(0.426635), F(0.438795),
	F(0.450991), F(0.463218), F(0.475466), F(0.487729), F(0.500000),
	F(0.512271), F(0.524534), F(0.536782), F(0.549009), F(0.561205),
	F(0.573365), F(0.585481), F(0.597545), F(0.609551), F(0.621490),
	F(0.633356), F(0.645142), F(0.656841), F(0.668445), F(0.679947),
	F(0.691342), F(0.702621), F(0.713778), F(0.724806), F(0.735698),
	F(0.746449), F(0.757051), F(0.767499), F(0.777785), F(0.787904),
	F(0.797850), F(0.807616), F(0.817197), F(0.826586), F(0.835780),
	F(0.844770), F(0.853553), F(0.862124), F(0.870476), F(0.878604),
	F(0.886505), F(0.894173), F(0.901604), F(0.908792), F(0.915735),
	F(0.922427), F(0.928864), F(0.935044), F(0.940961), F(0.946612),
	F(0.951995), F(0.957105), F(0.961940), F(0.966496), F(0.970772),
	F(0.974764), F(0.978470), F(0.981888), F(0.985016), F(0.987851),
	F(0.990393), F(0.992639), F(0.994588), F(0.996240), F(0.997592),
	F(0.998645), F(0.999398), F(0.999849), F(1.000000),
};
#undef F


/* This function provides a smooth ramp up from 0.0 to 1.0 and back to 0.0,
 * for input from 0x00 to 0xff. */
static inline int cycle_010(uint8_t i)
{
	return i < 128 ? _ramp_table[i] : _ramp_table[256-i];
}

/* This function provides a smooth oscillation between -0.5 and +0.5.
 * Zero starts at 0x00. */
static inline int cycle_0p0n0(uint8_t i)
{
	return cycle_010(i + 64) - FP_SCALE / 2;
}

/* This function provides a pulsing oscillation between -0.5 and +0.5. */
static inline int cycle_npn(uint16_t i)
{
	if ((i / 256) % 4)
		return -FP_SCALE / 2;
	return cycle_010(i) - FP_SCALE / 2;
}

/******************************************************************************/
/* Here's where we keep messages waiting to be delivered to the lightbar task.
 * If more than one is sent before the task responds, we only want to deliver
 * the latest one. */
static uint32_t pending_msg;
/* And here's the task event that we use to trigger delivery. */
#define PENDING_MSG 1

/* Interruptible delay. */
#define WAIT_OR_RET(A) do { \
	uint32_t msg = task_wait_event(A); \
	if (TASK_EVENT_CUSTOM(msg) == PENDING_MSG) \
		return PENDING_MSG; } while (0)

/******************************************************************************/
/* Here are the preprogrammed sequences. */
/******************************************************************************/

/* Pulse google colors once, off to on to off. */
static uint32_t pulse_google_colors(void)
{
	int w, i, r, g, b;
	int f;

	for (w = 0; w < 128; w += 2) {
		f = cycle_010(w);
		for (i = 0; i < NUM_LEDS; i++) {
			r = st.p.color[i].r * f / FP_SCALE;
			g = st.p.color[i].g * f / FP_SCALE;
			b = st.p.color[i].b * f / FP_SCALE;
			lb_set_rgb(i, r, g, b);
		}
		WAIT_OR_RET(st.p.google_ramp_up);
	}
	for (w = 128; w <= 256; w++) {
		f = cycle_010(w);
		for (i = 0; i < NUM_LEDS; i++) {
			r = st.p.color[i].r * f / FP_SCALE;
			g = st.p.color[i].g * f / FP_SCALE;
			b = st.p.color[i].b * f / FP_SCALE;
			lb_set_rgb(i, r, g, b);
		}
		WAIT_OR_RET(st.p.google_ramp_down);
	}

	return 0;
}

/* CPU is waking from sleep. */
static uint32_t sequence_S3S0(void)
{
	int w, r, g, b;
	int f, fmin;
	int ci;
	uint32_t res;

	lb_init();
	lb_on();
	get_battery_level();

	res = pulse_google_colors();
	if (res)
		return res;

#ifndef BLUE_PULSING
	return 0;
#endif

	/* Ramp up to starting brightness, using S0 colors */
	ci = st.p.s0_idx[st.battery_is_charging][st.battery_level];
	if (ci >= ARRAY_SIZE(st.p.color))
		ci = 0;

	fmin = st.p.osc_min[st.battery_is_charging] * FP_SCALE / 255;

	for (w = 0; w <= 128; w++) {
		f = cycle_010(w) * fmin / FP_SCALE;
		r = st.p.color[ci].r * f / FP_SCALE;
		g = st.p.color[ci].g * f / FP_SCALE;
		b = st.p.color[ci].b * f / FP_SCALE;
		lb_set_rgb(NUM_LEDS, r, g, b);
		WAIT_OR_RET(st.p.s3s0_ramp_up);
	}

	/* Initial conditions */
	st.w0 = -256;				/* start cycle_npn() quietly */
	st.ramp = 0;

	/* Ready for S0 */
	return 0;
}

#ifdef BLUE_PULSING

/* CPU is fully on */
static uint32_t sequence_S0(void)
{
	int tick, last_tick;
	timestamp_t start, now;
	uint8_t r, g, b;
	int i, ci;
	uint8_t w_ofs;
	uint16_t w;
	int f, fmin, fmax, base_s0, osc_s0, f_ramp;

	start = get_time();
	tick = last_tick = 0;

	lb_set_rgb(NUM_LEDS, 0, 0, 0);
	lb_on();

	while (1) {
		now = get_time();

		/* Only check the battery state every few seconds. The battery
		 * charging task doesn't update as quickly as we do, and isn't
		 * always valid for a bit after jumping from RO->RW. */
		tick = (now.le.lo - start.le.lo) / SECOND;
		if (tick % 4 == 3 && tick != last_tick) {
			get_battery_level();
			last_tick = tick;
		}

		/* Calculate the colors */
		ci = st.p.s0_idx[st.battery_is_charging][st.battery_level];
		if (ci >= ARRAY_SIZE(st.p.color))
			ci = 0;
		w_ofs = st.p.w_ofs[st.battery_is_charging];
		fmin = st.p.osc_min[st.battery_is_charging] * FP_SCALE / 255;
		fmax = st.p.osc_max[st.battery_is_charging] * FP_SCALE / 255;
		base_s0 = (fmax + fmin) / 2;
		osc_s0 = fmax - fmin;
		f_ramp = st.ramp * FP_SCALE / 255;

		for (i = 0; i < NUM_LEDS; i++) {
			w = st.w0 - i * w_ofs * f_ramp / FP_SCALE;
			f = base_s0 + osc_s0 * cycle_npn(w) / FP_SCALE;
			r = st.p.color[ci].r * f / FP_SCALE;
			g = st.p.color[ci].g * f / FP_SCALE;
			b = st.p.color[ci].b * f / FP_SCALE;
			lb_set_rgb(i, r, g, b);
		}

		/* Increment the phase */
		if (st.battery_is_charging)
			st.w0--;
		else
			st.w0++;

		/* Continue ramping in if needed */
		if (st.ramp < 0xff)
			st.ramp++;

		i = st.p.s0a_tick_delay[st.battery_is_charging];
		WAIT_OR_RET(i);
	}
	return 0;
}

#else  /* just simple google colors */

static uint32_t sequence_S0(void)
{
	int w, i, r, g, b;
	int f;

	lb_set_rgb(NUM_LEDS, 0, 0, 0);
	lb_on();

	/* Ramp up */
	for (w = 0; w < 128; w += 2) {
		f = cycle_010(w);
		for (i = 0; i < NUM_LEDS; i++) {
			r = st.p.color[i].r * f / FP_SCALE;
			g = st.p.color[i].g * f / FP_SCALE;
			b = st.p.color[i].b * f / FP_SCALE;
			lb_set_rgb(i, r, g, b);
		}
		WAIT_OR_RET(st.p.google_ramp_up);
	}

	while (1) {

		get_battery_level();

		/* Not really low use google colors */
		if (st.battery_level) {
			for (i = 0; i < NUM_LEDS; i++) {
				r = st.p.color[i].r;
				g = st.p.color[i].g;
				b = st.p.color[i].b;
				lb_set_rgb(i, r, g, b);
			}
		} else {
			r = st.p.color[5].r;
			g = st.p.color[5].g;
			b = st.p.color[5].b;
			lb_set_rgb(4, r, g, b);
		}

		WAIT_OR_RET(1 * SECOND);
	}
	return 0;
}

#endif

/* CPU is going to sleep. */
static uint32_t sequence_S0S3(void)
{
	int w, i, r, g, b;
	int f;
	uint8_t drop[NUM_LEDS][3];

	/* Grab current colors */
	for (i = 0; i < NUM_LEDS; i++)
		lb_get_rgb(i, &drop[i][0], &drop[i][1], &drop[i][2]);

	/* Fade down to black */
	for (w = 128; w <= 256; w++) {
		f = cycle_010(w);
		for (i = 0; i < NUM_LEDS; i++) {
			r = drop[i][0] * f / FP_SCALE;
			g = drop[i][1] * f / FP_SCALE;
			b = drop[i][2] * f / FP_SCALE;
			lb_set_rgb(i, r, g, b);
		}
		WAIT_OR_RET(st.p.s0s3_ramp_down);
	}

	/* pulse once and done */
	return pulse_google_colors();
}

/* CPU is sleeping */
static uint32_t sequence_S3(void)
{
	int r, g, b;
	int w;
	int f;
	int ci;

	lb_off();
	lb_init();
	lb_set_rgb(NUM_LEDS, 0, 0, 0);
	while (1) {
		WAIT_OR_RET(st.p.s3_sleep_for);
		get_battery_level();

		/* only pulse if we've been given a valid color index */
		ci = st.p.s3_idx[st.battery_is_charging][st.battery_level];
		if (ci >= ARRAY_SIZE(st.p.color))
			continue;

		/* pulse once */
		lb_on();

		for (w = 0; w < 128; w += 2) {
			f = cycle_010(w);
			r = st.p.color[ci].r * f / FP_SCALE;
			g = st.p.color[ci].g * f / FP_SCALE;
			b = st.p.color[ci].b * f / FP_SCALE;
			lb_set_rgb(NUM_LEDS, r, g, b);
			WAIT_OR_RET(st.p.s3_ramp_up);
		}
		for (w = 128; w <= 256; w++) {
			f = cycle_010(w);
			r = st.p.color[ci].r * f / FP_SCALE;
			g = st.p.color[ci].g * f / FP_SCALE;
			b = st.p.color[ci].b * f / FP_SCALE;
			lb_set_rgb(NUM_LEDS, r, g, b);
			WAIT_OR_RET(st.p.s3_ramp_down);
		}

		lb_set_rgb(NUM_LEDS, 0, 0, 0);
		lb_off();
	}
	return 0;
}


/* CPU is powering up. We generally boot fast enough that we don't have time
 * to do anything interesting in the S3 state, but go straight on to S0. */
static uint32_t sequence_S5S3(void)
{
	/* The controllers need 100us after power is applied before they'll
	 * respond. Don't return early, because we still want to initialize the
	 * lightbar even if another message comes along while we're waiting. */
	usleep(100);
	lb_init();
	lb_set_rgb(NUM_LEDS, 0, 0, 0);
	lb_on();
	return 0;
}

/* Sleep to off. The S3->S5 transition takes about 10msec, so just wait. */
static uint32_t sequence_S3S5(void)
{
	lb_off();
	return 0;
}

/* CPU is off. The lightbar loses power when the CPU is in S5, so there's
 * nothing to do. We'll just wait here until the state changes. */
static uint32_t sequence_S5(void)
{
	lb_off();
	WAIT_OR_RET(-1);
	return 0;
}

/* Used by factory. */
static uint32_t sequence_TEST_inner(void)
{
	int i, k, r, g, b;
	int kmax = 254;
	int kstep = 8;

	static const struct rgb_s testcolors[] = {
		{0xff, 0x00, 0x00},
		{0xff, 0xff, 0x00},
		{0x00, 0xff, 0x00},
		{0x00, 0x00, 0xff},
		{0x00, 0xff, 0xff},
		{0xff, 0x00, 0xff},
		{0xff, 0xff, 0xff},
	};

	lb_init();
	lb_on();
	for (i = 0; i < ARRAY_SIZE(testcolors); i++) {
		for (k = 0; k <= kmax; k += kstep) {
			r = testcolors[i].r ? k : 0;
			g = testcolors[i].g ? k : 0;
			b = testcolors[i].b ? k : 0;
			lb_set_rgb(NUM_LEDS, r, g, b);
			WAIT_OR_RET(10000);
		}
		for (k = kmax; k >= 0; k -= kstep) {
			r = testcolors[i].r ? k : 0;
			g = testcolors[i].g ? k : 0;
			b = testcolors[i].b ? k : 0;
			lb_set_rgb(NUM_LEDS, r, g, b);
			WAIT_OR_RET(10000);
		}
	}

	lb_set_rgb(NUM_LEDS, 0, 0, 0);
	return 0;
}

static uint32_t sequence_TEST(void)
{
	int tmp;
	uint32_t r;

	/* Force brightness to max, then restore it */
	tmp = lb_get_brightness();
	lb_set_brightness(255);
	r = sequence_TEST_inner();
	lb_set_brightness(tmp);
	return r;
}

static uint32_t sequence_PULSE(void)
{
	uint32_t msg;

	lb_init();
	lb_on();

	lb_start_builtin_cycle();

	/* Not using WAIT_OR_RET() here, because we want to clean up when we're
	 * done. The only way out is to get a message. */
	msg = task_wait_event(-1);
	lb_init();
	return TASK_EVENT_CUSTOM(msg);
}

/* The AP is going to poke at the lightbar directly, so we don't want the EC
 * messing with it. We'll just sit here and ignore all other messages until
 * we're told to continue (or until we think the AP is shutting down).
 */
static uint32_t sequence_STOP(void)
{
	uint32_t msg;

	do {
		msg = TASK_EVENT_CUSTOM(task_wait_event(-1));
		CPRINTS("LB_stop got pending_msg %d", pending_msg);
	} while (msg != PENDING_MSG || (
			 pending_msg != LIGHTBAR_RUN &&
			 pending_msg != LIGHTBAR_S0S3 &&
			 pending_msg != LIGHTBAR_S3 &&
			 pending_msg != LIGHTBAR_S3S5 &&
			 pending_msg != LIGHTBAR_S5));

	CPRINTS("LB_stop->running");
	return 0;
}

/* Telling us to run when we're already running should do nothing. */
static uint32_t sequence_RUN(void)
{
	return 0;
}

/* We shouldn't come here, but if we do it shouldn't hurt anything. This
 * sequence is to indicate an internal error in the lightbar logic, not an
 * error with the Chromebook itself.
 */
static uint32_t sequence_ERROR(void)
{
	lb_init();
	lb_on();

	lb_set_rgb(0, 255, 255, 255);
	lb_set_rgb(1, 255, 0, 255);
	lb_set_rgb(2, 0, 255, 255);
	lb_set_rgb(3, 255, 255, 255);

	WAIT_OR_RET(10 * SECOND);
	return 0;
}

static const struct {
	uint8_t led;
	uint8_t r, g, b;
	unsigned int delay;
} konami[] = {

	{1, 0xff, 0xff, 0x00, 0},
	{2, 0xff, 0xff, 0x00, 100000},
	{1, 0x00, 0x00, 0x00, 0},
	{2, 0x00, 0x00, 0x00, 100000},

	{1, 0xff, 0xff, 0x00, 0},
	{2, 0xff, 0xff, 0x00, 100000},
	{1, 0x00, 0x00, 0x00, 0},
	{2, 0x00, 0x00, 0x00, 100000},

	{0, 0x00, 0x00, 0xff, 0},
	{3, 0x00, 0x00, 0xff, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 100000},

	{0, 0x00, 0x00, 0xff, 0},
	{3, 0x00, 0x00, 0xff, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 100000},

	{0, 0xff, 0x00, 0x00, 0},
	{1, 0xff, 0x00, 0x00, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{1, 0x00, 0x00, 0x00, 100000},

	{2, 0x00, 0xff, 0x00, 0},
	{3, 0x00, 0xff, 0x00, 100000},
	{2, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 100000},

	{0, 0xff, 0x00, 0x00, 0},
	{1, 0xff, 0x00, 0x00, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{1, 0x00, 0x00, 0x00, 100000},

	{2, 0x00, 0xff, 0x00, 0},
	{3, 0x00, 0xff, 0x00, 100000},
	{2, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 100000},

	{0, 0x00, 0xff, 0xff, 0},
	{2, 0x00, 0xff, 0xff, 100000},
	{0, 0x00, 0x00, 0x00, 0},
	{2, 0x00, 0x00, 0x00, 150000},

	{1, 0xff, 0x00, 0xff, 0},
	{3, 0xff, 0x00, 0xff, 100000},
	{1, 0x00, 0x00, 0x00, 0},
	{3, 0x00, 0x00, 0x00, 250000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},

	{4, 0xff, 0xff, 0xff, 100000},
	{4, 0x00, 0x00, 0x00, 100000},
};

static uint32_t sequence_KONAMI_inner(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(konami); i++) {
		lb_set_rgb(konami[i].led,
			   konami[i].r, konami[i].g, konami[i].b);
		if (konami[i].delay)
			WAIT_OR_RET(konami[i].delay);
	}

	return 0;
}

static uint32_t sequence_KONAMI(void)
{
	int tmp;
	uint32_t r;

	/* Force brightness to max, then restore it */
	tmp = lb_get_brightness();
	lb_set_brightness(255);
	r = sequence_KONAMI_inner();
	lb_set_brightness(tmp);
	return r;
}

/* Returns 0.0 to 1.0 for val in [min, min + ofs] */
static int range(int val, int min, int ofs)
{
	if (val <= min)
		return 0;
	if (val >= min+ofs)
		return FP_SCALE;
	return (val - min) * FP_SCALE / ofs;
}

/* Handy constant */
#define CUT (100 / NUM_LEDS)

static uint32_t sequence_TAP_inner(void)
{
	enum { RED, YELLOW, GREEN } base_color;
	timestamp_t start, now;
	int i, ci, max_led;
	int f_min, f_delta, f_osc, f_power, f_mult;
	uint8_t w = 0;

	f_min = st.p.tap_seg_min_on * FP_SCALE / 100;
	f_delta = (st.p.tap_seg_max_on - st.p.tap_seg_min_on) * FP_SCALE / 100;
	f_osc = st.p.tap_seg_osc * FP_SCALE / 100;

	start = get_time();
	while (1) {
		get_battery_level();

		if (st.battery_percent < st.p.tap_pct_red)
			base_color = RED;
		else if (st.battery_percent > st.p.tap_pct_green)
			base_color = GREEN;
		else
			base_color = YELLOW;

		ci = st.p.tap_idx[base_color];
		max_led = st.battery_percent / CUT;

		for (i = 0; i < NUM_LEDS; i++) {

			if (max_led > i) {
				f_mult = FP_SCALE;
			} else if (max_led < i) {
				f_mult = 0;
			} else {
				switch (base_color) {
				case RED:
					f_power = range(st.battery_percent, 0,
							st.p.tap_pct_red - 1);
					break;
				case YELLOW:
					f_power = range(st.battery_percent,
							i * CUT, CUT - 1);
					break;
				case GREEN:
					/* green is always full on */
					f_power = FP_SCALE;
				}
				f_mult = f_min + f_power * f_delta / FP_SCALE;
			}

			/* Pulse when charging */
			if (st.battery_is_charging) {
				int scale = (FP_SCALE -
					     f_osc * cycle_010(w++) / FP_SCALE);
				f_mult = f_mult * scale / FP_SCALE;
			}

			lb_set_rgb(i, f_mult * st.p.color[ci].r / FP_SCALE,
				   f_mult * st.p.color[ci].g / FP_SCALE,
				   f_mult * st.p.color[ci].b / FP_SCALE);
		}

		WAIT_OR_RET(st.p.tap_tick_delay);

		/* Return after some time has elapsed */
		now = get_time();
		if (now.le.lo - start.le.lo > st.p.tap_display_time)
			break;
	}
	return 0;
}

static uint32_t sequence_TAP(void)
{
	int i;
	uint32_t r;
	uint8_t br, save[NUM_LEDS][3];

#ifdef CONFIG_LIGHTBAR_POWER_RAILS
	/* Request that the lightbar power rails be turned on. */
	if (lb_power(1)) {
		lb_init();
		lb_set_rgb(NUM_LEDS, 0, 0, 0);
	}
#endif
	lb_on();

	for (i = 0; i < NUM_LEDS; i++)
		lb_get_rgb(i, &save[i][0], &save[i][1], &save[i][2]);
	br = lb_get_brightness();
	lb_set_brightness(255);

	r = sequence_TAP_inner();

	lb_set_brightness(br);
	for (i = 0; i < NUM_LEDS; i++)
		lb_set_rgb(i, save[i][0], save[i][1], save[i][2]);

#ifdef CONFIG_LIGHTBAR_POWER_RAILS
	/* Suggest that the lightbar power rails can be shut down again. */
	lb_power(0);
#endif
	return r;
}

/****************************************************************************/
/* Lightbar bytecode interpreter: Lightbyte. */
/****************************************************************************/

static struct lb_program cur_prog;
static struct lb_program next_prog;
static uint8_t pc;

enum lb_color {
	LB_COL_RED,
	LB_COL_GREEN,
	LB_COL_BLUE,
	LB_COL_ALL
};

enum lb_control {
	LB_CONT_COLOR0,
	LB_CONT_COLOR1,
	LB_CONT_PHASE,
	LB_CONT_MAX
};

static uint8_t led_desc[NUM_LEDS][LB_CONT_MAX][3];
static uint32_t lb_ramp_delay;

/* Get one byte of data pointed to by the pc and advance
 * the pc forward.
 */
static inline uint32_t decode_8(uint8_t *dest)
{
	if (pc >= cur_prog.size) {
		CPRINTS("pc 0x%02x out of bounds", pc);
		return EC_RES_INVALID_PARAM;
	}
	*dest = cur_prog.data[pc++];
	return EC_SUCCESS;
}

/* Get four bytes of data pointed to by the pc and advance
 * the pc forward that amount.
 */
static inline uint32_t decode_32(uint32_t *dest)
{
	if (pc >= cur_prog.size - 3) {
		CPRINTS("pc 0x%02x near or out of bounds", pc);
		return EC_RES_INVALID_PARAM;
	}
	*dest  = cur_prog.data[pc++] << 24;
	*dest |= cur_prog.data[pc++] << 16;
	*dest |= cur_prog.data[pc++] <<  8;
	*dest |= cur_prog.data[pc++];
	return EC_SUCCESS;
}

/* JUMP xx - jump to immediate location
 * Changes the pc to the one-byte immediate argument.
 */
static uint32_t lightbyte_JUMP(void)
{
	uint8_t new_pc;
	if (decode_8(&new_pc) != EC_SUCCESS)
		return EC_RES_INVALID_PARAM;

	pc = new_pc;
	return EC_SUCCESS;
}

/* DELAY xx xx xx xx - yield processor for some time
 * Performs an interruptible wait for a number of microseconds
 * given in the four-byte immediate.
 */
static uint32_t lightbyte_DELAY(void)
{
	uint32_t delay_us;
	if (decode_32(&delay_us) != EC_SUCCESS)
		return EC_RES_INVALID_PARAM;

	WAIT_OR_RET(delay_us);
	return EC_SUCCESS;
}

/* SET_BRIGHTNESS xx
 * Sets the current brightness to the given one-byte
 * immediate argument.
 */
static uint32_t lightbyte_SET_BRIGHTNESS(void)
{
	uint8_t val;
	if (decode_8(&val) != EC_SUCCESS)
		return EC_RES_INVALID_PARAM;

	lb_set_brightness(val);
	return EC_SUCCESS;
}

/* SET_COLOR cc xx
 * SET_COLOR cc rr gg bb
 * Stores a color value in the led_desc structure.
 * cc is a bit-packed location to perform the action on.
 *
 * The high four bits are used to describe an LED. If the
 * value is less than NUM_LEDS, it describes a particular LED,
 * and if it is greater than or equal to that value, it
 * will perform the action on all LEDs.
 *
 * The next two bits are the control bits. This should be a value
 * in lb_control that is not LB_CONT_MAX, and the corresponding
 * color will be the one the action is performed on.
 *
 * The last two bits are the color bits. If this is LB_COL_RED,
 * LB_COL_GREEN, or LB_COL_BLUE, then there is only one more byte
 * to decode and this is a color value for that specific color
 * channel. If it is LB_COL_ALL, then there are three more bytes,
 * and it reads like a standard 24-bit color value.
 */
static uint32_t lightbyte_SET_COLOR(void)
{

	uint8_t packed_loc, led, control, color, value;
	int start_led, end_led, color_mask, i, j;
	if (decode_8(&packed_loc) != EC_SUCCESS)
		return EC_RES_INVALID_PARAM;

	led = packed_loc >> 4;
	control = (packed_loc >> 2) & 0x3;
	color = packed_loc & 0x3;

	if (control >= LB_CONT_MAX)
		return EC_RES_INVALID_PARAM;

	if (led >= NUM_LEDS) {
		start_led = 0;
		end_led = NUM_LEDS - 1;
	} else
		start_led = end_led = led;

	color_mask = color == LB_COL_ALL ? 7 : (1 << color);

	for (i = 0; i < 3; i++) {
		if (color_mask & (1 << i)) {
			if (decode_8(&value) != EC_SUCCESS)
				return EC_RES_INVALID_PARAM;
			for (j = start_led; j <= end_led; j++)
				led_desc[j][control][i] = value;
		}
	}

	return EC_SUCCESS;
}

/* SET_DELAY_TIME xx xx xx xx - change ramp speed
 * This sets the length of time between ramp/cycle steps to
 * the four-byte immediate argument, which represents a duration
 * in milliseconds.
 */
static uint32_t lightbyte_SET_DELAY_TIME(void)
{
	uint32_t delay_us;
	if (decode_32(&delay_us) != EC_SUCCESS)
		return EC_RES_INVALID_PARAM;

	lb_ramp_delay = delay_us;
	return EC_SUCCESS;
}

static inline int get_interp_value(int led, int color, int interp)
{
	int base = led_desc[led][LB_CONT_COLOR0][color];
	int delta = led_desc[led][LB_CONT_COLOR1][color] - base;
	return base + (delta * interp / FP_SCALE);
}

/* RAMP_ONCE - simple gradient or color set
 * If the ramp delay is set to zero, then this sets the color of
 * all LEDs to their respective COLOR1.
 * If the ramp delay is nonzero, then this sets their color to
 * their respective COLOR0, and takes them via interpolation to
 * COLOR1, with the delay time passing in between each step.
 */
static uint32_t lightbyte_RAMP_ONCE(void)
{
	int w, i, r, g, b;
	float f;

	/* special case for instantaneous set */
	if (lb_ramp_delay == 0) {
		for (i = 0; i < NUM_LEDS; i++) {
			r = led_desc[i][LB_CONT_COLOR1][LB_COL_RED];
			g = led_desc[i][LB_CONT_COLOR1][LB_COL_GREEN];
			b = led_desc[i][LB_CONT_COLOR1][LB_COL_BLUE];
			lb_set_rgb(i, r, g, b);
		}
		return EC_SUCCESS;
	}

	for (w = 0; w < 128; w++) {
		f = cycle_010(w);
		for (i = 0; i < NUM_LEDS; i++) {
			r = get_interp_value(i, LB_COL_RED, f);
			g = get_interp_value(i, LB_COL_GREEN, f);
			b = get_interp_value(i, LB_COL_BLUE, f);
			lb_set_rgb(i, r, g, b);
		}
		WAIT_OR_RET(lb_ramp_delay);
	}
	return EC_SUCCESS;
}

/* CYCLE_ONCE - simple cycle or color set
 * If the ramp delay is zero, then this sets the color of all LEDs
 * to their respective COLOR0.
 * If the ramp delay is nonzero, this sets the color of all LEDs
 * to COLOR0, then performs a ramp (as in RAMP_ONCE) to COLOR1,
 * and finally back to COLOR0.
 */
static uint32_t lightbyte_CYCLE_ONCE(void)
{
	int w, i, r, g, b;
	float f;

	/* special case for instantaneous set */
	if (lb_ramp_delay == 0) {
		for (i = 0; i < NUM_LEDS; i++) {
			r = led_desc[i][LB_CONT_COLOR0][LB_COL_RED];
			g = led_desc[i][LB_CONT_COLOR0][LB_COL_GREEN];
			b = led_desc[i][LB_CONT_COLOR0][LB_COL_BLUE];
			lb_set_rgb(i, r, g, b);
		}
		return EC_SUCCESS;
	}

	for (w = 0; w < 256; w++) {
		f = cycle_010(w);
		for (i = 0; i < NUM_LEDS; i++) {
			r = get_interp_value(i, LB_COL_RED, f);
			g = get_interp_value(i, LB_COL_GREEN, f);
			b = get_interp_value(i, LB_COL_BLUE, f);
			lb_set_rgb(i, r, g, b);
		}
		WAIT_OR_RET(lb_ramp_delay);
	}
	return EC_SUCCESS;
}

/* CYCLE - repeating cycle
 * Indefinitely ramps from COLOR0 to COLOR1, taking into
 * account the PHASE of each component of each color when
 * interpolating. (Different LEDs and different color channels
 * on a single LED can start at different places in the cycle,
 * though they will advance at the same rate.)
 *
 * If the ramp delay is zero, this instruction will error out.
 */
static uint32_t lightbyte_CYCLE(void)
{
	int w, i, r, g, b;

	/* what does it mean to cycle indefinitely with 0 delay? */
	if (lb_ramp_delay == 0)
		return EC_RES_INVALID_PARAM;

	for (w = 0;; w++) {
		for (i = 0; i < NUM_LEDS; i++) {
			r = get_interp_value(i, LB_COL_RED,
				cycle_010((w & 0xff) +
				led_desc[i][LB_CONT_PHASE][LB_COL_RED]));
			g = get_interp_value(i, LB_COL_GREEN,
				cycle_010((w & 0xff) +
				led_desc[i][LB_CONT_PHASE][LB_COL_GREEN]));
			b = get_interp_value(i, LB_COL_BLUE,
				cycle_010((w & 0xff) +
				led_desc[i][LB_CONT_PHASE][LB_COL_BLUE]));
			lb_set_rgb(i, r, g, b);
		}
		WAIT_OR_RET(lb_ramp_delay);
	}
	return EC_SUCCESS;
}

#undef GET_INTERP_VALUE

#define OPCODE_TABLE		\
	OP(JUMP),		\
	OP(DELAY),		\
	OP(SET_BRIGHTNESS),	\
	OP(SET_COLOR),		\
	OP(SET_DELAY_TIME),	\
	OP(RAMP_ONCE),		\
	OP(CYCLE_ONCE),		\
	OP(CYCLE),

#define OP(X) X
enum lightbyte_opcode {
	OPCODE_TABLE
	HALT,
	MAX_OPCODE
};
#undef OP

#define OP(X) lightbyte_ ## X
static uint32_t (*lightbyte_dispatch[])(void) = {
	OPCODE_TABLE
};
#undef OP

#define OP(X) # X
static const char * const lightbyte_names[] = {
	OPCODE_TABLE
	"HALT"
};
#undef OP

static uint32_t sequence_PROGRAM(void)
{
	uint8_t saved_brightness;
	uint8_t next_inst;
	uint32_t rc;
	uint8_t old_pc;

	/* load next program */
	memcpy(&cur_prog, &next_prog, sizeof(struct lb_program));

	/* reset program state */
	saved_brightness = lb_get_brightness();
	pc = 0;
	memset(led_desc, 0, sizeof(led_desc));
	lb_ramp_delay = 0;

	/* decode-execute loop */
	for (;;) {
		old_pc = pc;
		if (decode_8(&next_inst) != EC_SUCCESS)
			return EC_RES_INVALID_PARAM;

		if (next_inst == HALT) {
			CPRINTS("LB PROGRAM pc: 0x%02x, halting", old_pc);
			lb_set_brightness(saved_brightness);
			return 0;
		} else if (next_inst >= MAX_OPCODE) {
			CPRINTS("LB PROGRAM pc: 0x%02x, "
				"found invalid opcode 0x%02x",
				old_pc, next_inst);
			lb_set_brightness(saved_brightness);
			return EC_RES_INVALID_PARAM;
		} else {
			CPRINTS("LB PROGRAM pc: 0x%02x, opcode 0x%02x -> %s",
				 old_pc, next_inst, lightbyte_names[next_inst]);
			rc = lightbyte_dispatch[next_inst]();
			if (rc) {
				lb_set_brightness(saved_brightness);
				return rc;
			}
		}

		/* yield processor in case we are stuck in a tight loop */
		WAIT_OR_RET(100);
	}
}

/****************************************************************************/
/* The main lightbar task. It just cycles between various pretty patterns. */
/****************************************************************************/

/* Distinguish "normal" sequences from one-shot sequences */
static inline int is_normal_sequence(enum lightbar_sequence seq)
{
	return (seq >= LIGHTBAR_S5 && seq <= LIGHTBAR_S3S5);
}

/* Link each sequence with a command to invoke it. */
struct lightbar_cmd_t {
	const char * const string;
	uint32_t (*sequence)(void);
};

#define LBMSG(state) { #state, sequence_##state }
#include "lightbar_msg_list.h"
static struct lightbar_cmd_t lightbar_cmds[] = {
	LIGHTBAR_MSG_LIST
};
#undef LBMSG

void lightbar_task(void)
{
	uint32_t msg;

	CPRINTS("LB task starting");

	lightbar_restore_state();

	while (1) {
		CPRINTS("LB task %d = %s",
			st.cur_seq, lightbar_cmds[st.cur_seq].string);
		msg = lightbar_cmds[st.cur_seq].sequence();
		if (TASK_EVENT_CUSTOM(msg) == PENDING_MSG) {
			CPRINTS("LB msg %d = %s", pending_msg,
				lightbar_cmds[pending_msg].string);
			if (st.cur_seq != pending_msg) {
				if (is_normal_sequence(st.cur_seq))
					st.prev_seq = st.cur_seq;
				st.cur_seq = pending_msg;
			}
		} else {
			CPRINTS("LB msg 0x%x", msg);
			switch (st.cur_seq) {
			case LIGHTBAR_S5S3:
				st.cur_seq = LIGHTBAR_S3;
				break;
			case LIGHTBAR_S3S0:
				st.cur_seq = LIGHTBAR_S0;
				break;
			case LIGHTBAR_S0S3:
				st.cur_seq = LIGHTBAR_S3;
				break;
			case LIGHTBAR_S3S5:
				st.cur_seq = LIGHTBAR_S5;
				break;
			case LIGHTBAR_TEST:
			case LIGHTBAR_STOP:
			case LIGHTBAR_RUN:
			case LIGHTBAR_ERROR:
			case LIGHTBAR_KONAMI:
			case LIGHTBAR_TAP:
			case LIGHTBAR_PROGRAM:
				st.cur_seq = st.prev_seq;
			default:
				break;
			}
		}
	}
}

/* Function to request a preset sequence from the lightbar task. */
void lightbar_sequence(enum lightbar_sequence num)
{
	if (num > 0 && num < LIGHTBAR_NUM_SEQUENCES) {
		CPRINTS("LB_seq %d = %s", num,
			lightbar_cmds[num].string);
		pending_msg = num;
		task_set_event(TASK_ID_LIGHTBAR,
			       TASK_EVENT_WAKE | TASK_EVENT_CUSTOM(PENDING_MSG),
			       0);
	} else
		CPRINTS("LB_seq %d - ignored", num);
}

/****************************************************************************/
/* Get notifications from other parts of the system */

static void lightbar_startup(void)
{
	lightbar_sequence(LIGHTBAR_S5S3);
}
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, lightbar_startup, HOOK_PRIO_DEFAULT);

static void lightbar_resume(void)
{
	lightbar_sequence(LIGHTBAR_S3S0);
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, lightbar_resume, HOOK_PRIO_DEFAULT);

static void lightbar_suspend(void)
{
	lightbar_sequence(LIGHTBAR_S0S3);
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, lightbar_suspend, HOOK_PRIO_DEFAULT);

static void lightbar_shutdown(void)
{
	lightbar_sequence(LIGHTBAR_S3S5);
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, lightbar_shutdown, HOOK_PRIO_DEFAULT);

/****************************************************************************/
/* Host commands via LPC bus */
/****************************************************************************/

static int lpc_cmd_lightbar(struct host_cmd_handler_args *args)
{
	const struct ec_params_lightbar *in = args->params;
	struct ec_response_lightbar *out = args->response;
	int rv;

	switch (in->cmd) {
	case LIGHTBAR_CMD_DUMP:
		lb_hc_cmd_dump(out);
		args->response_size = sizeof(out->dump);
		break;
	case LIGHTBAR_CMD_OFF:
		lb_off();
		break;
	case LIGHTBAR_CMD_ON:
		lb_on();
		break;
	case LIGHTBAR_CMD_INIT:
		lb_init();
		break;
	case LIGHTBAR_CMD_SET_BRIGHTNESS:
		lb_set_brightness(in->set_brightness.num);
		break;
	case LIGHTBAR_CMD_GET_BRIGHTNESS:
		out->get_brightness.num = lb_get_brightness();
		args->response_size = sizeof(out->get_brightness);
		break;
	case LIGHTBAR_CMD_SEQ:
		lightbar_sequence(in->seq.num);
		break;
	case LIGHTBAR_CMD_REG:
		lb_hc_cmd_reg(in);
		break;
	case LIGHTBAR_CMD_SET_RGB:
		lb_set_rgb(in->set_rgb.led,
			   in->set_rgb.red,
			   in->set_rgb.green,
			   in->set_rgb.blue);
		break;
	case LIGHTBAR_CMD_GET_RGB:
		rv = lb_get_rgb(in->get_rgb.led,
				&out->get_rgb.red,
				&out->get_rgb.green,
				&out->get_rgb.blue);
		if (rv == EC_RES_SUCCESS)
			args->response_size = sizeof(out->get_rgb);
		return rv;
	case LIGHTBAR_CMD_GET_SEQ:
		out->get_seq.num = st.cur_seq;
		args->response_size = sizeof(out->get_seq);
		break;
	case LIGHTBAR_CMD_DEMO:
		demo_mode = in->demo.num ? 1 : 0;
		CPRINTS("LB_demo %d", demo_mode);
		break;
	case LIGHTBAR_CMD_GET_DEMO:
		out->get_demo.num = demo_mode;
		args->response_size = sizeof(out->get_demo);
		break;
	case LIGHTBAR_CMD_GET_PARAMS_V0:
		CPRINTS("LB_get_params_v0 not supported");
		return EC_RES_INVALID_VERSION;
		break;
	case LIGHTBAR_CMD_SET_PARAMS_V0:
		CPRINTS("LB_set_params_v0 not supported");
		return EC_RES_INVALID_VERSION;
		break;
	case LIGHTBAR_CMD_GET_PARAMS_V1:
		CPRINTS("LB_get_params_v1");
		memcpy(&out->get_params_v1, &st.p, sizeof(st.p));
		args->response_size = sizeof(out->get_params_v1);
		break;
	case LIGHTBAR_CMD_SET_PARAMS_V1:
		CPRINTS("LB_set_params_v1");
		memcpy(&st.p, &in->set_params_v1, sizeof(st.p));
		break;
	case LIGHTBAR_CMD_SET_PROGRAM:
		CPRINTS("LB_set_program");
		memcpy(&next_prog, &in->set_program, sizeof(struct lb_program));
		break;
	case LIGHTBAR_CMD_VERSION:
		CPRINTS("LB_version");
		out->version.num = LIGHTBAR_IMPLEMENTATION_VERSION;
		out->version.flags = LIGHTBAR_IMPLEMENTATION_FLAGS;
		args->response_size = sizeof(out->version);
		break;
	default:
		CPRINTS("LB bad cmd 0x%x", in->cmd);
		return EC_RES_INVALID_PARAM;
	}

	return EC_RES_SUCCESS;
}

DECLARE_HOST_COMMAND(EC_CMD_LIGHTBAR_CMD,
		     lpc_cmd_lightbar,
		     EC_VER_MASK(0));

/****************************************************************************/
/* EC console commands */
/****************************************************************************/

#ifdef CONFIG_CONSOLE_CMDHELP
static int help(const char *cmd)
{
	ccprintf("Usage:\n");
	ccprintf("  %s                       - dump all regs\n", cmd);
	ccprintf("  %s off                   - enter standby\n", cmd);
	ccprintf("  %s on                    - leave standby\n", cmd);
	ccprintf("  %s init                  - load default vals\n", cmd);
	ccprintf("  %s brightness [NUM]      - set intensity (0-ff)\n", cmd);
	ccprintf("  %s seq [NUM|SEQUENCE]    - run given pattern"
		 " (no arg for list)\n", cmd);
	ccprintf("  %s CTRL REG VAL          - set LED controller regs\n", cmd);
	ccprintf("  %s LED RED GREEN BLUE    - set color manually"
		 " (LED=%d for all)\n", cmd, NUM_LEDS);
	ccprintf("  %s LED                   - get current LED color\n", cmd);
	ccprintf("  %s demo [0|1]            - turn demo mode on & off\n", cmd);
	ccprintf("  %s params                - show current params\n", cmd);
	ccprintf("  %s program filename      - load lightbyte program\n", cmd);
	ccprintf("  %s version               - show current version\n", cmd);
	return EC_SUCCESS;
}
#endif

static uint8_t find_msg_by_name(const char *str)
{
	uint8_t i;
	for (i = 0; i < LIGHTBAR_NUM_SEQUENCES; i++)
		if (!strcasecmp(str, lightbar_cmds[i].string))
			return i;

	return LIGHTBAR_NUM_SEQUENCES;
}

static void show_msg_names(void)
{
	int i;
	ccprintf("Sequences:");
	for (i = 0; i < LIGHTBAR_NUM_SEQUENCES; i++)
		ccprintf(" %s", lightbar_cmds[i].string);
	ccprintf("\nCurrent = 0x%x %s\n", st.cur_seq,
		 lightbar_cmds[st.cur_seq].string);
}

static void show_params_v1(const struct lightbar_params_v1 *p)
{
	int i;

	ccprintf("%d\t\t# .google_ramp_up\n", p->google_ramp_up);
	ccprintf("%d\t\t# .google_ramp_down\n", p->google_ramp_down);
	ccprintf("%d\t\t# .s3s0_ramp_up\n", p->s3s0_ramp_up);
	ccprintf("%d\t\t# .s0_tick_delay (battery)\n", p->s0_tick_delay[0]);
	ccprintf("%d\t\t# .s0_tick_delay (AC)\n", p->s0_tick_delay[1]);
	ccprintf("%d\t\t# .s0a_tick_delay (battery)\n", p->s0a_tick_delay[0]);
	ccprintf("%d\t\t# .s0a_tick_delay (AC)\n", p->s0a_tick_delay[1]);
	ccprintf("%d\t\t# .s0s3_ramp_down\n", p->s0s3_ramp_down);
	ccprintf("%d\t\t# .s3_sleep_for\n", p->s3_sleep_for);
	ccprintf("%d\t\t# .s3_ramp_up\n", p->s3_ramp_up);
	ccprintf("%d\t\t# .s3_ramp_down\n", p->s3_ramp_down);
	ccprintf("%d\t\t# .tap_tick_delay\n", p->tap_tick_delay);
	ccprintf("%d\t\t# .tap_display_time\n", p->tap_display_time);
	ccprintf("%d\t\t# .tap_pct_red\n", p->tap_pct_red);
	ccprintf("%d\t\t# .tap_pct_green\n", p->tap_pct_green);
	ccprintf("%d\t\t# .tap_seg_min_on\n", p->tap_seg_min_on);
	ccprintf("%d\t\t# .tap_seg_max_on\n", p->tap_seg_max_on);
	ccprintf("%d\t\t# .tap_seg_osc\n", p->tap_seg_osc);
	ccprintf("%d %d %d\t\t# .tap_idx\n",
		 p->tap_idx[0], p->tap_idx[1], p->tap_idx[2]);
	ccprintf("0x%02x 0x%02x\t# .osc_min (battery, AC)\n",
		 p->osc_min[0], p->osc_min[1]);
	ccprintf("0x%02x 0x%02x\t# .osc_max (battery, AC)\n",
		 p->osc_max[0], p->osc_max[1]);
	ccprintf("%d %d\t\t# .w_ofs (battery, AC)\n",
		 p->w_ofs[0], p->w_ofs[1]);
	ccprintf("0x%02x 0x%02x\t# .bright_bl_off_fixed (battery, AC)\n",
		 p->bright_bl_off_fixed[0], p->bright_bl_off_fixed[1]);
	ccprintf("0x%02x 0x%02x\t# .bright_bl_on_min (battery, AC)\n",
		 p->bright_bl_on_min[0], p->bright_bl_on_min[1]);
	ccprintf("0x%02x 0x%02x\t# .bright_bl_on_max (battery, AC)\n",
		 p->bright_bl_on_max[0], p->bright_bl_on_max[1]);
	ccprintf("%d %d %d\t# .battery_threshold\n",
		 p->battery_threshold[0],
		 p->battery_threshold[1],
		 p->battery_threshold[2]);
	ccprintf("%d %d %d %d\t\t# .s0_idx[] (battery)\n",
		 p->s0_idx[0][0], p->s0_idx[0][1],
		 p->s0_idx[0][2], p->s0_idx[0][3]);
	ccprintf("%d %d %d %d\t\t# .s0_idx[] (AC)\n",
		 p->s0_idx[1][0], p->s0_idx[1][1],
		 p->s0_idx[1][2], p->s0_idx[1][3]);
	ccprintf("%d %d %d %d\t# .s3_idx[] (battery)\n",
		 p->s3_idx[0][0], p->s3_idx[0][1],
		 p->s3_idx[0][2], p->s3_idx[0][3]);
	ccprintf("%d %d %d %d\t# .s3_idx[] (AC)\n",
		 p->s3_idx[1][0], p->s3_idx[1][1],
		 p->s3_idx[1][2], p->s3_idx[1][3]);
	for (i = 0; i < ARRAY_SIZE(p->color); i++)
		ccprintf("0x%02x 0x%02x 0x%02x\t# color[%d]\n",
			 p->color[i].r,
			 p->color[i].g,
			 p->color[i].b, i);
}

static int command_lightbar(int argc, char **argv)
{
	int i;
	uint8_t num, led, r = 0, g = 0, b = 0;
	struct ec_response_lightbar out;
	char *e;

	if (argc == 1) {			/* no args = dump 'em all */
		lb_hc_cmd_dump(&out);
		for (i = 0; i < ARRAY_SIZE(out.dump.vals); i++)
			ccprintf(" %02x     %02x     %02x\n",
				 out.dump.vals[i].reg,
				 out.dump.vals[i].ic0,
				 out.dump.vals[i].ic1);

		return EC_SUCCESS;
	}

	if (!strcasecmp(argv[1], "init")) {
		lb_init();
		return EC_SUCCESS;
	}

	if (!strcasecmp(argv[1], "off")) {
		lb_off();
		return EC_SUCCESS;
	}

	if (!strcasecmp(argv[1], "on")) {
		lb_on();
		return EC_SUCCESS;
	}

	if (!strcasecmp(argv[1], "params")) {
#ifdef LIGHTBAR_SIMULATION
		if (argc > 2)
			lb_read_params_from_file(argv[2], &st.p);
#endif
		show_params_v1(&st.p);
		return EC_SUCCESS;
	}

	if (!strcasecmp(argv[1], "version")) {
		ccprintf("version %d flags 0x%x\n",
			 LIGHTBAR_IMPLEMENTATION_VERSION,
			 LIGHTBAR_IMPLEMENTATION_FLAGS);
		return EC_SUCCESS;
	}

	if (!strcasecmp(argv[1], "brightness")) {
		if (argc > 2) {
			num = 0xff & strtoi(argv[2], &e, 16);
			lb_set_brightness(num);
		}
		ccprintf("brightness is %02x\n", lb_get_brightness());
		return EC_SUCCESS;
	}

	if (!strcasecmp(argv[1], "demo")) {
		if (argc > 2) {
			if (!strcasecmp(argv[2], "on") ||
			    argv[2][0] == '1')
				demo_mode = 1;
			else if (!strcasecmp(argv[2], "off") ||
				 argv[2][0] == '0')
				demo_mode = 0;
			else
				return EC_ERROR_PARAM1;
		}
		ccprintf("demo mode is %s\n", demo_mode ? "on" : "off");
		return EC_SUCCESS;
	}

	if (!strcasecmp(argv[1], "seq")) {
		if (argc == 2) {
			show_msg_names();
			return 0;
		}
		num = 0xff & strtoi(argv[2], &e, 16);
		if (*e)
			num = find_msg_by_name(argv[2]);
		if (num >= LIGHTBAR_NUM_SEQUENCES)
			return EC_ERROR_PARAM2;
		lightbar_sequence(num);
		return EC_SUCCESS;
	}

	if (argc >= 3 && !strcasecmp(argv[1], "program")) {
#ifdef LIGHTBAR_SIMULATION
		return lb_load_program(argv[2], &next_prog);
#else
		ccprintf("can't load program from console\n");
		return EC_ERROR_INVAL;
#endif
	}

	if (argc == 4) {
		struct ec_params_lightbar in;
		in.reg.ctrl = strtoi(argv[1], &e, 16);
		in.reg.reg = strtoi(argv[2], &e, 16);
		in.reg.value = strtoi(argv[3], &e, 16);
		lb_hc_cmd_reg(&in);
		return EC_SUCCESS;
	}

	if (argc == 5) {
		led = strtoi(argv[1], &e, 16);
		r = strtoi(argv[2], &e, 16);
		g = strtoi(argv[3], &e, 16);
		b = strtoi(argv[4], &e, 16);
		lb_set_rgb(led, r, g, b);
		return EC_SUCCESS;
	}

	/* Only thing left is to try to read an LED value */
	num = strtoi(argv[1], &e, 16);
	if (!(e && *e)) {
		if (num >= NUM_LEDS) {
			for (i = 0; i < NUM_LEDS; i++) {
				lb_get_rgb(i, &r, &g, &b);
				ccprintf("%x: %02x %02x %02x\n", i, r, g, b);
			}
		} else {
			lb_get_rgb(num, &r, &g, &b);
			ccprintf("%02x %02x %02x\n", r, g, b);
		}
		return EC_SUCCESS;
	}


#ifdef CONFIG_CONSOLE_CMDHELP
	help(argv[0]);
#endif

	return EC_ERROR_INVAL;
}
DECLARE_CONSOLE_COMMAND(lightbar, command_lightbar,
			"[help | COMMAND [ARGS]]",
			"Get/set lightbar state",
			NULL);
