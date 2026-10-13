// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BCMBCA reduced XRDP Ethernet driver.
 *
 * Based on Broadcom's GPL U-Boot bcmbca/xrdp driver (2019-2020).
 * The reduced Runner program exchanges packets through on-chip SRAM.
 * SoC match data supplies the register layout, port routing and program.
 * The SRAM Buffer Pool Manager (SBPM) allocates and frees 128-byte buffers,
 * which are linked together to hold larger packets. The driver does not
 * use host DMA rings or the vendor RDPA ABI. Packet descriptors use the
 * vendor GEN45 SBPM format.
 * Firmware must have configured pin multiplexing and external PHYs.
 */

#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reset.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>

#include "bcmbca_xrdp.h"
#include "unimac.h"

#define RUNNER_ENABLE		BIT(0)
#define RUNNER_WAKEUP		0x4
#define CPU_TX_THREAD		1
#define TX_QUEUE_LEN		64
#define BBH_RX_MAXPKT0		0x28
#define BBH_RX_MAXPKT1		0x2c
#define BBH_RX_ENABLE		0x3c
#define SBPM_ALLOC		0x04
#define SBPM_ALLOC_REPLY		0x08
#define SBPM_CONNECT		0x1c
#define SBPM_CONNECT_REPLY	0x20
#define SBPM_NEXT		0x24
#define SBPM_NEXT_REPLY		0x28
#define SBPM_FREE		0x38
#define SBPM_FREE_REPLY		0x3c
#define SBPM_UG0_COUNT		0x78
#define SBPM_UG1_COUNT		0x7c
#define SBPM_GLOBAL_COUNT		0x98
#define SBPM_ISR			0x200
#define SBPM_READY		BIT(31)
#define SBPM_BUSY		BIT(30)
#define SBPM_ACK		BIT(0)
#define SBPM_NULL		BIT(15)
#define SBPM_BUFFER		GENMASK(14, 1)

/* BCM6846 QEGPHY control, relative to the XRDP resource. */
#define BCM6846_QEGPHY_CTRL		0xdb2208
#define BCM6846_QEGPHY_IDDQ_BIAS		BIT(0)
#define BCM6846_QEGPHY_EXT_PWR_DOWN	GENMASK(4, 1)
#define BCM6846_QEGPHY_IDDQ_GLOBAL	BIT(6)
#define BCM6846_QEGPHY_RESET		BIT(8)
#define BCM6846_QEGPHY_PLL_250		BIT(21)

#include "bcm6846_enet_data.h"

/* UniMAC/BBH registers, RGMII attachment and Runner TX routing per port. */
static const struct bcmbca_xrdp_port_data bcm6846_ports[] = {
	{
		.mac = 0xda0000, .mib = 0xda8000, .mac_top = 0xdaa000,
		.bbh_rx = 0xd94000, .tx_destination = 32, .tx_counter = 0x180,
	}, {
		.mac = 0xda1000, .mib = 0xda8400, .mac_top = 0xdaa400,
		.bbh_rx = 0xd94400, .tx_destination = 96, .tx_counter = 0x181,
	}, {
		.mac = 0xda2000, .mib = 0xda8800, .mac_top = 0xdaa800,
		.bbh_rx = 0xd94800, .tx_destination = 160, .tx_counter = 0x182,
	}, {
		.mac = 0xda3000, .mib = 0xda8c00, .mac_top = 0xdaac00,
		.bbh_rx = 0xd94c00, .tx_destination = 224, .tx_counter = 0x183,
	}, {
		.mac = 0xda4000, .mib = 0xda9000, .mac_top = 0xdab000,
		.bbh_rx = 0xd95000, .rgmii = 0xd97300,
		.tx_destination = 288, .tx_counter = 0x184,
	},
};

static const struct bcmbca_xrdp_enet_variant bcm6846_variant = {
	.init_data = bcm6846_init_data,
	.init_data_size = ARRAY_SIZE(bcm6846_init_data),
	.init_base = 0x82000000,
	.reg_size = 0x1000000,
	.sbpm = 0xd98000,
	.psram = 0x600000,
	.runner_ctrl = 0xd00000,
	.runner_stride = 0x1000,
	.runner_sram = 0xc00000,
	.rx_fifo = 0x800,
	.tx_descriptor = 0x50,
	.tx_destination = 0xdc,
	.rx_fifo_size = 64,
	.num_runners = 3,
	.num_buffers = 1536,
	.num_ports = ARRAY_SIZE(bcm6846_ports),
	.sbpm_alloc_sa = 30,
	.sbpm_free_sa = 18,
	.ports = bcm6846_ports,
};

struct bcmbca_xrdp_enet;

struct bcmbca_xrdp_port {
	struct bcmbca_xrdp_enet *enet;
	const struct bcmbca_xrdp_port_data *data;
	struct net_device *ndev;
	struct phylink *phylink;
	struct phylink_config phylink_config;
	struct sk_buff_head tx_queue;
	void __iomem *mac;
	u8 tx_producer;
	bool running;
};

