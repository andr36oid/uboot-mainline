/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * R36S family: the "screen DTB" (BOOT/rg351*-uboot.dtb) as plain data.
 *
 * The vendor U-Boot runs its devices straight from this file. Mainline
 * drivers cannot bind its vendor nodes, so the board parses the parts it
 * needs (panel, backlight, PMIC rails, battery, charge screen) into this
 * structure and the board drivers work from it.
 *
 * Only libfdt is used, so the parser also builds on the host and can be
 * checked against every shipped screen DTB (test/r36s-screen).
 */

#ifndef __R36S_SCREEN_H
#define __R36S_SCREEN_H

#ifdef R36S_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#else
#include <linux/types.h>
#endif

/* A GPIO from the screen DTB: bank 0-3, pin 0-31 */
struct r36s_gpio {
	bool valid;
	u8 bank;
	u8 pin;
	bool active_low;
};

/* RK817 rails, by vendor node name */
enum r36s_rail {
	R36S_RAIL_NONE = -1,
	R36S_DCDC1 = 0, R36S_DCDC2, R36S_DCDC3, R36S_DCDC4,
	R36S_LDO1, R36S_LDO2, R36S_LDO3, R36S_LDO4, R36S_LDO5,
	R36S_LDO6, R36S_LDO7, R36S_LDO8, R36S_LDO9,
	R36S_BOOST, R36S_OTG_SWITCH,
	R36S_RAIL_COUNT,
};

struct r36s_rail_cfg {
	bool present;
	const char *name;	/* regulator-name, e.g. "vcc_lcd" */
	u32 min_uv;		/* 0 when not given */
	u32 max_uv;
	bool always_on;
	bool boot_on;
};

struct r36s_timing {
	u32 clock;		/* pixel clock, Hz */
	u32 hactive, hfront_porch, hsync_len, hback_porch;
	u32 vactive, vfront_porch, vsync_len, vback_porch;
	bool hsync_active;	/* true: active high */
	bool vsync_active;
	bool de_active;
	bool pixelclk_active;	/* true: data on the rising edge */
};

struct r36s_panel {
	bool present;
	const char *compatible;
	u32 lanes;		/* dsi,lanes, default 4 */
	u32 format;		/* dsi,format, 0 = RGB888 */
	u32 mode_flags;		/* dsi,flags (MIPI_DSI_* bits) */
	u32 prepare_ms, unprepare_ms, enable_ms, disable_ms, init_ms, reset_ms;
	struct r36s_gpio reset;
	struct r36s_gpio enable;
	/* Vendor command streams: type, delay ms, length, bytes[length] */
	const u8 *init_seq;
	int init_len;
	const u8 *exit_seq;
	int exit_len;
	bool timing_ok;
	bool timing_fallback;	/* native-mode was broken: first timing used */
	struct r36s_timing timing;
	int power_rail;		/* enum r36s_rail of power-supply */
	int backlight_rail;	/* enum r36s_rail of backlight-supply */
	u32 width_mm, height_mm;
};

#define R36S_MAX_LEVELS	256

struct r36s_backlight {
	bool present;
	u32 pwm_reg;		/* PWM controller address, e.g. 0xff200010 */
	u32 pwm_channel;
	u32 period_ns;
	bool inverted;
	u32 num_levels;
	u32 levels[R36S_MAX_LEVELS];
	u32 default_index;	/* default-brightness-level */
};

#define R36S_OCV_POINTS	21

struct r36s_battery {
	bool present;
	u32 ocv_count;
	u32 ocv_table[R36S_OCV_POINTS];	/* mV at 0, 5, ..., 100 % */
	u32 design_capacity;		/* mAh */
	u32 design_qmax;
	u32 bat_res;
	u32 sample_res;			/* mOhm */
	u32 power_off_thresd;		/* mV */
	u32 zero_algorithm_vol;
	u32 virtual_power;
};

struct r36s_charger {
	bool present;
	u32 min_input_voltage;		/* mV */
	u32 max_input_current;		/* mA */
	u32 max_chrg_current;
	u32 max_chrg_voltage;		/* mV */
	u32 chrg_finish_cur;		/* mA */
	struct r36s_gpio dc_det;
};

/* rockchip,uboot-charge: the charge screen */
struct r36s_charge_cfg {
	bool present;			/* node exists */
	bool enabled;			/* and its status is okay */
	u32 uboot_charge_on;
	u32 android_charge_on;
	u32 exit_charge_level;		/* % */
	u32 exit_charge_voltage;	/* mV */
	u32 screen_on_voltage;		/* mV */
	u32 low_power_voltage;		/* mV */
	u32 system_suspend;
	u32 auto_off_screen_interval;	/* s */
	u32 auto_wakeup_interval;
	u32 auto_wakeup_screen_invert;
};

struct r36s_screen {
	const void *fdt;
	const char *model;
	const char *compatible;		/* root compatible, first string */
	struct r36s_panel panel;
	struct r36s_backlight backlight;
	struct r36s_rail_cfg rails[R36S_RAIL_COUNT];
	struct r36s_battery battery;
	struct r36s_charger charger;
	struct r36s_charge_cfg charge;
	/* /display-subsystem/route/route-dsi */
	const char *logo_uboot;		/* "logo.bmp" */
	const char *logo_kernel;	/* "logo_kernel.bmp" */
	const char *logo_mode;		/* "center" */
	const char *charge_logo_mode;
	/* any enable-gpios on GPIO0_B5 (the charge LED pin) */
	bool charge_led_is_enable;
};

/* Fill scr from a screen DTB; 0 or -ve. scr points into fdt afterwards. */
int r36s_screen_parse(const void *fdt, struct r36s_screen *scr);

const char *r36s_rail_node_name(int rail);

#endif /* __R36S_SCREEN_H */
