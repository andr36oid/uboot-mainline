// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family: read the vendor screen DTB into struct r36s_screen.
 *
 * Property meanings follow the vendor U-Boot readers (rockchip_panel.c,
 * dw_mipi_dsi.c, rockchip_display.c, pwm_backlight.c, rk8xx.c,
 * fg_rk817.c, charge_animation.c of the andr36oid U-Boot). Only libfdt
 * is used so this file also builds on the host for the corpus test.
 */

#ifdef R36S_HOST_TEST
#include <errno.h>
#include <string.h>
#include <libfdt.h>
#else
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#endif
#include "r36s-screen.h"

/* MIPI_DSI_MODE_VIDEO | _VIDEO_BURST | _VIDEO_HBP | _EOT_PACKET | _LPM */
#define R36S_DSI_DEFAULT_FLAGS	(0x1 | 0x2 | 0x40 | 0x200 | 0x800)

/* GPIO controllers of the PX30, by address */
static const u32 r36s_gpio_base[] = {
	0xff040000, 0xff250000, 0xff260000, 0xff270000,
};

static const char *const r36s_rail_nodes[R36S_RAIL_COUNT] = {
	"DCDC_REG1", "DCDC_REG2", "DCDC_REG3", "DCDC_REG4",
	"LDO_REG1", "LDO_REG2", "LDO_REG3", "LDO_REG4", "LDO_REG5",
	"LDO_REG6", "LDO_REG7", "LDO_REG8", "LDO_REG9",
	"BOOST", "OTG_SWITCH",
};

const char *r36s_rail_node_name(int rail)
{
	if (rail < 0 || rail >= R36S_RAIL_COUNT)
		return NULL;

	return r36s_rail_nodes[rail];
}

static u32 prop_u32(const void *fdt, int node, const char *name, u32 def)
{
	const fdt32_t *val;
	int len;

	val = fdt_getprop(fdt, node, name, &len);
	if (!val || len < (int)sizeof(*val))
		return def;

	return fdt32_to_cpu(val[0]);
}

static bool prop_bool(const void *fdt, int node, const char *name)
{
	return fdt_getprop(fdt, node, name, NULL) != NULL;
}

static const char *prop_str(const void *fdt, int node, const char *name,
			    const char *def)
{
	const char *str;
	int len;

	str = fdt_getprop(fdt, node, name, &len);
	if (!str || len < 1 || str[len - 1])
		return def;

	return str;
}

static bool node_okay(const void *fdt, int node)
{
	const char *status = prop_str(fdt, node, "status", NULL);

	return !status || !strcmp(status, "okay") || !strcmp(status, "ok");
}

static int node_by_phandle_prop(const void *fdt, int node, const char *name)
{
	u32 phandle = prop_u32(fdt, node, name, 0);

	if (!phandle)
		return -FDT_ERR_NOTFOUND;

	return fdt_node_offset_by_phandle(fdt, phandle);
}

/* Low 32 bits of a node's first reg address */
static u32 node_addr(const void *fdt, int node)
{
	const fdt32_t *reg;
	int parent, cells, len;

	parent = fdt_parent_offset(fdt, node);
	cells = parent >= 0 ? fdt_address_cells(fdt, parent) : 2;
	if (cells < 1)
		return 0;
	reg = fdt_getprop(fdt, node, "reg", &len);
	if (!reg || len < cells * (int)sizeof(*reg))
		return 0;

	return fdt32_to_cpu(reg[cells - 1]);
}

/* <&gpioN pin flags> */
static void prop_gpio(const void *fdt, int node, const char *name,
		      struct r36s_gpio *gpio)
{
	const fdt32_t *val;
	u32 addr, pin;
	int ctrl, len, i;

	memset(gpio, 0, sizeof(*gpio));
	val = fdt_getprop(fdt, node, name, &len);
	if (!val || len < 3 * (int)sizeof(*val))
		return;

	ctrl = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(val[0]));
	if (ctrl < 0)
		return;
	addr = node_addr(fdt, ctrl);
	pin = fdt32_to_cpu(val[1]);
	for (i = 0; i < (int)(sizeof(r36s_gpio_base) / sizeof(r36s_gpio_base[0]));
	     i++) {
		if (addr == r36s_gpio_base[i] && pin < 32) {
			gpio->valid = true;
			gpio->bank = i;
			gpio->pin = pin;
			gpio->active_low = fdt32_to_cpu(val[2]) & 1;
			return;
		}
	}
}

