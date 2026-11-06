/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * R36S family (RK3326 handhelds)
 */

#ifndef __CONFIG_R36S_H
#define __CONFIG_R36S_H

#include <configs/rockchip-common.h>

#define CFG_IRAM_BASE		0xff0e0000

#define GICD_BASE		0xff131000
#define GICC_BASE		0xff132000

#define CFG_SYS_SDRAM_BASE	0
#define SDRAM_MAX_SIZE		0xff000000

/*
 * Load addresses as in the vendor U-Boot (include/configs/odroidgoa.h) and
 * boot.ini: kernel 0x02000000, FDT 0x01f00000, ramdisk 0x0a200000.
 * boot.ini sets the first three itself; the rest are U-Boot's own.
 */
#define ENV_MEM_LAYOUT_SETTINGS \
	"scriptaddr=0x00500000\0" \
	"pxefile_addr_r=0x00600000\0" \
	"fdt_addr_r=0x01f00000\0" \
	"fdtoverlay_addr_r=0x01e00000\0" \
	"kernel_addr_r=0x02000000\0" \
	"ramdisk_addr_r=0x0a200000\0" \
	"kernel_comp_addr_r=0x0c000000\0" \
	"kernel_comp_size=0x2000000\0" \
	"loadaddr=0x02000000\0"

#define CFG_EXTRA_ENV_SETTINGS \
	ENV_MEM_LAYOUT_SETTINGS \
	"fdtfile=" CONFIG_DEFAULT_FDT_FILE "\0" \
	"stdin=serial\0" \
	"stdout=serial\0" \
	"stderr=serial\0"

#endif
