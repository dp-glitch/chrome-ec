/* Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Waddledoo board-specific configuration */

#include "adc_chip.h"
#include "button.h"
#include "cbi_fw_config.h"
#include "charge_manager.h"
#include "charge_state_v2.h"
#include "charger.h"
#include "chipset.h"
#include "common.h"
#include "compile_time_macros.h"
#include "driver/accel_bma2x2.h"
#include "driver/accelgyro_bmi_common.h"
#include "driver/accelgyro_lsm6dsm.h"
#include "driver/bc12/pi3usb9201.h"
#include "driver/charger/isl923x.h"
#include "driver/retimer/nb7v904m.h"
#include "driver/sync.h"
#include "driver/tcpm/raa489000.h"
#include "driver/tcpm/tcpci.h"
#include "driver/usb_mux/pi3usb3x532.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "i2c.h"
#include "keyboard_scan.h"
#include "lid_switch.h"
#include "motion_sense.h"
#include "power.h"
#include "power_button.h"
#include "pwm.h"
#include "pwm_chip.h"
#include "stdbool.h"
#include "switch.h"
#include "system.h"
#include "tablet_mode.h"
#include "task.h"
#include "usb_mux.h"
#include "usb_pd.h"
#include "usb_pd_tcpm.h"

#define CPRINTS(format, args...) cprints(CC_USBCHARGE, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_USBCHARGE, format, ## args)

static void tcpc_alert_event(enum gpio_signal s)
{
	int port = (s == GPIO_USB_C0_INT_ODL) ? 0 : 1;

	schedule_deferred_pd_interrupt(port);
}

static void usb_c0_interrupt(enum gpio_signal s)
{
	/*
	 * The interrupt line is shared between the TCPC and BC 1.2 detection
	 * chip.  Therefore we'll need to check both ICs.
	 */
	tcpc_alert_event(s);
	task_set_event(TASK_ID_USB_CHG_P0, USB_CHG_EVENT_BC12, 0);
}

static void sub_usb_c1_interrupt(enum gpio_signal s)
{
	/*
	 * The interrupt line is shared between the TCPC and BC 1.2 detection
	 * chip.  Therefore we'll need to check both ICs.
	 */
	tcpc_alert_event(s);
	task_set_event(TASK_ID_USB_CHG_P1, USB_CHG_EVENT_BC12, 0);
}

static void sub_hdmi_hpd_interrupt(enum gpio_signal s)
{
	int hdmi_hpd_odl = gpio_get_level(GPIO_EC_I2C_SUB_C1_SDA_HDMI_HPD_ODL);

	gpio_set_level(GPIO_EC_AP_USB_C1_HDMI_HPD, !hdmi_hpd_odl);
}

static void c0_ccsbu_ovp_interrupt(enum gpio_signal s)
{
	cprints(CC_USBPD, "C0: CC OVP, SBU OVP, or thermal event");
	pd_handle_cc_overvoltage(0);
}

#include "gpio_list.h"

/* ADC channels */
const struct adc_t adc_channels[] = {
	[ADC_TEMP_SENSOR_1] = {
		.name = "TEMP_SENSOR1",
		.input_ch = NPCX_ADC_CH0,
		.factor_mul = ADC_MAX_VOLT,
		.factor_div = ADC_READ_MAX + 1,
		.shift = 0,
	},
	[ADC_TEMP_SENSOR_2] = {
		.name = "TEMP_SENSOR2",
		.input_ch = NPCX_ADC_CH1,
		.factor_mul = ADC_MAX_VOLT,
		.factor_div = ADC_READ_MAX + 1,
		.shift = 0,
	},
	[ADC_SUB_ANALOG] = {
		.name = "SUB_ANALOG",
		.input_ch = NPCX_ADC_CH2,
		.factor_mul = ADC_MAX_VOLT,
		.factor_div = ADC_READ_MAX + 1,
		.shift = 0,
	},
	[ADC_VSNS_PP3300_A] = {
		.name = "PP3300_A_PGOOD",
		.input_ch = NPCX_ADC_CH9,
		.factor_mul = ADC_MAX_VOLT,
		.factor_div = ADC_READ_MAX + 1,
		.shift = 0,
	},
};
BUILD_ASSERT(ARRAY_SIZE(adc_channels) == ADC_CH_COUNT);