struct bcmbca_xrdp_enet {
	struct device *dev;
	void __iomem *base;
	struct reset_control *reset;
	struct clk_bulk_data *clks;
	int num_clks;
	const struct bcmbca_xrdp_enet_variant *variant;
	unsigned long *rx_seen;
	struct delayed_work work;
	/* Serialize all CPU accesses to SBPM's shared command registers. */
	struct mutex lock;
	unsigned int rx_index;
	bool hw_accessible;
	bool faulted;
	bool stopping;
	struct bcmbca_xrdp_port *ports[];
};

static void bcmbca_xrdp_quiesce(struct bcmbca_xrdp_enet *enet)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	unsigned int i;
	u32 val;

	/* Cold CFE handoff can leave the Runner register banks powered off. */
	if (!enet->hw_accessible)
		return;

	for (i = 0; i < variant->num_ports; i++) {
		void __iomem *mac = enet->base + variant->ports[i].mac;

		val = readl(mac + UMAC_CMD);
		writel(val & ~(CMD_RX_EN | CMD_TX_EN), mac + UMAC_CMD);
		writel(0, enet->base + variant->ports[i].bbh_rx + BBH_RX_ENABLE);
	}
	for (i = 0; i < variant->num_runners; i++) {
		void __iomem *ctrl = enet->base + variant->runner_ctrl +
				     i * variant->runner_stride;

		val = readl(ctrl);
		writel(val & ~RUNNER_ENABLE, ctrl);
		readl(ctrl);
	}
	usleep_range(1000, 2000);
}

static void bcmbca_xrdp_hw_stop(struct bcmbca_xrdp_enet *enet)
{
	if (!enet->hw_accessible)
		return;

	bcmbca_xrdp_quiesce(enet);
	clk_bulk_disable_unprepare(enet->num_clks, enet->clks);
	enet->hw_accessible = false;
}

static bool bcmbca_xrdp_range_valid(const struct bcmbca_xrdp_enet_variant *variant,
				    u32 offset, u32 size)
{
	return offset < variant->reg_size && size <= variant->reg_size - offset;
}

static int bcmbca_xrdp_hw_init(struct bcmbca_xrdp_enet *enet)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	unsigned int i, j;
	u32 status;
	int ret;

	bcmbca_xrdp_hw_stop(enet);

	/* The reset provider powers the zones before their clocks can be prepared. */
	ret = reset_control_reset(enet->reset);
	if (ret)
		return dev_err_probe(enet->dev, ret, "XRDP reset failed\n");
	ret = clk_bulk_prepare_enable(enet->num_clks, enet->clks);
	if (ret)
		return dev_err_probe(enet->dev, ret, "cannot enable XRDP clocks\n");
	/* All register banks are accessible only after reset and clock setup. */
	enet->hw_accessible = true;

	if (readl(enet->base + variant->sbpm + SBPM_GLOBAL_COUNT) ||
	    readl(enet->base + variant->sbpm + SBPM_UG0_COUNT) ||
	    readl(enet->base + variant->sbpm + SBPM_UG1_COUNT) ||
	    readl(enet->base + variant->sbpm + SBPM_ISR))
		return dev_err_probe(enet->dev, -EIO, "SBPM is not empty after reset\n");
	for (i = 0; i < variant->init_data_size; i++) {
		const struct bcmbca_xrdp_init_entry *entry = &variant->init_data[i];
		u32 offset = entry->command & 0xffffff;
		u32 op = (entry->command >> 24) & 0xf;
		u32 size = entry->command >> 28;
		u32 value = entry->value;
		u32 count;

		switch (op) {
		case 0: /* Recorded register write, already in CPU byte order. */
		case 1: /* Recorded Runner SRAM write, already byte-swapped. */
			if (!bcmbca_xrdp_range_valid(variant, offset, size) ||
			    (size != 1 && size != 2 && size != 4) ||
			    !IS_ALIGNED(offset, size))
				return -EINVAL;
			if (size == 4)
				writel(value, enet->base + offset);
			else if (size == 2)
				writew(value, enet->base + offset);
			else
				writeb(value, enet->base + offset);
			if (offset == variant->sbpm) {
				ret = readl_poll_timeout(enet->base + variant->sbpm, status,
							 status & SBPM_READY, 1, 1000);
				if (ret) {
					dev_err(enet->dev, "SBPM initialization timeout: %08x\n",
						status);
					return ret;
				}
			}
			break;
		case 2: /* Byte fill. */
		case 3: /* Big-endian word fill. */
		case 7: /* Incrementing native-endian word fill. */
			count = value;
			if (++i == variant->init_data_size)
				return -EINVAL;
			entry = &variant->init_data[i];
			offset = entry->command - variant->init_base;
			value = entry->value;
			if (count > variant->reg_size / 4 ||
			    !bcmbca_xrdp_range_valid(variant, offset,
						op == 2 ? count : count * 4) ||
			    (op != 2 && !IS_ALIGNED(offset, 4)))
				return -EINVAL;
			if (op == 2) {
				memset_io(enet->base + offset, value, count);
				break;
			}
			for (j = 0; j < count; j++) {
				if (op == 3)
					iowrite32be(value, enet->base + offset + j * 4);
				else
					writel((value & 0xffff) + j * (value >> 16),
					       enet->base + offset + j * 4);
			}
			break;
		case 5: /* The reduced image has exactly one Runner SRAM area. */
			if (offset != variant->runner_sram || value)
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
	}

	/* Flush the last write before exposing the packet interface. */
	readl(enet->base + variant->runner_ctrl);

	return 0;
}

