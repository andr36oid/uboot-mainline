// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family: load the screen DTB from BOOT and apply its PMIC rails.
 *
 * Like the vendor U-Boot, the file is BOOT/<dtb_uboot> (rg351mp-uboot.dtb,
 * rg351v-uboot.dtb or rg351p-uboot.dtb, picked by the board id), from the
 * SD card first and then from the built-in eMMC. When neither has it, the
 * same-format rg351X-kernel.dtb is used (the flasher's DTB converter writes
 * only those names).
 */

#include <dm.h>
#include <env.h>
#include <fs.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <power/regulator.h>
#include "r36s.h"

/* Screen DTBs are 58-90 KB */
#define R36S_SCREEN_MAX_SIZE	SZ_512K

static struct r36s_screen r36s_scr;
static bool r36s_scr_ok;

const struct r36s_screen *r36s_screen_get(void)
{
	return r36s_scr_ok ? &r36s_scr : NULL;
}

static void *r36s_screen_read(const char *part, const char *name, loff_t *size)
{
	loff_t len;
	void *buf;

	if (fs_set_blk_dev("mmc", part, FS_TYPE_ANY))
		return NULL;
	if (fs_size(name, &len) || len <= 0 || len > R36S_SCREEN_MAX_SIZE)
		return NULL;

	buf = malloc(len);
	if (!buf)
		return NULL;
	if (fs_set_blk_dev("mmc", part, FS_TYPE_ANY) ||
	    fs_read(name, map_to_sysmem(buf), 0, len, size) || *size != len) {
		free(buf);
		return NULL;
	}

	return buf;
}

/* rg351mp-uboot.dtb -> rg351mp-kernel.dtb */
static bool r36s_screen_kernel_name(const char *name, char *buf, size_t len)
{
	static const char suffix[] = "-uboot.dtb";
	size_t n = strlen(name), sl = strlen(suffix);

	if (n < sl || strcmp(name + n - sl, suffix))
		return false;

	return snprintf(buf, len, "%.*s-kernel.dtb", (int)(n - sl), name) <
	       (int)len;
}

int r36s_screen_load(void)
{
	static const char * const parts[] = { R36S_PART_SD, R36S_PART_EMMC };
	const char *names[2] = { r36s.dtb_uboot, NULL };
	char kname[32];
	const char *name = NULL;
	loff_t size = 0;
	void *fdt = NULL;
	int i, j, ret;

	if (!names[0])
		return -EINVAL;
	if (r36s_screen_kernel_name(names[0], kname, sizeof(kname)))
		names[1] = kname;

	for (j = 0; j < ARRAY_SIZE(names) && names[j] && !fdt; j++) {
		for (i = 0; i < ARRAY_SIZE(parts) && !fdt; i++) {
			if (!strcmp(parts[i], R36S_PART_EMMC))
				r36s_emmc_pins();
			fdt = r36s_screen_read(parts[i], names[j], &size);
			if (fdt) {
				r36s.screen_fdt_from = parts[i];
				name = names[j];
			}
		}
	}
	if (!fdt)
		return -ENOENT;

	ret = r36s_screen_parse(fdt, &r36s_scr);
	if (ret) {
		printf("%s on mmc %s is not a device tree\n", name,
		       r36s.screen_fdt_from);
		free(fdt);
		return ret;
	}

	r36s.screen_fdt = fdt;
	r36s.screen_fdt_size = size;
	r36s_scr_ok = true;

	printf("Screen: %s from mmc %s (%s)\n", name, r36s.screen_fdt_from,
	       r36s_scr.model);
	if (r36s_scr.panel.timing_ok)
		printf("Panel: %ux%u %u.%03u MHz, %u lanes%s\n",
		       r36s_scr.panel.timing.hactive,
		       r36s_scr.panel.timing.vactive,
		       r36s_scr.panel.timing.clock / 1000000,
		       r36s_scr.panel.timing.clock / 1000 % 1000,
		       r36s_scr.panel.lanes,
		       r36s_scr.panel.timing_fallback ?
		       " (native-mode broken, first timing)" : "");
	else
		printf("Panel: no usable timing in %s\n", name);

	return 0;
}

/* RK817 rail as a regulator device of the control DT ("LDO_REG8", ...) */
int r36s_rail_get(int rail, struct udevice **devp)
{
	const char *node = r36s_rail_node_name(rail);

	if (!node)
		return -EINVAL;

	return uclass_get_device_by_name(UCLASS_REGULATOR, node, devp);
}

/* Set a rail to uv (if non-zero) and switch it on or off */
int r36s_rail_set(int rail, u32 uv, bool on)
{
	struct udevice *dev;
	int ret;

	ret = r36s_rail_get(rail, &dev);
	if (ret)
		return ret;
	if (uv) {
		ret = regulator_set_value(dev, uv);
		if (ret)
			return ret;
	}

	return regulator_set_enable(dev, on);
}

/*
 * Vendor regulators_enable_boot_on() over the screen DTB rails: rails
 * marked always-on or boot-on are switched on, and get their voltage
 * written only when min equals max. BOOST and OTG_SWITCH never bind in
 * the vendor U-Boot, so they are left alone.
 */
int r36s_screen_apply_power(void)
{
	const struct r36s_screen *scr = r36s_screen_get();
	int i, ret;

	if (!scr)
		return -ENODEV;

	for (i = R36S_DCDC1; i <= R36S_LDO9; i++) {
		const struct r36s_rail_cfg *rail = &scr->rails[i];
		u32 uv = 0;

		if (!rail->present || !(rail->always_on || rail->boot_on))
			continue;
		if (rail->min_uv && rail->min_uv == rail->max_uv)
			uv = rail->min_uv;

		ret = r36s_rail_set(i, uv, true);
		if (ret)
			printf("%s (%s): %d\n", r36s_rail_node_name(i),
			       rail->name, ret);
		else
			log_debug("%s (%s) on, %u uV\n", r36s_rail_node_name(i),
				  rail->name, uv);
	}

	return 0;
}
