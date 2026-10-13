// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom BCMBCA Processor Monitor Bus (PMB) transport for the third
 * generation Power Management Controller (PMC3).
 *
 * Children access keyhole 0 through a shared regmap.
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/log2.h>
#include <linux/mfd/bcmbca-pmb.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define PMB_CONFIG		0x00
#define PMB_REGS_PER_DEVICE	GENMASK(29, 20)
#define PMB_CONTROL		0x10
#define PMB_WRITE_DATA		0x14
#define PMB_MUTEX		0x18
#define PMB_READ_DATA		0x1c
#define PMB_START		BIT(31)
#define PMB_TIMEOUT		BIT(30)
#define PMB_SLAVE_ERROR		BIT(29)
#define PMB_BUSY			BIT(28)
#define PMB_WRITE		BIT(24)
#define PMB_BUS			GENMASK(21, 20)
#define PMB_LOCKED		BIT(0)

struct bcmbca_pmb {
	struct device *dev;
	void __iomem *base;
	/* Serializes regmap operations and recovery state. */
	struct mutex lock;
	struct delayed_work recovery;
	u32 regs_per_device;
	bool keyhole_acquired;
	bool recovering;
	bool stopping;
};

static void bcmbca_pmb_recover(struct work_struct *work)
{
	struct bcmbca_pmb *pmb = container_of(to_delayed_work(work),
					   struct bcmbca_pmb, recovery);

	guard(mutex)(&pmb->lock);
	if (!pmb->stopping) {
		if (readl(pmb->base + PMB_CONTROL) & PMB_BUSY) {
			queue_delayed_work(system_dfl_wq, &pmb->recovery,
					   msecs_to_jiffies(100));
		} else {
			writel(PMB_LOCKED, pmb->base + PMB_MUTEX);
			pmb->recovering = false;
		}
	}
}

static int bcmbca_pmb_acquire(struct bcmbca_pmb *pmb)
{
	u32 value;
	int ret;

	lockdep_assert_held(&pmb->lock);
	if (pmb->stopping)
		return -ENODEV;
	if (pmb->recovering)
		return -EBUSY;
	if (pmb->keyhole_acquired)
		return 0;

	/* Reading bit 0 acquires the hardware lock; writing 1 releases it. */
	ret = readl_poll_timeout(pmb->base + PMB_MUTEX, value,
				 value & PMB_LOCKED, 1, 1000);
	if (!ret)
		pmb->keyhole_acquired = true;
	return ret;
}

static int bcmbca_pmb_op(struct bcmbca_pmb *pmb, u8 bus, u8 device,
			 u16 offset, u32 *value, bool write)
{
	u32 status, command;
	int ret;

	lockdep_assert_held(&pmb->lock);
	if (bus > FIELD_MAX(PMB_BUS) || offset >= pmb->regs_per_device)
		return -EINVAL;

	ret = readl_poll_timeout(pmb->base + PMB_CONTROL, status,
				 !(status & PMB_BUSY), 1, 1000);
	if (ret)
		return ret;
	command = PMB_START | FIELD_PREP(PMB_BUS, bus) |
		  (device * pmb->regs_per_device) | offset;
	if (write) {
		writel(*value, pmb->base + PMB_WRITE_DATA);
		command |= PMB_WRITE;
	}
	writel(command, pmb->base + PMB_CONTROL);
	ret = readl_poll_timeout(pmb->base + PMB_CONTROL, status,
				 !(status & PMB_BUSY), 1, 1000);
	if (ret || status & PMB_TIMEOUT)
		return -ETIMEDOUT;
	if (status & PMB_SLAVE_ERROR)
		return -EIO;
	if (!write)
		*value = readl(pmb->base + PMB_READ_DATA);
	return 0;
}

static void bcmbca_pmb_regmap_lock(void *arg)
{
	struct bcmbca_pmb *pmb = arg;

	mutex_lock(&pmb->lock);
}

static void bcmbca_pmb_regmap_unlock(void *arg)
{
	struct bcmbca_pmb *pmb = arg;

	if (pmb->keyhole_acquired) {
		if (readl(pmb->base + PMB_CONTROL) & PMB_BUSY) {
			pmb->recovering = true;
			dev_err(pmb->dev, "PMB keyhole still busy; deferring unlock\n");
			queue_delayed_work(system_dfl_wq, &pmb->recovery,
					   msecs_to_jiffies(100));
		} else {
			writel(PMB_LOCKED, pmb->base + PMB_MUTEX);
		}
		pmb->keyhole_acquired = false;
	}
	mutex_unlock(&pmb->lock);
}