static int bcmbca_xrdp_sbpm_wait(struct bcmbca_xrdp_enet *enet, u32 reg, u32 *value)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	int ret;

	ret = readl_poll_timeout(enet->base + variant->sbpm + reg, *value,
				 *value & SBPM_READY, 1, 1000);
	if (ret) {
		/* A timed-out command may still complete: do not submit another. */
		enet->faulted = true;
		dev_err(enet->dev, "SBPM timeout at %#x: %#x\n", reg, *value);
	}
	return ret;
}

static int bcmbca_xrdp_free(struct bcmbca_xrdp_enet *enet, u16 head)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	u32 value;
	int ret;

	if (enet->faulted)
		return -EIO;
	writel(head | (variant->sbpm_free_sa << 14) | BIT(31),
	       enet->base + variant->sbpm + SBPM_FREE);
	ret = bcmbca_xrdp_sbpm_wait(enet, SBPM_FREE_REPLY, &value);
	if (ret)
		return ret;
	if (!(value & SBPM_ACK)) {
		enet->faulted = true;
		dev_err(enet->dev, "SBPM free rejected head=%u reply=%08x\n",
			head, value);
		return -EIO;
	}
	return 0;
}

static int bcmbca_xrdp_alloc(struct bcmbca_xrdp_enet *enet, u16 *buffer)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	u32 value;
	int ret;

	writel(variant->sbpm_alloc_sa << 14, enet->base + variant->sbpm + SBPM_ALLOC);
	ret = bcmbca_xrdp_sbpm_wait(enet, SBPM_ALLOC_REPLY, &value);
	if (ret)
		return ret;
	if (!(value & SBPM_ACK))
		return -ENOSPC;
	*buffer = FIELD_GET(SBPM_BUFFER, value);
	if (*buffer >= variant->num_buffers) {
		enet->faulted = true;
		dev_err(enet->dev, "SBPM allocation returned invalid buffer=%u reply=%08x\n",
			*buffer, value);
		return -EIO;
	}
	return 0;
}

static int bcmbca_xrdp_connect(struct bcmbca_xrdp_enet *enet, u16 from, u16 to)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	u32 value;
	int ret;

	writel(from | BIT(14) | (to << 16), enet->base + variant->sbpm + SBPM_CONNECT);
	ret = bcmbca_xrdp_sbpm_wait(enet, SBPM_CONNECT_REPLY, &value);
	if (ret)
		return ret;
	if (!(value & SBPM_ACK))
		return -EIO;
	return 0;
}

static int bcmbca_xrdp_copy_rx(struct bcmbca_xrdp_enet *enet,
			       const struct bcmbca_xrdp_rx_info *info, u8 *data)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	unsigned long *seen = enet->rx_seen;
	unsigned int skip = BCMBCA_XRDP_RX_HEADROOM + info->offset;
	unsigned int remaining = info->length;
	u16 buffer = info->buffer;
	u32 value;
	int ret;

	bitmap_zero(seen, variant->num_buffers);
	while (remaining) {
		unsigned int len = min(remaining, BCMBCA_XRDP_BUFFER_SIZE - skip);

		if (test_and_set_bit(buffer, seen)) {
			/* Do not ask SBPM to walk a cyclic list when freeing it. */
			enet->faulted = true;
			return -EIO;
		}
		if (data) {
			memcpy_fromio(data, enet->base + variant->psram +
				      buffer * BCMBCA_XRDP_BUFFER_SIZE + skip, len);
			data += len;
		}
		remaining -= len;
		skip = 0;
		/* Even a single-buffer frame must end in a null list pointer. */
		writel(buffer, enet->base + variant->sbpm + SBPM_NEXT);
		ret = readl_poll_timeout(enet->base + variant->sbpm + SBPM_NEXT_REPLY,
					 value, !(value & SBPM_BUSY), 1, 1000);
		if (ret) {
			enet->faulted = true;
			dev_err(enet->dev, "SBPM next timeout head=%u buffer=%u reply=%08x\n",
				info->buffer, buffer, value);
			return ret;
		}
		if (value & SBPM_NULL)
			return remaining ? -EIO : 0;
		if (!remaining || !(value & SBPM_ACK) ||
		    FIELD_GET(SBPM_BUFFER, value) >= variant->num_buffers) {
			/* Free-without-context would follow this unvalidated tail. */
			enet->faulted = true;
			dev_err(enet->dev,
				"invalid RX list: head=%u buffer=%u remaining=%u next_reply=%08x\n",
				info->buffer, buffer, remaining, value);
			return -EIO;
		}
		buffer = FIELD_GET(SBPM_BUFFER, value);
	}
	return 0;
}

