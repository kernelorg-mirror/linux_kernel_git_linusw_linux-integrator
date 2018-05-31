// SPDX-License-Identifier: GPL-2.0
/*
 * 96boards Low-speed Connector driver
 * (C) 2018 Linus Walleij <linus.walleij@linaro.org>
 */
#include <linux/device.h>
#include <linux/init.h>
#include <linux/sysfs.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/i2c.h>
#include <linux/spi/spi.h>
#include <linux/idr.h>
#include "96boards-mezzanines.h"

static DEFINE_IDA(ls_device_ida);

static int ls_driver_probe(struct device *dev)
{
	struct ls_device *lsdev = to_ls_device(dev);
	struct ls_driver *lsdrv = to_ls_driver(dev->driver);

	if (lsdrv->probe)
		return lsdrv->probe(lsdev);
	return 0;
}

static int ls_driver_remove(struct device *dev)
{
	struct ls_device *lsdev = to_ls_device(dev);
	struct ls_driver *lsdrv = to_ls_driver(dev->driver);

	if (lsdrv->remove)
		lsdrv->remove(lsdev);
	return 0;
}

int ls_driver_register(struct ls_driver *lsdrv)
{
	lsdrv->drv.bus = &ls_bus_type;
	lsdrv->drv.probe = ls_driver_probe;
	lsdrv->drv.remove = ls_driver_remove;

	return driver_register(&lsdrv->drv);
}
EXPORT_SYMBOL_GPL(ls_driver_register);

void ls_driver_unregister(struct ls_driver *lsdrv)
{
	driver_unregister(&lsdrv->drv);
}
EXPORT_SYMBOL_GPL(ls_driver_unregister);

struct gpio_desc *ls_get_gpiod(struct ls_device *ls,
			       enum ls_gpio pin,
			       const char *consumer_name,
			       enum gpiod_flags flags)
{
	struct gpio_desc *retdesc;

	/*
	 * TODO: get all the LS GPIOs as an array on probe() then
	 * the consumers can skip error handling of IS_ERR() descriptors
	 * and this need only set the consumer name.
	 */
	retdesc = devm_gpiod_get_index(ls->dev.parent, NULL, pin, flags);
	if (!IS_ERR(retdesc) && consumer_name)
		gpiod_set_consumer_name(retdesc, consumer_name);

	return retdesc;
}
EXPORT_SYMBOL_GPL(ls_get_gpiod);

/*
 * Mezzanine boards will call this to orderly remove their claimed
 * gpio descriptors, since we acquired them all with devm_gpiod_get()
 * they will eventually be released once this connector device
 * disappears if the board do not release them in order.
 */
void ls_put_gpiod(struct ls_device *ls, struct gpio_desc *gpiod)
{
	devm_gpiod_put(ls->dev.parent, gpiod);
}
EXPORT_SYMBOL_GPL(ls_put_gpiod);

static int lscon_add_device(struct ls_connector *ls,
			    const char *name,
			    struct device_node *np)
{
	struct ls_device *lsdev;
	int ret;

	lsdev = kzalloc(sizeof(*lsdev), GFP_KERNEL);
	if (!lsdev)
		return -ENOMEM;

	lsdev->id = ida_simple_get(&ls_device_ida, 0, 0, GFP_KERNEL);
	if (lsdev->id < 0)
		return lsdev->id;
	lsdev->dev.bus = &ls_bus_type;
	lsdev->dev.of_node = np;
	lsdev->dev.parent = ls->dev;
	/* Propagate resources to the device */
	lsdev->i2c0 = ls->i2c0;
	lsdev->i2c1 = ls->i2c1;
	lsdev->spi = ls->spi;
	/*
	 * In /sys/bus/96boards-ls-connector-bus/devices/ we find
	 * mezzanine0, mezzanine1 etc OR the device name if inserted
	 * from userspace.
	 */
	if (name)
		dev_set_name(&lsdev->dev, "%s", name);
	else
		dev_set_name(&lsdev->dev, "mezzanine%d", lsdev->id);
	device_initialize(&lsdev->dev);
	ret = device_add(&lsdev->dev);
	if (ret) {
		dev_err(ls->dev, "failed to add device %s\n",
			dev_name(&lsdev->dev));
		return ret;
	}

	return 0;
}

static void lscon_del_device(struct ls_device *lsdev)
{
	device_del(&lsdev->dev);
	ida_simple_remove(&ls_device_ida, lsdev->id);
	kfree(lsdev);
}

struct ls_supported_buf {
	char *buf;
	size_t count;
};

static int ls_supported_print(struct device_driver *drv, void *data)
{
	struct ls_supported_buf *buf = data;
	size_t count;

	count = snprintf(buf->buf + buf->count,
			 PAGE_SIZE, "%s\n", drv->name);
	buf->count += count;
	return 0;
}

static ssize_t ls_supported_show(struct bus_type *bus, char *buf)
{
	struct ls_supported_buf sbuf;

	/* Loop over the driver list and show supported devices */
	sbuf.buf = buf;
	sbuf.count = 0;
	bus_for_each_drv(&ls_bus_type, NULL, &sbuf, ls_supported_print);

	return sbuf.count;
}

static BUS_ATTR(supported, 0444, ls_supported_show, NULL);

/*
 * Match the supplied string to a driver name, if we find
 * a match we return 1, saying this device is elegible for
 * insertion.
 */
static int ls_inject_match(struct device_driver *drv, void *data)
{
	const char *devname = data;

	if (!strcmp(devname, drv->name))
		return 1;
	return 0;
}

