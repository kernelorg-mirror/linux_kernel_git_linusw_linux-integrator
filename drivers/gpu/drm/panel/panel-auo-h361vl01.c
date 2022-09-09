// SPDX-License-Identifier: GPL-2.0-only
// AUO H361VL01 LCD 480x800 WVGA LVDS drm_panel driver.
// Copyright (C) 2019 Linus Walleij <linus.walleij@linaro.org>
// Based on drivers/video/msm/lcdc_auo_wvga.c in the vendor tree

#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <linux/backlight.h>
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#define PANEL_CMD_BACKLIGHT_LEVEL		0x6A18
#define PANEL_CMD_FORMAT			0x3A00
#define PANEL_CMD_RGBCTRL			0x3B00
#define PANEL_CMD_BCTRL				0x5300
#define PANEL_CMD_PWM_EN			0x6A17

#define PANEL_CMD_SLEEP_OUT			0x1100
#define PANEL_CMD_DISP_ON			0x2900
#define PANEL_CMD_DISP_OFF			0x2800
#define PANEL_CMD_SLEEP_IN			0x1000

struct h361vl01 {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct regulator *supply;
	struct videomode vm;
};

static inline struct h361vl01 *panel_to_h361vl01(struct drm_panel *panel)
{
	return container_of(panel, struct h361vl01, panel);
}

static int h361vl01_read_byte_cmd(struct h361vl01 *auop, u16 cmd)
{
	struct spi_device *spi = to_spi_device(auop->dev);
	u8 data[5];
	u8 val[1];
	int ret;

	data[0] = 0x20;
	data[1] = cmd >> 8;
	data[2] = 0;
	data[3] = cmd & 0xff;
	data[4] = 0xc0;
	ret = spi_write_then_read(spi, data, 5, val, 1);
	if (ret)
		return ret;
	dev_info(auop->dev, "read 0x%02x\n", val[0]);

	return 0;
}

static int h361vl01_write_cmd(struct h361vl01 *auop, u16 cmd)
{
	struct spi_device *spi = to_spi_device(auop->dev);
	u8 data[4];

	data[0] = 0x20;
	data[1] = cmd >> 8;
	data[2] = 0;
	data[3] = cmd & 0xff;
	return spi_write_then_read(spi, data, 4, NULL, 0);
}

static int h361vl01_write_cmd_param(struct h361vl01 *auop, u16 cmd, u8 p)
{
	struct spi_device *spi = to_spi_device(auop->dev);
	u8 data[6];

	data[0] = 0x20;
	data[1] = cmd >> 8;
	data[2] = 0;
	data[3] = cmd & 0xff;
	data[4] = 0x40;
	data[5] = p;
	return spi_write_then_read(spi, data, 6, NULL, 0);
}

static int h361vl01_disable(struct drm_panel *panel)
{
	struct h361vl01 *auop = panel_to_h361vl01(panel);

	dev_info(auop->dev, "called %s\n", __func__);
	backlight_disable(auop->backlight);

	return 0;
}

static int h361vl01_unprepare(struct drm_panel *panel)
{
	struct h361vl01 *auop = panel_to_h361vl01(panel);

	dev_info(auop->dev, "called %s\n", __func__);

	/* 0x2800: Display Off */
	h361vl01_write_cmd(auop, PANEL_CMD_DISP_OFF);
	msleep(120);

	/* 0x1000: Sleep In */
	h361vl01_write_cmd(auop, PANEL_CMD_SLEEP_IN);

	msleep(120);

	regulator_disable(auop->supply);

	return 0;
}

static int h361vl01_prepare(struct drm_panel *panel)
{
	struct h361vl01 *auop = panel_to_h361vl01(panel);
	int ret;

	dev_info(auop->dev, "called %s\n", __func__);

	/* Power on */
	ret = regulator_enable(auop->supply);
	if (ret) {
		dev_err(auop->dev, "failed to enable regulator\n");
		return ret;
	}

	/* 0x1100: Sleep Out */
	h361vl01_write_cmd(auop, PANEL_CMD_SLEEP_OUT);

	msleep(180);

	/* SET_PIXEL_FORMAT: Set how many bits per pixel are used (3A00h)*/
	h361vl01_write_cmd_param(auop, PANEL_CMD_FORMAT, 0x66); /* 18 bits */

	/* RGBCTRL: RGB Interface Signal Control (3B00h) */
	h361vl01_write_cmd_param(auop, PANEL_CMD_RGBCTRL, 0x2B);

	/* Display ON command */
	h361vl01_write_cmd(auop, PANEL_CMD_DISP_ON);
	msleep(20);

	/* Backlight on */
	h361vl01_write_cmd_param(auop, PANEL_CMD_BCTRL, 0x24); /* BCTRL, BL */
	/* Enable PWM Level */
	h361vl01_write_cmd_param(auop, PANEL_CMD_PWM_EN, 0x01);

	msleep(20);

	dev_info(auop->dev, "done %s\n", __func__);

	return ret;
}

static int h361vl01_enable(struct drm_panel *panel)
{
	struct h361vl01 *auop = panel_to_h361vl01(panel);

	dev_info(auop->dev, "called %s\n", __func__);
	backlight_enable(auop->backlight);

	return 0;
}

