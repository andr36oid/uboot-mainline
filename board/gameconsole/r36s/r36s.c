// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family (RK3326 handhelds): board detection and start-up order.
 *
 * Start-up follows the vendor U-Boot (Hardkernel odroidgoa board code with
 * the andr36oid changes): board id from SARADC channel 0, the screen DTB
 * from BOOT, PMIC rails, boot logo, battery and charge checks, then
 * boot.ini through cfgload.
 */

#include <adc.h>
#include <command.h>
#include <dm.h>
#include <env.h>
#include <init.h>
#include <log.h>
#include <spl.h>
#include <asm/global_data.h>
#include <linux/sizes.h>
#include "r36s.h"

DECLARE_GLOBAL_DATA_PTR;

struct r36s_board r36s = {
	.adc = -1,
	.part = R36S_PART_SD,
};

struct r36s_family_info {
	const char *hwrev;
	const char *dtb_uboot;
	const char *dtb_kernel;
	int adc_min;	/* exclusive */
	int adc_max;	/* exclusive */
};

/*
 * Vendor cmd/hwrev.c: SARADC channel 0, exclusive ranges, anything else is
 * "v00" and uses the rg351p names.
 */
static const struct r36s_family_info r36s_families[] = {
	[R36S_FAMILY_V00]	= { "v00", "rg351p-uboot.dtb",
				    "rk3326-rg351p-linux.dtb", 0, 0 },
	[R36S_FAMILY_RG351MP]	= { "rg351mp", "rg351mp-uboot.dtb",
				    "rk3326-rg351mp-linux.dtb", 146, 186 },
	[R36S_FAMILY_RG351V]	= { "rg351v", "rg351v-uboot.dtb",
				    "rk3326-rg351v-linux.dtb", 494, 534 },
	[R36S_FAMILY_RG351P]	= { "rg351p", "rg351p-uboot.dtb",
				    "rk3326-rg351p-linux.dtb", 655, 695 },
};

const char *r36s_family_name(enum r36s_family family)
{
	if (family < 0 || family >= ARRAY_SIZE(r36s_families))
		family = R36S_FAMILY_V00;

	return r36s_families[family].hwrev;
}

static enum r36s_family r36s_family_from_adc(int adc)
{
	int i;

	for (i = R36S_FAMILY_RG351MP; i < ARRAY_SIZE(r36s_families); i++) {
		if (adc > 0 && adc > r36s_families[i].adc_min &&
		    adc < r36s_families[i].adc_max)
			return i;
	}

	return R36S_FAMILY_V00;
}

int r36s_detect_family(void)
{
	const struct r36s_family_info *info;
	unsigned int val;
	int ret;

	ret = adc_channel_single_shot("saradc@ff288000", 0, &val);
	if (ret) {
		printf("board hw rev failed (%d)\n", ret);
		r36s.adc = -1;
	} else {
		r36s.adc = val;
	}

	r36s.family = r36s.adc < 0 ? R36S_FAMILY_V00 :
		      r36s_family_from_adc(r36s.adc);
	info = &r36s_families[r36s.family];
	r36s.hwrev = info->hwrev;
	r36s.dtb_uboot = info->dtb_uboot;
	r36s.dtb_kernel = info->dtb_kernel;

	env_set("hwrev", info->hwrev);
	env_set("dtb_uboot", info->dtb_uboot);
	env_set("dtb_kernel", info->dtb_kernel);

	printf("adc0 (hw rev) %d\n", r36s.adc);
	printf("Model = %s\n", info->hwrev);

	return ret;
}

static int do_hwrev(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	return r36s_detect_family() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(hwrev, 1, 1, do_hwrev,
	   "read the board id resistor and set hwrev/dtb_uboot/dtb_kernel", "");

/* RAM size in MiB for boot.ini (clones sold as 1 GB with 512 MB) */
static void r36s_set_ram_mb(void)
{
	env_set_ulong("ram_mb", (ulong)(gd->ram_size / SZ_1M));
}

/* Where did SPL load us from? The eMMC start needs the eMMC pins. */
static void r36s_detect_boot_device(void)
{
	const char *dev = ofnode_read_chosen_string("u-boot,spl-boot-device");

	r36s.booted_emmc = dev && strstr(dev, "mmc@ff390000");
	if (r36s.booted_emmc)
		r36s.part = R36S_PART_EMMC;
	log_debug("SPL boot device: %s\n", dev ? dev : "(unknown)");
}

/* Default (weak) steps, provided by the other board files */
__weak void r36s_board_pins(void) {}
__weak void r36s_emmc_pins(void) {}
__weak int r36s_screen_load(void) { return -ENOSYS; }
__weak int r36s_screen_apply_power(void) { return 0; }
__weak int r36s_power_check(void) { return 0; }
__weak int r36s_show_logo(void) { return 0; }
__weak int r36s_check_mandatory_files(void) { return 0; }

int rk_board_late_init(void)
{
	r36s_detect_boot_device();
	r36s_detect_family();
	r36s_set_ram_mb();

	/* Pin setup the vendor does in C (pulls, rumble off, Wi-Fi on, ...) */
	r36s_board_pins();
	if (r36s.booted_emmc)
		r36s_emmc_pins();

	/* Screen DTB from BOOT: SD card first, then the eMMC */
	if (r36s_screen_load())
		printf("No screen DTB (%s) on BOOT, no display\n",
		       r36s.dtb_uboot);
	else
		r36s_screen_apply_power();

	r36s_power_check();
	r36s_show_logo();
	r36s_check_mandatory_files();

	env_set("andr36oid_part", r36s.part);

	return 0;
}
