// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family: PMIC, battery and charger at start-up, as the vendor U-Boot
 * (odroidgoa_power.c, odroidgoa_status.c, rk8xx.c, charge_animation.c):
 *
 *   - the RK817 settings the vendor probe adds to mainline's
 *   - the vendor fuel gauge (fg-rk817.c), which also sets the charger
 *     input and charge current on every start
 *   - LEDs: charge LED (GPIO0_B5) off unless the panel uses that pin,
 *     power LED (GPIO0_C2) on
 *   - below 3530 mV without a charger: "voltage level" screen, power off
 *   - cold start with a charger: the charge screen (BOOT/battery_*.bmp)
 *   - no SD card and no boot.ini on the eMMC: message, power off
 *
 * Charger presence is the PMIC's PLUG_IN status, which the vendor charge
 * screen uses too. The vendor's other test, GPIO0_B3 (dc_det of the
 * genuine R36S), is a button or an LED on some clones.
 */

#include <command.h>
#include <console.h>
#include <dm.h>
#include <env.h>
#include <fs.h>
#include <hang.h>
#include <log.h>
#include <malloc.h>
#include <mmc.h>
#include <sysreset.h>
#include <time.h>
#include <timestamp.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <power/pmic.h>
#include <power/rk8xx_pmic.h>
#include "r36s.h"

#define RK817_SYS_CFG3		0xf4
#define  RK817_DEV_OFF		BIT(0)
#define  RK817_SLPPIN_FUNC	GENMASK(4, 3)
#define  RK817_SLPPIN_SLEEP	(1 << 3)
#define RK817_INT_STS0		0xf8
#define RK817_INT_MSK0		0xf9
#define  RK817_PWRON_FALL	BIT(0)
#define  RK817_PWRON_RISE	BIT(1)
#define RK817_GPIO_INT_CFG	0xfe
#define  RK817_INT_POL_HIGH	BIT(1)

#define R36S_MIN_VOL_MV		3530	/* vendor MIN_VOL_LEVEL */
#define R36S_PWRKEY_WAIT_MS	10000	/* vendor POWERDOWN_WAIT_TIME */
#define R36S_KEY_LONG_MS	1000	/* vendor KEY_LONG_DOWN_MS */
#define R36S_BMP_MAX		(4 * SZ_1M)

enum r36s_key {
	R36S_KEY_NONE,
	R36S_KEY_SHORT,
	R36S_KEY_LONG,
};

static struct udevice *r36s_pmic_dev;
static u32 r36s_pmic_variant;

static int r36s_pmic(struct udevice **devp)
{
	int ret;

	if (!r36s_pmic_dev) {
		ret = uclass_first_device_err(UCLASS_PMIC, &r36s_pmic_dev);
		if (ret) {
			r36s_pmic_dev = NULL;
			return ret;
		}
		r36s_pmic_variant =
			((struct rk8xx_priv *)dev_get_priv(r36s_pmic_dev))->variant;
	}
	*devp = r36s_pmic_dev;

	return 0;
}

/*
 * Settings of the vendor rk817_init_reg that mainline's probe leaves out:
 * PMIC_SLEEP pin as the sleep input, INT pin active low.
 */
static void r36s_pmic_vendor_init(struct udevice *pmic)
{
	if (r36s_pmic_variant != RK817_ID && r36s_pmic_variant != RK809_ID)
		return;

	pmic_clrsetbits(pmic, RK817_SYS_CFG3, RK817_SLPPIN_FUNC,
			RK817_SLPPIN_SLEEP);
	pmic_clrsetbits(pmic, RK817_GPIO_INT_CFG, RK817_INT_POL_HIGH, 0);
}

void r36s_poweroff(void)
{
	struct udevice *pmic;

	printf("poweroff ...\n");
	r36s_backlight(false);
	if (!r36s_pmic(&pmic)) {
		/* vendor rk8xx_shutdown: SYS_CFG3 DEV_OFF */
		pmic_clrsetbits(pmic, RK817_SYS_CFG3, 0, RK817_DEV_OFF);
		mdelay(50);
	}
	sysreset_walk_halt(SYSRESET_POWER_OFF);
	hang();
}

/*
 * Power key, from the PMIC's latched PWRON edges like the vendor rk8xx
 * power-key driver (which used the PMIC interrupt for the same bits).
 * Decoding as the vendor key-uclass: the release of the press that turned
 * the console on is no event, >= 1 s is a long press (also while still
 * held), anything shorter a short press.
 */
static ulong r36s_key_fall, r36s_key_rise;
static int r36s_key_mask = -1;

