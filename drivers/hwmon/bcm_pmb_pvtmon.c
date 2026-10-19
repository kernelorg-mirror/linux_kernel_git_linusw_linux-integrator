// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom BCM6846 process, voltage and temperature monitor (PVTMON).
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/hwmon.h>
#include <linux/mfd/bcmbca-pmb.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define PVTMON_BUS		1
#define PVTMON_DEVICE		6

/* PMB register offsets are in 32-bit words. */
#define PVTMON_CONFIG		17
#define PVTMON_RESET_N		BIT(0)
#define PVTMON_POWER_DOWN		BIT(1)

#define PVTMON_ACQ_CONFIG		20
#define PVTMON_ACCUM_ENABLE	BIT(0)
#define PVTMON_SKIP_LENGTH	GENMASK(11, 8)

#define PVTMON_CHANNEL_BASE	24
#define PVTMON_NUM_CHANNELS	8
#define PVTMON_VALUE		GENMASK(9, 0)
#define PVTMON_VALID		BIT(18)
#define PVTMON_SAMPLES		GENMASK(26, 24)
#define PVTMON_CHANNEL_ENABLE	BIT(31)

/* Voltage labels and divider ratios follow the BCM6846 vendor definitions. */
static const struct {
	const char *label;
	unsigned int divisor;
} bcm6846_pvtmon_voltages[] = {
	{ "V0.85_1", 10 },
	{ "V0.85_2", 10 },
	{ "VIN", 7 },
	{ "V1.0", 7 },
	{ "V1.8", 4 },
	{ "V3.3", 2 },
	{ "Vtest", 10 },
};

static int bcm_pmb_pvtmon_init(struct regmap *regmap,
			       struct reset_control *reset)
{
	unsigned int reg = BCMBCA_PMB_REG(PVTMON_BUS, PVTMON_DEVICE, PVTMON_CONFIG);
	u32 config, acq, channel;
	unsigned int i;
	int ret;

	ret = regmap_read(regmap, reg, &config);
	if (ret)
		return ret;

	/* Do not reset acquisition already running for the PMC firmware. */
	if ((config & (PVTMON_RESET_N | PVTMON_POWER_DOWN)) != PVTMON_RESET_N) {
		ret = regmap_update_bits(regmap, reg, PVTMON_POWER_DOWN, 0);
		if (ret)
			return ret;
		ret = reset_control_reset(reset);
		if (ret)
			return ret;
	}

	ret = regmap_read(regmap, BCMBCA_PMB_REG(PVTMON_BUS, PVTMON_DEVICE,
						 PVTMON_ACQ_CONFIG), &acq);
	if (ret)
		return ret;

	for (i = 0; i < PVTMON_NUM_CHANNELS; i++) {
		reg = BCMBCA_PMB_REG(PVTMON_BUS, PVTMON_DEVICE, PVTMON_CHANNEL_BASE + i);
		ret = regmap_read(regmap, reg, &channel);
		if (ret)
			return ret;

		if (channel & PVTMON_CHANNEL_ENABLE)
			continue;

		/* Average 32 samples, matching the vendor acquisition setup. */
		ret = regmap_update_bits(regmap, reg, PVTMON_CHANNEL_ENABLE | PVTMON_SAMPLES,
					 PVTMON_CHANNEL_ENABLE | FIELD_PREP(PVTMON_SAMPLES, 5));
		if (ret)
			return ret;
	}

	if (!(acq & PVTMON_ACCUM_ENABLE))
		return regmap_update_bits(regmap,
					  BCMBCA_PMB_REG(PVTMON_BUS, PVTMON_DEVICE,
							 PVTMON_ACQ_CONFIG),
					  PVTMON_SKIP_LENGTH | PVTMON_ACCUM_ENABLE,
					  PVTMON_ACCUM_ENABLE | FIELD_PREP(PVTMON_SKIP_LENGTH, 8));

	return 0;
}

static int bcm_pmb_pvtmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	struct regmap *map = dev_get_drvdata(dev);
	unsigned int index;
	u32 value;
	int ret;

	switch (type) {
	case hwmon_temp:
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;
		index = 0;
		break;
	case hwmon_in:
		if (attr != hwmon_in_input)
			return -EOPNOTSUPP;
		index = channel + 1;
		break;
	default:
		return -EOPNOTSUPP;
	}

	/* Release the PMB between samples so other clients can make progress. */
	ret = regmap_read_poll_timeout(map,
				       BCMBCA_PMB_REG(PVTMON_BUS, PVTMON_DEVICE,
						      PVTMON_CHANNEL_BASE + index),
				       value, value & PVTMON_VALID, 1000, 100000);
	if (ret)
		return ret;

	value = FIELD_GET(PVTMON_VALUE, value);
	if (type == hwmon_temp) {
		/* Signed arithmetic allows temperatures below zero Celsius. */
		*val = (41335000 - 49055 * (int)value) / 100;
	} else {
		/* 880 mV ADC reference, accounting for each input divider. */
		*val = 880 * value * 10 /
			(bcm6846_pvtmon_voltages[channel].divisor * 1024);
	}

	return 0;
}

static int bcm_pmb_pvtmon_read_string(struct device *dev,
				      enum hwmon_sensor_types type, u32 attr,
				      int channel, const char **str)
{
	if (type != hwmon_in || attr != hwmon_in_label)
		return -EOPNOTSUPP;

	*str = bcm6846_pvtmon_voltages[channel].label;
	return 0;
}

static const struct hwmon_ops bcm_pmb_pvtmon_ops = {
	.visible = 0444,
	.read = bcm_pmb_pvtmon_read,
	.read_string = bcm_pmb_pvtmon_read_string,
};

static const struct hwmon_channel_info * const bcm_pmb_pvtmon_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	NULL
};

static const struct hwmon_chip_info bcm_pmb_pvtmon_chip_info = {
	.ops = &bcm_pmb_pvtmon_ops,
	.info = bcm_pmb_pvtmon_info,
};

static int bcm_pmb_pvtmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap = dev_get_regmap(dev->parent, NULL);
	struct device *hwmon;
	struct reset_control *reset;
	struct clk *clk;
	int ret;

	if (!regmap)
		return -ENODEV;

	reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "failed to get PVTMON reset\n");

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "failed to enable PVTMON clock\n");

	ret = bcm_pmb_pvtmon_init(regmap, reset);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize PVTMON\n");

	hwmon = devm_hwmon_device_register_with_info(dev, "bcm6846_pvtmon", regmap,
						     &bcm_pmb_pvtmon_chip_info, NULL);
	if (IS_ERR(hwmon))
		return dev_err_probe(dev, PTR_ERR(hwmon), "failed to register hwmon device\n");

	/* Acquisition remains enabled for the PMC firmware after unbinding. */
	return 0;
}

static const struct of_device_id bcm_pmb_pvtmon_of_match[] = {
	{ .compatible = "brcm,bcm6846-pmb-pvtmon" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcm_pmb_pvtmon_of_match);

static struct platform_driver bcm_pmb_pvtmon_driver = {
	.probe = bcm_pmb_pvtmon_probe,
	.driver = {
		.name = "bcm6846-pmb-pvtmon",
		.of_match_table = bcm_pmb_pvtmon_of_match,
	},
};
module_platform_driver(bcm_pmb_pvtmon_driver);

MODULE_AUTHOR("Linus Walleij <linusw@kernel.org>");
MODULE_DESCRIPTION("Broadcom PMB PVTMON hardware monitoring driver");
MODULE_LICENSE("GPL");