/* A supply phandle that points at one of the RK817 rails */
static int prop_rail(const void *fdt, int node, const char *name)
{
	const char *rail_name, *parent_name;
	int reg, parent, i;

	reg = node_by_phandle_prop(fdt, node, name);
	if (reg < 0)
		return R36S_RAIL_NONE;
	parent = fdt_parent_offset(fdt, reg);
	parent_name = parent >= 0 ? fdt_get_name(fdt, parent, NULL) : NULL;
	if (!parent_name || strcmp(parent_name, "regulators"))
		return R36S_RAIL_NONE;

	rail_name = fdt_get_name(fdt, reg, NULL);
	for (i = 0; i < R36S_RAIL_COUNT; i++)
		if (rail_name && !strcmp(rail_name, r36s_rail_nodes[i]))
			return i;

	return R36S_RAIL_NONE;
}

static int find_okay_compatible(const void *fdt, const char *compat)
{
	int node, first = -FDT_ERR_NOTFOUND;

	for (node = fdt_node_offset_by_compatible(fdt, -1, compat); node >= 0;
	     node = fdt_node_offset_by_compatible(fdt, node, compat)) {
		if (node_okay(fdt, node))
			return node;
		if (first < 0)
			first = node;
	}

	return first;
}

static bool timing_node(const void *fdt, int node)
{
	return node >= 0 && prop_bool(fdt, node, "hactive") &&
	       prop_bool(fdt, node, "clock-frequency");
}

static void parse_timing(const void *fdt, int panel, struct r36s_panel *p)
{
	struct r36s_timing *t = &p->timing;
	int timings, node, sub;

	timings = fdt_subnode_offset(fdt, panel, "display-timings");
	if (timings < 0)
		return;

	/*
	 * The vendor takes the node native-mode points at. Panels/Panel2
	 * points it at the VOP IOMMU, which leaves the vendor U-Boot without
	 * a picture; use the first timing there instead.
	 */
	node = node_by_phandle_prop(fdt, timings, "native-mode");
	if (!timing_node(fdt, node) ||
	    fdt_parent_offset(fdt, node) != timings) {
		node = -FDT_ERR_NOTFOUND;
		fdt_for_each_subnode(sub, fdt, timings) {
			if (timing_node(fdt, sub)) {
				node = sub;
				break;
			}
		}
		if (node < 0)
			return;
		p->timing_fallback = true;
	}

	t->clock = prop_u32(fdt, node, "clock-frequency", 0);
	t->hactive = prop_u32(fdt, node, "hactive", 0);
	t->hfront_porch = prop_u32(fdt, node, "hfront-porch", 0);
	t->hsync_len = prop_u32(fdt, node, "hsync-len", 0);
	t->hback_porch = prop_u32(fdt, node, "hback-porch", 0);
	t->vactive = prop_u32(fdt, node, "vactive", 0);
	t->vfront_porch = prop_u32(fdt, node, "vfront-porch", 0);
	t->vsync_len = prop_u32(fdt, node, "vsync-len", 0);
	t->vback_porch = prop_u32(fdt, node, "vback-porch", 0);
	t->hsync_active = prop_u32(fdt, node, "hsync-active", 0);
	t->vsync_active = prop_u32(fdt, node, "vsync-active", 0);
	t->de_active = prop_u32(fdt, node, "de-active", 0);
	t->pixelclk_active = prop_u32(fdt, node, "pixelclk-active", 0);
	p->timing_ok = t->clock && t->hactive && t->vactive;
}

/* Walk a command stream; returns false if it runs past its end */
static bool seq_ok(const u8 *seq, int len)
{
	int pos = 0;

	while (len - pos > 3) {
		pos += 3 + seq[pos + 2];
		if (pos > len)
			return false;
	}

	return true;
}

static void parse_panel(const void *fdt, struct r36s_screen *scr)
{
	struct r36s_panel *p = &scr->panel;
	const void *seq;
	int node, len;

	node = find_okay_compatible(fdt, "simple-panel-dsi");
	if (node < 0)
		return;

	p->present = true;
	p->compatible = prop_str(fdt, node, "compatible", "");
	p->lanes = prop_u32(fdt, node, "dsi,lanes", 4);
	p->format = prop_u32(fdt, node, "dsi,format", 0);
	p->mode_flags = prop_u32(fdt, node, "dsi,flags",
				 R36S_DSI_DEFAULT_FLAGS);
	p->prepare_ms = prop_u32(fdt, node, "prepare-delay-ms", 0);
	p->unprepare_ms = prop_u32(fdt, node, "unprepare-delay-ms", 0);
	p->enable_ms = prop_u32(fdt, node, "enable-delay-ms", 0);
	p->disable_ms = prop_u32(fdt, node, "disable-delay-ms", 0);
	p->init_ms = prop_u32(fdt, node, "init-delay-ms", 0);
	p->reset_ms = prop_u32(fdt, node, "reset-delay-ms", 0);
	p->width_mm = prop_u32(fdt, node, "width-mm", 0);
	p->height_mm = prop_u32(fdt, node, "height-mm", 0);
	prop_gpio(fdt, node, "reset-gpios", &p->reset);
	prop_gpio(fdt, node, "enable-gpios", &p->enable);
	p->power_rail = prop_rail(fdt, node, "power-supply");
	p->backlight_rail = prop_rail(fdt, node, "backlight-supply");

	seq = fdt_getprop(fdt, node, "panel-init-sequence", &len);
	if (seq && len > 0 && seq_ok(seq, len)) {
		p->init_seq = seq;
		p->init_len = len;
	}
	seq = fdt_getprop(fdt, node, "panel-exit-sequence", &len);
	if (seq && len > 0 && seq_ok(seq, len)) {
		p->exit_seq = seq;
		p->exit_len = len;
	}

	parse_timing(fdt, node, p);

	scr->charge_led_is_enable = p->enable.valid && p->enable.bank == 0 &&
				    p->enable.pin == 13;
}

