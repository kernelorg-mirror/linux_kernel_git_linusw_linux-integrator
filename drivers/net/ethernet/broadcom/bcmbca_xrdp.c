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
 * vendor GEN45 SBPM format. Queue interrupts schedule one NAPI instance
 * for the shared packet engine and per-port GRO. A timer also services TX
 * completions and Runner programs without receive notifications.
 * Core control and the reduced firmware ABI live in bcmbca_runner.c.
 * Firmware must have configured pin multiplexing and external PHYs.
 */

#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
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
#include <linux/spinlock.h>

#include "bcmbca_xrdp.h"
#include "unimac.h"

#define TX_QUEUE_LEN		64
#define POLL_INTERVAL_US		1000

/* UBUS slave Runner queue interrupt controller, separate from RNR_REGS. */
#define QUEUE_IRQ_COUNT		32
#define QUEUE_ISR		0x0
#define QUEUE_ISM		0x4
#define QUEUE_IER		0x8
#define QUEUE_ITR		0xc
#define RX_QUEUE_IRQ		BIT(0)
#define MIB_RX_PAUSE		0x40
#define MIB_TX_PAUSE		0xb4
#define MIB_CONTROL		0x180
#define MIB_COUNTER_RESET	GENMASK(2, 0)
#define BBH_RX_MAXPKT0		0x28
#define BBH_RX_MAXPKT1		0x2c
#define BBH_RX_FLOWCTRL		0x34
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

extern const u8 bcm6846_runner_start[], bcm6846_runner_end[];

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
	.runner = {
		.ctrl = 0xd00000,
		.stride = 0x1000,
		.sram = 0xc00000,
		.num_cores = 3,
		.program = bcm6846_runner_start,
		.program_end = bcm6846_runner_end,
	},
	.reduced = {
		.rx_fifo = 0x800,
		.tx_descriptor = 0x50,
		.tx_destination = 0xdc,
		.rx_fifo_size = 64,
		.rx_irq_mailbox = 0x380,
	},
	.init_data = bcm6846_init_data,
	.init_data_size = ARRAY_SIZE(bcm6846_init_data),
	.init_base = 0x82000000,
	.reg_size = 0x1000000,
	.sbpm = 0xd98000,
	.psram = 0x600000,
	.queue_intr = 0xd97080,
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
	struct bcmbca_xrdp_tx_ring tx_ring;
	u64 rx_alloc_errors;
	u64 tx_credit_waits;
	u32 mib_control_at_probe;
	void __iomem *mac;
	bool running;
};

struct bcmbca_xrdp_enet {
	struct device *dev;
	void __iomem *base;
	struct bcmbca_runner runner;
	struct reset_control *reset;
	struct clk_bulk_data *clks;
	int num_clks;
	const struct bcmbca_xrdp_enet_variant *variant;
	unsigned long *rx_seen;
	struct net_device *napi_dev;
	struct napi_struct napi;
	struct hrtimer poll_timer;
	/* Serialize all CPU accesses to SBPM's shared command registers. */
	spinlock_t lock;
	/* The hard IRQ handler never takes the packet-engine lock. */
	spinlock_t irq_lock;
	/* Only one port may exercise the shared interrupt test register. */
	struct mutex irq_test_lock;
	struct completion irq_test_done;
	int irqs[QUEUE_IRQ_COUNT];
	unsigned int num_irqs;
	u32 irq_test_bit;
	u32 irq_seen;
	u64 irq_events;
	u64 irq_test_events;
	atomic_t timer_runs;
	bool irq_ready;
	/* Protected by irq_lock; writers of rx_irq also hold lock. */
	bool rx_irq;
	bool rx_active;
	u32 rx_generation;
	u32 rx_coalesce_usecs;
	u64 rx_irq_polls;
	u64 rx_polling_polls;
	u64 sbpm_timeouts;
	u64 sbpm_alloc_failures;
	u64 rx_invalid_descriptors;
	u64 rx_list_errors;
	u64 rx_budget_exhausted;
	u64 poll_runs;
	bool hw_accessible;
	bool faulted;
	bool stopping;
	struct bcmbca_xrdp_port *ports[];
};

static void __iomem *bcmbca_xrdp_irq_base(struct bcmbca_xrdp_enet *enet)
{
	return enet->base + enet->variant->queue_intr;
}

/* Caller holds irq_lock. Tests and packet notifications never run together. */
static u32 bcmbca_xrdp_irq_mask(struct bcmbca_xrdp_enet *enet)
{
	return enet->irq_test_bit | (enet->rx_irq ? RX_QUEUE_IRQ : 0);
}