void board_init(void)
{
	int on;

	gpio_enable_interrupt(GPIO_USB_C0_INT_ODL);
	gpio_enable_interrupt(GPIO_USB_C0_CCSBU_OVP_ODL);

	if (get_cbi_fw_config_db() == DB_1A_HDMI) {
		/* Disable i2c on HDMI pins */
		gpio_config_pin(MODULE_I2C,
				GPIO_EC_I2C_SUB_C1_SDA_HDMI_HPD_ODL, 0);
		gpio_config_pin(MODULE_I2C,
				GPIO_EC_I2C_SUB_C1_SCL_HDMI_EN_ODL, 0);

		/* Set HDMI and sub-rail enables to output */
		gpio_set_flags(GPIO_EC_I2C_SUB_C1_SCL_HDMI_EN_ODL,
			       chipset_in_state(CHIPSET_STATE_ON) ?
						GPIO_ODR_LOW : GPIO_ODR_HIGH);
		gpio_set_flags(GPIO_SUB_C1_INT_EN_RAILS_ODL,   GPIO_ODR_HIGH);

		/* Select HDMI option */
		gpio_set_level(GPIO_HDMI_SEL_L, 0);

		/* Enable interrupt for passing through HPD */
		gpio_enable_interrupt(GPIO_EC_I2C_SUB_C1_SDA_HDMI_HPD_ODL);
	} else {
		/* Set SDA as an input */
		gpio_set_flags(GPIO_EC_I2C_SUB_C1_SDA_HDMI_HPD_ODL,
			       GPIO_INPUT);

		/* Enable C1 interrupts */
		gpio_enable_interrupt(GPIO_SUB_C1_INT_EN_RAILS_ODL);
	}
	/* Enable gpio interrupt for base accelgyro sensor */
	gpio_enable_interrupt(GPIO_BASE_SIXAXIS_INT_L);

	/* Turn on 5V if the system is on, otherwise turn it off. */
	on = chipset_in_state(CHIPSET_STATE_ON | CHIPSET_STATE_ANY_SUSPEND);
	board_power_5v_enable(on);
}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_DEFAULT);

/* Enable HDMI any time the SoC is on */
static void hdmi_enable(void)
{
	gpio_set_level(GPIO_EC_I2C_SUB_C1_SCL_HDMI_EN_ODL, 0);
}
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, hdmi_enable, HOOK_PRIO_DEFAULT);

static void hdmi_disable(void)
{
	gpio_set_level(GPIO_EC_I2C_SUB_C1_SCL_HDMI_EN_ODL, 1);
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, hdmi_disable, HOOK_PRIO_DEFAULT);

void board_hibernate(void)
{
	/*
	 * Both charger ICs need to be put into their "low power mode" before
	 * entering the Z-state.
	 */
	if (board_get_charger_chip_count() > 1)
		raa489000_hibernate(1);
	raa489000_hibernate(0);
}

void board_reset_pd_mcu(void)
{
	/*
	 * TODO(b:147316511): Here we could issue a digital reset to the IC,
	 * unsure if we actually want to do that or not yet.
	 */
}

static void reconfigure_5v_gpio(void)
{
	/*
	 * b/147257497: On early boards, GPIO_EN_PP5000 was swapped with
	 * GPIO_VOLUP_BTN_ODL. Therefore, we'll actually need to set that GPIO
	 * instead for those boards.  Note that this breaks the volume up button
	 * functionality.
	 */
	if (system_get_board_version() < 0) {
		CPRINTS("old board - remapping 5V en");
		gpio_set_flags(GPIO_VOLUP_BTN_ODL, GPIO_OUT_LOW);
	}
}
DECLARE_HOOK(HOOK_INIT, reconfigure_5v_gpio, HOOK_PRIO_INIT_I2C+1);

