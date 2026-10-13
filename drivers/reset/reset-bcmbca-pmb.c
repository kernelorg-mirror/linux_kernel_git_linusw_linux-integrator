// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BCMBCA block resets through the Processor Monitor Bus (PMB) of the
 * third generation Platform Management Controller (PMC3).
 *
 * SoC match data maps reset IDs to groups of blocks and their sequences.
 * XRDP and its Runner cores share SRAM Buffer Pool Manager (SBPM) state
 * and must be reset together, preserving their bus interfaces.
 * PCIe uses soft-reset pulses; PVTMON resets analog acquisition.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reset-controller.h>
#include <linux/soc/bcm/bcmbca-pmb.h>

#include <dt-bindings/reset/brcm,bcm6846-pmb.h>

struct bcmbca_pmb_reset_block {
	u8 bus;
	u8 device;
	u16 offset; /* PMB word offset of the reset register. */
	u32 mask;
	u32 assert_value;
	u32 deassert_value;
};

struct bcmbca_pmb_reset;

struct bcmbca_pmb_reset_group {
	const char *name;
	int (*reset)(struct bcmbca_pmb_reset *rst,
		     const struct bcmbca_pmb_reset_group *group);
	const struct bcmbca_pmb_reset_block *blocks;
	unsigned int num_blocks;
	unsigned int assert_us;
};

struct bcmbca_pmb_reset_variant {
	const struct bcmbca_pmb_reset_group *groups;
	unsigned int num_groups;
};

struct bcmbca_pmb_reset {
	struct reset_controller_dev rcdev;
	const struct bcmbca_pmb_reset_variant *variant;
	struct bcmbca_pmb *pmb;
};

static int bcmbca_pmb_reset_sequence(struct bcmbca_pmb_reset *rst,
				     const struct bcmbca_pmb_reset_group *group);

static const struct bcmbca_pmb_reset_block bcm6846_xrdp_resets[] = {
	/* Keep the XRDP bus interfaces out of reset. */
	{
		.bus = 1,
		.device = 12,
		.offset = 5,
		.mask = GENMASK(27, 0),
		.assert_value = GENMASK(2, 0),
		.deassert_value = GENMASK(27, 0),
	},
	{
		.bus = 1,
		.device = 14,
		.offset = 5,
		.mask = GENMASK(27, 0),
		.deassert_value = GENMASK(27, 0),
	},
	{
		.bus = 1,
		.device = 15,
		.offset = 5,
		.mask = GENMASK(27, 0),
		.deassert_value = GENMASK(27, 0),
	},
	{
		.bus = 1,
		.device = 16,
		.offset = 5,
		.mask = GENMASK(27, 0),
		.deassert_value = GENMASK(27, 0),
	},
};

/* Bits 6 and 7 select the lane configuration and must remain unchanged. */
static const struct bcmbca_pmb_reset_block bcm6846_pcie_resets[] = {
	{
		.bus = 0,
		.device = 18,
		.offset = 5,
		.mask = GENMASK(5, 0),
		.assert_value = GENMASK(5, 0),
	},
	{
		.bus = 0,
		.device = 19,
		.offset = 5,
		.mask = GENMASK(5, 0),
		.assert_value = GENMASK(5, 0),
	},
};

/* Analog acquisition reset, independent of clock and power configuration. */
static const struct bcmbca_pmb_reset_block bcm6846_pvtmon_reset = {
	.bus = 1,
	.device = 6,
	.offset = 17,
	.mask = BIT(0),
	.deassert_value = BIT(0),
};

static const struct bcmbca_pmb_reset_group bcm6846_pmb_reset_groups[] = {
	[BCM6846_RESET_XRDP] = {
		.name = "XRDP",
		.reset = bcmbca_pmb_reset_sequence,
		.blocks = bcm6846_xrdp_resets,
		.num_blocks = ARRAY_SIZE(bcm6846_xrdp_resets),
		.assert_us = 10,
	},
	[BCM6846_RESET_PCIE0] = {
		.name = "PCIe0",
		.reset = bcmbca_pmb_reset_sequence,
		.blocks = &bcm6846_pcie_resets[0],
		.num_blocks = 1,
		.assert_us = 10000,
	},
	[BCM6846_RESET_PCIE1] = {
		.name = "PCIe1",
		.reset = bcmbca_pmb_reset_sequence,
		.blocks = &bcm6846_pcie_resets[1],
		.num_blocks = 1,
		.assert_us = 10000,
	},
	[BCM6846_RESET_PVTMON] = {
		.name = "PVTMON",
		.reset = bcmbca_pmb_reset_sequence,
		.blocks = &bcm6846_pvtmon_reset,
		.num_blocks = 1,
		.assert_us = 1000,
	},
};

