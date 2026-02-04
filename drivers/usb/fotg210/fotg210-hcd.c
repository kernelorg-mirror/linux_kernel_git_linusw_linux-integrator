// SPDX-License-Identifier: GPL-2.0+
/* Faraday FOTG210 EHCI driver */
#include <linux/bits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/io.h>

#include "../host/ehci.h"
#include "fotg210.h"

struct fotg210_hcd {
	bool test; // FIXME delete
};

static struct hc_driver __read_mostly fotg210_hc_driver;

static const struct ehci_driver_overrides fotg210_hc_overrides __initconst = {
	.extra_priv_size = sizeof(struct fotg210_hcd),
};

#if 0
static const struct hc_driver fotg210_fotg210_hc_driver = {
	.description		= "fotg210_hcd",
	.product_desc		= "Faraday USB2.0 Host Controller",
	.hcd_priv_size		= sizeof(struct ehci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq			= ehci_irq,
	.flags			= HCD_MEMORY | HCD_DMA | HCD_USB2 | HCD_BH,

	/*
	 * basic lifecycle operations
	 */
	.reset			= ehci_setup,
	.start			= ehci_run,
	.stop			= ehci_stop,
	.shutdown		= ehci_shutdown,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue		= ehci_urb_enqueue,
	.urb_dequeue		= ehci_urb_dequeue,
	.endpoint_disable	= ehci_endpoint_disable,
	.endpoint_reset		= ehci_endpoint_reset,

	/*
	 * scheduling support
	 */
	.get_frame_number	= ehci_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data	= ehci_hub_status_data,
	.hub_control		= ehci_hub_control,
#ifdef CONFIG_PM
	.bus_suspend		= ehci_bus_suspend,
	.bus_resume		= ehci_bus_resume,
#endif
	.relinquish_port	= ehci_relinquish_port,
	.port_handed_over	= ehci_port_handed_over,
	.clear_tt_buffer_complete = ehci_clear_tt_buffer_complete,
};
#endif

/*
 * fotg210_hcd_probe - initialize faraday FOTG210 HCDs
 *
 * Allocates basic resources for this USB host controller, and
 * then invokes the start() method for the HCD associated with it
 * through the hotplug entry's driver_data.
 */
int fotg210_hcd_probe(struct platform_device *pdev, struct fotg210 *fotg)
{
	struct device *dev = &pdev->dev;
	struct usb_hcd *hcd;
	struct ehci_hcd *ehci;
	int irq;
	int ret;

	if (usb_disabled())
		return -ENODEV;

	hcd = usb_create_hcd(&fotg210_hc_driver, dev,
			     dev_name(dev));
	if (!hcd)
		return dev_err_probe(dev, -ENOMEM, "failed to create hcd\n");

	hcd->rsrc_start = fotg->res->start;
	hcd->rsrc_len = resource_size(fotg->res);
	hcd->regs = fotg->base;
	hcd_to_ehci(hcd)->caps = fotg->base;
	/* We always have FS support */
	// hcd->has_tt = 1; FIXME: implement port speed detection

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	// ehci = hcd_to_ehci(hcd);

	ret = usb_add_hcd(hcd, irq, 0); // not IRQF_SHARED since we don't really share the IRQ
	if (ret) {
		dev_err_probe(dev, ret, "failed to add hcd\n");
		goto failed_put_hcd;
	}
	device_wakeup_enable(hcd->self.controller);

	return ret;

failed_put_hcd:
	usb_put_hcd(hcd);
	return dev_err_probe(dev, ret, "init %s fail\n", dev_name(dev));
}

/*
 * fotg210_hcd_remove - shutdown processing for EHCI HCDs
 * @dev: USB Host Controller being removed
 *
 */
int fotg210_hcd_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);

	usb_remove_hcd(hcd);
	usb_put_hcd(hcd);

	return 0;
}

int fotg210_hcd_init(void)
{
	ehci_init_driver(&fotg210_hc_driver, &fotg210_hc_overrides);
	return 0;
}

void fotg210_hcd_cleanup(void)
{
}