static void set_5v_gpio(int level)
{
	int version;
	enum gpio_signal gpio;

	/*
	 * b/147257497: On early boards, GPIO_EN_PP5000 was swapped with
	 * GPIO_VOLUP_BTN_ODL. Therefore, we'll actually need to set that GPIO
	 * instead for those boards.  Note that this breaks the volume up button
	 * functionality.
	 */
	version = system_get_board_version();

	/*
	 * If the CBI EEPROM wasn't formatted, assume it's a very early board.
	 */
	gpio = version < 0 ? GPIO_VOLUP_BTN_ODL : GPIO_EN_PP5000;

	gpio_set_level(gpio, level);
}

__override void board_power_5v_enable(int enable)
{
	/*
	 * Port 0 simply has a GPIO to turn on the 5V regulator, however, 5V is
	 * generated locally on the sub board and we need to set the comparator
	 * polarity on the sub board charger IC, or send enable signal to HDMI
	 * DB.
	 */
	set_5v_gpio(!!enable);

	if (get_cbi_fw_config_db() == DB_1A_HDMI) {
		gpio_set_level(GPIO_SUB_C1_INT_EN_RAILS_ODL, !enable);
	} else {
		if (isl923x_set_comparator_inversion(1, !!enable))
			CPRINTS("Failed to %sable sub rails!", enable ?
								"en" : "dis");
	}

}

__override uint8_t board_get_usb_pd_port_count(void)
{
	if (get_cbi_fw_config_db() == DB_1A_HDMI)
		return CONFIG_USB_PD_PORT_MAX_COUNT - 1;
	else
		return CONFIG_USB_PD_PORT_MAX_COUNT;
}

__override uint8_t board_get_charger_chip_count(void)
{
	if (get_cbi_fw_config_db() == DB_1A_HDMI)
		return CHARGER_NUM - 1;
	else
		return CHARGER_NUM;
}

int board_is_sourcing_vbus(int port)
{
	int regval;

	tcpc_read(port, TCPC_REG_POWER_STATUS, &regval);
	return !!(regval & TCPC_REG_POWER_STATUS_SOURCING_VBUS);

}

int board_set_active_charge_port(int port)
{
	int is_real_port = (port >= 0 &&
			    port < board_get_usb_pd_port_count());
	int i;
	int old_port;

	if (!is_real_port && port != CHARGE_PORT_NONE)
		return EC_ERROR_INVAL;

	old_port = charge_manager_get_active_charge_port();

	CPRINTS("New chg p%d", port);

	/* Disable all ports. */
	if (port == CHARGE_PORT_NONE) {
		for (i = 0; i < board_get_usb_pd_port_count(); i++)
			tcpc_write(i, TCPC_REG_COMMAND,
				   TCPC_REG_COMMAND_SNK_CTRL_LOW);

		return EC_SUCCESS;
	}

	/* Check if port is sourcing VBUS. */
	if (board_is_sourcing_vbus(port)) {
		CPRINTS("Skip enable p%d", port);
		return EC_ERROR_INVAL;
	}

	/*
	 * Turn off the other ports' sink path FETs, before enabling the
	 * requested charge port.
	 */
	for (i = 0; i < board_get_usb_pd_port_count(); i++) {
		if (i == port)
			continue;

		if (tcpc_write(i, TCPC_REG_COMMAND,
			       TCPC_REG_COMMAND_SNK_CTRL_LOW))
			CPRINTS("p%d: sink path disable failed.", i);
	}

	/*
	 * Stop the charger IC from switching while changing ports.  Otherwise,
	 * we can overcurrent the adapter we're switching to. (crbug.com/926056)
	 */
	if (old_port != CHARGE_PORT_NONE)
		charger_discharge_on_ac(1);

	/* Enable requested charge port. */
	if (tcpc_write(port, TCPC_REG_COMMAND,
		       TCPC_REG_COMMAND_SNK_CTRL_HIGH)) {
		CPRINTS("p%d: sink path enable failed.", port);
		charger_discharge_on_ac(0);
		return EC_ERROR_UNKNOWN;
	}

	/* Allow the charger IC to begin/continue switching. */
	charger_discharge_on_ac(0);

	return EC_SUCCESS;
}