static irqreturn_t bcmbca_xrdp_irq(int irq, void *data)
{
	struct bcmbca_xrdp_enet *enet = data;
	void __iomem *base = bcmbca_xrdp_irq_base(enet);
	u32 pending;
	unsigned int i;
	bool handled = false, schedule = false;

	spin_lock(&enet->irq_lock);
	if (!enet->irq_ready)
		goto out;
	for (i = 0; i < enet->num_irqs; i++)
		if (enet->irqs[i] == irq)
			break;
	if (i == enet->num_irqs)
		goto out;
	pending = readl(base + QUEUE_ISM);
	if (!pending)
		goto out;
	handled = true;
	if ((pending & enet->irq_test_bit) &&
	    (enet->irq_test_bit == BIT(i) || !(pending & BIT(i)))) {
		/* ITR is level-sensitive; clear it before acknowledging ISR. */
		writel(0, base + QUEUE_ITR);
		writel(enet->irq_test_bit, base + QUEUE_ISR);
		readl(base + QUEUE_ISR);
		/* A misrouted test must time out without leaving a level active. */
		if (enet->irq_test_bit == BIT(i)) {
			enet->irq_test_events++;
			complete(&enet->irq_test_done);
		}
	}
	pending &= ~enet->irq_test_bit;
	if (pending) {
		writel(enet->irq_test_bit, base + QUEUE_IER);
		/* Clear ITR too: CPU-originated writes produce a level interrupt. */
		writel(0, base + QUEUE_ITR);
		writel(pending, base + QUEUE_ISR);
		readl(base + QUEUE_IER);
		enet->irq_events++;
		enet->irq_seen |= pending;
		enet->rx_generation++;
		enet->rx_active = true;
		schedule = true;
	}
out:
	spin_unlock(&enet->irq_lock);
	if (schedule)
		napi_schedule_irqoff(&enet->napi);
	return IRQ_RETVAL(handled);
}

static void bcmbca_xrdp_irq_stop(struct bcmbca_xrdp_enet *enet)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&enet->irq_lock, flags);
	enet->irq_ready = false;
	if (enet->hw_accessible) {
		if (enet->variant->reduced.rx_irq_mailbox)
			bcmbca_runner_reduced_irq_enable(&enet->runner, 0);
		writel(0, bcmbca_xrdp_irq_base(enet) + QUEUE_IER);
		writel(0, bcmbca_xrdp_irq_base(enet) + QUEUE_ITR);
		readl(bcmbca_xrdp_irq_base(enet) + QUEUE_IER);
	}
	spin_unlock_irqrestore(&enet->irq_lock, flags);
	for (i = 0; i < enet->num_irqs; i++)
		synchronize_irq(enet->irqs[i]);
}

static int bcmbca_xrdp_irq_init(struct platform_device *pdev)
{
	struct bcmbca_xrdp_enet *enet = platform_get_drvdata(pdev);
	unsigned int i;
	int irq, ret;

	/* Old DTs can continue to use the timer-driven packet interface. */
	if (!of_property_present(pdev->dev.of_node, "interrupts"))
		return 0;
	for (i = 0; i < QUEUE_IRQ_COUNT; i++) {
		char name[16];

		snprintf(name, sizeof(name), "queue%u", i);
		irq = platform_get_irq_byname(pdev, name);
		if (irq < 0)
			return irq;
		ret = devm_request_irq(&pdev->dev, irq, bcmbca_xrdp_irq, 0,
				       dev_name(&pdev->dev), enet);
		if (ret)
			return ret;
		enet->irqs[enet->num_irqs++] = irq;
	}
	return 0;
}

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
	bcmbca_runner_stop(&enet->runner);
}

static void bcmbca_xrdp_hw_stop(struct bcmbca_xrdp_enet *enet)
{
	if (!enet->hw_accessible)
		return;

	bcmbca_xrdp_irq_stop(enet);
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
	writel(0, bcmbca_xrdp_irq_base(enet) + QUEUE_IER);
	writel(0, bcmbca_xrdp_irq_base(enet) + QUEUE_ITR);
	writel(U32_MAX, bcmbca_xrdp_irq_base(enet) + QUEUE_ISR);

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
			if (offset != variant->runner.sram || value)
				return -EINVAL;
			break;
		case 8: /* Assembled program, including instruction RAM NOP padding. */
			ret = bcmbca_runner_load_program(&enet->runner, offset, value,
							 variant->reg_size);
			if (ret)
				return ret;
			break;
		default:
			return -EINVAL;
		}
	}

	/* Flush the last write before exposing the packet interface. */
	bcmbca_runner_sync(&enet->runner);

	return 0;
}