static bool bcmbca_xrdp_receive(struct bcmbca_xrdp_enet *enet)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	void __iomem *desc = enet->base + variant->runner_sram + variant->rx_fifo +
			     enet->rx_index * 16;
	struct bcmbca_xrdp_rx_info info;
	struct bcmbca_xrdp_port *port;
	struct sk_buff *skb = NULL;
	u32 word0, word1, word2;
	int ret;

	word1 = ioread32be(desc + 4);
	if (!word1)
		return false;
	word0 = ioread32be(desc);
	word2 = ioread32be(desc + 8);
	/* Clearing word1 returns this descriptor to the Runner. */
	writel(0, desc + 12);
	writel(0, desc + 8);
	writel(0, desc);
	writel(0, desc + 4);
	enet->rx_index = (enet->rx_index + 1) % variant->rx_fifo_size;

	if (!bcmbca_xrdp_rx_decode(variant, word0, word1, word2, &info)) {
		/* Without a valid length, we cannot bound the list before freeing. */
		enet->faulted = true;
		goto free_buffer;
	}
	port = enet->ports[info.port];
	if (port && port->running) {
		skb = netdev_alloc_skb_ip_align(port->ndev, info.length);
		if (!skb)
			port->ndev->stats.rx_dropped++;
	}
	/* Validate discarded frames too, without copying their payload. */
	ret = bcmbca_xrdp_copy_rx(enet, &info, skb ? skb_put(skb, info.length) : NULL);
	if (ret) {
		if (port)
			port->ndev->stats.rx_errors++;
		dev_kfree_skb_any(skb);
		goto free_buffer;
	}
	if (!skb)
		goto free_buffer;
	port->ndev->stats.rx_packets++;
	port->ndev->stats.rx_bytes += info.length;
	skb->protocol = eth_type_trans(skb, port->ndev);
	netif_rx(skb);

free_buffer:
	bcmbca_xrdp_free(enet, info.buffer);
	return true;
}

static int bcmbca_xrdp_transmit(struct bcmbca_xrdp_port *port, struct sk_buff *skb)
{
	struct bcmbca_xrdp_enet *enet = port->enet;
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	void __iomem *desc = enet->base + variant->runner_sram + variant->tx_descriptor;
	u16 head = BCMBCA_XRDP_NULL_BUFFER, tail = 0, buffer;
	unsigned int remaining = skb->len, offset = 0, count = 0;
	int ret;

	while (remaining) {
		unsigned int len = min_t(unsigned int, remaining, BCMBCA_XRDP_BUFFER_SIZE);

		ret = bcmbca_xrdp_alloc(enet, &buffer);
		if (ret)
			goto free_list;
		memcpy_toio(enet->base + variant->psram + buffer * BCMBCA_XRDP_BUFFER_SIZE,
			    skb->data + offset, len);
		if (head == BCMBCA_XRDP_NULL_BUFFER) {
			head = buffer;
		} else {
			ret = bcmbca_xrdp_connect(enet, tail, buffer);
			if (ret) {
				bcmbca_xrdp_free(enet, buffer);
				goto free_list;
			}
		}
		tail = buffer;
		count++;
		offset += len;
		remaining -= len;
	}

	memset_io(desc, 0, 32);
	iowrite32be(BIT(31) | (count << 14), desc + 4);
	iowrite32be(BIT(30) | tail, desc + 8);
	iowrite32be(BIT(30) | head, desc + 12);
	iowrite32be(port->data->tx_destination,
		    enet->base + variant->runner_sram + variant->tx_destination);
	/* Publish the length only after the packet, descriptor and destination. */
	iowrite16be(bcmbca_xrdp_tx_length_word(count, skb->len), desc + 6);
	writel(CPU_TX_THREAD, enet->base + variant->runner_ctrl + RUNNER_WAKEUP);
	port->tx_producer++;
	return 0;

free_list:
	if (head != BCMBCA_XRDP_NULL_BUFFER)
		bcmbca_xrdp_free(enet, head);
	return ret;
}