/* RGB mode */
static const struct drm_display_mode h361vl01_480x800_mode = {
	.clock = 25600,
	.hdisplay = 480,
	.hsync_start = 480 + 14,
	.hsync_end = 480 + 14 + 2,
	.htotal = 480 + 14 + 2 + 16,
	.vdisplay = 800,
	.vsync_start = 800 + 1,
	.vsync_end = 800 + 1 + 2,
	.vtotal = 800 + 1 + 2 + 28,
	.flags = 0,
};

static int h361vl01_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	/*
	 * This visible area size measured with a ruler, interestingly one
	 * millimeter corresponds to exactly 10 pixels.
	 */
	connector->display_info.width_mm = 48;
	connector->display_info.height_mm = 80;

	mode = drm_mode_duplicate(connector->dev, &h361vl01_480x800_mode);
	if (!mode) {
		dev_err(panel->dev, "bad mode or failed to add mode\n");
		return -EINVAL;
	}
	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	mode->flags |= DRM_MODE_FLAG_NHSYNC;
	mode->flags |= DRM_MODE_FLAG_NVSYNC;

	mode->width_mm = connector->display_info.width_mm;
	mode->height_mm = connector->display_info.height_mm;
	drm_mode_probed_add(connector, mode);

	return 1; /* Number of modes */
}

static int h361vl01_bl_get_brightness(struct backlight_device *bl)
{
	// struct h361vl01 *auop = bl_get_data(bl);
	u16 brightness = bl->props.brightness;

	/* TODO: actually read it out, if we can */

	return brightness & 0xff;
}

static int h361vl01_bl_update_status(struct backlight_device *bl)
{
	struct h361vl01 *auop = bl_get_data(bl);
	u8 brightness = bl->props.brightness;
	int ret;

	if (brightness > 15) {
		dev_err(auop->dev, "too high brightness\n");
		brightness = 15;
	}
	dev_info(auop->dev, "set brightness to %d\n", brightness);
	h361vl01_write_cmd_param(auop, PANEL_CMD_BACKLIGHT_LEVEL,
				 brightness);

	if (brightness == 0) {
		dev_info(auop->dev, "display off\n");
		/* 0x2800: Display Off */
		h361vl01_write_cmd(auop, PANEL_CMD_DISP_OFF);
		msleep(120);
		/* 0x1000: Sleep In */
		h361vl01_write_cmd(auop, PANEL_CMD_SLEEP_IN);
		msleep(120);
	}
	dev_info(auop->dev, "read some bytes\n");
	ret = h361vl01_read_byte_cmd(auop, PANEL_CMD_BACKLIGHT_LEVEL);
	if (ret)
		dev_info(auop->dev, "failed\n");

	return 0;
}

static const struct backlight_ops h361vl01_bl_ops = {
	.update_status = h361vl01_bl_update_status,
	.get_brightness = h361vl01_bl_get_brightness,
};

static const struct drm_panel_funcs h361vl01_drm_funcs = {
	.disable = h361vl01_disable,
	.unprepare = h361vl01_unprepare,
	.prepare = h361vl01_prepare,
	.enable = h361vl01_enable,
	.get_modes = h361vl01_get_modes,
};

static int h361vl01_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct h361vl01 *auop;
	struct backlight_properties props;
	int ret;

	dev_info(dev, "%s\n", __func__);
	auop = devm_kzalloc(dev, sizeof(*auop), GFP_KERNEL);
	if (!auop)
		return -ENOMEM;

	spi_set_drvdata(spi, auop);
	auop->dev = dev;

	auop->supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(auop->supply))
		return PTR_ERR(auop->supply);

	spi->bits_per_word = 16;
	//spi->bits_per_word = 32;
	//spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(dev, "spi setup failed.\n");
		return ret;
	}

	props.type = BACKLIGHT_RAW;
	props.brightness = 15;
	props.max_brightness = 15;
	auop->backlight = devm_backlight_device_register(dev,
					dev_name(dev), dev, auop,
					&h361vl01_bl_ops, &props);
	if (IS_ERR(auop->backlight)) {
		dev_err(dev, "failed to register backlight\n");
		return PTR_ERR(auop->backlight);
	}

	drm_panel_init(&auop->panel, dev, &h361vl01_drm_funcs,
		       DRM_MODE_CONNECTOR_LVDS);

	drm_panel_add(&auop->panel);

	return 0;
}

static void h361vl01_remove(struct spi_device *spi)
{
	struct h361vl01 *auop = spi_get_drvdata(spi);

	drm_panel_remove(&auop->panel);
}

static const struct of_device_id h361vl01_of_match[] = {
	{ .compatible = "auo,h361vl01", },
	{ }
};
MODULE_DEVICE_TABLE(of, h361vl01_of_match);

static const struct spi_device_id h361vl01_spi_ids[] = {
	{ "h361vl01" },
	{ },
};
MODULE_DEVICE_TABLE(spi, h361vl01_spi_ids);

static struct spi_driver h361vl01_driver = {
	.probe = h361vl01_probe,
	.remove = h361vl01_remove,
	.id_table = h361vl01_spi_ids,
	.driver = {
		.name = "panel-auo-h361vl01",
		.of_match_table = h361vl01_of_match,
	},
};
module_spi_driver(h361vl01_driver);

MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("AUO H361VL01 WVGA LCD panel driver");
MODULE_LICENSE("GPL v2");