static int bcmbca_xrdp_sbpm_wait(struct bcmbca_xrdp_enet *enet, u32 reg, u32 *value)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	int ret;

	ret = readl_poll_timeout_atomic(enet->base + variant->sbpm + reg, *value,
					*value & SBPM_READY, 1, 1000);
	if (ret) {
		/* A timed-out command may still complete: do not submit another. */
		enet->sbpm_timeouts++;
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
	if (!(value & SBPM_ACK)) {
		enet->sbpm_alloc_failures++;
		return -ENOSPC;
	}
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
		ret = readl_poll_timeout_atomic(enet->base + variant->sbpm + SBPM_NEXT_REPLY,
						value, !(value & SBPM_BUSY), 1, 1000);
		if (ret) {
			enet->sbpm_timeouts++;
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

static bool bcmbca_xrdp_receive(struct bcmbca_xrdp_enet *enet,
				struct sk_buff_head *rx_queue)
{
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	struct bcmbca_xrdp_rx_info info;
	struct bcmbca_xrdp_port *port;
	struct sk_buff *skb = NULL;
	u32 word0, word1, word2;
	int ret;

	if (!bcmbca_runner_reduced_receive(&enet->runner, &word0, &word1, &word2))
		return false;

	if (!bcmbca_xrdp_rx_decode(variant, word0, word1, word2, &info)) {
		enet->rx_invalid_descriptors++;
		/* Without a valid length, we cannot bound the list before freeing. */
		enet->faulted = true;
		goto free_buffer;
	}
	port = enet->ports[info.port];
	if (port && port->running) {
		skb = netdev_alloc_skb_ip_align(port->ndev, info.length);
		if (!skb) {
			port->rx_alloc_errors++;
			port->ndev->stats.rx_dropped++;
		}
	}
	/* Validate discarded frames too, without copying their payload. */
	ret = bcmbca_xrdp_copy_rx(enet, &info, skb ? skb_put(skb, info.length) : NULL);
	if (ret) {
		enet->rx_list_errors++;
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
	__skb_queue_tail(rx_queue, skb);

free_buffer:
	bcmbca_xrdp_free(enet, info.buffer);
	return true;
}

static int bcmbca_xrdp_transmit(struct bcmbca_xrdp_port *port, struct sk_buff *skb)
{
	struct bcmbca_xrdp_enet *enet = port->enet;
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
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

	bcmbca_runner_reduced_transmit(&enet->runner, head, tail, count, skb->len,
				       port->data->tx_destination);
	bcmbca_xrdp_tx_record(&port->tx_ring, skb->len);
	return 0;

free_list:
	if (head != BCMBCA_XRDP_NULL_BUFFER)
		bcmbca_xrdp_free(enet, head);
	return ret;
}

static enum hrtimer_restart bcmbca_xrdp_poll_timer(struct hrtimer *timer)
{
	struct bcmbca_xrdp_enet *enet = container_of(timer, struct bcmbca_xrdp_enet,
						  poll_timer);

	atomic_inc(&enet->timer_runs);
	napi_schedule(&enet->napi);
	return HRTIMER_NORESTART;
}

static int bcmbca_xrdp_poll(struct napi_struct *napi, int budget)
{
	struct bcmbca_xrdp_enet *enet = container_of(napi, struct bcmbca_xrdp_enet, napi);
	const struct bcmbca_xrdp_enet_variant *variant = enet->variant;
	struct sk_buff_head rx_queue;
	struct sk_buff *skb;
	unsigned long flags;
	unsigned int i, tx_work;
	u32 rx_generation = 0;
	bool receive, tx_pending = false;
	int work_done = 0;

	__skb_queue_head_init(&rx_queue);

	spin_lock_bh(&enet->lock);
	if (enet->stopping || enet->faulted)
		goto out;
	spin_lock_irqsave(&enet->irq_lock, flags);
	receive = !enet->rx_irq || enet->rx_active;
	rx_generation = enet->rx_generation;
	if (budget && receive && enet->irq_ready) {
		writel(enet->irq_test_bit, bcmbca_xrdp_irq_base(enet) + QUEUE_IER);
		/* Clear old notifications before draining the SRAM FIFO. */
		if (enet->rx_irq)
			writel(0, bcmbca_xrdp_irq_base(enet) + QUEUE_ITR);
		writel(~enet->irq_test_bit, bcmbca_xrdp_irq_base(enet) + QUEUE_ISR);
	}
	spin_unlock_irqrestore(&enet->irq_lock, flags);
	enet->poll_runs++;
	if (budget && receive) {
		if (enet->rx_irq)
			enet->rx_irq_polls++;
		else
			enet->rx_polling_polls++;
	}
	for (; receive && work_done < budget && !enet->faulted; work_done++)
		if (!bcmbca_xrdp_receive(enet, &rx_queue))
			break;
	if (budget && work_done == budget)
		enet->rx_budget_exhausted++;
	for (i = 0; i < variant->num_ports && !enet->faulted; i++) {
		struct bcmbca_xrdp_port *port = enet->ports[i];
		unsigned int packets = 0, bytes = 0;

		if (!port || !port->running)
			continue;
		for (tx_work = 0; tx_work < BCMBCA_XRDP_TX_FIFO_SIZE && !enet->faulted;
		     tx_work++) {
			u16 length = bcmbca_runner_reduced_tx_length(&enet->runner);
			u8 consumer = bcmbca_runner_reduced_tx_counter(&enet->runner,
								       port->data->tx_counter);

			bytes += bcmbca_xrdp_tx_complete(&port->tx_ring, consumer, &packets);
			if (!bcmbca_xrdp_tx_available(length, port->tx_ring.producer, consumer)) {
				if (!skb_queue_empty(&port->tx_queue))
					port->tx_credit_waits++;
				break;
			}
			skb = skb_dequeue(&port->tx_queue);
			if (!skb)
				break;
			if (bcmbca_xrdp_transmit(port, skb)) {
				port->ndev->stats.tx_dropped++;
				port->ndev->stats.tx_errors++;
				/* These bytes were charged before entering the software queue. */
				packets++;
				bytes += skb->len;
			} else {
				port->ndev->stats.tx_packets++;
				port->ndev->stats.tx_bytes += skb->len;
			}
			dev_consume_skb_any(skb);
		}
		netdev_completed_queue(port->ndev, packets, bytes);
		tx_pending |= !skb_queue_empty(&port->tx_queue) ||
			      port->tx_ring.producer != port->tx_ring.consumer;
		if (skb_queue_len(&port->tx_queue) < TX_QUEUE_LEN / 2)
			netif_wake_queue(port->ndev);
	}
	if (enet->faulted) {
		dev_err(enet->dev, "packet engine stopped after SBPM failure\n");
		spin_lock_irqsave(&enet->irq_lock, flags);
		enet->irq_ready = false;
		writel(0, bcmbca_xrdp_irq_base(enet) + QUEUE_IER);
		spin_unlock_irqrestore(&enet->irq_lock, flags);
		for (i = 0; i < variant->num_ports; i++)
			if (enet->ports[i])
				netif_stop_queue(enet->ports[i]->ndev);
	} else if (budget && work_done < budget && (!enet->rx_irq || tx_pending)) {
		/* Arm before releasing NAPI ownership, while remove is excluded. */
		hrtimer_start(&enet->poll_timer, ns_to_ktime(POLL_INTERVAL_US * NSEC_PER_USEC),
			      HRTIMER_MODE_REL);
	}
out:
	spin_unlock_bh(&enet->lock);
	/* GRO may transmit replies, so do not hold the packet-engine lock. */
	while ((skb = __skb_dequeue(&rx_queue)))
		napi_gro_receive(napi, skb);
	/* Completion can flush GRO and transmit replies, too. */
	if (budget && work_done < budget && napi_complete_done(napi, work_done)) {
		spin_lock_irqsave(&enet->irq_lock, flags);
		if (enet->irq_ready && rx_generation == enet->rx_generation) {
			/* Keep RX active across NAPI's bounded, deferred repolls. */
			enet->rx_active = false;
			/* Do not acknowledge here: arrivals during the drain stay pending. */
			writel(bcmbca_xrdp_irq_mask(enet),
			       bcmbca_xrdp_irq_base(enet) + QUEUE_IER);
			readl(bcmbca_xrdp_irq_base(enet) + QUEUE_IER);
		}
		spin_unlock_irqrestore(&enet->irq_lock, flags);
	}
	/* netif_napi_del() waits for the poll callback before freeing enet. */
	return work_done;
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
	/* Source address for MAC-generated control frames. */
	writel(get_unaligned_be32(port->ndev->dev_addr), port->mac + UMAC_MAC0);
	writel(get_unaligned_be16(port->ndev->dev_addr + 4), port->mac + UMAC_MAC1);
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

	spin_lock_bh(&enet->lock);
	if (enet->faulted) {
		spin_unlock_bh(&enet->lock);
		return -EIO;
	}
	port->running = true;
	spin_unlock_bh(&enet->lock);
	phylink_start(port->phylink);
	netif_start_queue(ndev);
	/* Resume completion service for credits left over from a close. */
	napi_schedule(&enet->napi);
	return 0;
}

static int bcmbca_xrdp_stop(struct net_device *ndev)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	spin_lock_bh(&port->enet->lock);
	port->running = false;
	spin_unlock_bh(&port->enet->lock);
	netif_tx_disable(ndev);
	phylink_stop(port->phylink);
	skb_queue_purge(&port->tx_queue);
	bcmbca_xrdp_tx_reset(&port->tx_ring);
	netdev_reset_queue(ndev);
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
	/* Charge the padded length before NAPI can dequeue the skb. */
	netdev_sent_queue(ndev, skb->len);
	skb_queue_tail(&port->tx_queue, skb);
	if (skb_queue_len(&port->tx_queue) >= TX_QUEUE_LEN)
		netif_stop_queue(ndev);
	napi_schedule(&port->enet->napi);
	return NETDEV_TX_OK;
}

static int bcmbca_xrdp_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	return phylink_mii_ioctl(port->phylink, ifr, cmd);
}

static void bcmbca_xrdp_set_rx_mtu(struct bcmbca_xrdp_port *port, unsigned int mtu)
{
	void __iomem *bbh = port->enet->base + port->data->bbh_rx;
	u32 length = bcmbca_xrdp_frame_length(mtu);
	u32 value = length | (length << 16);

	/* Apply the same limit regardless of the selected BBH receive flow. */
	writel(value, bbh + BBH_RX_MAXPKT0);
	writel(value, bbh + BBH_RX_MAXPKT1);
	readl(bbh + BBH_RX_MAXPKT1);
}

static int bcmbca_xrdp_change_mtu(struct net_device *ndev, int mtu)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);
	struct bcmbca_xrdp_enet *enet = port->enet;

	spin_lock_bh(&enet->lock);
	if (enet->faulted || enet->stopping) {
		spin_unlock_bh(&enet->lock);
		return -EIO;
	}
	/* The SRAM pool and MAC ceiling do not change, even with the port up. */
	bcmbca_xrdp_set_rx_mtu(port, mtu);
	WRITE_ONCE(ndev->mtu, mtu);
	spin_unlock_bh(&enet->lock);

	return 0;
}

