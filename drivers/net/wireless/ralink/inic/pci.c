// SPDX-License-Identifier: GPL-2.0-only
/* Ralink RT2880 iNIC PCI packet transport. */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "inic.h"

#define INIC_INT_STATUS		0x10
#define INIC_INT_ENABLE		0x14
#define INIC_DMA_CFG		0x100
#define INIC_DELAY_INT		0x10c
#define INIC_TX_BASE		0x110
#define INIC_TX_MAX		0x114
#define INIC_TX_CPU		0x118
#define INIC_RX_BASE		0x130
#define INIC_RX_MAX		0x134
#define INIC_RX_CPU		0x138
#define INIC_RESET		0xfffc
#define INIC_INT_TX		BIT(1)
#define INIC_INT_RX		BIT(2)
#define INIC_INTERRUPTS		(INIC_INT_TX | INIC_INT_RX)
#define INIC_TX_ENABLE		BIT(0)
#define INIC_TX_BUSY		BIT(1)
#define INIC_RX_ENABLE		BIT(2)
#define INIC_RX_BUSY		BIT(3)
#define INIC_TX_WRITEBACK	BIT(6)
#define INIC_DONE		BIT(31)
#define INIC_LAST		BIT(30)
#define INIC_LENGTH		GENMASK(29, 16)
#define INIC_DESC_BYTES		((INIC_RX_COUNT + INIC_TX_COUNT) * sizeof(struct inic_desc))

static void inic_write(struct inic *inic, u32 reg, u32 value)
{
	writel(value, inic->regs + reg);
}

static void inic_reset(struct inic *inic)
{
	unsigned long flags;

	/* Other devices can invoke our shared handler while the BARs are absent. */
	spin_lock_irqsave(&inic->lock, flags);
	WRITE_ONCE(inic->running, false);
	spin_unlock_irqrestore(&inic->lock, flags);
	synchronize_irq(inic->pdev->irq);
	/* Reset loses the BARs and command register on this conventional PCI NIC. */
	pci_save_state(inic->pdev);
	inic_write(inic, INIC_RESET, 1);
	msleep(100);
	pci_restore_state(inic->pdev);
}

static void inic_free_rings(struct inic *inic)
{
	struct device *dev = &inic->pdev->dev;
	int i;

	for (i = 0; i < INIC_TX_COUNT; i++) {
		if (!inic->tx[i].skb)
			continue;
		dma_unmap_single(dev, inic->tx[i].dma, inic->tx[i].skb->len,
				 DMA_TO_DEVICE);
		if (inic->tx[i].sensitive)
			memzero_explicit(inic->tx[i].skb->data, inic->tx[i].skb->len);
		dev_kfree_skb_any(inic->tx[i].skb);
		inic->tx[i].skb = NULL;
	}
	for (i = 0; i < INIC_RX_COUNT; i++) {
		if (!inic->rx[i].skb)
			continue;
		dma_unmap_single(dev, inic->rx[i].dma, INIC_BUFFER_SIZE,
				 DMA_FROM_DEVICE);
		dev_kfree_skb_any(inic->rx[i].skb);
		inic->rx[i].skb = NULL;
	}
	if (inic->rx_desc) {
		dma_free_coherent(dev, INIC_DESC_BYTES, inic->rx_desc, inic->desc_dma);
		inic->rx_desc = NULL;
		inic->tx_desc = NULL;
	}
}

static int inic_alloc_rings(struct inic *inic)
{
	struct device *dev = &inic->pdev->dev;
	int i;

	inic->rx_desc = dma_alloc_coherent(dev, INIC_DESC_BYTES,
					   &inic->desc_dma, GFP_KERNEL);
	if (!inic->rx_desc)
		return -ENOMEM;
	memset(inic->rx_desc, 0, INIC_DESC_BYTES);
	inic->tx_desc = inic->rx_desc + INIC_RX_COUNT;
	for (i = 0; i < INIC_TX_COUNT; i++) {
		inic->tx_desc[i].control = cpu_to_le32(INIC_DONE | INIC_LAST);
		inic->tx_desc[i].info = cpu_to_le32(BIT(24)); /* Destination port 1. */
	}
	/*
	 * RX descriptors have no known capacity field. The pinned runtime
	 * firmware's PCI sender at 0x880073dc rejects frames above 1536 bytes
	 * (length check at 0x88007418) before queuing DMA, for both data and
	 * control traffic. The completion length check cannot bound DMA writes.
	 */
	for (i = 0; i < INIC_RX_COUNT; i++) {
		struct sk_buff *skb;
		dma_addr_t dma;

		skb = netdev_alloc_skb_ip_align(inic->netdev, INIC_BUFFER_SIZE);
		if (!skb)
			goto fail;
		dma = dma_map_single(dev, skb->data, INIC_BUFFER_SIZE, DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, dma)) {
			dev_kfree_skb_any(skb);
			goto fail;
		}
		inic->rx[i].skb = skb;
		inic->rx[i].dma = dma;
		inic->rx_desc[i].address = cpu_to_le32(dma);
		inic->rx_desc[i].control = cpu_to_le32(INIC_LAST);
	}
	inic->tx_head = 0;
	inic->tx_tail = 0;
	inic->rx_head = 0;
	return 0;