static void bcmbca_xrdp_work(struct work_struct *work)
{
	struct bcmbca_xrdp_enet *enet = container_of(to_delayed_work(work),
						struct bcmbca_xrdp_enet, work);
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	unsigned int i, budget;

	mutex_lock(&enet->lock);
	if (enet->stopping || enet->faulted)
		goto out;
	for (budget = 0; budget < variant->rx_fifo_size && !enet->faulted; budget++)
		if (!bcmbca_xrdp_receive(enet))
			break;
	for (i = 0; i < variant->num_ports && !enet->faulted; i++) {
		struct bcmbca_xrdp_port *port = enet->ports[i];
		struct sk_buff *skb;

		if (!port || !port->running)
			continue;
		for (budget = 0; budget < 8 && !enet->faulted; budget++) {
			u16 length = ioread16be(enet->base + variant->runner_sram +
					       variant->tx_descriptor + 6) & 0x3fff;
			u8 consumer = readb(enet->base + variant->runner_sram +
					    port->data->tx_counter);

			if (!bcmbca_xrdp_tx_available(length, port->tx_producer, consumer))
				break;
			skb = skb_dequeue(&port->tx_queue);
			if (!skb)
				break;
			if (bcmbca_xrdp_transmit(port, skb)) {
				port->ndev->stats.tx_dropped++;
				port->ndev->stats.tx_errors++;
			} else {
				port->ndev->stats.tx_packets++;
				port->ndev->stats.tx_bytes += skb->len;
			}
			dev_consume_skb_any(skb);
		}
		if (skb_queue_len(&port->tx_queue) < TX_QUEUE_LEN / 2)
			netif_wake_queue(port->ndev);
	}
	if (enet->faulted) {
		dev_err(enet->dev, "packet engine stopped after SBPM failure\n");
		for (i = 0; i < variant->num_ports; i++)
			if (enet->ports[i])
				netif_stop_queue(enet->ports[i]->ndev);
	} else {
		schedule_delayed_work(&enet->work, msecs_to_jiffies(1));
	}
out:
	mutex_unlock(&enet->lock);
}

static struct bcmbca_xrdp_port *bcmbca_xrdp_config_port(struct phylink_config *config)
{
	return container_of(config, struct bcmbca_xrdp_port, phylink_config);
}

static void bcmbca_xrdp_mac_config(struct phylink_config *config, unsigned int mode,
				   const struct phylink_link_state *state)
{
	struct bcmbca_xrdp_port *port = bcmbca_xrdp_config_port(config);
	void __iomem *rgmii = port->enet->base + port->data->rgmii;
	u32 value;

	if (!port->data->rgmii)
		return;
	/* phylink passes the delay requirement to the PHY or switch peer. */
	value = readl(rgmii);
	value &= ~GENMASK(4, 1);
	value |= BIT(0) | BIT(1) | (3 << 2);
	writel(value, rgmii);
	writel(0x28, rgmii + 8);
}

static void bcmbca_xrdp_mac_link_down(struct phylink_config *config,
				      unsigned int mode, phy_interface_t interface)
{
	struct bcmbca_xrdp_port *port = bcmbca_xrdp_config_port(config);
	u32 value = readl(port->mac + UMAC_CMD);

	writel(value & ~(CMD_TX_EN | CMD_RX_EN), port->mac + UMAC_CMD);
	if (port->data->rgmii)
		writel(BIT(4), port->enet->base + port->data->rgmii + 4);
}

static void bcmbca_xrdp_mac_link_up(struct phylink_config *config,
				    struct phy_device *phy, unsigned int mode,
				    phy_interface_t interface, int speed, int duplex,
				    bool tx_pause, bool rx_pause)
{
	struct bcmbca_xrdp_port *port = bcmbca_xrdp_config_port(config);
	u32 rate = speed == SPEED_1000 ? 2 : speed == SPEED_100 ? 1 : 0;
	u32 value = CMD_PROMISC | CMD_CRC_FWD | CMD_NO_LEN_CHK;

	value |= rate << CMD_SPEED_SHIFT;
	if (duplex == DUPLEX_HALF)
		value |= CMD_HD_EN;
	if (!tx_pause)
		value |= CMD_TX_PAUSE_IGNORE;
	if (!rx_pause)
		value |= CMD_RX_PAUSE_IGNORE;
	writel(value | CMD_TX_EN | CMD_RX_EN, port->mac + UMAC_CMD);
	if (port->data->rgmii) {
		value = BIT(4) | BIT(3) | rate;
		if (duplex == DUPLEX_FULL)
			value |= BIT(2);
		writel(value, port->enet->base + port->data->rgmii + 4);
	}
}

static const struct phylink_mac_ops bcmbca_xrdp_mac_ops = {
	.mac_config = bcmbca_xrdp_mac_config,
	.mac_link_down = bcmbca_xrdp_mac_link_down,
	.mac_link_up = bcmbca_xrdp_mac_link_up,
};

static int bcmbca_xrdp_open(struct net_device *ndev)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);
	struct bcmbca_xrdp_enet *enet = port->enet;

	mutex_lock(&enet->lock);
	if (enet->faulted) {
		mutex_unlock(&enet->lock);
		return -EIO;
	}
	port->running = true;
	mutex_unlock(&enet->lock);
	phylink_start(port->phylink);
	netif_start_queue(ndev);
	return 0;
}

static int bcmbca_xrdp_stop(struct net_device *ndev)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	mutex_lock(&port->enet->lock);
	port->running = false;
	mutex_unlock(&port->enet->lock);
	netif_tx_disable(ndev);
	phylink_stop(port->phylink);
	skb_queue_purge(&port->tx_queue);
	return 0;
}

