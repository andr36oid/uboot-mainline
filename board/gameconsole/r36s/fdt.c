// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family: what the kernel DT gets from U-Boot (ft_board_setup).
 *
 * The andr36oid kernel (Rockchip 4.19) adopts the boot screen when
 *   - /reserved-memory/drm-logo (compatible "rockchip,drm-logo", reg 0 0
 *     in every kernel DT) holds the framebuffer, and
 *   - /display-subsystem/route/route-dsi carries logo,* (where the picture
 *     is and its format) and video,* (the mode U-Boot set up).
 * If that mode is one of the kernel panel's modes, the kernel keeps
 * U-Boot's VOP, DSI and PHY programming and shows the picture until
 * Android draws; otherwise it initialises the display again. The vendor
 * U-Boot writes the same properties (rockchip_display_fixup).
 *
 * /memory, /serial-number and /chosen/bootargs come from the generic
 * U-Boot fixups (bank list in r36s.c, serial# in the environment).
 */

#include <dm.h>
#include <fdt_support.h>
#include <log.h>
#include <video.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include "r36s.h"

/* DRM mode flags as the vendor writes them into video,flags */
#define DRM_MODE_FLAG_PHSYNC	BIT(0)
#define DRM_MODE_FLAG_NHSYNC	BIT(1)
#define DRM_MODE_FLAG_PVSYNC	BIT(2)
#define DRM_MODE_FLAG_NVSYNC	BIT(3)
#define DRM_MODE_FLAG_PPIXDATA	BIT(31)		/* Rockchip BSP only */

static const char r36s_route_dsi[] = "/display-subsystem/route/route-dsi";

/* reg = <base size> in the cells of the node's parent */
static int r36s_fdt_set_reg(void *blob, int node, u64 base, u64 size)
{
	int parent = fdt_parent_offset(blob, node);
	int ac = fdt_address_cells(blob, parent);
	int sc = fdt_size_cells(blob, parent);
	fdt32_t reg[4];
	int n = 0;

	if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
		return -FDT_ERR_BADNCELLS;
	if (ac == 2)
		reg[n++] = cpu_to_fdt32(upper_32_bits(base));
	reg[n++] = cpu_to_fdt32(lower_32_bits(base));
	if (sc == 2)
		reg[n++] = cpu_to_fdt32(upper_32_bits(size));
	reg[n++] = cpu_to_fdt32(lower_32_bits(size));

	return fdt_setprop(blob, node, "reg", reg, n * sizeof(*reg));
}

static int r36s_fdt_display(void *blob)
{
	const struct r36s_screen *scr = r36s_screen_get();
	struct udevice *vid = r36s_display_dev();
	const struct r36s_timing *t;
	struct video_uc_plat *plat;
	struct video_priv *priv;
	u32 htotal, vtotal, flags, vrefresh;
	int logo, route, ret = 0;

	/* Nothing on the screen: the kernel starts the display itself */
	if (!vid || !scr)
		return 0;

	logo = fdt_node_offset_by_compatible(blob, -1, "rockchip,drm-logo");
	route = fdt_path_offset(blob, r36s_route_dsi);
	if (logo < 0 || route < 0) {
		printf("Kernel DT has no drm-logo/route-dsi: no boot screen hand-over\n");
		return 0;
	}

	plat = dev_get_uclass_plat(vid);
	priv = dev_get_uclass_priv(vid);
	t = &scr->panel.timing;

	/* A clean picture for the kernel: the logo without U-Boot's text */
	r36s_redraw_logo();

	htotal = t->hactive + t->hfront_porch + t->hsync_len + t->hback_porch;
	vtotal = t->vactive + t->vfront_porch + t->vsync_len + t->vback_porch;
	flags = (t->hsync_active ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC) |
		(t->vsync_active ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC) |
		(t->pixelclk_active ? DRM_MODE_FLAG_PPIXDATA : 0);
	/* the kernel's drm_mode_vrefresh() on a mode clock in kHz */
	vrefresh = DIV_ROUND_CLOSEST((u64)(t->clock / 1000) * 1000,
				     (u64)htotal * vtotal);

	ret = r36s_fdt_set_reg(blob, logo, plat->base, plat->size);
	if (ret)
		goto err;

#define SET(name, val) \
	do { \
		ret = fdt_setprop_u32(blob, route, name, val); \
		if (ret) \
			goto err; \
	} while (0)

	/* picture at the start of the reserved area, XRGB8888, panel size */
	SET("logo,offset", 0);
	SET("logo,width", priv->xsize);
	SET("logo,height", priv->ysize);
	SET("logo,bpp", 32);
	SET("logo,ymirror", 0);
	SET("video,hdisplay", t->hactive);
	SET("video,vdisplay", t->vactive);
	SET("video,crtc_hsync_end", t->hactive + t->hfront_porch +
				     t->hsync_len);
	SET("video,crtc_vsync_end", t->vactive + t->vfront_porch +
				     t->vsync_len);
	SET("video,vrefresh", vrefresh);
	SET("video,flags", flags);
	SET("overscan,left_margin", 100);
	SET("overscan,right_margin", 100);
	SET("overscan,top_margin", 100);
	SET("overscan,bottom_margin", 100);
#undef SET

	printf("Boot screen hand-over: %ux%u@%u at %08lx (%u KiB)\n",
	       t->hactive, t->vactive, vrefresh, plat->base, plat->size >> 10);

	return 0;

err:
	printf("Boot screen hand-over failed: %s\n", fdt_strerror(ret));
	/* no reg: the kernel then ignores the half-written route */
	r36s_fdt_set_reg(blob, logo, 0, 0);

	return 0;
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	return r36s_fdt_display(blob);
}