fail:
	inic_free_rings(inic);
	return -ENOMEM;
}

/* lock held; the DMA engine has returned ownership before unmapping. */
static void inic_tx_complete(struct inic *inic)
{
	while (inic->tx_tail != inic->tx_head) {
		u32 index = inic->tx_tail % INIC_TX_COUNT;
		struct inic_buffer *buf = &inic->tx[index];

		if (!(le32_to_cpu(READ_ONCE(inic->tx_desc[index].control)) & INIC_DONE))
			break;
		dma_rmb();
		dma_unmap_single(&inic->pdev->dev, buf->dma, buf->skb->len,
				 DMA_TO_DEVICE);
		if (buf->sensitive)
			memzero_explicit(buf->skb->data, buf->skb->len);
		dev_consume_skb_any(buf->skb);
		buf->skb = NULL;
		inic->tx_tail++;
	}
	if (inic->ap_started && netif_queue_stopped(inic->netdev) &&
	    inic->tx_head - inic->tx_tail < INIC_TX_COUNT - 4)
		netif_wake_queue(inic->netdev);
}

int inic_send(struct inic *inic, struct sk_buff *skb, bool control)
{
	struct device *dev = &inic->pdev->dev;
	unsigned long flags;
	dma_addr_t dma;
	u32 index;
	int ret = -ENOSPC;

	spin_lock_irqsave(&inic->lock, flags);
	if (!inic->running)
		goto unlock;
	inic_tx_complete(inic);
	/* Leave slots for bootstrap/control traffic when the data queue is full. */
	if (inic->tx_head - inic->tx_tail >= INIC_TX_COUNT - (control ? 1 : 4)) {
		if (!control)
			netif_stop_queue(inic->netdev);
		goto unlock;
	}
	if (skb_linearize(skb)) {
		ret = -ENOMEM;
		goto unlock;
	}
	dma = dma_map_single(dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma)) {
		ret = -EIO;
		goto unlock;
	}
	index = inic->tx_head % INIC_TX_COUNT;
	inic->tx[index].skb = skb;
	inic->tx[index].dma = dma;
	inic->tx[index].sensitive = control;
	inic->tx_desc[index].address = cpu_to_le32(dma);
	dma_wmb();
	WRITE_ONCE(inic->tx_desc[index].control,
		   cpu_to_le32(INIC_LAST | FIELD_PREP(INIC_LENGTH, skb->len)));
	inic->tx_head++;
	inic_write(inic, INIC_TX_CPU, inic->tx_head % INIC_TX_COUNT);
	if (!control && inic->tx_head - inic->tx_tail >= INIC_TX_COUNT - 4)
		netif_stop_queue(inic->netdev);
	ret = 0;
unlock:
	spin_unlock_irqrestore(&inic->lock, flags);
	return ret;
}

static void inic_rx_data(struct inic *inic, struct sk_buff *skb)
{
	struct vlan_ethhdr *vh;

	if (skb->len < ETH_HLEN)
		goto drop;
	if (eth_hdr(skb)->h_proto == htons(0xffff)) {
		inic_receive_control(inic, skb);
		goto free;
	}
	/* Pair with the AP policy publication in inic_start_ap(). */
	if (!smp_load_acquire(&inic->ap_started) || skb->len < VLAN_ETH_HLEN)
		goto drop;
	/* The outer tag selects a firmware interface, not an Ethernet VLAN. */
	vh = (struct vlan_ethhdr *)skb->data;
	if (vh->h_vlan_proto != htons(ETH_P_8021Q) ||
	    (ntohs(vh->h_vlan_TCI) & VLAN_VID_MASK) != 1)
		goto drop;
	memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
	skb_pull(skb, VLAN_HLEN);
	if (!inic_port_allowed(inic, (const void *)skb->data, false))
		goto drop;
	skb_reset_mac_header(skb);
	skb->protocol = eth_hdr(skb)->h_proto;
	if (vlan_get_protocol(skb) == htons(0xffff))
		goto drop;
	skb->protocol = eth_type_trans(skb, inic->netdev);
	DEV_STATS_INC(inic->netdev, rx_packets);
	DEV_STATS_ADD(inic->netdev, rx_bytes, skb->len + ETH_HLEN);
	napi_gro_receive(&inic->napi, skb);
	return;
drop:
	DEV_STATS_INC(inic->netdev, rx_dropped);
free:
	dev_kfree_skb_any(skb);
}