static ssize_t ls_inject_store(struct bus_type *bus,
			       const char *buf, size_t count)
{
	struct device *dev = ls_bus_type.dev_root;
	struct ls_connector *ls = dev_get_drvdata(dev);
	char *devname;
	int ret;

	devname = kstrdup(buf, GFP_KERNEL);
	devname = strstrip(devname);
	/* Look if we have a driver for this device */
	ret = bus_for_each_drv(&ls_bus_type, NULL, devname, ls_inject_match);
	if (!ret) {
		kfree(devname);
		return count;
	}

	dev_info(ls->dev, "create %s device\n", devname);
	/*
	 * No corresponding DT node
	 *
	 * TODO: when we have device tree overlays, this is a good
	 * place to start when inserting dynamic devices.
	 */
	lscon_add_device(ls, devname, NULL);

	kfree(devname);
	return count;
}

static BUS_ATTR(inject, 0644, NULL, ls_inject_store);

static ssize_t ls_eject_store(struct bus_type *bus,
			      const char *buf, size_t count)
{
	struct device *busdev = ls_bus_type.dev_root;
	struct ls_connector *ls = dev_get_drvdata(busdev);
	struct ls_device *lsdev;
	struct device *dev;
	char *devname;

	devname = kstrdup(buf, GFP_KERNEL);
	devname = strstrip(devname);
	/* Look if we have this device */
	dev = bus_find_device_by_name(&ls_bus_type, NULL, devname);
	if (!dev) {
		kfree(devname);
		return count;
	}

	dev_info(ls->dev, "destroy %s device\n", devname);

	lsdev = to_ls_device(dev);
	lscon_del_device(lsdev);
	kfree(devname);

	return count;
}

static BUS_ATTR(eject, 0644, NULL, ls_eject_store);

static int lscon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *child;
	struct spi_controller *spi;
	struct ls_connector *ls;
	int ret;

	ls = devm_kzalloc(dev, sizeof(*ls), GFP_KERNEL);
	if (!ls)
		return -ENOMEM;
	ls->dev = dev;

	/* Bridge I2C busses */
	child = of_parse_phandle(np, "i2c0", 0);
	if (!child) {
		dev_err(dev, "no i2c0 phandle\n");
		return -ENODEV;
	}
	ls->i2c0 = of_get_i2c_adapter_by_node(child);
	if (!ls->i2c0) {
		dev_err(dev, "no i2c0 adapter, deferring\n");
		return -EPROBE_DEFER;
	}

	child = of_parse_phandle(np, "i2c1", 0);
	if (!child) {
		dev_err(dev, "no i2c1 phandle\n");
		ret = -ENODEV;
		goto out_put_i2c0;
	}
	ls->i2c1 = of_get_i2c_adapter_by_node(child);
	if (!ls->i2c1) {
		dev_err(dev, "no i2c0 adapter, deferring\n");
		ret = -EPROBE_DEFER;
		goto out_put_i2c0;
	}

	/* Bridge SPI bus */
	child = of_parse_phandle(np, "spi", 0);
	if (!child) {
		dev_err(dev, "no spi phandle\n");
		ret = -ENODEV;
		goto out_put_i2c1;
	}
	spi = of_find_spi_controller_by_node(child);
	if (!spi) {
		dev_err(dev, "no spi controller, deferring\n");
		ret = -EPROBE_DEFER;
		goto out_put_i2c1;
	}
	ls->spi = spi_controller_get(spi);
	if (!ls->spi) {
		dev_err(dev, "no spi reference\n");
		ret = -ENODEV;
		goto out_put_i2c1;
	}

	platform_set_drvdata(pdev, ls);

	ls_bus_type.dev_root = dev;
	ret = bus_create_file(&ls_bus_type, &bus_attr_supported);
	if (ret)
		goto out_put_i2c1;
	ret = bus_create_file(&ls_bus_type, &bus_attr_inject);
	if (ret)
		goto out_put_i2c1;
	ret = bus_create_file(&ls_bus_type, &bus_attr_eject);
	if (ret)
		goto out_put_i2c1;

	/*
	 * Add mezzanine boards as children, stacking possible.
	 * All direct children of the LS connector will be considered
	 * mezzanines.
	 */
	for_each_available_child_of_node(np, child)
		lscon_add_device(ls, NULL, child);

	return 0;

out_put_i2c1:
	i2c_put_adapter(ls->i2c1);
out_put_i2c0:
	i2c_put_adapter(ls->i2c0);
	return ret;
}

static int lscon_del_dev(struct device *dev, void *data)
{
	struct ls_device *lsdev = to_ls_device(dev);

	lscon_del_device(lsdev);
	return 0;
}

static int lscon_remove(struct platform_device *pdev)
{
	struct ls_connector *ls = platform_get_drvdata(pdev);

	/* Make sure we remove any registered devices */
	bus_for_each_dev(&ls_bus_type, NULL, NULL, lscon_del_dev);

	ls_bus_type.dev_root = NULL;
	spi_controller_put(ls->spi);
	i2c_put_adapter(ls->i2c1);
	i2c_put_adapter(ls->i2c0);

	return 0;
}

static const struct of_device_id lscon_of_match[] = {
	{
		.compatible = "96boards,low-speed-connector",
	},
	{ },
};

static struct platform_driver lscon_driver = {
	.driver = {
		.name = "lscon",
		.of_match_table = of_match_ptr(lscon_of_match),
	},
	.probe  = lscon_probe,
	.remove = lscon_remove,
};
builtin_platform_driver(lscon_driver);