static netdev_tx_t bcmbca_xrdp_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	if (skb->len > BCMBCA_XRDP_FRAME_LEN || skb_linearize(skb)) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	if (skb_put_padto(skb, ETH_ZLEN)) {
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	skb_queue_tail(&port->tx_queue, skb);
	if (skb_queue_len(&port->tx_queue) >= TX_QUEUE_LEN)
		netif_stop_queue(ndev);
	mod_delayed_work(system_percpu_wq, &port->enet->work, 0);
	return NETDEV_TX_OK;
}

static int bcmbca_xrdp_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	return phylink_mii_ioctl(port->phylink, ifr, cmd);
}

static const struct net_device_ops bcmbca_xrdp_netdev_ops = {
	.ndo_open = bcmbca_xrdp_open,
	.ndo_stop = bcmbca_xrdp_stop,
	.ndo_start_xmit = bcmbca_xrdp_start_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_eth_ioctl = bcmbca_xrdp_ioctl,
};

static int bcmbca_xrdp_get_link_ksettings(struct net_device *ndev,
					  struct ethtool_link_ksettings *cmd)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	return phylink_ethtool_ksettings_get(port->phylink, cmd);
}

static int bcmbca_xrdp_set_link_ksettings(struct net_device *ndev,
					  const struct ethtool_link_ksettings *cmd)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	return phylink_ethtool_ksettings_set(port->phylink, cmd);
}

/* Free-running 32-bit UniMAC counters; reads do not clear them. */
static const struct {
	char name[ETH_GSTRING_LEN];
	u16 offset;
} bcmbca_xrdp_mib_stats[] = {
	{ "mac_rx_packets", 0x28 },
	{ "mac_rx_bytes", 0x2c },
	{ "mac_rx_fcs_errors", 0x38 },
	{ "mac_rx_alignment_errors", 0x48 },
	{ "mac_rx_length_errors", 0x4c },
	{ "mac_rx_code_errors", 0x50 },
	{ "mac_rx_oversize", 0x58 },
	{ "mac_rx_jabber", 0x5c },
	{ "mac_rx_mtu_errors", 0x60 },
	{ "mac_rx_good_packets", 0x64 },
	{ "mac_rx_runts", 0x100 },
	{ "mac_tx_packets", 0xa8 },
	{ "mac_tx_fcs_errors", 0xbc },
	{ "mac_tx_oversize", 0xc0 },
	{ "mac_tx_bytes", 0xe8 },
	{ "mac_tx_good_packets", 0xec },
};

static int bcmbca_xrdp_get_sset_count(struct net_device *ndev, int sset)
{
	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;
	return ARRAY_SIZE(bcmbca_xrdp_mib_stats);
}

static void bcmbca_xrdp_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	unsigned int i;

	if (sset != ETH_SS_STATS)
		return;
	for (i = 0; i < ARRAY_SIZE(bcmbca_xrdp_mib_stats); i++) {
		memcpy(data, bcmbca_xrdp_mib_stats[i].name, ETH_GSTRING_LEN);
		data += ETH_GSTRING_LEN;
	}
}

static void bcmbca_xrdp_get_stats(struct net_device *ndev,
				  struct ethtool_stats *stats, u64 *data)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);
	struct bcmbca_xrdp_enet *enet = port->enet;
	void __iomem *mib = enet->base + port->data->mib;
	unsigned int i;

	mutex_lock(&enet->lock);
	for (i = 0; i < ARRAY_SIZE(bcmbca_xrdp_mib_stats); i++)
		data[i] = readl(mib + bcmbca_xrdp_mib_stats[i].offset);
	mutex_unlock(&enet->lock);
}

static const struct ethtool_ops bcmbca_xrdp_ethtool_ops = {
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = bcmbca_xrdp_get_link_ksettings,
	.set_link_ksettings = bcmbca_xrdp_set_link_ksettings,
	.get_sset_count = bcmbca_xrdp_get_sset_count,
	.get_strings = bcmbca_xrdp_get_strings,
	.get_ethtool_stats = bcmbca_xrdp_get_stats,
};