static void parse_backlight(const void *fdt, struct r36s_screen *scr)
{
	struct r36s_backlight *bl = &scr->backlight;
	const fdt32_t *val;
	int node, panel, pwm, len, i;

	node = -FDT_ERR_NOTFOUND;
	panel = find_okay_compatible(fdt, "simple-panel-dsi");
	if (panel >= 0)
		node = node_by_phandle_prop(fdt, panel, "backlight");
	if (node < 0)
		node = find_okay_compatible(fdt, "pwm-backlight");
	if (node < 0)
		return;

	/* pwms = <&pwm channel period flags> */
	val = fdt_getprop(fdt, node, "pwms", &len);
	if (!val || len < 3 * (int)sizeof(*val))
		return;
	pwm = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(val[0]));
	if (pwm < 0)
		return;
	bl->pwm_reg = node_addr(fdt, pwm);
	bl->pwm_channel = fdt32_to_cpu(val[1]);
	bl->period_ns = fdt32_to_cpu(val[2]);
	bl->inverted = len >= 4 * (int)sizeof(*val) &&
		       (fdt32_to_cpu(val[3]) & 1);

	val = fdt_getprop(fdt, node, "brightness-levels", &len);
	if (val && len >= (int)sizeof(*val)) {
		bl->num_levels = len / sizeof(*val);
		if (bl->num_levels > R36S_MAX_LEVELS)
			bl->num_levels = R36S_MAX_LEVELS;
		for (i = 0; i < (int)bl->num_levels; i++)
			bl->levels[i] = fdt32_to_cpu(val[i]);
	} else {
		/* no table: 0..255 */
		bl->num_levels = R36S_MAX_LEVELS;
		for (i = 0; i < R36S_MAX_LEVELS; i++)
			bl->levels[i] = i;
	}
	bl->default_index = prop_u32(fdt, node, "default-brightness-level",
				     bl->num_levels - 1);
	if (bl->default_index >= bl->num_levels)
		bl->default_index = bl->num_levels - 1;

	bl->present = bl->pwm_reg && bl->period_ns;
}

static void parse_pmic(const void *fdt, struct r36s_screen *scr)
{
	int pmic, regs, node, i;
	const fdt32_t *val;
	int len;

	pmic = find_okay_compatible(fdt, "rockchip,rk817");
	if (pmic < 0)
		pmic = find_okay_compatible(fdt, "rockchip,rk809");
	if (pmic < 0)
		return;

	regs = fdt_subnode_offset(fdt, pmic, "regulators");
	if (regs >= 0) {
		fdt_for_each_subnode(node, fdt, regs) {
			const char *name = fdt_get_name(fdt, node, NULL);
			struct r36s_rail_cfg *rail;

			for (i = 0; i < R36S_RAIL_COUNT; i++)
				if (name && !strcmp(name, r36s_rail_nodes[i]))
					break;
			if (i == R36S_RAIL_COUNT)
				continue;

			rail = &scr->rails[i];
			rail->present = true;
			rail->name = prop_str(fdt, node, "regulator-name",
					      r36s_rail_nodes[i]);
			rail->min_uv = prop_u32(fdt, node,
						"regulator-min-microvolt", 0);
			rail->max_uv = prop_u32(fdt, node,
						"regulator-max-microvolt", 0);
			rail->always_on = prop_bool(fdt, node,
						    "regulator-always-on");
			rail->boot_on = prop_bool(fdt, node,
						  "regulator-boot-on");
		}
	}

	/* The vendor fuel gauge binds by node name ("battery...") */
	fdt_for_each_subnode(node, fdt, pmic) {
		const char *name = fdt_get_name(fdt, node, NULL);
		struct r36s_battery *bat = &scr->battery;

		if (!name || strncmp(name, "battery", 7) || bat->present)
			continue;
		bat->present = true;
		val = fdt_getprop(fdt, node, "ocv_table", &len);
		if (val && len > 0) {
			bat->ocv_count = len / sizeof(*val);
			if (bat->ocv_count > R36S_OCV_POINTS)
				bat->ocv_count = R36S_OCV_POINTS;
			for (i = 0; i < (int)bat->ocv_count; i++)
				bat->ocv_table[i] = fdt32_to_cpu(val[i]);
		}
		bat->design_capacity = prop_u32(fdt, node, "design_capacity", 0);
		bat->design_qmax = prop_u32(fdt, node, "design_qmax", 0);
		bat->bat_res = prop_u32(fdt, node, "bat_res", 0);
		bat->sample_res = prop_u32(fdt, node, "sample_res", 0);
		bat->power_off_thresd = prop_u32(fdt, node, "power_off_thresd",
						 0);
		bat->zero_algorithm_vol = prop_u32(fdt, node,
						   "zero_algorithm_vol", 0);
		bat->virtual_power = prop_u32(fdt, node, "virtual_power", 0);
	}

	node = fdt_subnode_offset(fdt, pmic, "charger");
	if (node >= 0) {
		struct r36s_charger *chg = &scr->charger;

		chg->present = true;
		chg->min_input_voltage = prop_u32(fdt, node,
						  "min_input_voltage", 0);
		chg->max_input_current = prop_u32(fdt, node,
						  "max_input_current", 0);
		chg->max_chrg_current = prop_u32(fdt, node,
						 "max_chrg_current", 0);
		chg->max_chrg_voltage = prop_u32(fdt, node,
						 "max_chrg_voltage", 0);
		chg->chrg_finish_cur = prop_u32(fdt, node, "chrg_finish_cur",
						0);
		prop_gpio(fdt, node, "dc_det_gpio", &chg->dc_det);
	}
}