static void r36s_pwrkey_begin(void)
{
	struct udevice *pmic;

	if (r36s_pmic(&pmic))
		return;
	if (r36s_key_mask < 0)
		r36s_key_mask = pmic_reg_read(pmic, RK817_INT_MSK0);
	if (r36s_key_mask >= 0)
		pmic_reg_write(pmic, RK817_INT_MSK0,
			       r36s_key_mask & ~(RK817_PWRON_FALL |
						 RK817_PWRON_RISE));
}

/* Leave the PMIC as we found it, with no stale key edge for the kernel */
static void r36s_pwrkey_end(void)
{
	struct udevice *pmic;

	if (r36s_pmic(&pmic))
		return;
	pmic_reg_write(pmic, RK817_INT_STS0, RK817_PWRON_FALL |
		       RK817_PWRON_RISE);
	if (r36s_key_mask >= 0)
		pmic_reg_write(pmic, RK817_INT_MSK0, r36s_key_mask);
	r36s_key_mask = -1;
	r36s_key_fall = 0;
	r36s_key_rise = 0;
}

static enum r36s_key r36s_pwrkey_event(void)
{
	struct udevice *pmic;
	ulong now = get_timer(0) + 1;	/* never 0: 0 means "no edge" */
	enum r36s_key key = R36S_KEY_NONE;
	int sts;

	if (r36s_pmic(&pmic))
		return R36S_KEY_NONE;

	sts = pmic_reg_read(pmic, RK817_INT_STS0);
	if (sts < 0)
		return R36S_KEY_NONE;
	sts &= RK817_PWRON_FALL | RK817_PWRON_RISE;
	if (sts) {
		pmic_reg_write(pmic, RK817_INT_STS0, sts);
		if (sts & RK817_PWRON_FALL)
			r36s_key_fall = now;
		if (sts & RK817_PWRON_RISE)
			r36s_key_rise = now;
	}

	if (!r36s_key_fall && r36s_key_rise) {
		r36s_key_rise = 0;
	} else if (r36s_key_fall && r36s_key_rise) {
		key = r36s_key_rise >= r36s_key_fall &&
		      r36s_key_rise - r36s_key_fall >= R36S_KEY_LONG_MS ?
		      R36S_KEY_LONG : R36S_KEY_SHORT;
		r36s_key_fall = 0;
		r36s_key_rise = 0;
	} else if (r36s_key_fall && now - r36s_key_fall >= R36S_KEY_LONG_MS) {
		key = R36S_KEY_LONG;
		r36s_key_fall = 0;
	}

	return key;
}

/*
 * Vendor odroid_wait_pwrkey: up to 10 s for a short press, then
 * "power off..." and off. Ctrl+C on the serial console stops it and
 * keeps U-Boot at its prompt instead.
 */
static void r36s_wait_pwrkey_poweroff(const char *bmp)
{
	ulong start = get_timer(0);

	printf("Wait power key\n");
	printf("Hit ctrl+c key to enter uboot console\n");

	r36s_pwrkey_begin();
	while (get_timer(start) < R36S_PWRKEY_WAIT_MS) {
		if (ctrlc()) {
			r36s_pwrkey_end();
			env_set("bootdelay", "-1");
			return;
		}
		if (r36s_pwrkey_event() == R36S_KEY_SHORT)
			break;
		mdelay(25);
	}
	r36s_pwrkey_end();

	printf("power key long pressed...\n");
	r36s_status_screen(bmp, R36S_BLACK, R36S_WHITE, "power off...");
	mdelay(500);
	r36s_poweroff();
}

/* Vendor board_chg_led and odroid_check_dcjack's power LED */
static void r36s_leds(void)
{
	const struct r36s_screen *scr = r36s_screen_get();

	/* GPIO0_B5: charge LED, unless the panel uses it as its enable pin */
	if (!scr || !scr->charge_led_is_enable)
		r36s_gpio_out("A13", 0);
	/* GPIO0_C2: power LED on */
	r36s_gpio_out("A18", 1);
}

/* Vendor message rows 18/19 + disp_offs, before its clamp to the screen */
static int r36s_vendor_row(int row)
{
	int offs = r36s.family == R36S_FAMILY_RG351MP ||
		   r36s.family == R36S_FAMILY_RG351V ? 25 : 10;

	return row + offs;
}

static void r36s_charge_text(int row, const char *str)
{
	/* Off-screen rows stay off-screen, as on the vendor (640x480 panels) */
	if (!r36s_display_ok())
		return;
	r36s_lcd_print(r36s_vendor_row(row), -1, R36S_GREY, R36S_BLACK, str);
}

static const char * const r36s_batt_names[] = {
	"battery_0.bmp", "battery_1.bmp", "battery_2.bmp", "battery_3.bmp",
	"battery_fail.bmp",
};