static int inic_poll(struct napi_struct *napi, int budget)
{
	struct inic *inic = container_of(napi, struct inic, napi);
	struct device *dev = &inic->pdev->dev;
	unsigned long flags;
	int done = 0;

	spin_lock_irqsave(&inic->lock, flags);
	inic_tx_complete(inic);
	spin_unlock_irqrestore(&inic->lock, flags);
	while (done < budget) {
		u32 index = inic->rx_head % INIC_RX_COUNT;
		struct inic_buffer *buf = &inic->rx[index];
		struct inic_desc *desc = &inic->rx_desc[index];
		u32 status = le32_to_cpu(READ_ONCE(desc->control));
		struct sk_buff *skb;
		u32 len;

		if (!(status & INIC_DONE))
			break;
		dma_rmb();
		len = FIELD_GET(INIC_LENGTH, status);
		dma_sync_single_for_cpu(dev, buf->dma, INIC_BUFFER_SIZE, DMA_FROM_DEVICE);
		if (len >= ETH_HLEN && len <= INIC_BUFFER_SIZE && (status & INIC_LAST)) {
			skb = napi_alloc_skb(napi, len);
			if (skb) {
				skb_put_data(skb, buf->skb->data, len);
				skb_reset_mac_header(skb);
				inic_rx_data(inic, skb);
			} else {
				DEV_STATS_INC(inic->netdev, rx_dropped);
			}
		} else {
			DEV_STATS_INC(inic->netdev, rx_length_errors);
		}
		dma_sync_single_for_device(dev, buf->dma, INIC_BUFFER_SIZE, DMA_FROM_DEVICE);
		dma_wmb();
		WRITE_ONCE(desc->control, cpu_to_le32(INIC_LAST));
		inic_write(inic, INIC_RX_CPU, index);
		inic->rx_head++;
		done++;
	}
	if (done < budget && napi_complete_done(napi, done)) {
		spin_lock_irqsave(&inic->lock, flags);
		if (inic->running)
			inic_write(inic, INIC_INT_ENABLE, INIC_INTERRUPTS);
		spin_unlock_irqrestore(&inic->lock, flags);
	}
	return done;
}

static irqreturn_t inic_irq(int irq, void *data)
{
	struct inic *inic = data;
	unsigned long flags;
	irqreturn_t ret = IRQ_NONE;
	u32 status;

	spin_lock_irqsave(&inic->lock, flags);
	if (!inic->running)
		goto unlock;
	status = readl(inic->regs + INIC_INT_STATUS);
	if (status == U32_MAX || !(status & INIC_INTERRUPTS))
		goto unlock;
	inic_write(inic, INIC_INT_ENABLE, 0);
	inic_write(inic, INIC_INT_STATUS, status & INIC_INTERRUPTS);
	napi_schedule_irqoff(&inic->napi);
	ret = IRQ_HANDLED;
unlock:
	spin_unlock_irqrestore(&inic->lock, flags);
	return ret;
}

