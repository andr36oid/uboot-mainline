// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family: what U-Boot shows on the screen.
 *
 *   - boot logo: BOOT/logo.bmp (24 bpp, usually the panel's size,
 *     pre-turned by mkimg for portrait panels), centred
 *   - the U-Boot version and build time in grey on rows 0 and 1
 *   - status screens (low battery, no card, boot failed): an optional
 *     BOOT/<name>.bmp and one centred line of text
 *   - the vendor "lcd" command boot.ini uses (setfg, setline, putstr, ...)
 *
 * Text uses the 8x16 font like the vendor. Portrait panels get a console
 * turned by 270 degrees (or by lcd_rotate, 0-3) so their text reads the
 * right way round.
 */

#include <command.h>
#include <dm.h>
#include <env.h>
#include <fs.h>
#include <log.h>
#include <malloc.h>
#include <mapmem.h>
#include <panel.h>
#include <backlight.h>
#include <splash.h>
#include <timestamp.h>
#include <version.h>
#include <video.h>
#include <video_console.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <vsprintf.h>
#include "r36s.h"

#define R36S_BMP_MAX	(4 * SZ_1M)

static struct udevice *r36s_video;
static struct udevice *r36s_con;
static struct udevice *r36s_panel_dev;
static int r36s_display_state;		/* 0 untried, 1 up, -1 failed */

/* The last logo drawn, kept for the clean redraw before the kernel */
static void *r36s_logo_bmp;

bool r36s_display_ok(void)
{
	return r36s_display_state > 0;
}

struct udevice *r36s_display_dev(void)
{
	return r36s_display_ok() ? r36s_video : NULL;
}

int r36s_display_start(void)
{
	int ret;

	if (r36s_display_state)
		return r36s_display_state > 0 ? 0 : -ENODEV;

	r36s_display_state = -1;
	if (!r36s_screen_get())
		return -ENODEV;

	ret = uclass_first_device_err(UCLASS_VIDEO, &r36s_video);
	if (ret) {
		printf("Display: not started (%d)\n", ret);
		return ret;
	}
	ret = uclass_first_device_err(UCLASS_VIDEO_CONSOLE, &r36s_con);
	if (ret) {
		printf("Display: no text console (%d)\n", ret);
		return ret;
	}
	ret = uclass_first_device_err(UCLASS_PANEL, &r36s_panel_dev);
	if (ret)
		return ret;

	r36s_display_state = 1;

	return 0;
}

void r36s_backlight(bool on)
{
	if (r36s_display_ok())
		panel_set_backlight(r36s_panel_dev,
				    on ? BACKLIGHT_DEFAULT : BACKLIGHT_OFF);
}

static void r36s_set_colours(u32 fg, u32 bg)
{
	struct video_priv *priv = dev_get_uclass_priv(r36s_video);

	/* X8R8G8B8: the pixel value is the RGB value */
	priv->colour_fg = fg;
	priv->colour_bg = bg;
}

static void r36s_sync(void)
{
	video_sync(r36s_video, true);
}

/*
 * Text at a console row (negative: from the bottom), centred if col < 0.
 * Rows outside the screen are not drawn.
 */
void r36s_lcd_print(int row, int col, u32 fg, u32 bg, const char *str)
{
	struct vidconsole_priv *cp;

	if (!r36s_display_ok())
		return;

	cp = dev_get_uclass_priv(r36s_con);
	if (row < 0)
		row += cp->rows;
	if (row < 0 || row >= cp->rows)
		return;
	if (col < 0)
		col = max(0, (int)(cp->cols - strlen(str)) / 2);
	r36s_set_colours(fg, bg);
	vidconsole_position_cursor(r36s_con, col, row);
	vidconsole_put_string(r36s_con, str);
	r36s_sync();
}

/* Read a file from the BOOT partition U-Boot uses into a new buffer */
void *r36s_boot_file(const char *name, ulong max, loff_t *sizep)
{
	const char *part = r36s.part;
	loff_t len, got;
	void *buf;

	if (fs_set_blk_dev("mmc", part, FS_TYPE_ANY) || fs_size(name, &len) ||
	    len <= 0 || len > max)
		return NULL;
	buf = malloc(len);
	if (!buf)
		return NULL;
	if (fs_set_blk_dev("mmc", part, FS_TYPE_ANY) ||
	    fs_read(name, map_to_sysmem(buf), 0, len, &got) || got != len) {
		free(buf);
		return NULL;
	}
	if (sizep)
		*sizep = len;

	return buf;
}

