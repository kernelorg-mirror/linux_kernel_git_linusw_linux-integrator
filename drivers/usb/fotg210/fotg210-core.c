// SPDX-License-Identifier: GPL-2.0+
/*
 * Central probing code for the FOTG210 dual role driver
 * We register one driver for the hardware and then we decide
 * whether to proceed with probing the host or the peripheral
 * driver.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/string_choices.h>
#include <linux/usb.h>
#include <linux/usb/otg.h>

#include "fotg210.h"

/* Misc Register 0x40 timings for 30 MHz speed, Low Speed, Full Speed and Low Speed */
#define FOTG210_MISC			0x40
#define FOTG210_MISC_EOF2_MASK		GENMASK(5,4)
#define FOTG210_MISC_EOF2_2_20_40	(0 << 4) /* 2@HS, 20@FS, 40@LS */
#define FOTG210_MISC_EOF2_4_40_80	(1 << 4) /* 4@HS, 40@FS, 80@LS */
#define FOTG210_MISC_EOF2_8_80_160	(2 << 4) /* 8@HS, 80@FS, 160@LS */
#define FOTG210_MISC_EOF2_16_160_320	(3 << 4) /* 16@HS, 160@FS, 320@LS */
#define FOTG210_MISC_EOF1_MASK		GENMASK(3,2)
#define FOTG210_MISC_EOF1_540_1600_3750	(0 << 2) /* 540@HS, 1600@FS, 3750@LS */
#define FOTG210_MISC_EOF1_360_1400_3500	(1 << 2) /* 360@HS, 1400@FS, 3500@LS */
#define FOTG210_MISC_EOF1_180_1200_3250	(2 << 2) /* 180@HS, 1200@FS, 3250@LS */
#define FOTG210_MISC_EOF1_720_21000_4000 (3 << 2) /* 720@HS, 21000@FS, 4000@LS */
#define FOTG210_MISC_AS_SLP_MASK	GENMASK(1,0)
#define FOTG210_MISC_AS_SLP_5US		(0 << 0) /* 5us sleep timer */
#define FOTG210_MISC_AS_SLP_10US	(1 << 0) /* 10us sleep timer */
#define FOTG210_MISC_AS_SLP_15US	(2 << 0) /* 15us sleep timer */
#define FOTG210_MISC_AS_SLP_20US	(3 << 0) /* 20us sleep timer */

/*
 * OTGCSR OTG Control/Status Register
 *
 * FIXME: implement core support in ehci.h and ehci-hub.c to deal with
 * the speed detection like Moorestown does.
 */
#define FOTG210_OTGCSR			0x80
#define FOTG210_OTGCSR_OTG_RESET	BIT(24)
#define FOTG210_OTGCSR_HOST_SPD_TYP	(3 << 22)
#define FOTG210_OTGCSR_ID		BIT(21)
#define FOTG210_OTGCSR_CROLE		BIT(20)
#define FOTG210_OTGCSR_A_VBUS_VLD	BIT(19)
#define FOTG210_OTGCSR_A_SESS_VLD	BIT(18)
#define FOTG210_OTGCSR_B_SESS_VLD	BIT(17)
#define FOTG210_OTGCSR_B_SESS_END	BIT(16)
#define FOTG210_OTGCSR_PHY_RESET	BIT(15)
#define FOTG210_OTGCSR_A_SRP_RESP_TYPE	BIT(8) /* 0 = VBUS, 1 = DATA LINE */
#define FOTG210_OTGCSR_A_SRP_DET_EN	BIT(7)
#define FOTG210_OTGCSR_B_SET_HNP_EN	BIT(6)
#define FOTG210_OTGCSR_A_BUS_DROP	BIT(5)
#define FOTG210_OTGCSR_A_BUS_REQ	BIT(4)
#define FOTG210_OTGCSR_B_DSCHG_VBUS	BIT(2)
#define FOTG210_OTGCSR_B_HNP_EN		BIT(1)
#define FOTG210_OTGCSR_B_BUS_REQ	BIT(0)

/* OTGISR interrupt status register: offset 0x74, 0x84 in Faraday source code */
#define FOTG210_OTGISR			0x84
#define FOTG210_OTGISR_APLGRMV		BIT(12)
#define FOTG210_OTGISR_BPLGRMV		BIT(11)
#define FOTG210_OTGISR_OVC		BIT(10) /* Overcurrent indication  */
#define FOTG210_OTGISR_IDCHG		BIT(9)
#define FOTG210_OTGISR_RLCHG		BIT(8)
#define FOTG210_OTGISR_AVBUSERR		BIT(5)
#define FOTG210_OTGISR_ASRPDET		BIT(4)
#define FOTG210_OTGISR_BSRPDN		BIT(0)