static const struct net_device_ops bcmbca_xrdp_netdev_ops = {
	.ndo_open = bcmbca_xrdp_open,
	.ndo_stop = bcmbca_xrdp_stop,
	.ndo_start_xmit = bcmbca_xrdp_start_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_eth_ioctl = bcmbca_xrdp_ioctl,
	.ndo_change_mtu = bcmbca_xrdp_change_mtu,
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

static void bcmbca_xrdp_get_pauseparam(struct net_device *ndev,
				       struct ethtool_pauseparam *pause)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	phylink_ethtool_get_pauseparam(port->phylink, pause);
}

static int bcmbca_xrdp_set_pauseparam(struct net_device *ndev,
				      struct ethtool_pauseparam *pause)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	return phylink_ethtool_set_pauseparam(port->phylink, pause);
}

static void bcmbca_xrdp_get_pause_stats(struct net_device *ndev,
					struct ethtool_pause_stats *stats)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);
	struct bcmbca_xrdp_enet *enet = port->enet;
	void __iomem *mib = enet->base + port->data->mib;

	spin_lock_bh(&enet->lock);
	stats->rx_pause_frames = readl(mib + MIB_RX_PAUSE);
	stats->tx_pause_frames = readl(mib + MIB_TX_PAUSE);
	spin_unlock_bh(&enet->lock);
}