static const struct bcmbca_pmb_reset_variant bcm6846_pmb_variant = {
	.groups = bcm6846_pmb_reset_groups,
	.num_groups = ARRAY_SIZE(bcm6846_pmb_reset_groups),
};

/* Best effort with both locks held; never restart a partially reset group. */
static void bcmbca_pmb_reset_cleanup(struct bcmbca_pmb_reset *rst,
				     const struct bcmbca_pmb_reset_group *group)
{
	const struct bcmbca_pmb_reset_block *block;
	unsigned int i;
	u32 value;
	int ret;

	for (i = 0; i < group->num_blocks; i++) {
		block = &group->blocks[i];
		ret = bcmbca_pmb_read(rst->pmb, block->bus, block->device,
				      block->offset, &value);
		if (!ret) {
			value &= ~block->mask;
			value |= block->assert_value;
			ret = bcmbca_pmb_write(rst->pmb, block->bus, block->device,
					       block->offset, value);
		}
		if (ret)
			dev_err(rst->rcdev.dev,
				"failed to hold PMB bus %u device %u in reset: %d\n",
				block->bus, block->device, ret);
	}
}

/* Called with both PMB locks held, including during error cleanup. */
static int bcmbca_pmb_reset_sequence(struct bcmbca_pmb_reset *rst,
				     const struct bcmbca_pmb_reset_group *group)
{
	const struct bcmbca_pmb_reset_block *block;
	unsigned int i;
	u32 value;
	int ret, pass;

	if (!group->num_blocks)
		return -EINVAL;

	/* Assert every block before releasing any of them. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < group->num_blocks; i++) {
			block = &group->blocks[i];
			ret = bcmbca_pmb_read(rst->pmb, block->bus, block->device,
					      block->offset, &value);
			if (ret)
				goto release;
			value &= ~block->mask;
			value |= pass ? block->deassert_value : block->assert_value;
			ret = bcmbca_pmb_write(rst->pmb, block->bus, block->device,
					       block->offset, value);
			if (ret)
				goto release;
		}
		if (!pass && group->assert_us)
			usleep_range(group->assert_us, group->assert_us * 2);
	}
release:
	if (ret) {
		dev_err(rst->rcdev.dev, "%s %s failed at PMB bus %u device %u: %d\n",
			group->name, pass ? "release" : "assert",
			block->bus, block->device, ret);
		bcmbca_pmb_reset_cleanup(rst, group);
	}
	return ret;
}

static int bcmbca_pmb_reset(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct bcmbca_pmb_reset *rst = container_of(rcdev, struct bcmbca_pmb_reset, rcdev);
	const struct bcmbca_pmb_reset_group *group;
	int ret;

	if (id >= rst->variant->num_groups)
		return -EINVAL;
	group = &rst->variant->groups[id];
	if (!group->reset)
		return -EINVAL;

	ret = bcmbca_pmb_lock(rst->pmb);
	if (ret)
		return ret;
	ret = group->reset(rst, group);
	bcmbca_pmb_unlock(rst->pmb);
	return ret;
}

static const struct reset_control_ops bcmbca_pmb_reset_ops = {
	.reset = bcmbca_pmb_reset,
};

static int bcmbca_pmb_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bcmbca_pmb_reset *rst;

	rst = devm_kzalloc(dev, sizeof(*rst), GFP_KERNEL);
	if (!rst)
		return -ENOMEM;
	rst->variant = device_get_match_data(dev);
	if (!rst->variant)
		return -EINVAL;
	rst->pmb = bcmbca_pmb_get(dev);
	if (IS_ERR(rst->pmb))
		return dev_err_probe(dev, PTR_ERR(rst->pmb), "cannot get PMB transport\n");
	rst->rcdev.owner = THIS_MODULE;
	rst->rcdev.dev = dev;
	rst->rcdev.ops = &bcmbca_pmb_reset_ops;
	rst->rcdev.of_node = dev->of_node;
	rst->rcdev.of_reset_n_cells = 1;
	rst->rcdev.nr_resets = rst->variant->num_groups;
	return devm_reset_controller_register(dev, &rst->rcdev);
}

static const struct of_device_id bcmbca_pmb_reset_match[] = {
	{ .compatible = "brcm,bcm6846-pmb-reset", .data = &bcm6846_pmb_variant },
	{ }
};
MODULE_DEVICE_TABLE(of, bcmbca_pmb_reset_match);

static struct platform_driver bcmbca_pmb_reset_driver = {
	.probe = bcmbca_pmb_reset_probe,
	.driver = {
		.name = "bcmbca-pmb-reset",
		.of_match_table = bcmbca_pmb_reset_match,
	},
};
module_platform_driver(bcmbca_pmb_reset_driver);

MODULE_DESCRIPTION("Broadcom BCMBCA PMB reset controller");
MODULE_LICENSE("GPL");