/* OTGISR interrupt enable register: offset 0x88 in Faraday source code */
#define FOTG210_OTGIEN			0x88 /* ? */
#define FOTG210_OTGIEN_APLGRMV		BIT(12)
#define FOTG210_OTGIEN_BPLGRMV		BIT(11)
#define FOTG210_OTGIEN_OVC		BIT(10) /* Overcurrent indication  */
#define FOTG210_OTGIEN_IDCHG		BIT(9)
#define FOTG210_OTGIEN_RLCHG		BIT(8)
#define FOTG210_OTGIEN_AVBUSERR		BIT(5)
#define FOTG210_OTGIEN_ASRPDET		BIT(4)
#define FOTG210_OTGIEN_BSRPDN		BIT(0)

/* Global interrupt status */
#define FOTG210_GINT			0xc0
#define FOTG210_GINT_MHC_INT		BIT(2) /* Host interrupt */
#define FOTG210_GINT_MOTG_INT		BIT(1) /* OTG interrupt */
#define FOTG210_GINT_MDEV_INT		BIT(0) /* Peripheral interrupt */

/* GMIR global mask interrupt enable register: offset 0xB4, 0xc4 in Faraday source code */
#define FOTG210_GINTM			0xc4
#define FOTG210_GINTM_INT_POLARITY	BIT(3) /* Active High*/
#define FOTG210_GINTM_MHC_INT		BIT(2) /* Mask Host interrupt */
#define FOTG210_GINTM_MOTG_INT		BIT(1) /* Mask OTG interrupt */
#define FOTG210_GINTM_MDEV_INT		BIT(0) /* Mask peripheral interrupt */

/*
 * Gemini-specific initialization function, only executed on the
 * Gemini SoC using the global misc control register.
 *
 * The gemini USB blocks are connected to either Mini-A (host mode) or
 * Mini-B (peripheral mode) plugs. There is no role switch support on the
 * Gemini SoC, just either-or.
 */
#define GEMINI_GLOBAL_MISC_CTRL		0x30
#define GEMINI_MISC_USB0_WAKEUP		BIT(14)
#define GEMINI_MISC_USB1_WAKEUP		BIT(15)
#define GEMINI_MISC_USB0_VBUS_ON	BIT(22)
#define GEMINI_MISC_USB1_VBUS_ON	BIT(23)
#define GEMINI_MISC_USB0_MINI_B		BIT(29)
#define GEMINI_MISC_USB1_MINI_B		BIT(30)

static int fotg210_gemini_init(struct fotg210 *fotg, struct resource *res,
			       enum usb_dr_mode mode)
{
	struct device *dev = fotg->dev;
	struct device_node *np = dev->of_node;
	struct regmap *map;
	bool wakeup;
	u32 mask, val;
	int ret;

	map = syscon_regmap_lookup_by_phandle(np, "syscon");
	if (IS_ERR(map))
		return dev_err_probe(dev, PTR_ERR(map), "no syscon\n");
	fotg->map = map;
	wakeup = of_property_read_bool(np, "wakeup-source");

	/*
	 * Figure out if this is USB0 or USB1 by simply checking the
	 * physical base address.
	 */
	mask = 0;
	if (res->start == 0x69000000) {
		fotg->port = GEMINI_PORT_1;
		mask = GEMINI_MISC_USB1_VBUS_ON | GEMINI_MISC_USB1_MINI_B |
			GEMINI_MISC_USB1_WAKEUP;
		if (mode == USB_DR_MODE_HOST)
			val = GEMINI_MISC_USB1_VBUS_ON;
		else
			val = GEMINI_MISC_USB1_MINI_B;
		if (wakeup)
			val |= GEMINI_MISC_USB1_WAKEUP;
	} else {
		fotg->port = GEMINI_PORT_0;
		mask = GEMINI_MISC_USB0_VBUS_ON | GEMINI_MISC_USB0_MINI_B |
			GEMINI_MISC_USB0_WAKEUP;
		if (mode == USB_DR_MODE_HOST) {
			val = GEMINI_MISC_USB0_VBUS_ON;
		} else
			val = GEMINI_MISC_USB0_MINI_B;
		if (wakeup)
			val |= GEMINI_MISC_USB0_WAKEUP;
	}

	ret = regmap_update_bits(map, GEMINI_GLOBAL_MISC_CTRL, mask, val);
	if (ret) {
		dev_err(dev, "failed to initialize Gemini PHY\n");
		return ret;
	}

	/* Set up some default timings (called a bug in Gemini) */
	val = readl(fotg->base + FOTG210_MISC);
	val &= ~(FOTG210_MISC_EOF1_MASK | FOTG210_MISC_AS_SLP_MASK);
	val |= (FOTG210_MISC_EOF1_720_21000_4000 | FOTG210_MISC_AS_SLP_10US);
	writel(val, fotg->base + FOTG210_MISC);

	dev_info(dev, "initialized Gemini PHY in %s mode\n",
		 (mode == USB_DR_MODE_HOST) ? "host" : "gadget");
	return 0;
}

/**
 * fotg210_vbus() - Called by gadget driver to enable/disable VBUS
 * @fotg: pointer to a private fotg210 object
 * @enable: true to enable VBUS, false to disable VBUS
 */
