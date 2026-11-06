// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family: keep this start's U-Boot log on the BOOT partition.
 *
 * These consoles have no serial port within reach. When BOOT holds a file
 * named uboot-log.txt, U-Boot overwrites it with its console output right
 * before it starts the kernel (and the cfgload failure path), so a test
 * card shows what U-Boot did. Without the file nothing is written.
 */

#include <console.h>
#include <env.h>
#include <fs.h>
#include <linux/errno.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <membuf.h>
#include <asm/global_data.h>
#include "r36s.h"

DECLARE_GLOBAL_DATA_PTR;

#define R36S_LOG_FILE	"uboot-log.txt"

int r36s_save_log(void)
{
	const char *part = env_get("andr36oid_part");
	loff_t written;
	char *buf;
	int len, ret;

	if (!IS_ENABLED(CONFIG_CONSOLE_RECORD) || !gd->console_out.start)
		return -ENOSYS;
	if (!part)
		part = r36s.part;
	if (!file_exists("mmc", part, R36S_LOG_FILE, FS_TYPE_FAT))
		return -ENOENT;

	len = membuf_avail(&gd->console_out);
	buf = malloc(len + 1);
	if (!buf)
		return -ENOMEM;
	len = membuf_get(&gd->console_out, buf, len);

	ret = fs_set_blk_dev("mmc", part, FS_TYPE_FAT);
	if (!ret)
		ret = fs_write(R36S_LOG_FILE, map_to_sysmem(buf), 0, len,
			       &written);
	free(buf);

	return ret;
}

/* Last call before the kernel starts (boot/bootm.c bootm_final) */
void board_quiesce_devices(void)
{
	r36s_save_log();
}