int inic_hw_start(struct inic *inic)
{
	unsigned long flags;
	u32 cfg;
	int ret;

	mutex_lock(&inic->hw_mutex);
	if (inic->shutting_down) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (inic->running) {
		ret = -EBUSY;
		goto unlock;
	}
	inic_reset(inic);
	pci_write_config_byte(inic->pdev, PCI_LATENCY_TIMER, 0x20);
	inic_write(inic, INIC_INT_ENABLE, 0);
	cfg = readl(inic->regs + INIC_DMA_CFG);
	cfg &= ~(INIC_TX_ENABLE | INIC_RX_ENABLE | INIC_TX_WRITEBACK);
	inic_write(inic, INIC_DMA_CFG, cfg);
	ret = readl_poll_timeout(inic->regs + INIC_DMA_CFG, cfg,
				 !(cfg & (INIC_TX_BUSY | INIC_RX_BUSY)), 100, 100000);
	if (ret)
		goto unlock;
	ret = inic_alloc_rings(inic);
	if (ret)
		goto unlock;
	inic_write(inic, INIC_RX_BASE, lower_32_bits(inic->desc_dma));
	inic_write(inic, INIC_TX_BASE,
		   lower_32_bits(inic->desc_dma + INIC_RX_COUNT * sizeof(struct inic_desc)));
	inic_write(inic, INIC_RX_MAX, INIC_RX_COUNT);
	inic_write(inic, INIC_TX_MAX, INIC_TX_COUNT);
	inic_write(inic, INIC_TX_CPU, 0);
	inic_write(inic, INIC_RX_CPU, INIC_RX_COUNT - 1);
	/* Delayed TX IRQ: at most 64 packets or 255 * 20 microseconds. */
	inic_write(inic, INIC_DELAY_INT, BIT(31) | (64 << 24) | (255 << 16));
	inic_write(inic, INIC_INT_STATUS, INIC_INTERRUPTS);
	napi_enable(&inic->napi);
	spin_lock_irqsave(&inic->lock, flags);
	WRITE_ONCE(inic->running, true);
	spin_unlock_irqrestore(&inic->lock, flags);
	pci_set_master(inic->pdev);
	dma_wmb();
	inic_write(inic, INIC_DMA_CFG,
		   cfg | INIC_TX_ENABLE | INIC_RX_ENABLE | INIC_TX_WRITEBACK);
	inic_write(inic, INIC_INT_ENABLE, INIC_INTERRUPTS);
	ret = 0;
unlock:
	mutex_unlock(&inic->hw_mutex);
	return ret;
}

static void __inic_hw_stop(struct inic *inic)
{
	unsigned long flags;
	u32 cfg;
	int ret;

	if (!inic->running)
		return;
	spin_lock_irqsave(&inic->lock, flags);
	WRITE_ONCE(inic->running, false);
	WRITE_ONCE(inic->ap_started, false);
	if (inic->command_pending) {
		inic->command_error = -ENETDOWN;
		inic->command_pending = false;
		complete(&inic->command_reply);
	}
	inic_write(inic, INIC_INT_ENABLE, 0);
	spin_unlock_irqrestore(&inic->lock, flags);
	/* Completion cannot wake TX again once ap_started has been cleared. */
	netif_tx_disable(inic->netdev);
	netif_carrier_off(inic->netdev);
	synchronize_irq(inic->pdev->irq);
	napi_disable(&inic->napi);
	cfg = readl(inic->regs + INIC_DMA_CFG);
	inic_write(inic, INIC_DMA_CFG,
		   cfg & ~(INIC_TX_ENABLE | INIC_RX_ENABLE | INIC_TX_WRITEBACK));
	ret = readl_poll_timeout(inic->regs + INIC_DMA_CFG, cfg,
				 !(cfg & (INIC_TX_BUSY | INIC_RX_BUSY)), 100, 100000);
	if (ret)
		dev_warn(&inic->pdev->dev, "DMA stop timed out; resetting card\n");
	pci_clear_master(inic->pdev);
	/* Reset also stops the autonomous AP, even if DMA was already idle. */
	inic_reset(inic);
	inic_write(inic, INIC_INT_ENABLE, 0);
	inic_free_rings(inic);
}

void inic_hw_stop(struct inic *inic)
{
	mutex_lock(&inic->hw_mutex);
	__inic_hw_stop(inic);
	mutex_unlock(&inic->hw_mutex);
}

static int inic_open(struct net_device *dev)
{
	netif_carrier_off(dev);
	netif_start_queue(dev);
	return 0;
}

static int inic_stop(struct net_device *dev)
{
	struct inic *inic = *(struct inic **)netdev_priv(dev);

	inic_hw_stop(inic);
	return 0;
}

static netdev_tx_t inic_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct inic *inic = *(struct inic **)netdev_priv(dev);
	unsigned int len = skb->len;
	int ret;

	/* Pair with the AP policy publication in inic_start_ap(). */
	if (!smp_load_acquire(&inic->ap_started) || !pskb_may_pull(skb, ETH_HLEN) ||
	    get_unaligned_be16(skb->data + 12) == 0xffff ||
	    vlan_get_protocol(skb) == htons(0xffff))
		goto drop;
	if (!inic_port_allowed(inic, (const void *)skb->data, true))
		goto drop;
	if (skb_vlan_tag_present(skb)) {
		skb = __vlan_hwaccel_push_inside(skb);
		if (!skb)
			goto dropped;
	}
	/* Always add a separate firmware BSSID tag, preserving user VLANs. */
	skb = vlan_insert_tag_set_proto(skb, htons(ETH_P_8021Q), 1);
	if (!skb)
		goto dropped;
	if (skb->len > INIC_BUFFER_SIZE)
		goto drop;
	ret = inic_send(inic, skb, false);
	if (ret)
		goto drop;
	DEV_STATS_INC(dev, tx_packets);
	DEV_STATS_ADD(dev, tx_bytes, len);
	return NETDEV_TX_OK;
