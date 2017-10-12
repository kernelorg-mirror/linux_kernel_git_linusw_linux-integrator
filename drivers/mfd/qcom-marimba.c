/*
 * Core driver for Qualcomm's Marimba and Timpani mixed signal
 * chips. These chips have names like WCN2243 and contain a
 * Tavarua FM radio and other things.
 *
 * Copyright (C) 2017 Linus Walleij <linus.walleij@linaro.org>
 *
 * License terms: GNU General Public License (GPL) version 2
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/clk.h>

/* Registers inside Marimba/Timpani */
#define MARIMBA_VERSION		0x00U

#define MARIMBA_NUM_SUPPLIES 6
static const char *marimba_supply_names[MARIMBA_NUM_SUPPLIES] = {
	"vdd-io",
	"vdd-xo-bias",
	"vreg-dig",
	"vreg-ana",
	"vdd-ldo-in",
	"vdd-dig-1p3",
};

struct marimba {
	struct i2c_client	*client;
	struct regmap		*map;
	struct regulator_bulk_data supplies[MARIMBA_NUM_SUPPLIES];
	struct gpio_desc	*reset;
	struct clk		*sys_clk;
};

/*
 * MFD cells.
 */
static struct mfd_cell marimba_cells[] = {
	{
		.of_compatible = "qcom,tavarua",
		.name = "qcom-tavarua-fm-radio",
		.id = -1,
	},
};

static const struct regmap_config marimba_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int marimba_probe(struct i2c_client *client)
{
	struct marimba *marimba;
	struct device *dev = &client->dev;
	unsigned int val;
	int ret;
	int i;

	marimba = devm_kzalloc(dev, sizeof(*marimba), GFP_KERNEL);
	if (!marimba)
		return -ENOMEM;

	/* Enable regulators */
	for (i = 0; i < ARRAY_SIZE(marimba->supplies); i++)
		marimba->supplies[i].supply = marimba_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(marimba->supplies),
				      marimba->supplies);
	if (ret) {
		dev_err(dev, "failed to request supplies: %d\n", ret);
		return ret;
	}
	ret = regulator_bulk_enable(ARRAY_SIZE(marimba->supplies),
				    marimba->supplies);
	if (ret) {
		dev_err(dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}

	marimba->sys_clk = devm_clk_get(dev, "SYS_CLK");
	if (IS_ERR(marimba->sys_clk)) {
		ret = PTR_ERR(marimba->sys_clk);
		dev_err(dev, "could not retrieve SYS_CLK: %d\n", ret);
		goto err_dis_reg;
	}
	ret = clk_prepare_enable(marimba->sys_clk);
	if (ret) {
		dev_err(dev, "could not activate SYS_CLK: %d\n", ret);
		goto err_dis_reg;
	}

	marimba->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(marimba->reset)) {
		ret = PTR_ERR(marimba->reset);
		dev_err(dev, "could not get reset gpio: %d", ret);
		goto err_dis_clk;
	}
	usleep_range(1000,1050);
	gpiod_set_value(marimba->reset, 0);
	dev_info(dev, "deasserted reset\n");
	usleep_range(1000,1050);

	i2c_set_clientdata(client, marimba);
	marimba->client = client;
	marimba->map = devm_regmap_init_i2c(client, &marimba_regmap_config);
	if (IS_ERR(marimba->map)) {
		ret = PTR_ERR(marimba->map);
		dev_err(dev, "Failed to allocate register map: %d\n",
			ret);
		goto err_dis_clk;
	}

	ret = regmap_read(marimba->map, MARIMBA_VERSION, &val);
	if (ret) {
		dev_err(dev, "failed to read version ID: %d retry\n", ret);
		ret = regmap_read(marimba->map, MARIMBA_VERSION, &val);
		if (ret) {
			dev_err(dev, "failed to read version ID: %d again \n", ret);
			goto err_dis_clk;
		}
	}
	val &= 0x1FU;
	dev_info(dev, "marimba version %02x\n", val);

	ret = devm_mfd_add_devices(dev, 0, marimba_cells,
				   ARRAY_SIZE(marimba_cells), NULL, 0, NULL);
	if (ret)
		goto err_dis_clk;

	dev_info(dev, "initialized Marimba device\n");

	return ret;

err_dis_clk:
	clk_disable_unprepare(marimba->sys_clk);
err_dis_reg:
	regulator_bulk_disable(ARRAY_SIZE(marimba->supplies),
			       marimba->supplies);
	return ret;
}

static int marimba_remove(struct i2c_client *client)
{
	struct marimba *marimba = i2c_get_clientdata(client);

	clk_disable_unprepare(marimba->sys_clk);
	regulator_bulk_disable(ARRAY_SIZE(marimba->supplies),
			       marimba->supplies);
	return 0;
}

static const struct of_device_id marimba_match[] = {
	{ .compatible = "qcom,wcn2243", },
	{ .compatible = "qcom,marimba", },
	{ .compatible = "qcom,timpani", },
	{ },
};
MODULE_DEVICE_TABLE(of, marimba_match);

static struct i2c_driver marimba_driver = {
	.driver = {
		.name = "marimba",
		.of_match_table = marimba_match,
	},
	.probe_new = marimba_probe,
	.remove = marimba_remove,
};
module_i2c_driver(marimba_driver);

MODULE_AUTHOR("Linus Walleij");
MODULE_DESCRIPTION("Qualcomm Marimba/Timpani driver");
MODULE_LICENSE("GPL v2");