int r36s_draw_bmp_buf(void *bmp)
{
	int ret;

	if (!r36s_display_ok())
		return -ENODEV;
	ret = video_bmp_display(r36s_video, map_to_sysmem(bmp),
				BMP_ALIGN_CENTER, BMP_ALIGN_CENTER, true);
	r36s_sync();

	return ret;
}

/* Clear the screen to black and draw BOOT/<name> if it is there */
int r36s_show_bmp(const char *name)
{
	void *bmp;
	int ret;

	if (!r36s_display_ok())
		return -ENODEV;

	r36s_set_colours(R36S_WHITE, R36S_BLACK);
	video_clear(r36s_video);
	bmp = name ? r36s_boot_file(name, R36S_BMP_MAX, NULL) : NULL;
	if (!bmp) {
		r36s_sync();
		return -ENOENT;
	}

	ret = r36s_draw_bmp_buf(bmp);
	if (ret)
		printf("%s: not a BMP U-Boot can show (%d)\n", name, ret);
	free(bmp);

	return ret;
}

static void r36s_version_rows(void)
{
	r36s_lcd_print(0, 0, R36S_GREY, R36S_BLACK, " U-Boot " PLAIN_VERSION);
	r36s_lcd_print(1, 0, R36S_GREY, R36S_BLACK,
		       " " U_BOOT_DATE " " U_BOOT_TIME);
}

int r36s_show_logo(void)
{
	const struct r36s_screen *scr = r36s_screen_get();
	void *bmp;
	int ret;

	if (r36s_display_start())
		return -ENODEV;

	r36s_set_colours(R36S_WHITE, R36S_BLACK);
	video_clear(r36s_video);
	bmp = r36s_boot_file(scr->logo_uboot, R36S_BMP_MAX, NULL);
	if (bmp) {
		ret = r36s_draw_bmp_buf(bmp);
		if (ret) {
			printf("%s: not a BMP U-Boot can show (%d)\n",
			       scr->logo_uboot, ret);
			free(bmp);
		} else {
			free(r36s_logo_bmp);
			r36s_logo_bmp = bmp;
		}
	} else {
		printf("No %s on BOOT\n", scr->logo_uboot);
	}

	r36s_version_rows();
	r36s_backlight(true);

	return 0;
}

/*
 * Before the kernel: the logo again without U-Boot's text, so the kernel
 * takes over a clean picture (as the vendor's display fixup does).
 */
void r36s_redraw_logo(void)
{
	if (!r36s_display_ok() || !r36s_logo_bmp)
		return;

	r36s_set_colours(R36S_WHITE, R36S_BLACK);
	video_clear(r36s_video);
	r36s_draw_bmp_buf(r36s_logo_bmp);
}

/*
 * A status screen as the vendor draws it: the screen cleared to black,
 * BOOT/<bmp> if it is there (none is shipped today), then one centred
 * message line in fg on bg (the vendor's row 18 + disp_offs, which is
 * three rows above the bottom on the family's panels).
 */
void r36s_status_screen(const char *bmp, u32 fg, u32 bg, const char *msg)
{
	if (r36s_display_start())
		return;

	if (bmp)
		r36s_show_bmp(bmp);
	if (msg)
		r36s_lcd_print(-3, -1, fg, bg, msg);
	r36s_backlight(true);
}

/*
 * The vendor "lcd" command (odroid-goadv rockchip_display_cmds.c), as
 * boot.ini uses it: setfg/setbg by name or r g b, setline, putstr,
 * setcur, clear, clrline, init, settransp.
 */
static u32 r36s_lcd_fg = R36S_WHITE, r36s_lcd_bg = R36S_BLACK;

static const struct {
	const char *name;
	u32 rgb;
} r36s_lcd_colours[] = {
	{ "red", 0xff0000 }, { "green", 0x00ff00 }, { "yellow", 0xffff00 },
	{ "blue", 0x0000ff }, { "magenta", 0xff00ff }, { "cyan", 0x00ffff },
	{ "grey", 0xaaaaaa }, { "black", 0x000000 }, { "white", 0xffffff },
};

