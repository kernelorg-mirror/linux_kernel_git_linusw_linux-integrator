// SPDX-License-Identifier: GPL-2.0
/*
 * 96boards Secure96 mezzanine board driver
 * (C) 2018 Linus Walleij <linus.walleij@linaro.org>
 */
#include <linux/gpio/consumer.h>
#include <linux/leds.h>
#include <linux/i2c.h>
#include <linux/spi/spi.h>
#include <linux/sizes.h>
#include <linux/platform_data/at24.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/module.h>

#include "96boards-mezzanines.h"

struct secure96 {
	struct device *dev;
	struct ls_device *ls;
	struct platform_device *leds_device;
	struct gpio_led *secure96_leds;
	struct i2c_client *eeprom;
	struct i2c_client *crypto;
	struct i2c_client *hash;
	struct gpio_desc *tpm_reset;
	struct gpio_desc *tpm_irq;
	struct spi_device *tpm;
};

struct secure96_ledinfo {
	enum ls_gpio pin;
	const char *ledname;
};

/*
 * GPIO-F, G, H and I are connected to LEDs, two red and two green
 */
static const struct secure96_ledinfo ledinfos[] = {
	{
		.pin = LS_GPIO_F,
		.ledname = "secure96:red:0",
	},
	{
		.pin = LS_GPIO_G,
		.ledname = "secure96:red:1",
	},
	{
		.pin = LS_GPIO_H,
		.ledname = "secure96:green:0",
	},
	{
		.pin = LS_GPIO_I,
		.ledname = "secure96:green:1",
	},
};

/*
 * The On Semiconductor CAT21M01 is 131072bits i.e. 16KB. This should be
 * mostly compatible to 24c128 so we register that with special pdata so
 * that we can fill in the GPIO descriptor for write protect.
 */
static struct at24_platform_data secure96_eeprom_pdata = {
	.byte_len = SZ_16K / 8,
	.page_size = 256,
	.flags = AT24_FLAG_ADDR16,
};

static const struct i2c_board_info secure96_eeprom = {
	I2C_BOARD_INFO("24c128", 0x50),
	.platform_data  = &secure96_eeprom_pdata,
};

/* Crypto chip */
static const struct i2c_board_info secure96_crypto = {
	I2C_BOARD_INFO("atecc508a", 0x60),
};

/* SHA hash chip */
static const struct i2c_board_info secure96_hash = {
	I2C_BOARD_INFO("atsha204a", 0x64),
};

/* Infineon SLB9670 TPM 2.0 chip */
static struct spi_board_info secure96_tpm = {
	.modalias = "tpm_tis_spi",
	/* The manual says 22.5MHz for 1.8V supply */
	.max_speed_hz = 22500000,
	.chip_select = 0,
};