static void parse_charge(const void *fdt, struct r36s_screen *scr)
{
	struct r36s_charge_cfg *c = &scr->charge;
	int node;

	node = find_okay_compatible(fdt, "rockchip,uboot-charge");
	if (node < 0)
		return;

	c->present = true;
	c->enabled = node_okay(fdt, node);
	c->uboot_charge_on = prop_u32(fdt, node, "rockchip,uboot-charge-on", 0);
	c->android_charge_on = prop_u32(fdt, node,
					"rockchip,android-charge-on", 0);
	c->exit_charge_level = prop_u32(fdt, node,
					"rockchip,uboot-exit-charge-level", 0);
	c->exit_charge_voltage = prop_u32(fdt, node,
					  "rockchip,uboot-exit-charge-voltage",
					  0);
	c->screen_on_voltage = prop_u32(fdt, node, "rockchip,screen-on-voltage",
					0);
	c->low_power_voltage = prop_u32(fdt, node,
					"rockchip,uboot-low-power-voltage", 0);
	c->system_suspend = prop_u32(fdt, node, "rockchip,system-suspend", 0);
	c->auto_off_screen_interval =
		prop_u32(fdt, node, "rockchip,auto-off-screen-interval", 0);
	c->auto_wakeup_interval = prop_u32(fdt, node,
					   "rockchip,auto-wakeup-interval", 0);
	c->auto_wakeup_screen_invert =
		prop_u32(fdt, node, "rockchip,auto-wakeup-screen-invert", 0);
}

static void parse_route(const void *fdt, struct r36s_screen *scr)
{
	int node = fdt_path_offset(fdt, "/display-subsystem/route/route-dsi");

	scr->logo_uboot = "logo.bmp";
	scr->logo_kernel = "logo_kernel.bmp";
	scr->logo_mode = "center";
	scr->charge_logo_mode = "center";
	if (node < 0)
		return;

	scr->logo_uboot = prop_str(fdt, node, "logo,uboot", scr->logo_uboot);
	scr->logo_kernel = prop_str(fdt, node, "logo,kernel", scr->logo_kernel);
	scr->logo_mode = prop_str(fdt, node, "logo,mode", scr->logo_mode);
	scr->charge_logo_mode = prop_str(fdt, node, "charge_logo,mode",
					 scr->charge_logo_mode);
}

int r36s_screen_parse(const void *fdt, struct r36s_screen *scr)
{
	int i;

	memset(scr, 0, sizeof(*scr));
	scr->panel.power_rail = R36S_RAIL_NONE;
	scr->panel.backlight_rail = R36S_RAIL_NONE;
	if (fdt_check_header(fdt))
		return -EINVAL;

	scr->fdt = fdt;
	scr->model = prop_str(fdt, 0, "model", "");
	scr->compatible = prop_str(fdt, 0, "compatible", "");
	for (i = 0; i < R36S_RAIL_COUNT; i++)
		scr->rails[i].name = r36s_rail_nodes[i];

	parse_panel(fdt, scr);
	parse_backlight(fdt, scr);
	parse_pmic(fdt, scr);
	parse_charge(fdt, scr);
	parse_route(fdt, scr);

	return 0;
}
