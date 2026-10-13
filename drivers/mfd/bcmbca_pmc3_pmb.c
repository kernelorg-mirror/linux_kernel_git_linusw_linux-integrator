// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom BCMBCA Power Management Bus (PMB) transport for the third
 * generation Power Management Controller (PMC3).
 *
 * Children access keyhole 0 through a shared regmap.
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
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

/* Limit MMIO polling if the keyhole stays busy after a software timeout. */
#define PMB_RECOVERY_INTERVAL_MS	100

static const struct mfd_cell bcm6846_pmb_cells[] = {
	MFD_CELL_OF("bcm6846-pmb-power", NULL, NULL, 0, 0,
		    "brcm,bcm6846-pmb-power"),
	MFD_CELL_OF("bcm6846-pmb-clock", NULL, NULL, 0, 0,
		    "brcm,bcm6846-pmb-clock"),
	MFD_CELL_OF("bcm6846-pmb-reset", NULL, NULL, 0, 0,
		    "brcm,bcm6846-pmb-reset"),
	MFD_CELL_OF("bcm6846-pmb-pvtmon", NULL, NULL, 0, 0,
		    "brcm,bcm6846-pmb-pvtmon"),
};

struct bcmbca_pmb {
	struct device *dev;
	void __iomem *base;
	/* Serializes regmap operations and recovery state. */
	struct mutex lock;
	struct delayed_work recovery;
	u32 regs_per_device;
	/* The worker owns semaphore release while a transaction is still busy. */
	bool recovering;
};

static void bcmbca_pmb_recover(struct work_struct *work)
{
	struct bcmbca_pmb *pmb = container_of(to_delayed_work(work),
					   struct bcmbca_pmb, recovery);

	guard(mutex)(&pmb->lock);
	if (readl(pmb->base + PMB_CONTROL) & PMB_BUSY) {
		queue_delayed_work(system_dfl_wq, &pmb->recovery,
				   msecs_to_jiffies(PMB_RECOVERY_INTERVAL_MS));
		return;
	}

	writel(PMB_LOCKED, pmb->base + PMB_MUTEX);
	pmb->recovering = false;
}

static int bcmbca_pmb_acquire(struct bcmbca_pmb *pmb)
{
	u32 value;

	lockdep_assert_held(&pmb->lock);
	if (pmb->recovering)
		return -EBUSY;

	/* Reading bit 0 acquires the hardware lock; writing 1 releases it. */
	return readl_poll_timeout(pmb->base + PMB_MUTEX, value,
				  value & PMB_LOCKED, 1, 1000);
}

static void bcmbca_pmb_release(struct bcmbca_pmb *pmb)
{
	lockdep_assert_held(&pmb->lock);
	if (!(readl(pmb->base + PMB_CONTROL) & PMB_BUSY)) {
		writel(PMB_LOCKED, pmb->base + PMB_MUTEX);
		return;
	}

	/*
	 * A software timeout leaves the transaction running. Hold the lock
	 * until the worker sees it complete so nobody can reuse the keyhole.
	 */
	pmb->recovering = true;
	dev_err(pmb->dev, "PMB keyhole still busy; deferring unlock\n");
	queue_delayed_work(system_dfl_wq, &pmb->recovery,
			   msecs_to_jiffies(PMB_RECOVERY_INTERVAL_MS));
}

static int bcmbca_pmb_op(struct bcmbca_pmb *pmb, unsigned int reg,
			 u32 *value, bool write)
{
	u8 bus = reg >> 24;
	u8 device = reg >> 16;
	u16 offset = reg;
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

static int bcmbca_pmb_transfer(struct bcmbca_pmb *pmb, unsigned int reg,
			       u32 *value, bool write)
{
	int ret;

	guard(mutex)(&pmb->lock);
	ret = bcmbca_pmb_acquire(pmb);
	if (ret)
		return ret;

	ret = bcmbca_pmb_op(pmb, reg, value, write);
	bcmbca_pmb_release(pmb);

	return ret;
}

static int bcmbca_pmb_reg_read(void *context, unsigned int reg, unsigned int *value)
{
	return bcmbca_pmb_transfer(context, reg, value, false);
}

static int bcmbca_pmb_reg_write(void *context, unsigned int reg, unsigned int value)
{
	return bcmbca_pmb_transfer(context, reg, &value, true);
}

static int bcmbca_pmb_reg_update_bits(void *context, unsigned int reg,
				      unsigned int mask, unsigned int value)
{
	struct bcmbca_pmb *pmb = context;
	unsigned int tmp;
	int ret;

	/* Hold the hardware semaphore across both operations. */
	guard(mutex)(&pmb->lock);
	ret = bcmbca_pmb_acquire(pmb);
	if (ret)
		return ret;

	ret = bcmbca_pmb_op(pmb, reg, &tmp, false);
	if (!ret) {
		tmp = (tmp & ~mask) | (value & mask);
		ret = bcmbca_pmb_op(pmb, reg, &tmp, true);
	}

	bcmbca_pmb_release(pmb);

	return ret;
}

static const struct regmap_config bcmbca_pmb_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 1,
	.max_register = BCMBCA_PMB_REG(3, 255, 65535),
	.cache_type = REGCACHE_NONE,
	/* Callbacks serialize I/O and disable_locking suppresses debugfs scans. */
	.disable_locking = true,
	.reg_read = bcmbca_pmb_reg_read,
	.reg_write = bcmbca_pmb_reg_write,
	.reg_update_bits = bcmbca_pmb_reg_update_bits,
	.can_sleep = true,
};

/* Children are removed before this action, while MMIO is still mapped. */
static void bcmbca_pmb_stop(void *data)
{
	struct bcmbca_pmb *pmb = data;

	disable_delayed_work_sync(&pmb->recovery);

	guard(mutex)(&pmb->lock);
	if (!pmb->recovering)
		return;

	if (readl(pmb->base + PMB_CONTROL) & PMB_BUSY) {
		dev_err(pmb->dev, "PMB keyhole remains busy; hardware reset required\n");
		return;
	}

	writel(PMB_LOCKED, pmb->base + PMB_MUTEX);
}

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

	ret = devm_mutex_init(dev, &pmb->lock);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&pmb->recovery, bcmbca_pmb_recover);
	ret = devm_add_action_or_reset(dev, bcmbca_pmb_stop, pmb);
	if (ret)
		return ret;

	regmap = devm_regmap_init(dev, NULL, pmb, &bcmbca_pmb_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "failed to register PMB regmap\n");

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, bcm6846_pmb_cells,
				   ARRAY_SIZE(bcm6846_pmb_cells), NULL, 0, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register PMB functions\n");

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

MODULE_DESCRIPTION("Broadcom BCMBCA Power Management Bus transport for PMC3");
MODULE_LICENSE("GPL");