drop:
	dev_kfree_skb_any(skb);
dropped:
	DEV_STATS_INC(dev, tx_dropped);
	return NETDEV_TX_OK;
}

const struct net_device_ops inic_netdev_ops = {
	.ndo_open = inic_open,
	.ndo_stop = inic_stop,
	.ndo_start_xmit = inic_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static int inic_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct inic *inic;
	struct wiphy *wiphy;
	int ret;

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto disable;
	ret = pci_request_regions(pdev, "ralink-inic");
	if (ret)
		goto disable;
	if (!(pci_resource_flags(pdev, 1) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 1) < SZ_64K) {
		ret = -ENODEV;
		goto release;
	}
	wiphy = wiphy_new(&inic_cfg_ops, sizeof(*inic));
	if (!wiphy) {
		ret = -ENOMEM;
		goto release;
	}
	inic = wiphy_priv(wiphy);
	inic->wiphy = wiphy;
	inic->pdev = pdev;
	spin_lock_init(&inic->lock);
	mutex_init(&inic->hw_mutex);
	init_completion(&inic->reply);
	init_completion(&inic->notify);
	init_completion(&inic->ready);
	init_completion(&inic->command_reply);
	skb_queue_head_init(&inic->events);
	wiphy_work_init(&inic->event_work, inic_station_event);
	inic->regs = pci_iomap(pdev, 1, SZ_64K);
	if (!inic->regs) {
		ret = -ENOMEM;
		goto free_wiphy;
	}
	inic_write(inic, INIC_INT_ENABLE, 0);
	pci_clear_master(pdev);
	inic->netdev = alloc_etherdev(sizeof(struct inic *));
	if (!inic->netdev) {
		ret = -ENOMEM;
		goto unmap;
	}
	*(struct inic **)netdev_priv(inic->netdev) = inic;
	netif_napi_add(inic->netdev, &inic->napi, inic_poll);
	ret = request_irq(pdev->irq, inic_irq, IRQF_SHARED, "ralink-inic", inic);
	if (ret)
		goto free_netdev;
	ret = inic_wireless_register(inic);
	if (ret)
		goto free_irq;
	pci_set_drvdata(pdev, inic);
	dev_info(&pdev->dev, "RT2880 iNIC BAR1 %pR IRQ %u; firmware loaded on AP start\n",
		 &pdev->resource[1], pdev->irq);
	return 0;
free_irq:
	free_irq(pdev->irq, inic);
free_netdev:
	netif_napi_del(&inic->napi);
	free_netdev(inic->netdev);
unmap:
	pci_iounmap(pdev, inic->regs);
free_wiphy:
	wiphy_free(wiphy);
release:
	pci_release_regions(pdev);
disable:
	pci_disable_device(pdev);
	return ret;
}

static void inic_remove(struct pci_dev *pdev)
{
	struct inic *inic = pci_get_drvdata(pdev);

	inic_wireless_unregister(inic);
	free_irq(pdev->irq, inic);
	netif_napi_del(&inic->napi);
	free_netdev(inic->netdev);
	pci_iounmap(pdev, inic->regs);
	wiphy_free(inic->wiphy);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static void inic_shutdown(struct pci_dev *pdev)
{
	struct inic *inic = pci_get_drvdata(pdev);

	wiphy_lock(inic->wiphy);
	mutex_lock(&inic->hw_mutex);
	inic->shutting_down = true;
	__inic_hw_stop(inic);
	pci_clear_master(pdev);
	mutex_unlock(&inic->hw_mutex);
	wiphy_unlock(inic->wiphy);
}

static const struct pci_device_id inic_ids[] = {
	{ PCI_DEVICE(0x1814, 0x0801) },
	{}
};
MODULE_DEVICE_TABLE(pci, inic_ids);

static struct pci_driver inic_driver = {
	.name = "ralink-inic",
	.id_table = inic_ids,
	.probe = inic_probe,
	.remove = inic_remove,
	.shutdown = inic_shutdown,
};
module_pci_driver(inic_driver);

MODULE_DESCRIPTION("Ralink RT2880 iNIC PCI FullMAC driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(INIC_FIRMWARE);