void fotg210_vbus(struct fotg210 *fotg, bool enable)
{
	u32 mask;
	u32 val;
	int ret;

	switch (fotg->port) {
	case GEMINI_PORT_0:
		mask = GEMINI_MISC_USB0_VBUS_ON;
		val = enable ? GEMINI_MISC_USB0_VBUS_ON : 0;
		break;
	case GEMINI_PORT_1:
		mask = GEMINI_MISC_USB1_VBUS_ON;
		val = enable ? GEMINI_MISC_USB1_VBUS_ON : 0;
		break;
	default:
		return;
	}
	ret = regmap_update_bits(fotg->map, GEMINI_GLOBAL_MISC_CTRL, mask, val);
	if (ret)
		dev_err(fotg->dev, "failed to %s VBUS\n",
			str_enable_disable(enable));
	dev_info(fotg->dev, "%s: %s VBUS\n", __func__, str_enable_disable(enable));
}

static int fotg210_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	enum usb_dr_mode mode;
	struct fotg210 *fotg;
	u32 val;
	int ret;

	fotg = devm_kzalloc(dev, sizeof(*fotg), GFP_KERNEL);
	if (!fotg)
		return -ENOMEM;
	fotg->dev = dev;

	fotg->base = devm_platform_get_and_ioremap_resource(pdev, 0, &fotg->res);
	if (IS_ERR(fotg->base))
		return PTR_ERR(fotg->base);

	fotg->pclk = devm_clk_get_optional_enabled(dev, "PCLK");
	if (IS_ERR(fotg->pclk))
		return PTR_ERR(fotg->pclk);

	mode = usb_get_dr_mode(dev);

	if (of_device_is_compatible(dev->of_node, "cortina,gemini-usb")) {
		ret = fotg210_gemini_init(fotg, fotg->res, mode);
		if (ret)
			return ret;
	}

	val = readl(fotg->base + FOTG210_OTGCSR);
	if (mode == USB_DR_MODE_PERIPHERAL) {
		if (!(val & FOTG210_OTGCSR_CROLE))
			dev_err(dev, "block not in device role\n");
		ret = fotg210_udc_probe(pdev, fotg);
	} else {
		if (val & FOTG210_OTGCSR_CROLE)
			dev_err(dev, "block not in host role\n");

		/* Mask off device and OTG interrupts, set polarity high */
		writel(FOTG210_GINTM_MDEV_INT | FOTG210_GINTM_MOTG_INT | FOTG210_GINTM_INT_POLARITY,
		       fotg->base + FOTG210_GINTM);

		/* Power off device A: drop VBUS and BUS request */
		val = readl(fotg->base + FOTG210_OTGCSR);
		val |= FOTG210_OTGCSR_A_BUS_DROP;
		writel(val, fotg->base + FOTG210_OTGCSR);
		val &= ~FOTG210_OTGCSR_A_BUS_REQ;
		writel(val, fotg->base + FOTG210_OTGCSR);
		msleep(10);

		/* Power it all back on */
		val &= ~FOTG210_OTGCSR_A_BUS_DROP;
		writel(val, fotg->base + FOTG210_OTGCSR);
		val |= FOTG210_OTGCSR_A_BUS_REQ;
		writel(val, fotg->base + FOTG210_OTGCSR);
		msleep(10);

		ret = fotg210_hcd_probe(pdev, fotg);
	}

	return ret;
}

static void fotg210_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	enum usb_dr_mode mode;

	mode = usb_get_dr_mode(dev);

	if (mode == USB_DR_MODE_PERIPHERAL)
		fotg210_udc_remove(pdev);
	else
		fotg210_hcd_remove(pdev);
}

#ifdef CONFIG_OF
static const struct of_device_id fotg210_of_match[] = {
	{ .compatible = "faraday,fotg200" },
	{ .compatible = "faraday,fotg210" },
	/* TODO: can we also handle FUSB220? */
	{},
};
MODULE_DEVICE_TABLE(of, fotg210_of_match);
#endif

static struct platform_driver fotg210_driver = {
	.driver = {
		.name   = "fotg210",
		.of_match_table = of_match_ptr(fotg210_of_match),
	},
	.probe  = fotg210_probe,
	.remove = fotg210_remove,
};

static int __init fotg210_init(void)
{
	if (IS_ENABLED(CONFIG_USB_FOTG210_HCD) && !usb_disabled())
		fotg210_hcd_init();
	return platform_driver_register(&fotg210_driver);
}
module_init(fotg210_init);

static void __exit fotg210_cleanup(void)
{
	platform_driver_unregister(&fotg210_driver);
	if (IS_ENABLED(CONFIG_USB_FOTG210_HCD))
		fotg210_hcd_cleanup();
}
module_exit(fotg210_cleanup);

MODULE_AUTHOR("Yuan-Hsin Chen, Feng-Hsin Chiang");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FOTG210 Dual Role Controller Driver");
