/*
 * Hardware definitions for HP iPAQ H3xxx Handheld Computers
 *
 * Copyright 2000,1 Compaq Computer Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * COMPAQ COMPUTER CORPORATION MAKES NO WARRANTIES, EXPRESSED OR IMPLIED,
 * AS TO THE USEFULNESS OR CORRECTNESS OF THIS CODE OR ITS
 * FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 * Author: Jamey Hicks.
 *
 * History:
 *
 * 2001-10-??   Andrew Christian   Added support for iPAQ H3800
 *                                 and abstracted EGPIO interface.
 *
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#include <video/sa1100fb.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <linux/platform_data/irda-sa11x0.h>

#include <mach/h3xxx.h>
#include <mach/irqs.h>

#include "generic.h"

#define H3800_EGPIO_IR_ON_N          (H3XXX_EGPIO_BASE + 0)   /* Apply power to the IR Module */
#define H3800_EGPIO_SD_PWR_ON        (H3XXX_EGPIO_BASE + 1)   /* Secure Digital power on */
#define H3800_EGPIO_RS232_ON         (H3XXX_EGPIO_BASE + 2)   /* Turn on power to the RS232 chip ? */
#define H3800_EGPIO_PULSE_GEN        (H3XXX_EGPIO_BASE + 3)   /* Goes to speaker / earphone */
#define H3800_EGPIO_CH_TIMER         (H3XXX_EGPIO_BASE + 4)   /* Charger */
#define H3800_EGPIO_LCD_5V_ON        (H3XXX_EGPIO_BASE + 5)   /* Enables LCD_5V */
#define H3800_EGPIO_LCD_ON           (H3XXX_EGPIO_BASE + 6)   /* Enables LCD_3V */
#define H3800_EGPIO_LCD_PCI          (H3XXX_EGPIO_BASE + 7)   /* Connects to PDWN on LCD controller */
#define H3800_EGPIO_VGH_ON           (H3XXX_EGPIO_BASE + 8)   /* Drives VGH on the LCD (+9??) */
#define H3800_EGPIO_VGL_ON           (H3XXX_EGPIO_BASE + 9)   /* Drivers VGL on the LCD (-6??) */
#define H3800_EGPIO_FL_PWR_ON        (H3XXX_EGPIO_BASE + 10)  /* Frontlight power on */
#define H3800_EGPIO_BT_PWR_ON        (H3XXX_EGPIO_BASE + 11)  /* Bluetooth power on */
#define H3800_EGPIO_SPK_ON           (H3XXX_EGPIO_BASE + 12)  /* Built-in speaker on */
#define H3800_EGPIO_EAR_ON_N         (H3XXX_EGPIO_BASE + 13)  /* Headphone jack on */
#define H3800_EGPIO_AUD_PWR_ON       (H3XXX_EGPIO_BASE + 14)  /* All audio power */
/*
 * helper for sa1100fb
 */
static struct gpio h3800_lcd_gpio[] = {
	{ H3800_EGPIO_LCD_ON,	GPIOF_OUT_INIT_LOW,	"LCD 3V power" },
	{ H3800_EGPIO_VGL_ON,	GPIOF_OUT_INIT_LOW,	"LCD VGL" },
	{ H3800_EGPIO_VGH_ON,   GPIOF_OUT_INIT_LOW,	"LCD VGH" },
	{ H3800_EGPIO_LCD_5V_ON,GPIOF_OUT_INIT_LOW,	"LCD 5V power" },
	{ H3800_EGPIO_LCD_PCI,  GPIOF_OUT_INIT_LOW,	"LCD PCI" },
};

static bool h3800_lcd_request(void)
{
	static bool h3800_lcd_ok;
	int rc;

	if (h3800_lcd_ok)
		return true;

	rc = gpio_request_array(h3800_lcd_gpio, ARRAY_SIZE(h3800_lcd_gpio));
	if (rc)
		pr_err("%s: can't request GPIOs\n", __func__);
	else
		h3800_lcd_ok = true;

	return h3800_lcd_ok;
}

static void h3800_lcd_power(int enable)
{
	if (!h3800_lcd_request())
		return;

	if (enable) {
		gpio_direction_output(H3800_EGPIO_LCD_ON, 1);
		mdelay(30);
		gpio_direction_output(H3800_EGPIO_VGL_ON, 1);
		mdelay(5);
		gpio_direction_output(H3800_EGPIO_VGH_ON, 1);
		mdelay(50);
		gpio_direction_output(H3800_EGPIO_LCD_5V_ON, 1);
		mdelay(5);
		mdelay(17);
		gpio_direction_output(H3800_EGPIO_LCD_PCI, 1);
	} else {
		gpio_direction_output(H3800_EGPIO_LCD_ON, 0);
		mdelay(30);
		gpio_direction_output(H3800_EGPIO_LCD_5V_ON, 0);
		mdelay(50);
		gpio_direction_output(H3800_EGPIO_VGL_ON, 0);
		mdelay(5);
		gpio_direction_output(H3800_EGPIO_VGH_ON, 0);
		mdelay(100);
		gpio_direction_output(H3800_EGPIO_LCD_PCI, 0);
	}
}

static const struct sa1100fb_rgb h3800_rgb_16 = {
	.red	= { .offset = 12, .length = 4, },
	.green	= { .offset = 7,  .length = 4, },
	.blue	= { .offset = 1,  .length = 4, },
	.transp	= { .offset = 0,  .length = 0, },
};

static struct sa1100fb_mach_info h3800_lcd_info = {
	.pixclock	= 174757, 	.bpp		= 16,
	.xres		= 320,		.yres		= 240,

	.hsync_len	= 3,		.vsync_len	= 3,
	.left_margin	= 12,		.upper_margin	= 10,
	.right_margin	= 17,		.lower_margin	= 1,

	.cmap_static	= 1,

	.lccr0		= LCCR0_Color | LCCR0_Sngl | LCCR0_Act,
	.lccr3		= LCCR3_OutEnH | LCCR3_PixRsEdg | LCCR3_ACBsDiv(2),

	.rgb[RGB_16] = &h3800_rgb_16,

	.lcd_power = h3800_lcd_power,
};

static void __init h3800_map_io(void)
{
	h3xxx_map_io();
}

static void __init h3800_mach_init(void)
{
	h3xxx_mach_init();
	sa11x0_register_lcd(&h3800_lcd_info);
}

MACHINE_START(H3800, "HP iPAQ H3800")
	.atag_offset	= 0x100,
	.map_io		= h3800_map_io,
	.nr_irqs	= SA1100_NR_IRQS,
	.init_irq	= sa1100_init_irq,
	.init_time	= sa1100_timer_init,
	.init_machine	= h3800_mach_init,
	.init_late	= sa11x0_init_late,
	.restart	= sa11x0_restart,
MACHINE_END
