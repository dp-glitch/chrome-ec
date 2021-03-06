/* Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __ZEPHYR_GPIO_MAP_H
#define __ZEPHYR_GPIO_MAP_H

#include <devicetree.h>
#include <gpio_signal.h>

#define GPIO_AC_PRESENT			NAMED_GPIO(acok_od)
#define GPIO_CPU_PROCHOT		NAMED_GPIO(ec_prochot_odl)
#define GPIO_EC_PCH_WAKE_ODL		NAMED_GPIO(pch_wake_l)
#define GPIO_EN_A_RAILS			NAMED_GPIO(en_a_rails)
#define GPIO_EN_PP5000_A		NAMED_GPIO(en_pp5000_a)
#define GPIO_EN_PP5000			NAMED_GPIO(en_pp5000_a)
#define GPIO_ENABLE_BACKLIGHT		NAMED_GPIO(gpio_edp_bklten_od)
#define GPIO_ENTERING_RW		NAMED_GPIO(entering_rw)
#define GPIO_LID_OPEN			NAMED_GPIO(lid_open)
#define GPIO_PCH_PWRBTN_L		NAMED_GPIO(pch_pwrbtn_l)
#define GPIO_PCH_RSMRST_L		NAMED_GPIO(ec_pch_rsmrst_l)
#define GPIO_PCH_SLP_S0_L		NAMED_GPIO(slp_s0_l)
#define GPIO_PCH_SLP_S3_L		NAMED_GPIO(slp_s3_l)
#define GPIO_PCH_SLP_S4_L		NAMED_GPIO(slp_s4_l)
#define GPIO_PCH_SYS_PWROK		NAMED_GPIO(ec_pch_sys_pwrok)
#define GPIO_PG_EC_ALL_SYS_PWRGD	NAMED_GPIO(pg_ec_all_sys_pwrgd)
#define GPIO_POWER_BUTTON_L		NAMED_GPIO(power_button_l)
#define GPIO_PP5000_A_PG_OD		NAMED_GPIO(pp5000_a_pg_od)
#define GPIO_RSMRST_L_PGOOD		NAMED_GPIO(pg_ec_rsmrst_l)
#define GPIO_SYS_RESET_L		NAMED_GPIO(sys_reset_l)
#define GPIO_WP_L			NAMED_GPIO(wp_l)

/* Cometlake power sequencing requires this definition */
#define PP5000_PGOOD_POWER_SIGNAL_MASK POWER_SIGNAL_MASK(X86_PP5000_A_PGOOD)

/*
 * Set EC_CROS_GPIO_INTERRUPTS to a space-separated list of GPIO_INT items.
 *
 * Each GPIO_INT requires three parameters:
 *   gpio_signal - The enum gpio_signal for the interrupt gpio
 *   interrupt_flags - The interrupt-related flags (e.g. GPIO_INT_EDGE_BOTH)
 *   handler - The platform/ec interrupt handler.
 *
 * Ensure that this files includes all necessary headers to declare all
 * referenced handler functions.
 *
 * For example, one could use the follow definition:
 * #define EC_CROS_GPIO_INTERRUPTS \
 *   GPIO_INT(NAMED_GPIO(h1_ec_pwr_btn_odl), GPIO_INT_EDGE_BOTH, button_print)
 */
#define EC_CROS_GPIO_INTERRUPTS                                               \
	GPIO_INT(NAMED_GPIO(lid_open), GPIO_INT_EDGE_BOTH, lid_interrupt)     \
	GPIO_INT(NAMED_GPIO(power_button_l), GPIO_INT_EDGE_BOTH,              \
		 power_button_interrupt)                                      \
	GPIO_INT(NAMED_GPIO(acok_od), GPIO_INT_EDGE_BOTH, extpower_interrupt) \
	GPIO_INT(NAMED_GPIO(slp_s0_l), GPIO_INT_EDGE_BOTH,                    \
		 power_signal_interrupt)                                      \
	GPIO_INT(NAMED_GPIO(slp_s3_l), GPIO_INT_EDGE_BOTH,                    \
		 power_signal_interrupt)                                      \
	GPIO_INT(NAMED_GPIO(slp_s4_l), GPIO_INT_EDGE_BOTH,                    \
		 power_signal_interrupt)                                      \
	GPIO_INT(NAMED_GPIO(pg_ec_rsmrst_l), GPIO_INT_EDGE_BOTH,              \
		 intel_x86_rsmrst_signal_interrupt)                           \
	GPIO_INT(NAMED_GPIO(pg_ec_all_sys_pwrgd), GPIO_INT_EDGE_BOTH,         \
		 power_signal_interrupt)                                      \
	GPIO_INT(NAMED_GPIO(pp5000_a_pg_od), GPIO_INT_EDGE_BOTH,              \
		 power_signal_interrupt)

#endif /* __ZEPHYR_GPIO_MAP_H */
