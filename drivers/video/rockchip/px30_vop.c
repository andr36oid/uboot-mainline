// SPDX-License-Identifier: GPL-2.0+
/*
 * PX30 / RK3326 VOP-big display controller, MIPI DSI output.
 *
 * Programs the VOP the way the Rockchip BSP U-Boot does (rockchip_vop.c
 * with the px30 data), so a kernel that keeps the boot screen sees the
 * same hardware state: picture on window 1 (the kernel's primary plane),
 * 32 bpp XRGB8888, register writes through a shadow copy.
 *
 * Bring-up order, as the BSP: pixel clock, VOP timing and window, DSI
 * host and PHY (command mode), panel power and init commands, VOP out of
 * standby, DSI video mode. The backlight is left to the board, which
 * turns it on through the panel once a picture is drawn.
 *
 * The driver has no remove or OS_PREPARE flag on purpose: the screen keeps
 * running into the kernel, which adopts it (rockchip,drm-logo).
 */

#include <backlight.h>
#include <clk.h>
#include <dm.h>
#include <env.h>
#include <log.h>
#include <panel.h>
#include <video.h>
#include <video_bridge.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <dm/ofnode.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/errno.h>

DECLARE_GLOBAL_DATA_PTR;

#define VOP_REG_CFG_DONE		0x000
#define VOP_DSP_BG			0x008
#define VOP_SYS_CTRL2			0x018
#define  SYS_CTRL2_STANDBY		BIT(1)
#define  SYS_CTRL2_GLOBAL_REGDONE_EN	BIT(13)
#define VOP_DSP_CTRL0			0x020
#define  DSP_CTRL0_CORE_DCLK_DIV	BIT(13)
#define  DSP_CTRL0_DCLK_DDR		BIT(14)
#define  DSP_CTRL0_MIPI_EN		BIT(24)
#define  DSP_CTRL0_MIPI_DCLK_POL	BIT(25)
#define  DSP_CTRL0_MIPI_PIN_POL		GENMASK(28, 26)
#define VOP_DSP_CTRL2			0x028
#define  DSP_CTRL2_INTERLACE		BIT(0)
#define  DSP_CTRL2_OVERLAY_MODE		BIT(4)
#define  DSP_CTRL2_DITHER_DOWN		GENMASK(8, 6)
#define  DSP_CTRL2_DATA_SWAP		GENMASK(13, 9)
#define  DSP_CTRL2_BLANK		BIT(14)
#define  DSP_CTRL2_OUT_MODE		GENMASK(19, 16)
#define VOP_LINE_FLAG			0x030
#define  LINE_FLAG_NUM			GENMASK(11, 0)
#define VOP_WIN1_CTRL0			0x090
#define  WIN_EN				BIT(0)
#define  WIN_FORMAT			GENMASK(6, 4)
#define  WIN_RB_SWAP			BIT(12)
#define VOP_WIN1_VIR			0x098
#define  WIN_VIR_STRIDE			GENMASK(12, 0)
#define VOP_WIN1_MST			0x0a0
#define VOP_WIN1_DSP_INFO		0x0a4
#define VOP_WIN1_DSP_ST			0x0a8
#define VOP_DSP_HTOTAL_HS_END		0x100
#define VOP_DSP_HACT_ST_END		0x104
#define VOP_DSP_VTOTAL_VS_END		0x108
#define VOP_DSP_VACT_ST_END		0x10c
#define VOP_BCSH_CTRL			0x160
#define  BCSH_R2Y_CSC			BIT(1)
#define  BCSH_Y2R_CSC			GENMASK(5, 4)
#define  BCSH_Y2R_EN			BIT(6)
#define  BCSH_R2Y_EN			BIT(7)

#define VOP_SHADOW_LEN			0x170

#define WIN_FORMAT_ARGB8888		0
#define OUT_MODE_P888			0

struct px30_vop_priv {
	void __iomem *regs;
	u32 shadow[VOP_SHADOW_LEN / 4];
};

/* All writes go through the shadow, as in the BSP (no live RMW) */
static void vop_mask_write(struct px30_vop_priv *priv, u32 reg, u32 mask,
			   u32 val)
{
	u32 *cache = &priv->shadow[reg / 4];

	*cache = (*cache & ~mask) | (val & mask);
	writel(*cache, priv->regs + reg);
}

static void vop_write(struct px30_vop_priv *priv, u32 reg, u32 val)
{
	vop_mask_write(priv, reg, ~0U, val);
}

static void vop_cfg_done(struct px30_vop_priv *priv)
{
	writel(1, priv->regs + VOP_REG_CFG_DONE);
}