static int bcmbca_xrdp_port_create(struct bcmbca_xrdp_enet *enet, struct device_node *np)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	struct phylink_config *config;
	struct bcmbca_xrdp_port *port;
	phy_interface_t interface;
	struct net_device *ndev;
	u32 id, value;
	int ret;

	ret = of_property_read_u32(np, "reg", &id);
	if (ret || id >= variant->num_ports || enet->ports[id])
		return -EINVAL;
	ret = of_get_phy_mode(np, &interface);
	if (ret)
		return ret;
	ndev = alloc_etherdev(sizeof(*port));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, enet->dev);
	ndev->dev.of_node = np;
	port = netdev_priv(ndev);
	port->enet = enet;
	port->ndev = ndev;
	port->data = &variant->ports[id];
	port->mac = enet->base + port->data->mac;
	port->tx_producer = readb(enet->base + variant->runner_sram +
				 port->data->tx_counter);
	skb_queue_head_init(&port->tx_queue);
	ndev->netdev_ops = &bcmbca_xrdp_netdev_ops;
	ndev->ethtool_ops = &bcmbca_xrdp_ethtool_ops;
	ndev->dev_port = id;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = 2000;
	ret = of_get_ethdev_address(np, ndev);
	if (ret == -EPROBE_DEFER)
		goto free_netdev;
	if (ret)
		eth_hw_addr_random(ndev);
	netif_carrier_off(ndev);
	config = &port->phylink_config;
	config->dev = &ndev->dev;
	config->type = PHYLINK_NETDEV;
	config->mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE |
				   MAC_10 | MAC_100 | MAC_1000FD;
	if (port->data->rgmii)
		phy_interface_set_rgmii(config->supported_interfaces);
	else
		__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
	port->phylink = phylink_create(config, of_fwnode_handle(np), interface,
				       &bcmbca_xrdp_mac_ops);
	if (IS_ERR(port->phylink)) {
		ret = PTR_ERR(port->phylink);
		goto free_netdev;
	}
	ret = phylink_fwnode_phy_connect(port->phylink, of_fwnode_handle(np), 0);
	if (ret)
		goto destroy_phylink;

	writel(CMD_PROMISC | CMD_CRC_FWD | CMD_NO_LEN_CHK,
	       port->mac + UMAC_CMD);
	writel(BCMBCA_XRDP_FRAME_LEN, port->mac + UMAC_MAX_FRAME_LEN);
	value = readl(port->mac + UMAC_TX_IPG_LEN);
	writel((value & ~GENMASK(6, 0)) | 12, port->mac + UMAC_TX_IPG_LEN);
	value = readl(enet->base + port->data->mac_top);
	writel(value | BIT(0), enet->base + port->data->mac_top);
	value = readl(enet->base + port->data->mac_top + 4);
	value = (value & ~GENMASK(13, 0)) | BCMBCA_XRDP_FRAME_LEN;
	writel(value, enet->base + port->data->mac_top + 4);
	/* Allow DSA tags in addition to the Ethernet frame. */
	value = BCMBCA_XRDP_FRAME_LEN | (BCMBCA_XRDP_FRAME_LEN << 16);
	writel(value, enet->base + port->data->bbh_rx + BBH_RX_MAXPKT0);
	writel(value, enet->base + port->data->bbh_rx + BBH_RX_MAXPKT1);
	ret = register_netdev(ndev);
	if (ret)
		goto disconnect_phy;
	enet->ports[id] = port;
	netdev_info(ndev, "UniMAC%u, reduced XRDP SRAM datapath\n", id);
	return 0;

disconnect_phy:
	rtnl_lock();
	phylink_disconnect_phy(port->phylink);
	rtnl_unlock();
destroy_phylink:
	phylink_destroy(port->phylink);
free_netdev:
	free_netdev(ndev);
	return ret;
}

static void bcmbca_xrdp_ports_destroy(struct bcmbca_xrdp_enet *enet)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	unsigned int i;

	for (i = 0; i < variant->num_ports; i++) {
		struct bcmbca_xrdp_port *port = enet->ports[i];

		if (!port)
			continue;
		unregister_netdev(port->ndev);
		rtnl_lock();
		phylink_disconnect_phy(port->phylink);
		rtnl_unlock();
		phylink_destroy(port->phylink);
		free_netdev(port->ndev);
		enet->ports[i] = NULL;
	}
}

static int bcm6846_xrdp_qegphy_powerup(void __iomem *ctrl, u32 port_mask)
{
	u32 mask = BCM6846_QEGPHY_RESET | BCM6846_QEGPHY_PLL_250 |
		   BCM6846_QEGPHY_IDDQ_BIAS | BCM6846_QEGPHY_IDDQ_GLOBAL |
		   BCM6846_QEGPHY_EXT_PWR_DOWN;
	u32 val = readl(ctrl);

	/* BCM6846 vendor _phy_cfg(): reset, 250 MHz, power up, 125 MHz. */
	val |= BCM6846_QEGPHY_RESET;
	writel(val, ctrl);
	usleep_range(1000, 2000);
	val |= BCM6846_QEGPHY_PLL_250;
	writel(val, ctrl);
	usleep_range(1000, 2000);
	val &= ~(BCM6846_QEGPHY_IDDQ_BIAS | BCM6846_QEGPHY_IDDQ_GLOBAL);
	writel(val, ctrl);
	usleep_range(1000, 2000);
	/* Preserve the base address at which MDIO discovered the PHYs. */
	val &= ~BCM6846_QEGPHY_EXT_PWR_DOWN;
	val |= FIELD_PREP(BCM6846_QEGPHY_EXT_PWR_DOWN, ~port_mask & 0xf);
	writel(val, ctrl);
	usleep_range(1000, 2000);
	val &= ~BCM6846_QEGPHY_PLL_250;
	writel(val, ctrl);
	val &= ~BCM6846_QEGPHY_RESET;
	writel(val, ctrl);
	usleep_range(1000, 2000);

	return (readl(ctrl) & mask) == (val & mask) ? 0 : -EIO;
}

static int bcm6846_xrdp_phy_init(struct bcmbca_xrdp_enet *enet,
				 struct device_node *ports)
{
	struct device_node *phy_np __free(device_node) = NULL;
	struct phy_device *phydev;
	u32 port_mask = 0;
	int ret;

