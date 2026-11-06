// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S family: pin and GPIO setup the vendor U-Boot does in C
 * (board/rockchip/odroidgoa/odroidgoa.c and arch/arm/mach-rockchip/board.c
 * of the andr36oid U-Boot), with the same register values.
 */

#include <asm/gpio.h>
#include <asm/io.h>
#include <asm/arch-rockchip/hardware.h>
#include <dm.h>
#include <log.h>
#include <vsprintf.h>
#include <linux/errno.h>
#include "r36s.h"

#define GRF_BASE		0xff140000
#define GRF_GPIO1AL_IOMUX	0x00
#define GRF_GPIO1AH_IOMUX	0x04
#define GRF_GPIO1BL_IOMUX	0x08
#define GRF_GPIO1BH_IOMUX	0x0c
#define GRF_GPIO1A_P		0x60
#define GRF_GPIO1B_P		0x64
#define GRF_GPIO2A_P		0x70
#define GRF_GPIO3B_P		0x84
#define GRF_GPIO1A_E		0xf0
#define GRF_GPIO1B_E		0xf4
#define GRF_IOFUNC_CON0		0x184

#define GRF(off)		((void __iomem *)(ulong)(GRF_BASE + (off)))
#define PMUGRF_BASE		0xff010000

/*
 * Mux a pin to its GPIO function. GPIO0 has 2-bit fields in the PMUGRF
 * (8 pins per register), GPIO1-3 4-bit fields in the GRF (4 pins per
 * register), as in the PX30 pinctrl driver.
 */
void r36s_pin_gpio(unsigned int bank, unsigned int pin)
{
	if (bank > 3 || pin > 31)
		return;

	if (!bank) {
		writel(3 << (16 + (pin % 8) * 2),
		       (void __iomem *)(ulong)(PMUGRF_BASE + (pin / 8) * 4));
		return;
	}

	writel(0xf << (16 + (pin % 4) * 4),
	       GRF((bank - 1) * 0x20 + (pin / 4) * 4));
}

/* Request a screen DTB GPIO by its rk_gpio name ("D16" = GPIO3_C0) */
int r36s_gpio_request(const struct r36s_gpio *gpio, const char *label,
		      ulong flags, struct gpio_desc *desc)
{
	char name[8];
	int ret;

	if (!gpio->valid)
		return -ENOENT;

	snprintf(name, sizeof(name), "%c%u", 'A' + gpio->bank, gpio->pin);
	ret = dm_gpio_lookup_name(name, desc);
	if (ret)
		return ret;
	ret = dm_gpio_request(desc, label);
	if (ret)
		return ret;

	r36s_pin_gpio(gpio->bank, gpio->pin);
	if (gpio->active_low)
		flags |= GPIOD_ACTIVE_LOW;
	ret = dm_gpio_set_dir_flags(desc, flags);
	if (ret)
		dm_gpio_free(desc->dev, desc);

	return ret;
}

/* Drive a GPIO given as "<bank letter><pin>" (rk_gpio bank naming) */
int r36s_gpio_out(const char *name, int value)
{
	struct gpio_desc desc;
	int ret;

	ret = dm_gpio_lookup_name(name, &desc);
	if (ret)
		return ret;
	ret = dm_gpio_request(&desc, "r36s");
	if (ret && ret != -EBUSY)
		return ret;
	ret = dm_gpio_set_dir_flags(&desc, GPIOD_IS_OUT |
				    (value ? GPIOD_IS_OUT_ACTIVE : 0));
	dm_gpio_free(desc.dev, &desc);

	return ret;
}

/* UART2 on its M1 pins (GPIO2_B4/B6), as the DDR init already set them */
static void r36s_uart2_m1(void)
{
	rk_clrsetreg(GRF(GRF_IOFUNC_CON0), 3 << 10, 1 << 10);
}

/* vendor board_init_switch_gpio(): button pins as GPIO with pull-ups */
static void r36s_switch_pins(void)
{
	rk_clrreg(GRF(GRF_GPIO1AL_IOMUX), 0x0f00);	/* GPIO1_A2 */
	rk_clrreg(GRF(GRF_GPIO1AH_IOMUX), 0xfff0);	/* GPIO1_A5-A7 */
	rk_clrreg(GRF(GRF_GPIO1BH_IOMUX), 0xffff);	/* GPIO1_B4-B7 */

	rk_clrsetreg(GRF(GRF_GPIO1B_P), 0xff00, 0x5500);	/* B4-B7 up */
	rk_clrsetreg(GRF(GRF_GPIO1A_P), 0xfcc0, 0x5440);	/* A3, A5-A7 up */
	rk_clrsetreg(GRF(GRF_GPIO2A_P), 0xffff, 0x5555);	/* GPIO2_A0-A7 up */
	rk_clrsetreg(GRF(GRF_GPIO3B_P), 0xc030, 0x4010);	/* GPIO3_B2, B7 up */
}

void r36s_board_pins(void)
{
	/*
	 * Rumble motor off (GPIO0_B7), as the vendor does. The vendor also
	 * drives GPIO3_B6 high as the ODROID-GO Wi-Fi enable; no console of
	 * this family has Wi-Fi there, and on EE and Soy Sauce clones it is
	 * a button, so that one is left alone.
	 */
	if (r36s_gpio_out("A15", 0))
		log_warning("rumble GPIO0_B7 not set\n");

	r36s_uart2_m1();
	r36s_switch_pins();
}

/*
 * vendor andr36oid_emmc_pins(): GPIO1_A0-A7, B1, B2 to the eMMC controller
 * (function 2), pull-ups on the data and command lines, 8 mA like the
 * kernel's pin config. These pins are buttons on boards without eMMC, so
 * this only runs when the eMMC is used.
 */
void r36s_emmc_pins(void)
{
	writel(0xffff2222, GRF(GRF_GPIO1AL_IOMUX));	/* GPIO1_A0-A3 */
	writel(0xffff2222, GRF(GRF_GPIO1AH_IOMUX));	/* GPIO1_A4-A7 */
	writel(0x0ff00220, GRF(GRF_GPIO1BL_IOMUX));	/* GPIO1_B1-B2 */
	writel(0xffff5555, GRF(GRF_GPIO1A_P));		/* GPIO1_A pulls */
	writel(0x003c0010, GRF(GRF_GPIO1B_P));		/* GPIO1_B1-B2 pulls */
	writel(0xffffaaaa, GRF(GRF_GPIO1A_E));		/* GPIO1_A0-A7 8 mA */
	writel(0x003c0028, GRF(GRF_GPIO1B_E));		/* GPIO1_B1-B2 8 mA */
	r36s.emmc_pins = true;
}