static const char bcmbca_xrdp_tests[][ETH_GSTRING_LEN] = {
	"queue_interrupt_delivery",
};

static void bcmbca_xrdp_self_test(struct net_device *ndev,
				  struct ethtool_test *test, u64 *data)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);
	struct bcmbca_xrdp_enet *enet = port->enet;
	void __iomem *base = bcmbca_xrdp_irq_base(enet);
	unsigned long flags, delivered;
	unsigned int i;
	u32 failed = 0;

	/* Exercise the routing, independently of Runner packet notifications. */
	mutex_lock(&enet->irq_test_lock);
	spin_lock_irqsave(&enet->irq_lock, flags);
	if (enet->rx_irq) {
		spin_unlock_irqrestore(&enet->irq_lock, flags);
		mutex_unlock(&enet->irq_test_lock);
		data[0] = U32_MAX;
		test->flags |= ETH_TEST_FL_FAILED;
		return;
	}
	spin_unlock_irqrestore(&enet->irq_lock, flags);
	for (i = 0; i < QUEUE_IRQ_COUNT; i++) {
		spin_lock_irqsave(&enet->irq_lock, flags);
		if (!enet->irq_ready) {
			spin_unlock_irqrestore(&enet->irq_lock, flags);
			failed = U32_MAX;
			break;
		}
		reinit_completion(&enet->irq_test_done);
		enet->irq_test_bit = BIT(i);
		writel(readl(base + QUEUE_IER) | BIT(i), base + QUEUE_IER);
		writel(BIT(i), base + QUEUE_ITR);
		readl(base + QUEUE_ITR);
		spin_unlock_irqrestore(&enet->irq_lock, flags);

		delivered = wait_for_completion_timeout(&enet->irq_test_done,
							msecs_to_jiffies(100));
		spin_lock_irqsave(&enet->irq_lock, flags);
		if (enet->irq_ready) {
			writel(0, base + QUEUE_ITR);
			writel(BIT(i), base + QUEUE_ISR);
			readl(base + QUEUE_ISR);
		}
		enet->irq_test_bit = 0;
		writel(0, base + QUEUE_IER);
		spin_unlock_irqrestore(&enet->irq_lock, flags);
		if (!delivered)
			failed |= BIT(i);
	}
	mutex_unlock(&enet->irq_test_lock);
	data[0] = failed;
	if (failed)
		test->flags |= ETH_TEST_FL_FAILED;
}

/* Free-running 32-bit UniMAC counters; reads do not clear them. */
static const struct {
	char name[ETH_GSTRING_LEN];
	u16 offset;
} bcmbca_xrdp_mib_stats[] = {
	{ "mac_rx_packets", 0x28 },
	{ "mac_rx_bytes", 0x2c },
	{ "mac_rx_fcs_errors", 0x38 },
	{ "mac_rx_pause_frames", MIB_RX_PAUSE },
	{ "mac_rx_alignment_errors", 0x48 },
	{ "mac_rx_length_errors", 0x4c },
	{ "mac_rx_code_errors", 0x50 },
	{ "mac_rx_oversize", 0x58 },
	{ "mac_rx_jabber", 0x5c },
	{ "mac_rx_mtu_errors", 0x60 },
	{ "mac_rx_good_packets", 0x64 },
	{ "mac_rx_runts", 0x100 },
	{ "mac_tx_packets", 0xa8 },
	{ "mac_tx_pause_frames", MIB_TX_PAUSE },
	{ "mac_tx_fcs_errors", 0xbc },
	{ "mac_tx_oversize", 0xc0 },
	{ "mac_tx_bytes", 0xe8 },
	{ "mac_tx_good_packets", 0xec },
};

/* Shared counters describe the packet engine across all ports. */
static const char bcmbca_xrdp_sw_stats[][ETH_GSTRING_LEN] = {
	"shared_sbpm_timeouts",
	"shared_sbpm_alloc_failures",
	"shared_rx_invalid_descriptors",
	"shared_rx_list_errors",
	"shared_rx_budget_exhausted",
	"shared_engine_faulted",
	"rx_alloc_errors",
	"tx_credit_waits",
	"tx_queued_packets",
	"tx_pending_packets",
	"mac_mib_control",
	"mac_mib_control_at_probe",
	"shared_poll_runs",
	"poll_interval_us",
	"mac_frame_limit",
	"bbh_frame_limit0",
	"bbh_frame_limit1",
	"mac_command",
	"mac_pause_quanta",
	"mac_pause_refresh",
	"bbh_flow_control",
	"queue_irqs_registered",
	"shared_queue_irq_events",
	"shared_queue_irq_seen",
	"shared_irq_test_events",
	"shared_poll_timer_runs",
	"queue_irq_status",
	"queue_irq_enable",
	"rx_irq_mode",
	"shared_rx_irq_polls",
	"shared_rx_polling_polls",
	"shared_runner_notifications",
};