	for_each_available_child_of_node_scoped(ports, np) {
		u32 id;

		ret = of_property_read_u32(np, "reg", &id);
		if (ret)
			return ret;
		/* UniMAC0..3 use QEGPHY; UniMAC4 is the external RGMII link. */
		if (id >= 4)
			continue;
		port_mask |= BIT(id);
		if (!phy_np) {
			phy_np = of_parse_phandle(np, "phy-handle", 0);
			if (!phy_np)
				return -EINVAL;
		}
	}
	if (!port_mask)
		return 0;

	phydev = of_phy_find_device(phy_np);
	if (!phydev)
		return -EPROBE_DEFER;

	/* All four PHYs share reset. Exclude MDIO until it is released. */
	phy_lock_mdio_bus(phydev);
	ret = bcm6846_xrdp_qegphy_powerup(enet->base + BCM6846_QEGPHY_CTRL,
					  port_mask);
	phy_unlock_mdio_bus(phydev);
	put_device(&phydev->mdio.dev);

	return ret;
}

static int bcmbca_xrdp_probe(struct platform_device *pdev)
{
	const struct bcmbca_xrdp_enet_variant *variant;
	struct device *dev = &pdev->dev;
	struct device_node *ports;
	struct bcmbca_xrdp_enet *enet;
	struct resource *res;
	int ret;

	variant = device_get_match_data(dev);
	if (!variant)
		return -ENODEV;
	enet = devm_kzalloc(dev, struct_size(enet, ports, variant->num_ports),
			    GFP_KERNEL);
	if (!enet)
		return -ENOMEM;
	enet->dev = dev;
	enet->variant = variant;
	enet->rx_seen = devm_bitmap_zalloc(dev, variant->num_buffers, GFP_KERNEL);
	if (!enet->rx_seen)
		return -ENOMEM;
	enet->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(enet->reset))
		return dev_err_probe(dev, PTR_ERR(enet->reset), "cannot get XRDP reset\n");
	ret = devm_clk_bulk_get_all(dev, &enet->clks);
	if (ret <= 0)
		return dev_err_probe(dev, ret ?: -ENODEV, "cannot get XRDP clocks\n");
	enet->num_clks = ret;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) != variant->reg_size)
		return -EINVAL;
	enet->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(enet->base))
		return PTR_ERR(enet->base);
	mutex_init(&enet->lock);
	INIT_DELAYED_WORK(&enet->work, bcmbca_xrdp_work);
	platform_set_drvdata(pdev, enet);
	ports = of_get_child_by_name(dev->of_node, "ethernet-ports");
	if (!ports)
		return -EINVAL;
	ret = bcmbca_xrdp_hw_init(enet);
	if (ret)
		goto put_ports;
	/* The common reset must precede every per-PHY calibration/attach. */
	if (variant == &bcm6846_variant) {
		ret = bcm6846_xrdp_phy_init(enet, ports);
		if (ret) {
			dev_err_probe(dev, ret, "QEGPHY power-up failed\n");
			goto put_ports;
		}
	}
	for_each_available_child_of_node_scoped(ports, np) {
		ret = bcmbca_xrdp_port_create(enet, np);
		if (ret)
			goto destroy_ports;
	}
	of_node_put(ports);
	schedule_delayed_work(&enet->work, 0);
	return 0;

destroy_ports:
	mutex_lock(&enet->lock);
	enet->stopping = true;
	mutex_unlock(&enet->lock);
	bcmbca_xrdp_ports_destroy(enet);
	cancel_delayed_work_sync(&enet->work);
put_ports:
	bcmbca_xrdp_hw_stop(enet);
	of_node_put(ports);
	return dev_err_probe(dev, ret, "failed to initialize XRDP\n");
}

static void bcmbca_xrdp_remove(struct platform_device *pdev)
{
	struct bcmbca_xrdp_enet *enet = platform_get_drvdata(pdev);

	mutex_lock(&enet->lock);
	enet->stopping = true;
	mutex_unlock(&enet->lock);
	bcmbca_xrdp_ports_destroy(enet);
	cancel_delayed_work_sync(&enet->work);
	bcmbca_xrdp_hw_stop(enet);
}

static const struct of_device_id bcmbca_xrdp_of_match[] = {
	{ .compatible = "brcm,bcm6846-enet", .data = &bcm6846_variant },
	{ }
};
MODULE_DEVICE_TABLE(of, bcmbca_xrdp_of_match);

static struct platform_driver bcmbca_xrdp_driver = {
	.probe = bcmbca_xrdp_probe,
	.remove = bcmbca_xrdp_remove,
	.shutdown = bcmbca_xrdp_remove,
	.driver = {
		.name = "bcmbca-xrdp-enet",
		.of_match_table = bcmbca_xrdp_of_match,
	},
};
module_platform_driver(bcmbca_xrdp_driver);

MODULE_DESCRIPTION("Broadcom BCMBCA reduced XRDP Ethernet");
MODULE_LICENSE("GPL");
