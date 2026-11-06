// SPDX-License-Identifier: GPL-2.0+
/*
 * cfgload: run boot.ini (or boot.scr) from the BOOT partition, as the vendor
 * U-Boot (Hardkernel cmd/cfgload.c with the andr36oid eMMC fallback) does.
 *
 * boot.ini is a plain text U-Boot script whose first line is
 * "odroidgoa-uboot-config". The SD card (mmc 1:1) always comes first, so a
 * card can start the console whatever is on the eMMC. With no boot.ini or
 * boot.scr on the card, the built-in eMMC's BOOT (mmc 0:1) is tried
 * (andr36oid installed on the eMMC). boot.ini sees which one it came from
 * in andr36oid_part.
 */

#include <command.h>
#include <env.h>
#include <fs.h>
#include <linux/errno.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <vsprintf.h>
#include <linux/string.h>
#include "r36s.h"

#define R36S_SCRIPT_MAGIC	"odroidgoa-uboot-config"

/*
 * Run a text script at addr (size bytes) whose first line is the magic,
 * like the vendor "source" command does for boot.ini. Carriage returns are
 * dropped so a boot.ini saved with Windows line ends still runs.
 */
int r36s_run_text_script(ulong addr, ulong size)
{
	const size_t mlen = strlen(R36S_SCRIPT_MAGIC);
	const char *src = map_sysmem(addr, size);
	char *buf;
	ulong i, n = 0;
	int ret;

	if (size < mlen || strncasecmp(src, R36S_SCRIPT_MAGIC, mlen)) {
		unmap_sysmem(src);
		puts("Wrong image format for \"source\" command\n");
		return CMD_RET_FAILURE;
	}

	buf = malloc(size - mlen + 1);
	if (!buf) {
		unmap_sysmem(src);
		return CMD_RET_FAILURE;
	}
	for (i = mlen; i < size; i++) {
		if (!src[i])
			break;
		if (src[i] != '\r')
			buf[n++] = src[i];
	}
	buf[n] = '\0';
	unmap_sysmem(src);

	ret = run_command_list(buf, n, 0);
	free(buf);

	return ret;
}

static int r36s_try_script(const char *part, const char *name, ulong addr)
{
	char cmd[128];
	ulong size;

	if (!file_exists("mmc", part, name, FS_TYPE_ANY))
		return -ENOENT;

	env_set("andr36oid_part", part);
	snprintf(cmd, sizeof(cmd), "load mmc %s 0x%lx %s", part, addr, name);
	if (run_command(cmd, 0))
		return -EIO;

	size = env_get_hex("filesize", 0);
	if (!strcmp(name, "boot.scr")) {
		snprintf(cmd, sizeof(cmd), "source 0x%lx", addr);
		return run_command(cmd, 0) ? -EINVAL : 0;
	}

	return r36s_run_text_script(addr, size) ? -EINVAL : 0;
}

static int do_cfgload(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	static const char * const sd_scripts[] = { "boot.ini", "boot.scr" };
	ulong addr = CONFIG_SYS_LOAD_ADDR;
	int i;

	if (argc > 1)
		addr = hextoul(argv[1], NULL);

	for (i = 0; i < ARRAY_SIZE(sd_scripts); i++)
		if (!r36s_try_script(R36S_PART_SD, sd_scripts[i], addr))
			return CMD_RET_SUCCESS;

	/* andr36oid on the built-in eMMC: hand it its pins first */
	r36s_emmc_pins();
	if (!r36s_try_script(R36S_PART_EMMC, "boot.ini", addr))
		return CMD_RET_SUCCESS;

	printf("cfgload: no boot.ini or boot.scr on BOOT\n");
	r36s_save_log();

	return CMD_RET_FAILURE;
}

U_BOOT_CMD(cfgload, 2, 0, do_cfgload,
	   "run boot.ini (or boot.scr) from the BOOT partition",
	   "[addr]\n"
	   "  - SD card (mmc 1:1) first, then the built-in eMMC (mmc 0:1)");