static const char bcmbca_xrdp_priv_flags[][ETH_GSTRING_LEN] = {
	"rx-irq",
};

static u32 bcmbca_xrdp_get_priv_flags(struct net_device *ndev)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	return READ_ONCE(port->enet->rx_irq);
}

/*
 * The dummy netdevice owns the NAPI instance shared by all ports. Defer
 * rearming for one empty poll, then leave RX asleep until the next IRQ.
 */
static void bcmbca_xrdp_set_irq_coalesce(struct bcmbca_xrdp_enet *enet)
{
	u32 usecs = enet->rx_irq ? enet->rx_coalesce_usecs : 0;

	/* These settings are read locklessly by the NAPI core. */
	WRITE_ONCE(enet->napi.gro_flush_timeout, usecs * NSEC_PER_USEC);
	WRITE_ONCE(enet->napi.defer_hard_irqs, usecs ? 1 : 0);
}

static int bcmbca_xrdp_get_coalesce(struct net_device *ndev,
				    struct ethtool_coalesce *ec,
				    struct kernel_ethtool_coalesce *kernel_ec,
				    struct netlink_ext_ack *extack)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);

	ec->rx_coalesce_usecs = READ_ONCE(port->enet->rx_coalesce_usecs);
	return 0;
}

static int bcmbca_xrdp_set_coalesce(struct net_device *ndev,
				    struct ethtool_coalesce *ec,
				    struct kernel_ethtool_coalesce *kernel_ec,
				    struct netlink_ext_ack *extack)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);
	struct bcmbca_xrdp_enet *enet = port->enet;

	if (!enet->variant->reduced.rx_irq_mailbox || enet->num_irqs != QUEUE_IRQ_COUNT)
		return -EOPNOTSUPP;
	if (ec->rx_coalesce_usecs > POLL_INTERVAL_US)
		return -EINVAL;
	/* Serialize configuration through different ports of the same engine. */
	netdev_lock(enet->napi_dev);
	spin_lock_bh(&enet->lock);
	WRITE_ONCE(enet->rx_coalesce_usecs, ec->rx_coalesce_usecs);
	bcmbca_xrdp_set_irq_coalesce(enet);
	spin_unlock_bh(&enet->lock);
	netdev_unlock(enet->napi_dev);
	return 0;
}

/* This setting applies to the shared RX FIFO, hence to all ports. */
static int bcmbca_xrdp_set_priv_flags(struct net_device *ndev, u32 flags)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);
	struct bcmbca_xrdp_enet *enet = port->enet;
	unsigned long irqflags;
	int ret = 0;

	if (flags & ~BIT(0))
		return -EINVAL;
	if (!enet->variant->reduced.rx_irq_mailbox || enet->num_irqs != QUEUE_IRQ_COUNT)
		return -EOPNOTSUPP;
	mutex_lock(&enet->irq_test_lock);
	netdev_lock(enet->napi_dev);
	spin_lock_bh(&enet->lock);
	spin_lock_irqsave(&enet->irq_lock, irqflags);
	if (!enet->irq_ready || enet->stopping || enet->faulted) {
		ret = -EIO;
		goto out;
	}
	if (enet->rx_irq == !!flags)
		goto out;
	writel(0, bcmbca_xrdp_irq_base(enet) + QUEUE_IER);
	writel(0, bcmbca_xrdp_irq_base(enet) + QUEUE_ITR);
	writel(U32_MAX, bcmbca_xrdp_irq_base(enet) + QUEUE_ISR);
	WRITE_ONCE(enet->rx_irq, !!flags);
	bcmbca_xrdp_set_irq_coalesce(enet);
	bcmbca_runner_reduced_irq_enable(&enet->runner, flags ? RX_QUEUE_IRQ : 0);
	/* Start a drain on transition; subsequent RX batches require an IRQ. */
	enet->rx_generation++;
	enet->rx_active = true;
	napi_schedule(&enet->napi);
out:
	spin_unlock_irqrestore(&enet->irq_lock, irqflags);
	spin_unlock_bh(&enet->lock);
	netdev_unlock(enet->napi_dev);
	mutex_unlock(&enet->irq_test_lock);
	return ret;
}

static int bcmbca_xrdp_get_sset_count(struct net_device *ndev, int sset)
{
	if (sset == ETH_SS_TEST)
		return ARRAY_SIZE(bcmbca_xrdp_tests);
	if (sset == ETH_SS_PRIV_FLAGS)
		return ARRAY_SIZE(bcmbca_xrdp_priv_flags);
	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;
	return ARRAY_SIZE(bcmbca_xrdp_mib_stats) + ARRAY_SIZE(bcmbca_xrdp_sw_stats);
}