/* Acquire lazily so semaphore errors reach the caller of the regmap API. */
static int bcmbca_pmb_reg_read(void *context, unsigned int reg, unsigned int *value)
{
	struct bcmbca_pmb *pmb = context;
	int ret;

	ret = bcmbca_pmb_acquire(pmb);
	if (ret)
		return ret;
	return bcmbca_pmb_op(pmb, reg >> 24, (reg >> 16) & 0xff,
			     reg & 0xffff, value, false);
}

static int bcmbca_pmb_reg_write(void *context, unsigned int reg, unsigned int value)
{
	struct bcmbca_pmb *pmb = context;
	int ret;

	ret = bcmbca_pmb_acquire(pmb);
	if (ret)
		return ret;
	return bcmbca_pmb_op(pmb, reg >> 24, (reg >> 16) & 0xff,
			     reg & 0xffff, &value, true);
}

static bool bcmbca_pmb_precious_reg(struct device *dev, unsigned int reg)
{
	/* Do not let debugfs probe unimplemented or firmware-owned registers. */
	return true;
}

static struct regmap *bcmbca_pmb_regmap_init(struct bcmbca_pmb *pmb)
{
	const struct regmap_config config = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 1,
		.max_register = BCMBCA_PMB_REG(3, 255, 65535),
		.cache_type = REGCACHE_NONE,
		.precious_reg = bcmbca_pmb_precious_reg,
		.reg_read = bcmbca_pmb_reg_read,
		.reg_write = bcmbca_pmb_reg_write,
		.lock = bcmbca_pmb_regmap_lock,
		.unlock = bcmbca_pmb_regmap_unlock,
		.can_sleep = true,
		.lock_arg = pmb,
	};

	return devm_regmap_init(pmb->dev, NULL, pmb, &config);
}

/* Children are removed before this action, while MMIO is still mapped. */
static void bcmbca_pmb_stop(void *data)
{
	struct bcmbca_pmb *pmb = data;

	scoped_guard(mutex, &pmb->lock)
		pmb->stopping = true;
	cancel_delayed_work_sync(&pmb->recovery);

	if (pmb->recovering) {
		if (readl(pmb->base + PMB_CONTROL) & PMB_BUSY)
			dev_err(pmb->dev, "PMB keyhole remains busy; hardware reset required\n");
		else
			writel(PMB_LOCKED, pmb->base + PMB_MUTEX);
	}
}

static const struct mfd_cell bcm6846_pmb_cells[] = {
	{
		.name = "bcm6846-pmb-power",
		.of_compatible = "brcm,bcm6846-pmb-power",
	},
	{
		.name = "bcm6846-pmb-clock",
		.of_compatible = "brcm,bcm6846-pmb-clock",
	},
	{
		.name = "bcm6846-pmb-reset",
		.of_compatible = "brcm,bcm6846-pmb-reset",
	},
	{
		.name = "bcm6846-pmb-pvtmon",
		.of_compatible = "brcm,bcm6846-pmb-pvtmon",
	},
};

static int bcmbca_pmb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bcmbca_pmb *pmb;
	struct regmap *regmap;
	struct resource *res;
	int ret;

	pmb = devm_kzalloc(dev, sizeof(*pmb), GFP_KERNEL);
	if (!pmb)
		return -ENOMEM;
	pmb->dev = dev;
	pmb->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(pmb->base))
		return PTR_ERR(pmb->base);
	if (resource_size(res) < PMB_READ_DATA + sizeof(u32))
		return dev_err_probe(dev, -EINVAL, "PMB register range is too small\n");
	pmb->regs_per_device = FIELD_GET(PMB_REGS_PER_DEVICE,
					 readl(pmb->base + PMB_CONFIG));
	if (!is_power_of_2(pmb->regs_per_device))
		return dev_err_probe(dev, -EINVAL, "invalid PMB register stride\n");
	mutex_init(&pmb->lock);
	INIT_DELAYED_WORK(&pmb->recovery, bcmbca_pmb_recover);
	ret = devm_add_action_or_reset(dev, bcmbca_pmb_stop, pmb);
	if (ret)
		return ret;
	regmap = bcmbca_pmb_regmap_init(pmb);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "failed to register PMB regmap\n");
	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, bcm6846_pmb_cells,
				   ARRAY_SIZE(bcm6846_pmb_cells), NULL, 0, NULL);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register PMB functions\n");

	return 0;
}

static const struct of_device_id bcmbca_pmb_match[] = {
	{ .compatible = "brcm,bcm6846-pmb" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcmbca_pmb_match);

static struct platform_driver bcmbca_pmb_driver = {
	.probe = bcmbca_pmb_probe,
	.driver = {
		.name = "bcmbca-pmb",
		.of_match_table = bcmbca_pmb_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(bcmbca_pmb_driver);

MODULE_DESCRIPTION("Broadcom BCMBCA Processor Monitor Bus transport for PMC3");
MODULE_LICENSE("GPL");
