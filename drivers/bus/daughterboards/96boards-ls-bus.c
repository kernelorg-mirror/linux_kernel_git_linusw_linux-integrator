// SPDX-License-Identifier: GPL-2.0
/*
 * 96boards Low-speed Connector bus initialization
 * (C) 2018 Linus Walleij <linus.walleij@linaro.org>
 */
#include <linux/device.h>
#include <linux/init.h>
#include <linux/of_platform.h>
#include "96boards-mezzanines.h"

static int ls_match(struct device *dev, struct device_driver *drv)
{
	/* First match on OF node */
	if (of_driver_match_device(dev, drv))
		return 1;

	/* Second match on name */
	return !strcmp(dev_name(dev), drv->name);
}

struct bus_type ls_bus_type = {
	.name = "96boards-ls-connector-bus",
	.match = ls_match,
};
EXPORT_SYMBOL_GPL(ls_bus_type);

static int __init ls_bus_init(void)
{
	int ret;

	/* Register the LS connector bus so devices can start to probe */
	ret = bus_register(&ls_bus_type);
	if (ret) {
		pr_err("could not register LS connector bus\n");
		return ret;
	}
	return 0;
}
postcore_initcall(ls_bus_init);