static void bcmbca_xrdp_get_strings(struct net_device *ndev, u32 sset, u8 *data)
{
	unsigned int i;

	if (sset == ETH_SS_PRIV_FLAGS) {
		memcpy(data, bcmbca_xrdp_priv_flags, sizeof(bcmbca_xrdp_priv_flags));
		return;
	}
	if (sset == ETH_SS_TEST) {
		memcpy(data, bcmbca_xrdp_tests, sizeof(bcmbca_xrdp_tests));
		return;
	}
	if (sset != ETH_SS_STATS)
		return;
	for (i = 0; i < ARRAY_SIZE(bcmbca_xrdp_mib_stats); i++) {
		memcpy(data, bcmbca_xrdp_mib_stats[i].name, ETH_GSTRING_LEN);
		data += ETH_GSTRING_LEN;
	}
	memcpy(data, bcmbca_xrdp_sw_stats, sizeof(bcmbca_xrdp_sw_stats));
}

static void bcmbca_xrdp_get_stats(struct net_device *ndev,
				  struct ethtool_stats *stats, u64 *data)
{
	struct bcmbca_xrdp_port *port = netdev_priv(ndev);
	struct bcmbca_xrdp_enet *enet = port->enet;
	void __iomem *mib = enet->base + port->data->mib;
	unsigned long flags;
	unsigned int i;

	spin_lock_bh(&enet->lock);
	for (i = 0; i < ARRAY_SIZE(bcmbca_xrdp_mib_stats); i++)
		data[i] = readl(mib + bcmbca_xrdp_mib_stats[i].offset);
	data[i++] = enet->sbpm_timeouts;
	data[i++] = enet->sbpm_alloc_failures;
	data[i++] = enet->rx_invalid_descriptors;
	data[i++] = enet->rx_list_errors;
	data[i++] = enet->rx_budget_exhausted;
	data[i++] = enet->faulted;
	data[i++] = port->rx_alloc_errors;
	data[i++] = port->tx_credit_waits;
	data[i++] = skb_queue_len(&port->tx_queue);
	data[i++] = (u8)(port->tx_ring.producer - port->tx_ring.consumer);
	data[i++] = readl(mib + MIB_CONTROL);
	data[i++] = port->mib_control_at_probe;
	data[i++] = enet->poll_runs;
	data[i++] = POLL_INTERVAL_US;
	data[i++] = readl(port->mac + UMAC_MAX_FRAME_LEN);
	data[i++] = readl(enet->base + port->data->bbh_rx + BBH_RX_MAXPKT0);
	data[i++] = readl(enet->base + port->data->bbh_rx + BBH_RX_MAXPKT1);
	data[i++] = readl(port->mac + UMAC_CMD);
	data[i++] = readl(port->mac + UMAC_PAUSE_QUANTA);
	data[i++] = readl(port->mac + UMAC_PAUSE_CTRL);
	data[i++] = readl(enet->base + port->data->bbh_rx + BBH_RX_FLOWCTRL);
	spin_lock_irqsave(&enet->irq_lock, flags);
	data[i++] = enet->num_irqs;
	data[i++] = enet->irq_events;
	data[i++] = enet->irq_seen;
	data[i++] = enet->irq_test_events;
	data[i++] = (u32)atomic_read(&enet->timer_runs);
	data[i++] = readl(bcmbca_xrdp_irq_base(enet) + QUEUE_ISR);
	data[i++] = readl(bcmbca_xrdp_irq_base(enet) + QUEUE_IER);
	data[i++] = enet->rx_irq;
	data[i++] = enet->rx_irq_polls;
	data[i++] = enet->rx_polling_polls;
	data[i++] = bcmbca_runner_reduced_irq_count(&enet->runner);
	spin_unlock_irqrestore(&enet->irq_lock, flags);
	spin_unlock_bh(&enet->lock);
}