int secure96_probe(struct ls_device *ls)
{
	struct device *dev = &ls->dev;
	struct secure96 *sec;
	struct gpio_desc *gpiod;
	struct gpio_led_platform_data secure96_leds_pdata;
	int ret;
	int i;

	sec = devm_kzalloc(dev, sizeof(*sec), GFP_KERNEL);
	if (!sec)
		return -ENOMEM;
	sec->dev = dev;
	sec->ls = ls;

	sec->secure96_leds = devm_kzalloc(dev,
			ARRAY_SIZE(ledinfos) * sizeof(*sec->secure96_leds),
			GFP_KERNEL);
	if (!sec->secure96_leds)
		return -ENOMEM;

	dev_info(dev, "populate secure96\n");

	/* Populate the four LEDs */
	for (i = 0; i < ARRAY_SIZE(ledinfos); i++) {
		const struct secure96_ledinfo *linfo;

		linfo = &ledinfos[i];

		gpiod = ls_get_gpiod(ls, linfo->pin, linfo->ledname,
				     GPIOD_OUT_LOW);
		if (IS_ERR(gpiod)) {
			dev_err(dev, "failed to get GPIO line %d\n",
				linfo->pin);
			return -ENODEV;
		}
		sec->secure96_leds[i].gpiod = gpiod;
		sec->secure96_leds[i].name = linfo->ledname;
		/* Heartbeat on first LED */
		if (i == 0)
			sec->secure96_leds[i].default_trigger = "heartbeat";
	}

	secure96_leds_pdata.num_leds = ARRAY_SIZE(ledinfos);
	secure96_leds_pdata.leds = sec->secure96_leds;

	sec->leds_device = platform_device_register_data(dev,
					"leds-gpio",
					PLATFORM_DEVID_AUTO,
					&secure96_leds_pdata,
					sizeof(secure96_leds_pdata));
	if (IS_ERR(sec->leds_device)) {
		dev_err(dev, "failed to populate LEDs device\n");
		return -ENODEV;
	}

	/* Populate the three I2C0 devices */
	gpiod = ls_get_gpiod(ls, LS_GPIO_B, "cat21m01-wp",
			     GPIOD_OUT_HIGH);
	if (IS_ERR(gpiod))
		dev_err(dev, "no CAT21M01 write-protect GPIO\n");
	else
		secure96_eeprom_pdata.wp_gpiod = gpiod;
	sec->eeprom = i2c_new_device(ls->i2c0, &secure96_eeprom);
	if (!sec->eeprom) {
		dev_err(dev, "failed to populate EEPROM\n");
		ret = -ENODEV;
		goto out_unreg_leds;
	}

	sec->crypto = i2c_new_device(ls->i2c0, &secure96_crypto);
	if (!sec->eeprom) {
		dev_err(dev, "failed to populate crypto device\n");
		ret = -ENODEV;
		goto out_remove_eeprom;
	}

	sec->hash = i2c_new_device(ls->i2c0, &secure96_hash);
	if (!sec->eeprom) {
		dev_err(dev, "failed to populate hash device\n");
		ret = -ENODEV;
		goto out_remove_crypto;
	}

	/* Populate the SPI TPM device */
	gpiod = ls_get_gpiod(ls, LS_GPIO_D,
			     "tpm-slb9670-rst",
			     GPIOD_OUT_LOW);
	if (IS_ERR(gpiod)) {
		dev_err(dev, "failed to get TPM RESET\n");
		ret = -ENODEV;
		goto out_remove_hash;
	}
	udelay(80);
	/* Deassert RST */
	gpiod_set_value(gpiod, 1);
	sec->tpm_reset = gpiod;

	gpiod = ls_get_gpiod(ls, LS_GPIO_C,
			     "tpm-slb9670-irq",
			     GPIOD_IN);
	if (IS_ERR(gpiod)) {
		dev_err(dev, "failed to get TPM IRQ GPIO\n");
		ret = -ENODEV;
		goto out_remove_tpm_reset;
	}
	sec->tpm_irq = gpiod;
	secure96_tpm.irq = gpiod_to_irq(gpiod);
	sec->tpm = spi_new_device(ls->spi, &secure96_tpm);
	if (!sec->tpm) {
		dev_err(dev, "failed to populate TPM device\n");
		ret = -ENODEV;
		goto out_remove_tpm_irq;
	}

	dev_set_drvdata(&ls->dev, sec);

	return 0;

out_remove_tpm_irq:
	ls_put_gpiod(ls, sec->tpm_irq);
out_remove_tpm_reset:
	ls_put_gpiod(ls, sec->tpm_reset);
out_remove_hash:
	i2c_unregister_device(sec->hash);
out_remove_crypto:
	i2c_unregister_device(sec->crypto);
out_remove_eeprom:
	i2c_unregister_device(sec->eeprom);
	if (secure96_eeprom_pdata.wp_gpiod)
		ls_put_gpiod(ls, secure96_eeprom_pdata.wp_gpiod);
out_unreg_leds:
	platform_device_unregister(sec->leds_device);
	for (i = 0; i < ARRAY_SIZE(ledinfos); i++)
		ls_put_gpiod(ls, sec->secure96_leds[i].gpiod);
	return ret;
}

static void secure96_remove(struct ls_device *ls)
{
	struct secure96 *sec = dev_get_drvdata(&ls->dev);
	int i;

	spi_unregister_device(sec->tpm);
	ls_put_gpiod(sec->ls, sec->tpm_irq);
	ls_put_gpiod(sec->ls, sec->tpm_reset);
	i2c_unregister_device(sec->hash);
	i2c_unregister_device(sec->crypto);
	i2c_unregister_device(sec->eeprom);
	if (secure96_eeprom_pdata.wp_gpiod)
		ls_put_gpiod(sec->ls, secure96_eeprom_pdata.wp_gpiod);
	platform_device_unregister(sec->leds_device);
	for (i = 0; i < ARRAY_SIZE(ledinfos); i++)
		ls_put_gpiod(sec->ls, sec->secure96_leds[i].gpiod);
	dev_set_drvdata(&ls->dev, NULL);
}

static const struct of_device_id secure96_of_match[] = {
	{
		.compatible = "96boards,secure96",
	},
	{},
};

struct ls_driver secure96_driver = {
	.drv = {
		.owner = THIS_MODULE,
		.name = "secure96",
		.of_match_table = of_match_ptr(secure96_of_match),
	},
	.probe = secure96_probe,
	.remove = secure96_remove,
};
module_driver(secure96_driver, ls_driver_register, ls_driver_unregister);
