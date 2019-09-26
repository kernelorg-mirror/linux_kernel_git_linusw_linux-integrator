// SPDX-License-Identifier: GPL-2.0
/*
 * CPU idle driver for Cortina Systems Gemini SoCs
 * This SoC enters idle by switching to clock the whole system at
 * 32kHz.
 *
 * Author: Linus Walleij
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/cpuidle.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/bits.h>
#include <asm/cpuidle.h>

#define GEMINI_GLOBAL_CLOCK_CONTROL	0x34
#define GEMINI_GCC_MASK			GENMASK(31, 30)
#define GEMINI_GCC_G0_STATE		BIT(31)|BIT(30)
#define GEMINI_GCC_S1_STATE		BIT(30)
#define GEMINI_GCC_S3_S4_STATE		0x0

struct gemini_idle {
	struct device *dev;
	struct regmap *map;
};

/* Just one instance in the system by its nature */
static struct gemini_idle *gemini_idle_singleton;

static int gemini_enter_idle(struct cpuidle_device *dev,
			     struct cpuidle_driver *drv,
			     int index)
{
	struct gemini_idle *gi = gemini_idle_singleton;

	dev_info(gi->dev, "32 kHz\n");
	/* Gear down to 32kHz */
	regmap_update_bits(gi->map,
			   GEMINI_GLOBAL_CLOCK_CONTROL,
			   GEMINI_GCC_MASK,
			   GEMINI_GCC_S3_S4_STATE);
	/* Wait for interrupt */
	/* FIXME: Enabling interrupts here is racy! */
	raw_local_irq_enable();
	cpu_do_idle();
	raw_local_irq_disable();
	/* Gear back up */
	regmap_update_bits(gi->map,
			   GEMINI_GLOBAL_CLOCK_CONTROL,
			   GEMINI_GCC_MASK,
			   GEMINI_GCC_G0_STATE);

	return index;
}

static struct cpuidle_driver gemini_idle_driver = {
	.name			= "gemini_idle",
	.owner			= THIS_MODULE,
	.states = {
		{
			.enter			= gemini_enter_idle,
			.exit_latency		= 1,
			.target_residency	= 10000,
			.name			= "S4",
			.desc			= "S3/S4 32kHz clock",
		},
	},
	.safe_state_index = 0,
	.state_count = 1,
};

static int gemini_cpuidle_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct gemini_idle *gi;
	int ret;

	gi = devm_kzalloc(&pdev->dev, sizeof(*gi), GFP_KERNEL);
	if (!gi)
		return -ENOMEM;
	gi->dev = dev;
	gi->map = syscon_regmap_lookup_by_phandle(np, "regmap");
	if (!gi->map) {
		dev_err(dev, "could not find regmap\n");
		return -ENODEV;
	}
	gemini_idle_singleton = gi;
	ret = cpuidle_register(&gemini_idle_driver, NULL);
	if (ret) {
		dev_err(dev, "could not register driver\n");
		return ret;
	}

	dev_info(dev, "registered gemini cpuidle\n");
	return 0;
}

static int gemini_cpuidle_remove(struct platform_device *pdev)
{
	cpuidle_unregister(&gemini_idle_driver);
	return 0;
}

static const struct of_device_id gemini_idle_of_match[] = {
        { .compatible = "cortina,gemini-cpuidle" },
        {}
};

static struct platform_driver gemini_cpuidle_driver = {
	.probe = gemini_cpuidle_probe,
	.remove = gemini_cpuidle_remove,
	.driver = {
		.name = "gemini-cpuidle",
		.of_match_table = gemini_idle_of_match,
	},
};
builtin_platform_driver(gemini_cpuidle_driver);

MODULE_AUTHOR("Linus Walleij <linusw@kernel.org>");
MODULE_DESCRIPTION("Gemini CPU idle driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:gemini-cpuidle");
