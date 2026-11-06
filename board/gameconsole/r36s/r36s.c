// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family (RK3326 handhelds): board detection and start-up order.
 *
 * Start-up follows the vendor U-Boot (Hardkernel odroidgoa board code with
 * the andr36oid changes):
 *
 *   EVT_SETTINGS_R (environment loaded, nothing on the screen yet):
 *     boot-mode register, boot device, button pins, board id (hwrev),
 *     ram_mb, serial#
 *   rk_board_late_init (after mainline setup_boot_mode):
 *     boot-mode actions (preboot), BOOT partition, screen DTB and its PMIC
 *     rails, battery and charge checks, boot logo, "no SD card" check
 *   bootcmd: cfgload (boot.ini / boot.scr from BOOT)
 */

#include <adc.h>
#include <command.h>
#include <dm.h>
#include <env.h>
#include <event.h>
#include <fs.h>
#include <init.h>
#include <log.h>
#include <spl.h>
#include <stdio.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/unaligned.h>
#include <asm/arch-rockchip/boot_mode.h>
#include <dm/ofnode.h>
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

/*
 * Rockchip pre-loader ATAGs, [2 MiB - 8 KiB, 2 MiB). The vendor miniloader
 * (classic flavour) leaves the boot device and the OP-TEE memory here.
 */
#define R36S_ATAGS_BASE		(CFG_SYS_SDRAM_BASE + SZ_2M - SZ_8K)
#define R36S_ATAGS_SIZE		SZ_8K
#define R36S_ATAG_NONE		0x00000000
#define R36S_ATAG_CORE		0x54410001
#define R36S_ATAG_BOOTDEV	0x54410051
#define R36S_ATAG_TOS_MEM	0x54410053
#define R36S_BOOT_TYPE_EMMC	BIT(1)
#define R36S_BOOT_TYPE_SD0	BIT(2)

/* Tag data (after the size/magic header) or NULL */
static const u8 *r36s_atag(u32 magic, u32 *lenp)
{
	const u32 *p = (const u32 *)R36S_ATAGS_BASE;
	const u32 *end = p + R36S_ATAGS_SIZE / 4;

	if (p[1] != R36S_ATAG_CORE)
		return NULL;

	while (p + 2 <= end) {
		u32 size = p[0], tag = p[1];

		if (!size || tag == R36S_ATAG_NONE || p + size > end)
			break;
		if (tag == magic) {
			if (lenp)
				*lenp = (size - 2) * 4;
			return (const u8 *)(p + 2);
		}
		p += size;
	}

	return NULL;
}

/* Where did the boot ROM load us from? (informational, and rockusb) */
static void r36s_detect_boot_device(void)
{
	const char *dev = ofnode_read_chosen_string("u-boot,spl-boot-device");
	const u8 *tag;
	u32 len;

	if (dev) {
		/* mainline flavour: our SPL says which device */
		r36s.booted_emmc = strstr(dev, "mmc@ff390000");
		log_debug("SPL boot device: %s\n", dev);
		return;
	}

	/* classic flavour: struct tag_bootdev { version, devtype, ... } */
	tag = r36s_atag(R36S_ATAG_BOOTDEV, &len);
	if (tag && len >= 8) {
		u32 type = get_unaligned_le32(tag + 4);

		r36s.booted_emmc = type & R36S_BOOT_TYPE_EMMC;
		log_debug("pre-loader boot device type 0x%x\n", type);
	}
}

/*
 * OP-TEE memory left by the vendor trust image (classic flavour), as the
 * vendor param_parse_optee_mem(): ATAG_TOS_MEM, else the legacy parameter
 * block at 34 MiB with its 16-bit checksum. 0 if there is no OP-TEE.
 */
struct r36s_tos_param {
	u32 version;
	u32 checksum;
	struct {
		char name[8];
		s64 phy_addr;
		u32 size;
		u32 flags;
	} tee_mem;
	struct {
		char name[8];
		s64 phy_addr;
		u32 size;
		u32 flags;
	} drm_mem;
	s64 reserve[8];
};

static u16 r36s_trust_checksum(const u8 *buf, u16 len)
{
	u16 i, sum = 0;

	for (i = 0; i < len; i++)
		sum += i % 2 ? buf[i] << 8 : buf[i];

	return ~sum;
}

