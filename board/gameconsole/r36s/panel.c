// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family: the MIPI DSI panel, driven from the screen DTB.
 *
 * The control DT only has a "gameconsole,r36s-panel" placeholder under the
 * DSI host. Init and exit commands, delays, GPIOs, rails, timing and the
 * backlight PWM all come from the screen DTB on BOOT and are used the way
 * the vendor rockchip_panel.c and pwm_backlight.c use them:
 *
 *   prepare:  power-supply and backlight-supply at their minimum voltage,
 *             enable GPIO, prepare delay, reset pulse, init delay, init
 *             commands (DSI host in command mode, low power)
 *   enable:   enable delay, then the backlight
 *
 * The DSI glue calls panel_enable_backlight() for "prepare" before it
 * switches the host to video mode; the board turns the backlight on with
 * panel_set_backlight() once the picture is drawn.
 */

#include <backlight.h>
#include <dm.h>
#include <log.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <pwm.h>
#include <asm/gpio.h>
#include <dm/device-internal.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include "r36s.h"

struct r36s_panel_priv {
	const struct r36s_screen *scr;
	struct gpio_desc reset;
	struct gpio_desc enable;
	struct udevice *pwm;
	bool prepared;
	bool bl_on;
};

/* Vendor command stream: type, delay ms, length, payload */
static int r36s_panel_send(struct mipi_dsi_device *dsi, const u8 *seq,
			   int len)
{
	int pos = 0;

	while (len - pos > 3) {
		u8 type = seq[pos], delay = seq[pos + 1], n = seq[pos + 2];
		const u8 *data = seq + pos + 3;
		ssize_t ret;

		/*
		 * Like the vendor: generic types go out as generic writes,
		 * DCS types as DCS writes. The packet type then follows the
		 * payload length, so a 3-byte "0x15" is sent as a long write.
		 */
		switch (type) {
		case 0x03:
		case 0x13:
		case 0x23:
		case 0x29:
			ret = mipi_dsi_generic_write(dsi, data, n);
			break;
		case 0x05:
		case 0x15:
		case 0x39:
			ret = mipi_dsi_dcs_write_buffer(dsi, data, n);
			break;
		default:
			printf("panel: command type 0x%02x not supported\n",
			       type);
			return -EINVAL;
		}
		if (ret < 0) {
			printf("panel: command at %d failed (%zd)\n", pos, ret);
			return ret;
		}
		if (delay)
			mdelay(delay);
		pos += 3 + n;
	}

	return 0;
}

static void r36s_panel_rail(const struct r36s_screen *scr, int rail, bool on)
{
	int ret;

	if (rail == R36S_RAIL_NONE)
		return;

	ret = r36s_rail_set(rail, on ? scr->rails[rail].min_uv : 0, on);
	if (ret)
		printf("panel: %s %s failed (%d)\n", r36s_rail_node_name(rail),
		       on ? "on" : "off", ret);
}

/* "prepare" in vendor terms: power, reset, init commands */
static int r36s_panel_enable_backlight(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct r36s_panel_priv *priv = dev_get_priv(dev);
	const struct r36s_panel *p = &priv->scr->panel;
	int ret;

	if (priv->prepared)
		return 0;

	ret = mipi_dsi_attach(plat->device);
	if (ret < 0)
		return ret;

	r36s_panel_rail(priv->scr, p->power_rail, true);
	r36s_panel_rail(priv->scr, p->backlight_rail, true);

	if (dm_gpio_is_valid(&priv->enable))
		dm_gpio_set_value(&priv->enable, 1);
	mdelay(p->prepare_ms);

	if (dm_gpio_is_valid(&priv->reset))
		dm_gpio_set_value(&priv->reset, 1);
	mdelay(p->reset_ms);
	if (dm_gpio_is_valid(&priv->reset))
		dm_gpio_set_value(&priv->reset, 0);
	mdelay(p->init_ms);

	if (p->init_seq) {
		ret = r36s_panel_send(plat->device, p->init_seq, p->init_len);
		if (ret)
			return ret;
	}

	priv->prepared = true;

	return 0;
}

static int r36s_backlight_pwm(struct r36s_panel_priv *priv, int percent)
{
	const struct r36s_backlight *bl = &priv->scr->backlight;
	u32 level, max, duty;
	int idx, ret;

	if (!priv->pwm)
		return -ENODEV;

	if (percent == BACKLIGHT_OFF) {
		priv->bl_on = false;
		return pwm_set_enable(priv->pwm, bl->pwm_channel, false);
	}

	if (percent == BACKLIGHT_DEFAULT || percent < 0)
		idx = bl->default_index;
	else
		idx = DIV_ROUND_CLOSEST(min(percent, 100) *
					((int)bl->num_levels - 1), 100);
	level = bl->levels[idx];

	/* Vendor pwm_backlight: period x level / (max + 1) */
	max = bl->levels[bl->num_levels - 1];
	if (bl->levels[0] > max)
		max = bl->levels[0];
	duty = (u64)bl->period_ns * level / (max + 1);

	ret = pwm_set_invert(priv->pwm, bl->pwm_channel, bl->inverted);
	if (!ret)
		ret = pwm_set_config(priv->pwm, bl->pwm_channel,
				     bl->period_ns, duty);
	if (!ret)
		ret = pwm_set_enable(priv->pwm, bl->pwm_channel, true);
	if (!ret)
		mdelay(10);

	return ret;
}

