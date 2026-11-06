/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * R36S family (RK3326 handhelds): board state shared by the board files.
 *
 * The board files follow the vendor U-Boot (Hardkernel ODROID-GO Advance
 * fork of Rockchip U-Boot 2017.09 with the andr36oid changes) so the BOOT
 * partition, boot.ini and the kernel see the same behaviour.
 */

#ifndef __R36S_BOARD_H
#define __R36S_BOARD_H

#include <linux/types.h>

/* Board family from the board id resistor on SARADC channel 0 (vendor "hwrev") */
enum r36s_family {
	R36S_FAMILY_V00 = 0,	/* anything else: vendor hwrev "v00" */
	R36S_FAMILY_RG351MP,	/* R36S and the clones wired like it */
	R36S_FAMILY_RG351V,	/* EE clones: K36, G80, V12, R36 Max, ... */
	R36S_FAMILY_RG351P,
};

/* Boot partitions as boot.ini names them in andr36oid_part */
#define R36S_PART_SD	"1:1"
#define R36S_PART_EMMC	"0:1"
#define R36S_MMC_SD	1
#define R36S_MMC_EMMC	0

struct r36s_board {
	enum r36s_family family;
	int adc;		/* raw SARADC channel 0 value, -1 if the read failed */
	const char *hwrev;	/* "rg351mp", "rg351v", "rg351p" or "v00" */
	const char *dtb_uboot;	/* screen DTB file name on BOOT */
	const char *dtb_kernel;	/* vendor's kernel DTB name (exported, unused by boot.ini) */
	bool booted_emmc;	/* SPL/U-Boot itself came from the built-in eMMC */
	bool emmc_pins;		/* the eMMC pins are muxed to the eMMC controller */
	const char *part;	/* R36S_PART_SD or R36S_PART_EMMC: where BOOT is */
	/* The screen DTB read from BOOT (vendor rg351*-uboot.dtb), or NULL */
	const void *screen_fdt;
	ulong screen_fdt_size;
	const char *screen_fdt_from;	/* "mmc 1:1" / "mmc 0:1", for the log */
};

extern struct r36s_board r36s;

/* r36s.c */
int r36s_detect_family(void);
const char *r36s_family_name(enum r36s_family family);

/*
 * Steps of the start-up sequence (rk_board_late_init in r36s.c calls them in
 * this order). Each one is a weak no-op in r36s.c until its file provides it.
 */
void r36s_board_pins(void);		/* pins.c: pulls, rumble, Wi-Fi, LEDs, UART2 M1 */
void r36s_emmc_pins(void);		/* pins.c: hand GPIO1_A0-A7/B1/B2 to the eMMC */
int r36s_screen_load(void);		/* screen.c: read the screen DTB from BOOT */
int r36s_screen_apply_power(void);	/* screen.c: PMIC rails etc. from the screen DTB */
int r36s_power_check(void);		/* power.c: low battery, charge screen */
int r36s_show_logo(void);		/* ui.c: boot logo, version, message rows */
int r36s_check_mandatory_files(void);	/* boot.c: "no SD card" screen */

/* pins.c */
int r36s_gpio_out(const char *name, int value);

/* cfgload.c */
int r36s_run_text_script(ulong addr, ulong size);

/* log.c: write the console log to BOOT/uboot-log.txt if that file exists */
int r36s_save_log(void);

#endif /* __R36S_BOARD_H */