void board_set_charge_limit(int port, int supplier, int charge_ma,
			    int max_ma, int charge_mv)
{
	int icl = MAX(charge_ma, CONFIG_CHARGER_INPUT_CURRENT);

	/*
	 * TODO(b:147463641): Characterize the input current limit in case that
	 * a scaling needs to be applied here.
	 */
	charge_set_input_current_limit(icl, charge_mv);
}

/* Sensors */
static struct mutex g_lid_mutex;
static struct mutex g_base_mutex;

/* Matrices to rotate accelerometers into the standard reference. */
static const mat33_fp_t lid_standard_ref = {
	{ 0, FLOAT_TO_FP(1), 0},
	{ FLOAT_TO_FP(-1), 0, 0},
	{ 0, 0, FLOAT_TO_FP(1)}
};

static struct accelgyro_saved_data_t g_bma253_data;
static struct lsm6dsm_data lsm6dsm_data = LSM6DSM_DATA;

struct motion_sensor_t motion_sensors[] = {
	[LID_ACCEL] = {
		.name = "Lid Accel",
		.active_mask = SENSOR_ACTIVE_S0_S3,
		.chip = MOTIONSENSE_CHIP_BMA255,
		.type = MOTIONSENSE_TYPE_ACCEL,
		.location = MOTIONSENSE_LOC_LID,
		.drv = &bma2x2_accel_drv,
		.mutex = &g_lid_mutex,
		.drv_data = &g_bma253_data,
		.port = I2C_PORT_SENSOR,
		.i2c_spi_addr_flags = BMA2x2_I2C_ADDR1_FLAGS,
		.rot_standard_ref = &lid_standard_ref,
		.default_range = 2,
		.min_frequency = BMA255_ACCEL_MIN_FREQ,
		.max_frequency = BMA255_ACCEL_MAX_FREQ,
		.config = {
			[SENSOR_CONFIG_EC_S0] = {
				.odr = 10000 | ROUND_UP_FLAG,
			},
			[SENSOR_CONFIG_EC_S3] = {
				.odr = 10000 | ROUND_UP_FLAG,
			},
		},
	},
	[BASE_ACCEL] = {
		.name = "Base Accel",
		.active_mask = SENSOR_ACTIVE_S0_S3,
		.chip = MOTIONSENSE_CHIP_LSM6DSM,
		.type = MOTIONSENSE_TYPE_ACCEL,
		.location = MOTIONSENSE_LOC_BASE,
		.drv = &lsm6dsm_drv,
		.mutex = &g_base_mutex,
		.drv_data = LSM6DSM_ST_DATA(lsm6dsm_data,
				MOTIONSENSE_TYPE_ACCEL),
		.int_signal = GPIO_BASE_SIXAXIS_INT_L,
		.flags = MOTIONSENSE_FLAG_INT_SIGNAL,
		.port = I2C_PORT_SENSOR,
		.i2c_spi_addr_flags = LSM6DSM_ADDR0_FLAGS,
		.rot_standard_ref = NULL,
		.default_range = 4,  /* g */
		.min_frequency = LSM6DSM_ODR_MIN_VAL,
		.max_frequency = LSM6DSM_ODR_MAX_VAL,
		.config = {
			[SENSOR_CONFIG_EC_S0] = {
				.odr = 13000 | ROUND_UP_FLAG,
				.ec_rate = 100 * MSEC,
			},
			[SENSOR_CONFIG_EC_S3] = {
				.odr = 10000 | ROUND_UP_FLAG,
				.ec_rate = 100 * MSEC,
			},
		},
	},
	[BASE_GYRO] = {
		.name = "Base Gyro",
		.active_mask = SENSOR_ACTIVE_S0_S3,
		.chip = MOTIONSENSE_CHIP_LSM6DSM,
		.type = MOTIONSENSE_TYPE_GYRO,
		.location = MOTIONSENSE_LOC_BASE,
		.drv = &lsm6dsm_drv,
		.mutex = &g_base_mutex,
		.drv_data = LSM6DSM_ST_DATA(lsm6dsm_data,
				MOTIONSENSE_TYPE_GYRO),
		.int_signal = GPIO_BASE_SIXAXIS_INT_L,
		.flags = MOTIONSENSE_FLAG_INT_SIGNAL,
		.port = I2C_PORT_SENSOR,
		.i2c_spi_addr_flags = LSM6DSM_ADDR0_FLAGS,
		.default_range = 1000 | ROUND_UP_FLAG, /* dps */
		.rot_standard_ref = NULL,
		.min_frequency = LSM6DSM_ODR_MIN_VAL,
		.max_frequency = LSM6DSM_ODR_MAX_VAL,
	},
	[VSYNC] = {
		.name = "Camera VSYNC",
		.active_mask = SENSOR_ACTIVE_S0,
		.chip = MOTIONSENSE_CHIP_GPIO,
		.type = MOTIONSENSE_TYPE_SYNC,
		.location = MOTIONSENSE_LOC_CAMERA,
		.drv = &sync_drv,
		.default_range = 0,
		.min_frequency = 0,
		.max_frequency = 1,
	},
};