static int r36s_optee_mem(u64 *base, u64 *size)
{
	const struct r36s_tos_param *tos;
	const u8 *tag;
	u32 len;

	/* struct tag_tos_mem (packed): version, tee_mem { name[8], u64, u32, u32 } */
	tag = r36s_atag(R36S_ATAG_TOS_MEM, &len);
	if (tag && len >= 28 && get_unaligned_le32(tag + 24) == 1) {
		*base = get_unaligned_le64(tag + 12);
		*size = get_unaligned_le32(tag + 20);
		if (*size)
			return 0;
	}

	tos = (const struct r36s_tos_param *)(CFG_SYS_SDRAM_BASE + SZ_32M +
					     SZ_2M);
	if (tos->tee_mem.flags == 1 && tos->tee_mem.size &&
	    tos->checksum == r36s_trust_checksum((const u8 *)tos + 8,
						 sizeof(*tos) - 8)) {
		*base = tos->tee_mem.phy_addr;
		*size = tos->tee_mem.size;
		return 0;
	}

	return -ENOENT;
}

/*
 * Leave OP-TEE out of the memory U-Boot and the kernel use, as the vendor
 * does (dmesg shows 992 MiB on 1 GB consoles: 2 MiB BL31 + 30 MiB OP-TEE).
 * Called by dram_init_banksize() for both the ATAG and the fallback layout.
 */
int rockchip_dram_init_banksize_fixup(struct bd_info *bd)
{
	u64 hole, hole_size, hole_end;
	int i, j;

	if (r36s_optee_mem(&hole, &hole_size))
		return 0;
	hole_end = hole + hole_size;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		u64 start = gd->dram[i].start;
		u64 end = start + gd->dram[i].size;

		if (!gd->dram[i].size || hole >= end || hole_end <= start)
			continue;

		if (hole <= start) {
			/* the hole covers the start of the bank */
			gd->dram[i].start = min(hole_end, end);
			gd->dram[i].size = end - gd->dram[i].start;
			continue;
		}

		gd->dram[i].size = hole - start;
		if (hole_end >= end)
			continue;

		/* split: move the following banks up by one */
		for (j = CONFIG_NR_DRAM_BANKS - 1; j > i + 1; j--)
			gd->dram[j] = gd->dram[j - 1];
		if (i + 1 >= CONFIG_NR_DRAM_BANKS) {
			printf("DRAM: no bank left for the memory above OP-TEE\n");
			break;
		}
		gd->dram[i + 1].start = hole_end;
		gd->dram[i + 1].size = end - hole_end;
		i++;
	}

	return 0;
}

/* RAM size in MiB for boot.ini, as the vendor: the sum of the usable banks */
static void r36s_set_ram_mb(void)
{
	u64 total = 0;
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++)
		total += gd->dram[i].size;
	if (!total)
		total = gd->ram_size;

	env_set_ulong("ram_mb", (ulong)(total / SZ_1M));
}

/*
 * The vendor U-Boot reports the same serial number on every console (its
 * unseeded rand(): c3d9b8674f4b94f6). adb, the USB gadget serial and the
 * R36 Ultra Wi-Fi MAC follow it, so keep it unless the board is set to use
 * the CPU id (CONFIG_R36S_SERIAL empty).
 */
static void r36s_set_serial(void)
{
	if (CONFIG_R36S_SERIAL[0] && !env_get("serial#"))
		env_set("serial#", CONFIG_R36S_SERIAL);
}

/* Cold start (register lost its value) or panic/watchdog reboot */
bool r36s_cold_boot(void)
{
	switch (r36s.boot_mode) {
	case BOOT_NORMAL:
	case BOOT_LOADER:
	case BOOT_RECOVERY:
	case BOOT_FASTBOOT:
	case BOOT_CHARGING:
	case BOOT_UMS:
		return false;
	default:
		return true;
	}
}

/*
 * The SARADC download key of the Rockchip boards is channel 1 here, the
 * left stick: never treat it as a request for the boot ROM download mode.
 */
int rockchip_dnl_key_pressed(void)
{
	return 0;
}

