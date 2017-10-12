/*
 *  Bluetooth HCI UART H4 driver with Qualcomm WCN2243 UART connection.
 *
 *  Copyright (C) 2017 Linus Walleij <linus.walleij@linaro.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * https://android.googlesource.com/platform/hardware/qcom/bt/
 */

#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/serdev.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned/le_struct.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "hci_uart.h"
#include "btbcm.h"

#define VERSION "0.1"

#define FIRMWARE_WCN2243	"qcom/wcn2243.bin"

#define MAX_BAUD_RATE		4000000
#define SETUP_BAUD_RATE		921600
#define INIT_BAUD_RATE		120000

#define WCN2243_NUM_SUPPLIES 7
static const char *wcn2243_supply_names[WCN2243_NUM_SUPPLIES] = {
	"vdd-io",
	"vdd-xo-bias",
	"vreg-ana",
	"vdd-ldo-in",
	"vdd-dig-1p3",
	"vdd-bt-1p3",
	"vdd-bt-pa",
};

struct h4_struct {
	struct sk_buff *rx_skb;
	struct sk_buff_head txq;
};

struct wcn2243_bt {
	struct hci_uart hu;
	struct serdev_device *serdev;
	struct h4_struct *h4;

	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[WCN2243_NUM_SUPPLIES];
};

/* Initialize protocol */
static int h4_open(struct hci_uart *hu)
{
	struct wcn2243_bt *wcn = hu->priv;
	struct h4_struct *h4;

	BT_DBG("hu %p", hu);

	h4 = kzalloc(sizeof(*h4), GFP_KERNEL);
	if (!h4)
		return -ENOMEM;

	skb_queue_head_init(&h4->txq);
	wcn->h4 = h4;
	serdev_device_open(hu->serdev);

	return 0;
}

/* Flush protocol data */
static int h4_flush(struct hci_uart *hu)
{
	struct wcn2243_bt *wcn = hu->priv;
	struct h4_struct *h4 = wcn->h4;

	dev_dbg(&hu->serdev->dev, "flush device");

	skb_queue_purge(&h4->txq);

	return 0;
}

/* Close protocol */
static int h4_close(struct hci_uart *hu)
{
	struct wcn2243_bt *wcn = hu->priv;
	struct h4_struct *h4 = wcn->h4;

	serdev_device_close(hu->serdev);

	BT_DBG("hu %p", hu);

	skb_queue_purge(&h4->txq);

	kfree_skb(h4->rx_skb);
	wcn->h4 = NULL;
	kfree(h4);

	return 0;
}

/* Enqueue frame for transmittion (padding, crc, etc) */
static int h4_enqueue(struct hci_uart *hu, struct sk_buff *skb)
{
	struct wcn2243_bt *wcn = hu->priv;
	struct h4_struct *h4 = wcn->h4;

	BT_DBG("hu %p skb %p", hu, skb);

	/* Prepend skb with frame type */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	skb_queue_tail(&h4->txq, skb);

	return 0;
}

static const struct h4_recv_pkt h4_recv_pkts[] = {
	{ H4_RECV_ACL,   .recv = hci_recv_frame },
	{ H4_RECV_SCO,   .recv = hci_recv_frame },
	{ H4_RECV_EVENT, .recv = hci_recv_frame },
};

/* Recv data */
static int h4_recv(struct hci_uart *hu, const void *data, int count)
{
	struct wcn2243_bt *wcn = hu->priv;
	struct h4_struct *h4 = wcn->h4;

	if (!test_bit(HCI_UART_REGISTERED, &hu->flags))
		return -EUNATCH;

	h4->rx_skb = h4_recv_buf(hu->hdev, h4->rx_skb, data, count,
				 h4_recv_pkts, ARRAY_SIZE(h4_recv_pkts));
	if (IS_ERR(h4->rx_skb)) {
		int err = PTR_ERR(h4->rx_skb);
		BT_ERR("%s: Frame reassembly failed (%d)", hu->hdev->name, err);
		h4->rx_skb = NULL;
		return err;
	}

	return count;
}

static struct sk_buff *h4_dequeue(struct hci_uart *hu)
{
	struct wcn2243_bt *wcn = hu->priv;
	struct h4_struct *h4 = wcn->h4;

	return skb_dequeue(&h4->txq);
}

static const struct hci_uart_proto h4_serdev_proto = {
	.id		= HCI_UART_H4,
	.name		= "H4",
	.open		= h4_open,
	.close		= h4_close,
	.recv		= h4_recv,
	.enqueue	= h4_enqueue,
	.dequeue	= h4_dequeue,
	.flush		= h4_flush,
};

static int wcn2243_bluetooth_serdev_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct wcn2243_bt *wcn;
	int ret = 0;
	int i;

	wcn = devm_kzalloc(dev, sizeof(*wcn), GFP_KERNEL);
	if (!wcn)
		return -ENOMEM;

	wcn->hu.serdev = wcn->serdev = serdev;
	serdev_device_set_drvdata(serdev, wcn);

	/* Enable regulators */
	for (i = 0; i < ARRAY_SIZE(wcn->supplies); i++)
		wcn->supplies[i].supply = wcn2243_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(wcn->supplies),
				      wcn->supplies);
	if (ret) {
		dev_err(dev, "failed to request supplies: %d\n", ret);
		return ret;
	}
	ret = regulator_bulk_enable(ARRAY_SIZE(wcn->supplies),
				    wcn->supplies);
	if (ret) {
		dev_err(dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}

	wcn->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(wcn->reset)) {
		ret = PTR_ERR(wcn->reset);
		dev_err(dev, "could not get reset gpio: %d", ret);
		goto err_dis_reg;
	}
	/* FIXME: sleeping */
	usleep_range(100,1000);
	gpiod_set_value(wcn->reset, 0);
	dev_info(dev, "deasserted reset\n");
	msleep(10);

	wcn->hu.priv = wcn;
	ret = hci_uart_register_device(&wcn->hu, &h4_serdev_proto);
	if (ret) {
		dev_err(dev, "could not register bluetooth uart: %d", ret);
		return ret;
	}

	dev_info(dev, "registered WCN2243 HCI serial device\n");

	return 0;

err_dis_reg:
	regulator_bulk_disable(ARRAY_SIZE(wcn->supplies),
			       wcn->supplies);
	return ret;
}

static void wcn2243_bluetooth_serdev_remove(struct serdev_device *serdev)
{
	struct wcn2243_bt *wcn = serdev_device_get_drvdata(serdev);

	hci_uart_unregister_device(&wcn->hu);
	regulator_bulk_disable(ARRAY_SIZE(wcn->supplies),
			       wcn->supplies);
}

#ifdef CONFIG_OF
static const struct of_device_id wcn2243_of_match[] = {
	{ .compatible = "qcom,wcn2243-bluetooth", },
	{},
};
MODULE_DEVICE_TABLE(of, wcn2243_of_match);
#endif

static struct serdev_device_driver wcn2243_serdev_driver = {
	.probe = wcn2243_bluetooth_serdev_probe,
	.remove = wcn2243_bluetooth_serdev_remove,
	.driver = {
		.name = "wcn2243-bluetooth",
		.of_match_table = of_match_ptr(wcn2243_of_match),
	},
};

module_serdev_device_driver(wcn2243_serdev_driver);

MODULE_AUTHOR("Linus Walleij <linus.walleij@linaro.org>");
MODULE_DESCRIPTION("Bluetooth HCI UART H4 serdev driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