static void px30_vop_mode_set(struct px30_vop_priv *priv,
			      const struct display_timing *t, ulong fb)
{
	u32 hactive = t->hactive.typ, vactive = t->vactive.typ;
	u32 hsync_len = t->hsync_len.typ, vsync_len = t->vsync_len.typ;
	u32 htotal = hactive + t->hfront_porch.typ + hsync_len +
		     t->hback_porch.typ;
	u32 vtotal = vactive + t->vfront_porch.typ + vsync_len +
		     t->vback_porch.typ;
	u32 hact_st = hsync_len + t->hback_porch.typ;
	u32 vact_st = vsync_len + t->vback_porch.typ;
	u32 pin_pol = 0;
	int i;

	for (i = 0; i < VOP_SHADOW_LEN / 4; i++)
		priv->shadow[i] = readl(priv->regs + i * 4);

	if (t->flags & DISPLAY_FLAGS_HSYNC_HIGH)
		pin_pol |= BIT(0);
	if (t->flags & DISPLAY_FLAGS_VSYNC_HIGH)
		pin_pol |= BIT(1);

	vop_mask_write(priv, VOP_SYS_CTRL2, SYS_CTRL2_GLOBAL_REGDONE_EN, ~0U);
	vop_mask_write(priv, VOP_DSP_CTRL2, DSP_CTRL2_BLANK, 0);
	vop_mask_write(priv, VOP_DSP_CTRL0, DSP_CTRL0_MIPI_EN, ~0U);
	vop_mask_write(priv, VOP_DSP_CTRL0, DSP_CTRL0_MIPI_PIN_POL,
		       FIELD_PREP(DSP_CTRL0_MIPI_PIN_POL, pin_pol));
	/* BSP: the MIPI dclk polarity is the inverse of pixelclk-active */
	vop_mask_write(priv, VOP_DSP_CTRL0, DSP_CTRL0_MIPI_DCLK_POL,
		       t->flags & DISPLAY_FLAGS_PIXDATA_POSEDGE ? 0 : ~0U);
	/* BSP value for an RGB888 bus (only the low bit of the field) */
	vop_mask_write(priv, VOP_DSP_CTRL2, DSP_CTRL2_DITHER_DOWN,
		       FIELD_PREP(DSP_CTRL2_DITHER_DOWN, 1));
	vop_mask_write(priv, VOP_DSP_CTRL0, DSP_CTRL0_DCLK_DDR, 0);
	vop_mask_write(priv, VOP_DSP_CTRL2, DSP_CTRL2_DATA_SWAP, 0);
	vop_mask_write(priv, VOP_DSP_CTRL2, DSP_CTRL2_OUT_MODE,
		       FIELD_PREP(DSP_CTRL2_OUT_MODE, OUT_MODE_P888));
	vop_mask_write(priv, VOP_DSP_CTRL2, DSP_CTRL2_OVERLAY_MODE, 0);
	vop_mask_write(priv, VOP_BCSH_CTRL, BCSH_R2Y_EN | BCSH_Y2R_EN, 0);
	vop_mask_write(priv, VOP_BCSH_CTRL, BCSH_R2Y_CSC | BCSH_Y2R_CSC,
		       BCSH_R2Y_CSC | FIELD_PREP(BCSH_Y2R_CSC, 1));
	vop_write(priv, VOP_DSP_BG, 0);

	vop_write(priv, VOP_DSP_HTOTAL_HS_END, htotal << 16 | hsync_len);
	vop_write(priv, VOP_DSP_HACT_ST_END,
		  hact_st << 16 | (hact_st + hactive));
	vop_write(priv, VOP_DSP_VACT_ST_END,
		  vact_st << 16 | (vact_st + vactive));
	vop_mask_write(priv, VOP_DSP_CTRL2, DSP_CTRL2_INTERLACE, 0);
	vop_write(priv, VOP_DSP_VTOTAL_VS_END, vtotal << 16 | vsync_len);
	vop_mask_write(priv, VOP_DSP_CTRL0, DSP_CTRL0_CORE_DCLK_DIV, 0);
	vop_mask_write(priv, VOP_LINE_FLAG, LINE_FLAG_NUM,
		       vact_st + vactive - 3);
	vop_cfg_done(priv);

	/* Window 1: the whole screen from the framebuffer */
	vop_mask_write(priv, VOP_WIN1_CTRL0, WIN_FORMAT,
		       FIELD_PREP(WIN_FORMAT, WIN_FORMAT_ARGB8888));
	vop_mask_write(priv, VOP_WIN1_VIR, WIN_VIR_STRIDE, hactive);
	vop_write(priv, VOP_WIN1_MST, fb);
	vop_write(priv, VOP_WIN1_DSP_INFO,
		  (vactive - 1) << 16 | (hactive - 1));
	vop_write(priv, VOP_WIN1_DSP_ST, vact_st << 16 | hact_st);
	vop_mask_write(priv, VOP_WIN1_CTRL0, WIN_RB_SWAP, 0);
	vop_mask_write(priv, VOP_WIN1_CTRL0, WIN_EN, ~0U);
	vop_cfg_done(priv);
}