/* "enable" in vendor terms: enable delay, then the backlight */
static int r36s_panel_set_backlight(struct udevice *dev, int percent)
{
	struct r36s_panel_priv *priv = dev_get_priv(dev);
	int ret;

	if (percent != BACKLIGHT_OFF && !priv->bl_on)
		mdelay(priv->scr->panel.enable_ms);

	ret = r36s_backlight_pwm(priv, percent);
	if (!ret && percent != BACKLIGHT_OFF)
		priv->bl_on = true;

	return ret;
}

static int r36s_panel_get_display_timing(struct udevice *dev,
					 struct display_timing *timing)
{
	struct r36s_panel_priv *priv = dev_get_priv(dev);
	const struct r36s_timing *t = &priv->scr->panel.timing;

	memset(timing, 0, sizeof(*timing));
	timing->pixelclock.typ = t->clock;
	timing->hactive.typ = t->hactive;
	timing->hfront_porch.typ = t->hfront_porch;
	timing->hsync_len.typ = t->hsync_len;
	timing->hback_porch.typ = t->hback_porch;
	timing->vactive.typ = t->vactive;
	timing->vfront_porch.typ = t->vfront_porch;
	timing->vsync_len.typ = t->vsync_len;
	timing->vback_porch.typ = t->vback_porch;
	timing->flags = (t->hsync_active ? DISPLAY_FLAGS_HSYNC_HIGH :
					   DISPLAY_FLAGS_HSYNC_LOW) |
			(t->vsync_active ? DISPLAY_FLAGS_VSYNC_HIGH :
					   DISPLAY_FLAGS_VSYNC_LOW) |
			(t->de_active ? DISPLAY_FLAGS_DE_HIGH :
					DISPLAY_FLAGS_DE_LOW) |
			(t->pixelclk_active ? DISPLAY_FLAGS_PIXDATA_POSEDGE :
					      DISPLAY_FLAGS_PIXDATA_NEGEDGE);

	return 0;
}

static struct udevice *r36s_panel_find_pwm(u32 reg)
{
	struct udevice *dev;
	struct uclass *uc;

	if (uclass_get(UCLASS_PWM, &uc))
		return NULL;
	uclass_foreach_dev(dev, uc) {
		if ((u32)dev_read_addr(dev) == reg &&
		    !device_probe(dev))
			return dev;
	}

	return NULL;
}

static int r36s_panel_probe(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct r36s_panel_priv *priv = dev_get_priv(dev);
	const struct r36s_screen *scr = r36s_screen_get();
	const struct r36s_panel *p;
	int ret;

	if (!scr || !scr->panel.present || !scr->panel.timing_ok)
		return -ENODEV;

	priv->scr = scr;
	p = &scr->panel;
	plat->lanes = p->lanes;
	plat->format = p->format;
	plat->mode_flags = p->mode_flags;

	ret = r36s_gpio_request(&p->reset, "lcd-reset", GPIOD_IS_OUT,
				&priv->reset);
	if (ret && ret != -ENOENT)
		printf("panel: reset GPIO %c%u: %d\n", 'A' + p->reset.bank,
		       p->reset.pin, ret);
	ret = r36s_gpio_request(&p->enable, "lcd-enable", GPIOD_IS_OUT,
				&priv->enable);
	if (ret && ret != -ENOENT)
		printf("panel: enable GPIO %c%u: %d\n", 'A' + p->enable.bank,
		       p->enable.pin, ret);

	if (scr->backlight.present) {
		priv->pwm = r36s_panel_find_pwm(scr->backlight.pwm_reg);
		if (!priv->pwm)
			printf("panel: no PWM at %08x for the backlight\n",
			       scr->backlight.pwm_reg);
	}

	return 0;
}

static const struct panel_ops r36s_panel_ops = {
	.enable_backlight	= r36s_panel_enable_backlight,
	.set_backlight		= r36s_panel_set_backlight,
	.get_display_timing	= r36s_panel_get_display_timing,
};

static const struct udevice_id r36s_panel_ids[] = {
	{ .compatible = "gameconsole,r36s-panel" },
	{ }
};

U_BOOT_DRIVER(r36s_panel) = {
	.name		= "r36s_panel",
	.id		= UCLASS_PANEL,
	.of_match	= r36s_panel_ids,
	.ops		= &r36s_panel_ops,
	.probe		= r36s_panel_probe,
	.priv_auto	= sizeof(struct r36s_panel_priv),
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
};