/*
 * Early part, before anything is shown: the boot-mode register is read here
 * because setup_boot_mode() in board_late_init clears it.
 */
static int r36s_settings_r(void)
{
	r36s.boot_mode = readl((void __iomem *)CONFIG_ROCKCHIP_BOOT_MODE_REG);
	log_debug("boot mode 0x%08x\n", r36s.boot_mode);

	r36s_detect_boot_device();

	/* Pin setup the vendor does in C (pulls, rumble off, UART2 M1) */
	r36s_board_pins();
	if (r36s.booted_emmc)
		r36s_emmc_pins();

	r36s_detect_family();
	r36s_set_ram_mb();
	r36s_set_serial();

	return 0;
}
EVENT_SPY_SIMPLE(EVT_SETTINGS_R, r36s_settings_r);

/* rockusb on the device we started from, as the vendor's download mode */
static void r36s_preboot_rockusb(void)
{
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "setenv preboot; rockusb 0 mmc %d",
		 r36s.booted_emmc ? R36S_MMC_EMMC : R36S_MMC_SD);
	env_set("preboot", cmd);
}

/*
 * What a "reboot <mode>" from Linux does, as in the vendor U-Boot:
 * loader/bootloader -> rockusb on the boot device, ums -> USB storage of
 * mmc 0, fastboot and recovery -> a normal start (the vendor has no
 * fastboot; recovery is FN or recovery.txt in boot.ini), charge -> the
 * charge screen.
 */
static void r36s_boot_mode_actions(void)
{
	switch (r36s.boot_mode) {
	case BOOT_LOADER:
		printf("boot mode: loader\n");
		r36s_preboot_rockusb();
		break;
	case BOOT_UMS:
		printf("boot mode: ums\n");
		env_set("preboot", "setenv preboot; ums mmc 0");
		break;
	case BOOT_FASTBOOT:
	case BOOT_RECOVERY:
		printf("boot mode: %s (normal start)\n",
		       r36s.boot_mode == BOOT_FASTBOOT ? "fastboot" :
							 "recovery");
		env_set("preboot", NULL);
		break;
	case BOOT_CHARGING:
		printf("boot mode: charge\n");
		env_set("preboot", "setenv preboot; charge");
		break;
	default:
		break;
	}
}

/*
 * Serial hotkeys of the vendor U-Boot, typed while it starts: ctrl+b
 * restarts into the BootROM download mode (maskrom), ctrl+d starts rockusb.
 */
static void r36s_serial_hotkeys(void)
{
	int c;

	if (!tstc())
		return;

	c = getchar();
	if (c == 0x02) {
		printf("Hotkey: ctrl+b, entering BootROM download\n");
		writel(BOOT_BROM_DOWNLOAD,
		       (void __iomem *)CONFIG_ROCKCHIP_BOOT_MODE_REG);
		do_reset(NULL, 0, 0, NULL);
	} else if (c == 0x04) {
		printf("Hotkey: ctrl+d, entering rockusb\n");
		r36s_preboot_rockusb();
	}
}

/*
 * The BOOT partition boot.ini is on, as the vendor board_late_init: the SD
 * card's, unless only the built-in eMMC has a boot.ini.
 */
static void r36s_detect_part(void)
{
	r36s.part = R36S_PART_SD;
	if (!file_exists("mmc", R36S_PART_SD, "boot.ini", FS_TYPE_FAT)) {
		r36s_emmc_pins();
		if (file_exists("mmc", R36S_PART_EMMC, "boot.ini", FS_TYPE_FAT))
			r36s.part = R36S_PART_EMMC;
	}
	env_set("andr36oid_part", r36s.part);
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
	r36s_boot_mode_actions();
	r36s_serial_hotkeys();
	r36s_detect_part();

	/* Screen DTB from BOOT: SD card first, then the eMMC */
	if (r36s_screen_load())
		printf("No screen DTB (%s) on BOOT, no display\n",
		       r36s.dtb_uboot);
	else
		r36s_screen_apply_power();

	/* Low battery (powers off) and the charge screen */
	if (r36s_power_check())
		return 0;

	if (!strcmp(r36s.part, R36S_PART_EMMC))
		r36s_emmc_pins();

	r36s_show_logo();
	r36s_check_mandatory_files();

	return 0;
}