static void px30_vop_standby(struct px30_vop_priv *priv, bool standby)
{
	vop_mask_write(priv, VOP_SYS_CTRL2, SYS_CTRL2_STANDBY,
		       standby ? ~0U : 0);
	vop_cfg_done(priv);
}

/* Follow the output endpoints to the encoder (the DSI host bridge) */
static int px30_vop_find_bridge(struct udevice *dev, struct udevice **bridgep)
{
	ofnode port, ep, node;
	u32 phandle;

	port = dev_read_subnode(dev, "port");
	ofnode_for_each_subnode(ep, port) {
		if (ofnode_read_u32(ep, "remote-endpoint", &phandle))
			continue;
		for (node = ofnode_get_by_phandle(phandle); ofnode_valid(node);
		     node = ofnode_get_parent(node)) {
			if (!ofnode_is_enabled(node))
				break;
			if (!uclass_get_device_by_ofnode(UCLASS_VIDEO_BRIDGE,
							 node, bridgep))
				return 0;
		}
	}

	return -ENODEV;
}

/* Console rotation: lcd_rotate (degrees) or portrait panels turned 270 */
static int px30_vop_rotation(const struct display_timing *t)
{
	ulong deg = env_get_ulong("lcd_rotate", 10, ~0UL);

	if (deg != ~0UL)
		return (deg / 90) & 3;

	return t->vactive.typ > t->hactive.typ ? 3 : 0;
}

static int px30_vop_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct px30_vop_priv *priv = dev_get_priv(dev);
	struct display_timing timing;
	struct udevice *bridge, *panel;
	struct clk clk;
	ulong rate;
	int ret;

	/* Before relocation the uclass only needs the size from bind */
	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

	priv->regs = dev_read_addr_ptr(dev);
	if (!priv->regs)
		return -EINVAL;

	ret = uclass_first_device_err(UCLASS_PANEL, &panel);
	if (ret) {
		log_debug("no panel (%d)\n", ret);
		return ret;
	}
	ret = panel_get_display_timing(panel, &timing);
	if (ret)
		return ret;
	if ((ulong)timing.hactive.typ * timing.vactive.typ * 4 > plat->size) {
		printf("%s: %ux%u does not fit the framebuffer\n", dev->name,
		       timing.hactive.typ, timing.vactive.typ);
		return -ENOSPC;
	}

	ret = px30_vop_find_bridge(dev, &bridge);
	if (ret) {
		printf("%s: no DSI host (%d)\n", dev->name, ret);
		return ret;
	}

	/* The BSP never needs these; their gates are open after reset */
	if (!clk_get_by_name(dev, "aclk_vop", &clk))
		clk_enable(&clk);
	if (!clk_get_by_name(dev, "hclk_vop", &clk))
		clk_enable(&clk);
	ret = clk_get_by_name(dev, "dclk_vop", &clk);
	if (ret)
		return ret;
	rate = clk_set_rate(&clk, timing.pixelclock.typ);
	if (IS_ERR_VALUE(rate) || !rate) {
		printf("%s: pixel clock %u Hz: %ld\n", dev->name,
		       timing.pixelclock.typ, (long)rate);
		return rate ? (int)rate : -EINVAL;
	}
	clk_enable(&clk);

	px30_vop_mode_set(priv, &timing, plat->base);

	ret = video_bridge_attach(bridge);
	if (ret) {
		printf("%s: DSI host: %d\n", dev->name, ret);
		return ret;
	}
	ret = panel_enable_backlight(panel);	/* panel power and init */
	if (ret) {
		printf("%s: panel init: %d\n", dev->name, ret);
		return ret;
	}
	px30_vop_standby(priv, false);
	ret = video_bridge_set_backlight(bridge, BACKLIGHT_OFF); /* video mode */
	if (ret)
		return ret;

	uc_priv->xsize = timing.hactive.typ;
	uc_priv->ysize = timing.vactive.typ;
	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->format = VIDEO_X8R8G8B8;
	uc_priv->rot = px30_vop_rotation(&timing);
	video_set_flush_dcache(dev, true);

	return 0;
}

static int px30_vop_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->size = 4 * CONFIG_VIDEO_ROCKCHIP_MAX_XRES *
		     CONFIG_VIDEO_ROCKCHIP_MAX_YRES;

	return 0;
}

static const struct udevice_id px30_vop_ids[] = {
	{ .compatible = "rockchip,px30-vop-big" },
	{ }
};

U_BOOT_DRIVER(px30_vop) = {
	.name		= "px30_vop",
	.id		= UCLASS_VIDEO,
	.of_match	= px30_vop_ids,
	.bind		= px30_vop_bind,
	.probe		= px30_vop_probe,
	.priv_auto	= sizeof(struct px30_vop_priv),
	.flags		= DM_FLAG_PRE_RELOC,
};