static int r36s_lcd_colour(int argc, char *const argv[], u32 *rgb)
{
	int i;

	if (argc == 3) {
		for (i = 0; i < ARRAY_SIZE(r36s_lcd_colours); i++) {
			if (!strcmp(argv[2], r36s_lcd_colours[i].name)) {
				*rgb = r36s_lcd_colours[i].rgb;
				return 0;
			}
		}
		/* the vendor ignores unknown names */
		return 0;
	}
	if (argc == 5) {
		*rgb = (dectoul(argv[2], NULL) & 0xff) << 16 |
		       (dectoul(argv[3], NULL) & 0xff) << 8 |
		       (dectoul(argv[4], NULL) & 0xff);
		return 0;
	}

	return CMD_RET_USAGE;
}

static int do_lcd(struct cmd_tbl *cmdtp, int flag, int argc,
		  char *const argv[])
{
	struct vidconsole_priv *cp;
	const char *sub;

	if (argc < 2)
		return CMD_RET_USAGE;
	sub = argv[1];

	if (!strcmp(sub, "init"))
		return r36s_display_start() ? CMD_RET_FAILURE : 0;
	if (r36s_display_start())
		return CMD_RET_FAILURE;
	cp = dev_get_uclass_priv(r36s_con);

	if (!strcmp(sub, "setfg"))
		return r36s_lcd_colour(argc, argv, &r36s_lcd_fg);
	if (!strcmp(sub, "setbg"))
		return r36s_lcd_colour(argc, argv, &r36s_lcd_bg);
	if (!strcmp(sub, "settransp"))
		return argc == 3 ? 0 : CMD_RET_USAGE;
	if (!strcmp(sub, "putstr") && argc == 3) {
		r36s_set_colours(r36s_lcd_fg, r36s_lcd_bg);
		vidconsole_put_string(r36s_con, argv[2]);
		r36s_sync();
		return 0;
	}
	if (!strcmp(sub, "setline") && argc == 3) {
		ulong line = dectoul(argv[2], NULL);

		if (line > cp->rows - 1) {
			printf("lcd line overflow (line max = %d)\n",
			       cp->rows - 1);
			return CMD_RET_USAGE;
		}
		vidconsole_position_cursor(r36s_con, 0, line);
		return 0;
	}
	if (!strcmp(sub, "setcur") && argc == 4) {
		vidconsole_set_cursor_pos(r36s_con, dectoul(argv[2], NULL),
					  dectoul(argv[3], NULL));
		return 0;
	}
	if (!strcmp(sub, "clear") && argc == 2) {
		r36s_set_colours(r36s_lcd_fg, r36s_lcd_bg);
		video_clear(r36s_video);
		vidconsole_position_cursor(r36s_con, 0, 0);
		r36s_sync();
		return 0;
	}
	if (!strcmp(sub, "clrline") && argc == 3) {
		ulong line = dectoul(argv[2], NULL);
		int i;

		if (line > cp->rows - 1)
			return CMD_RET_USAGE;
		r36s_set_colours(r36s_lcd_fg, r36s_lcd_bg);
		vidconsole_position_cursor(r36s_con, 0, line);
		for (i = 0; i < cp->cols; i++)
			vidconsole_put_char(r36s_con, ' ');
		vidconsole_position_cursor(r36s_con, 0, line);
		r36s_sync();
		return 0;
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(lcd, 5, 1, do_lcd,
	   "text on the screen (vendor odroid-goadv lcd command)",
	   "init - start the screen\n"
	   "lcd clear - clear to the background colour, cursor to 0,0\n"
	   "lcd clrline <line> - clear a text line\n"
	   "lcd putstr <string> - print at the cursor\n"
	   "lcd setcur <x> <y> - cursor to pixel x,y\n"
	   "lcd setline <line> - cursor to the start of a text line\n"
	   "lcd setfg|setbg <colour> - red, green, yellow, blue, magenta,\n"
	   "    cyan, grey, white, black\n"
	   "lcd setfg|setbg <r> <g> <b> - colour from 0-255 values\n"
	   "lcd settransp <0|1> - accepted, has no effect");