static const struct ethtool_ops bcmbca_xrdp_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_RX_USECS,
	.get_coalesce = bcmbca_xrdp_get_coalesce,
	.set_coalesce = bcmbca_xrdp_set_coalesce,
	.get_priv_flags = bcmbca_xrdp_get_priv_flags,
	.set_priv_flags = bcmbca_xrdp_set_priv_flags,
	.get_link = ethtool_op_get_link,
	.get_link_ksettings = bcmbca_xrdp_get_link_ksettings,
	.set_link_ksettings = bcmbca_xrdp_set_link_ksettings,
	.get_pauseparam = bcmbca_xrdp_get_pauseparam,
	.set_pauseparam = bcmbca_xrdp_set_pauseparam,
	.get_pause_stats = bcmbca_xrdp_get_pause_stats,
	.get_sset_count = bcmbca_xrdp_get_sset_count,
	.get_strings = bcmbca_xrdp_get_strings,
	.get_ethtool_stats = bcmbca_xrdp_get_stats,
	.self_test = bcmbca_xrdp_self_test,
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
	port->tx_ring.producer = bcmbca_runner_reduced_tx_counter(&enet->runner,
								  port->data->tx_counter);
	port->tx_ring.consumer = port->tx_ring.producer;
	skb_queue_head_init(&port->tx_queue);
	ndev->netdev_ops = &bcmbca_xrdp_netdev_ops;
	ndev->ethtool_ops = &bcmbca_xrdp_ethtool_ops;
	ndev->dev_port = id;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = BCMBCA_XRDP_MAX_MTU;
	ret = of_get_ethdev_address(np, ndev);
	if (ret && (ret == -EPROBE_DEFER || of_property_present(np, "nvmem-cells"))) {
		dev_err_probe(enet->dev, ret, "Failed to read MAC address for port %u\n", id);
		goto free_netdev;
	}
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
	/* Initialize all MIB counter banks before enabling the port. */
	value = readl(enet->base + port->data->mib + MIB_CONTROL);
	port->mib_control_at_probe = value;
	writel(value | MIB_COUNTER_RESET,
	       enet->base + port->data->mib + MIB_CONTROL);
	readl(enet->base + port->data->mib + MIB_CONTROL);
	writel(value & ~MIB_COUNTER_RESET,
	       enet->base + port->data->mib + MIB_CONTROL);
	readl(enet->base + port->data->mib + MIB_CONTROL);
	writel(BCMBCA_XRDP_FRAME_LEN, port->mac + UMAC_MAX_FRAME_LEN);
	value = readl(port->mac + UMAC_TX_IPG_LEN);
	writel((value & ~GENMASK(6, 0)) | 12, port->mac + UMAC_TX_IPG_LEN);
	value = readl(enet->base + port->data->mac_top);
	writel(value | BIT(0), enet->base + port->data->mac_top);
	value = readl(enet->base + port->data->mac_top + 4);
	value = (value & ~GENMASK(13, 0)) | BCMBCA_XRDP_FRAME_LEN;
	writel(value, enet->base + port->data->mac_top + 4);
	bcmbca_xrdp_set_rx_mtu(port, ndev->mtu);
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
	unsigned long flags;
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
	enet->runner.base = enet->base;
	enet->runner.data = &variant->runner;
	enet->runner.reduced = &variant->reduced;
	spin_lock_init(&enet->lock);
	spin_lock_init(&enet->irq_lock);
	mutex_init(&enet->irq_test_lock);
	init_completion(&enet->irq_test_done);
	hrtimer_setup(&enet->poll_timer, bcmbca_xrdp_poll_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	platform_set_drvdata(pdev, enet);
	ports = of_get_child_by_name(dev->of_node, "ethernet-ports");
	if (!ports)
		return -EINVAL;
	ret = bcmbca_xrdp_hw_init(enet);
	if (ret)
		goto put_ports;
	if (variant->reduced.rx_irq_mailbox)
		bcmbca_runner_reduced_irq_address(&enet->runner,
						  res->start + variant->queue_intr + QUEUE_ITR);
	/* The common reset must precede every per-PHY calibration/attach. */
	if (variant == &bcm6846_variant) {
		ret = bcm6846_xrdp_phy_init(enet, ports);
		if (ret) {
			dev_err_probe(dev, ret, "QEGPHY power-up failed\n");
			goto put_ports;
		}
	}
	/* All ports share the Runner RX FIFO and the SBPM command interface. */
	enet->napi_dev = alloc_netdev_dummy(0);
	if (!enet->napi_dev) {
		ret = -ENOMEM;
		goto put_ports;
	}
	netif_napi_add(enet->napi_dev, &enet->napi, bcmbca_xrdp_poll);
	ret = bcmbca_xrdp_irq_init(pdev);
	if (ret)
		goto destroy_ports;
	enet->rx_irq = variant->reduced.rx_irq_mailbox && enet->num_irqs == QUEUE_IRQ_COUNT;
	enet->rx_active = enet->rx_irq;
	for_each_available_child_of_node_scoped(ports, np) {
		ret = bcmbca_xrdp_port_create(enet, np);
		if (ret)
			goto destroy_ports;
	}
	of_node_put(ports);
	napi_enable(&enet->napi);
	/* IRQ sources remain masked until the first NAPI pass completes. */
	spin_lock_irqsave(&enet->irq_lock, flags);
	enet->irq_ready = enet->num_irqs == QUEUE_IRQ_COUNT;
	if (enet->rx_irq)
		bcmbca_runner_reduced_irq_enable(&enet->runner, RX_QUEUE_IRQ);
	spin_unlock_irqrestore(&enet->irq_lock, flags);
	napi_schedule(&enet->napi);
	return 0;

destroy_ports:
	bcmbca_xrdp_ports_destroy(enet);
	netif_napi_del(&enet->napi);
	free_netdev(enet->napi_dev);
put_ports:
	bcmbca_xrdp_hw_stop(enet);
	of_node_put(ports);
	return dev_err_probe(dev, ret, "failed to initialize XRDP\n");
}

static void bcmbca_xrdp_remove(struct platform_device *pdev)
{
	struct bcmbca_xrdp_enet *enet = platform_get_drvdata(pdev);

	spin_lock_bh(&enet->lock);
	enet->stopping = true;
	spin_unlock_bh(&enet->lock);
	bcmbca_xrdp_irq_stop(enet);
	hrtimer_cancel(&enet->poll_timer);
	napi_disable(&enet->napi);
	/* Flush GRO before freeing any of the port devices. */
	netif_napi_del(&enet->napi);
	bcmbca_xrdp_ports_destroy(enet);
	free_netdev(enet->napi_dev);
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