const unsigned int motion_sensor_count = ARRAY_SIZE(motion_sensors);

__override void ocpc_get_pid_constants(int *kp, int *kp_div,
				       int *ki, int *ki_div,
				       int *kd, int *kd_div)
{
	/*
	 * Early boards need different constants due to a change in charger IC
	 * silicon revision.
	 */
	if (system_get_board_version() >= 0) {
		*kp = 1;
		*kp_div = 128;
		*ki = 1;
		*ki_div = 1024;
		*kd = 0;
		*kd_div = 1;
	} else {
		*kp = 1;
		*kp_div = 4;
		*ki = 1;
		*ki_div = 15;
		*kd = 1;
		*kd_div = 10;
	}
}

int pd_snk_is_vbus_provided(int port)
{
	return pd_check_vbus_level(port, VBUS_PRESENT);
}

const struct charger_config_t chg_chips[] = {
	{
		.i2c_port = I2C_PORT_USB_C0,
		.i2c_addr_flags = ISL923X_ADDR_FLAGS,
		.drv = &isl923x_drv,
	},

	{
		.i2c_port = I2C_PORT_SUB_USB_C1,
		.i2c_addr_flags = ISL923X_ADDR_FLAGS,
		.drv = &isl923x_drv,
	},
};

const struct pi3usb9201_config_t pi3usb9201_bc12_chips[] = {
	{
		.i2c_port = I2C_PORT_USB_C0,
		.i2c_addr_flags = PI3USB9201_I2C_ADDR_3_FLAGS,
		.flags = PI3USB9201_ALWAYS_POWERED,
	},

	{
		.i2c_port = I2C_PORT_SUB_USB_C1,
		.i2c_addr_flags = PI3USB9201_I2C_ADDR_3_FLAGS,
		.flags = PI3USB9201_ALWAYS_POWERED,
	},
};

/* PWM channels. Must be in the exactly same order as in enum pwm_channel. */
const struct pwm_t pwm_channels[] = {
	[PWM_CH_KBLIGHT] = {
		.channel = 3,
		.flags = PWM_CONFIG_DSLEEP,
		.freq = 10000,
	},

	[PWM_CH_LED1_AMBER] = {
		.channel = 2,
		.flags = PWM_CONFIG_DSLEEP | PWM_CONFIG_ACTIVE_LOW,
		.freq = 2400,
	},

	[PWM_CH_LED2_WHITE] = {
		.channel = 0,
		.flags = PWM_CONFIG_DSLEEP | PWM_CONFIG_ACTIVE_LOW,
		.freq = 2400,
	}
};
BUILD_ASSERT(ARRAY_SIZE(pwm_channels) == PWM_CH_COUNT);