/* SOC thresholds of battery_0..3 (vendor GOADV image table) */
static const int r36s_batt_soc[] = { 5, 60, 80, 100 };
#define R36S_BATT_FAIL	4

static void *r36s_batt_bmp[ARRAY_SIZE(r36s_batt_names)];

static void r36s_charge_picture(int idx, int mv, int ma)
{
	char str[40];

	if (!r36s_batt_bmp[idx])
		r36s_batt_bmp[idx] = r36s_boot_file(r36s_batt_names[idx],
						    R36S_BMP_MAX, NULL);
	r36s_show_bmp(NULL);
	if (r36s_batt_bmp[idx])
		r36s_draw_bmp_buf(r36s_batt_bmp[idx]);

	if (idx == R36S_BATT_FAIL)
		return;
	snprintf(str, sizeof(str), "battery : %d.%d V", mv / 1000,
		 mv % 1000 / 100);
	r36s_charge_text(18, str);
	snprintf(str, sizeof(str), "current : %d mA", ma);
	r36s_charge_text(19, str);
}

static void r36s_charge_free(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(r36s_batt_bmp); i++) {
		free(r36s_batt_bmp[i]);
		r36s_batt_bmp[i] = NULL;
	}
}

/*
 * Vendor charge_extrem_low_power: charge with the screen off until the
 * battery reaches the low-power voltage + 50 mV. Unplugging powers off.
 */
static void r36s_charge_extreme_low(int threshold_mv)
{
	ulong start = get_timer(0);
	bool lcd_on = true;
	int mv;

	printf("Extreme low battery, charging until %d mV\n", threshold_mv);
	r36s_charge_picture(R36S_BATT_FAIL, 0, 0);
	r36s_charge_text(15,
			 "Extreme Low Battery, please wait until charged to 5%");
	r36s_charge_text(16, " LCD will be off soon.");
	r36s_backlight(true);

	while ((mv = r36s_fg_voltage()) >= 0 && mv < threshold_mv) {
		if (!r36s_fg_charger_online()) {
			printf("Not charging, low power: shutdown\n");
			r36s_poweroff();
		}
		r36s_fg_soc();
		if (lcd_on && get_timer(start) >= 5000) {
			r36s_backlight(false);
			lcd_on = false;
		}
		if (ctrlc())
			break;
		mdelay(500);
	}
	r36s_backlight(true);
}

/*
 * Vendor charge_animation_show: the battery picture for the charge level,
 * animated every 800 ms, voltage and current below it, gauge read every
 * second. Power key: short press turns the screen on or off (off by
 * itself after auto-off-screen-interval), long press starts the system
 * if the battery is above exit-charge-level / exit-charge-voltage.
 * Unplugging powers off. Ctrl+C on serial leaves.
 */
int r36s_charge_screen(bool forced)
{
	const struct r36s_screen *scr = r36s_screen_get();
	const struct r36s_charge_cfg *c;
	ulong t_fg, t_anim, t_screen;
	int soc = 0, mv = 0, ma = 0, idx = 0, start_idx = 0, i;
	int screen_on_mv;
	bool screen_on = true, low_off = false, first = true;
	enum r36s_key key;

	if (!scr)
		return 0;
	c = &scr->charge;
	if (!c->present || !c->enabled || !c->uboot_charge_on) {
		log_debug("charge screen off in the screen DTB\n");
		return 0;
	}
	if (!r36s_fg_battery_present()) {
		printf("Exit charge: battery is not exist\n");
		return 0;
	}
	if (!r36s_fg_charger_online()) {
		printf("Exit charge: due to charger offline\n");
		return 0;
	}
	if (r36s_display_start())
		printf("charge screen without a display\n");

	/* GOADV: the screen-on voltage is the low-power voltage, <= exit */
	screen_on_mv = min(c->low_power_voltage, c->exit_charge_voltage);

	mv = r36s_fg_voltage();
	if (mv >= 0 && mv < (int)c->low_power_voltage + 50)
		r36s_charge_extreme_low(c->low_power_voltage + 50);

	if (mv >= 0 && mv <= screen_on_mv + 50) {
		low_off = true;
		screen_on = false;
	}

	printf("Charging, power key: short = screen on/off, long = start\n");
	r36s_pwrkey_begin();
	t_fg = t_anim = t_screen = get_timer(0);
	r36s_backlight(screen_on);

	for (;;) {
		if (first || get_timer(t_fg) >= 1000) {
			t_fg = get_timer(0);
			soc = r36s_fg_soc();
			mv = r36s_fg_voltage();
			ma = r36s_fg_current();
			if (!r36s_fg_charger_online()) {
				printf("Not charging (charger offline): shutdown\n");
				r36s_pwrkey_end();
				r36s_poweroff();
			}
			if (low_off && mv > screen_on_mv)
				low_off = false;

			/* first picture: soc between its threshold and the next */
			if (soc >= 100) {
				start_idx = 3;
			} else {
				for (i = 0; i < 3; i++)
					if (soc >= r36s_batt_soc[i] &&
					    soc < r36s_batt_soc[i + 1])
						start_idx = i;
			}
			if (first)
				idx = start_idx;
		}

		if (screen_on && (first || get_timer(t_anim) >= 800)) {
			t_anim = get_timer(0);
			r36s_charge_picture(idx, mv, ma);
			idx = idx >= 3 ? start_idx : idx + 1;
		}
		first = false;

		key = r36s_pwrkey_event();
		if (key == R36S_KEY_SHORT) {
			if (screen_on || low_off) {
				screen_on = false;
			} else {
				screen_on = true;
				t_screen = get_timer(0);
				first = true;
			}
			r36s_backlight(screen_on);
		} else if (key == R36S_KEY_LONG) {
			screen_on = true;
			t_screen = get_timer(0);
			if (soc < (int)c->exit_charge_level ||
			    mv < (int)c->exit_charge_voltage) {
				printf("Not enough charge to start (%d%%, %d mV)\n",
				       soc, mv);
				r36s_charge_picture(R36S_BATT_FAIL, mv, ma);
				r36s_backlight(true);
				mdelay(1000);
				first = true;
			} else {
				break;
			}
		}

		if (screen_on && c->auto_off_screen_interval &&
		    get_timer(t_screen) >= c->auto_off_screen_interval * 1000) {
			screen_on = false;
			r36s_backlight(false);
		}

		if (ctrlc()) {
			printf("charge screen left by ctrl+c\n");
			break;
		}
		mdelay(25);
	}

	r36s_pwrkey_end();
	r36s_charge_free();
	if (forced)
		r36s_show_logo();

	return 0;
}