const struct tcpc_config_t tcpc_config[CONFIG_USB_PD_PORT_MAX_COUNT] = {
	{
		.bus_type = EC_BUS_TYPE_I2C,
		.i2c_info = {
			.port = I2C_PORT_USB_C0,
			.addr_flags = RAA489000_TCPC0_I2C_FLAGS,
		},
		.flags = TCPC_FLAGS_TCPCI_REV2_0,
		.drv = &raa489000_tcpm_drv,
	},

	{
		.bus_type = EC_BUS_TYPE_I2C,
		.i2c_info = {
			.port = I2C_PORT_SUB_USB_C1,
			.addr_flags = RAA489000_TCPC0_I2C_FLAGS,
		},
		.flags = TCPC_FLAGS_TCPCI_REV2_0,
		.drv = &raa489000_tcpm_drv,
	},
};

const struct usb_mux usbc0_retimer = {
	.usb_port = 0,
	.i2c_port = I2C_PORT_USB_C0,
	.i2c_addr_flags = NB7V904M_I2C_ADDR0,
	.driver = &nb7v904m_usb_redriver_drv,
};
const struct usb_mux usbc1_retimer = {
	.usb_port = 1,
	.i2c_port = I2C_PORT_SUB_USB_C1,
	.i2c_addr_flags = NB7V904M_I2C_ADDR0,
	.driver = &nb7v904m_usb_redriver_drv,
};
const struct usb_mux usb_muxes[CONFIG_USB_PD_PORT_MAX_COUNT] = {
	{
		.usb_port = 0,
		.i2c_port = I2C_PORT_USB_C0,
		.i2c_addr_flags = PI3USB3X532_I2C_ADDR0,
		.driver = &pi3usb3x532_usb_mux_driver,
		.next_mux = &usbc0_retimer,
	},
	{
		.usb_port = 1,
		.i2c_port = I2C_PORT_SUB_USB_C1,
		.i2c_addr_flags = PI3USB3X532_I2C_ADDR0,
		.driver = &pi3usb3x532_usb_mux_driver,
		.next_mux = &usbc1_retimer,
	}
};

uint16_t tcpc_get_alert_status(void)
{
	uint16_t status = 0;
	int regval;

	/*
	 * The interrupt line is shared between the TCPC and BC1.2 detector IC.
	 * Therefore, go out and actually read the alert registers to report the
	 * alert status.
	 */
	if (!gpio_get_level(GPIO_USB_C0_INT_ODL)) {
		if (!tcpc_read16(0, TCPC_REG_ALERT, &regval)) {
			/* The TCPCI Rev 1.0 spec says to ignore bits 14:12. */
			if (!(tcpc_config[0].flags & TCPC_FLAGS_TCPCI_REV2_0))
				regval &= ~((1 << 14) | (1 << 13) | (1 << 12));

			if (regval)
				status |= PD_STATUS_TCPC_ALERT_0;
		}
	}

	if (board_get_usb_pd_port_count() > 1 &&
				!gpio_get_level(GPIO_SUB_C1_INT_EN_RAILS_ODL)) {
		if (!tcpc_read16(1, TCPC_REG_ALERT, &regval)) {
			/* TCPCI spec Rev 1.0 says to ignore bits 14:12. */
			if (!(tcpc_config[1].flags & TCPC_FLAGS_TCPCI_REV2_0))
				regval &= ~((1 << 14) | (1 << 13) | (1 << 12));

			if (regval)
				status |= PD_STATUS_TCPC_ALERT_1;
		}
	}

	return status;
}

#ifndef TEST_BUILD
/* This callback disables keyboard when convertibles are fully open */
void lid_angle_peripheral_enable(int enable)
{
	int chipset_in_s0 = chipset_in_state(CHIPSET_STATE_ON);

	/*
	 * If the lid is in tablet position via other sensors,
	 * ignore the lid angle, which might be faulty then
	 * disable keyboard.
	 */
	if (tablet_get_mode())
		enable = 0;

	if (enable) {
		keyboard_scan_enable(1, KB_SCAN_DISABLE_LID_ANGLE);
	} else {
		/*
		 * Ensure that the chipset is off before disabling the keyboard.
		 * When the chipset is on, the EC keeps the keyboard enabled and
		 * the AP decides whether to ignore input devices or not.
		 */
		if (!chipset_in_s0)
			keyboard_scan_enable(0, KB_SCAN_DISABLE_LID_ANGLE);
	}
}
#endif