static int do_charge(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	return r36s_charge_screen(true) ? CMD_RET_FAILURE : 0;
}

U_BOOT_CMD(charge, 1, 0, do_charge, "show the charge screen until the power key",
	   "");

/* Vendor board_check_power */
int r36s_power_check(void)
{
	const struct r36s_screen *scr = r36s_screen_get();
	struct udevice *pmic;
	char str[40];
	int mv;

	r36s_leds();
	if (r36s_pmic(&pmic)) {
		printf("No PMIC, no battery checks\n");
		return 0;
	}
	r36s_pmic_vendor_init(pmic);

	if (!scr || r36s_fg_init(pmic, r36s_pmic_variant, &scr->battery))
		return 0;

	mv = r36s_fg_voltage();
	printf("Battery: %d mV, charger %s\n", mv,
	       r36s_fg_charger_online() ? "in" : "out");

	if (mv >= 0 && mv < R36S_MIN_VOL_MV && !r36s_fg_charger_online()) {
		snprintf(str, sizeof(str), "voltage level : %d.%02dV",
			 mv / 1000, mv % 1000 / 10);
		printf("Low battery, %s\n", str);
		r36s_status_screen("low_battery.bmp", R36S_BLACK, R36S_WHITE,
				   str);
		r36s_wait_pwrkey_poweroff("low_battery.bmp");
		return -1;
	}

	/* Cold start (or panic/watchdog) with a charger: the charge screen */
	if (r36s_cold_boot() && r36s_fg_charger_online()) {
		if (file_exists("mmc", r36s.part, "manufacture", FS_TYPE_ANY))
			printf("BOOT/manufacture: no charge screen\n");
		else
			r36s_charge_screen(false);
	}

	return 0;
}

/*
 * Vendor board_check_mandatory_files: an SD card, or a boot.ini on the
 * built-in eMMC. Otherwise say so and power off.
 */
int r36s_check_mandatory_files(void)
{
	struct mmc *mmc = find_mmc_device(R36S_MMC_SD);

	if (mmc && (!mmc_init(mmc) || !mmc_init(mmc)))
		return 0;

	r36s_emmc_pins();
	if (file_exists("mmc", R36S_PART_EMMC, "boot.ini", FS_TYPE_FAT))
		return 0;

	printf("No SD card and no boot.ini on the eMMC\n");
	if (r36s_display_start() == 0) {
		r36s_lcd_print(-3, -1, R36S_BLACK, R36S_WHITE,
			       " No SD card with a BOOT partition ");
		r36s_lcd_print(-2, -1, R36S_GREY, R36S_WHITE,
			       " U-Boot " U_BOOT_DATE " " U_BOOT_TIME " ");
		r36s_backlight(true);
	}
	r36s_wait_pwrkey_poweroff(NULL);

	return -ENODEV;
}
