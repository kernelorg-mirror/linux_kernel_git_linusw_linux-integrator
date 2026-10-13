// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BCMBCA Runner core control and reduced-program SRAM interface.
 *
 * Runner cores belong to the shared XRDP packet processor used by Ethernet
 * and PON. The reduced program implements a packet interface in SRAM; its
 * descriptor and mailbox layouts are firmware conventions, not core registers.
 * The caller coordinates the shared reset, clocks and packet buffers, and
 * serializes accesses. Core control requires powered, clocked register banks.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>

#include "bcmbca_runner.h"

#define RUNNER_ENABLE		BIT(0)
#define RUNNER_WAKEUP		0x4
#define CPU_TX_THREAD		1

void bcmbca_runner_stop(struct bcmbca_runner *runner)
{
	unsigned int i;

	for (i = 0; i < runner->data->num_cores; i++) {
		void __iomem *ctrl = runner->base + runner->data->ctrl +
				     i * runner->data->stride;
		u32 val = readl(ctrl);

		writel(val & ~RUNNER_ENABLE, ctrl);
		readl(ctrl);
	}
	usleep_range(1000, 2000);
}

void bcmbca_runner_sync(struct bcmbca_runner *runner)
{
	readl(runner->base + runner->data->ctrl);
}

bool bcmbca_runner_reduced_receive(struct bcmbca_runner *runner, u32 *word0,
				   u32 *word1, u32 *word2)
{
	void __iomem *desc = runner->base + runner->data->sram +
			     runner->reduced->rx_fifo + runner->rx_index * 16;

	*word1 = ioread32be(desc + 4);
	if (!*word1)
		return false;
	*word0 = ioread32be(desc);
	*word2 = ioread32be(desc + 8);
	/* Clearing word1 returns this descriptor to the Runner. */
	writel(0, desc + 12);
	writel(0, desc + 8);
	writel(0, desc);
	writel(0, desc + 4);
	runner->rx_index = (runner->rx_index + 1) % runner->reduced->rx_fifo_size;
	return true;
}

void bcmbca_runner_reduced_transmit(struct bcmbca_runner *runner, u16 head,
				    u16 tail, unsigned int buffers,
				    unsigned int len, u16 destination)
{
	void __iomem *desc = runner->base + runner->data->sram +
			     runner->reduced->tx_descriptor;

	memset_io(desc, 0, 32);
	iowrite32be(BIT(31) | (buffers << 14), desc + 4);
	iowrite32be(BIT(30) | tail, desc + 8);
	iowrite32be(BIT(30) | head, desc + 12);
	iowrite32be(destination, runner->base + runner->data->sram +
		    runner->reduced->tx_destination);
	/* Publish the length only after the packet, descriptor and destination. */
	iowrite16be(bcmbca_runner_reduced_tx_length_word(buffers, len), desc + 6);
	writel(CPU_TX_THREAD, runner->base + runner->data->ctrl + RUNNER_WAKEUP);
}

u16 bcmbca_runner_reduced_tx_length(struct bcmbca_runner *runner)
{
	return ioread16be(runner->base + runner->data->sram +
			 runner->reduced->tx_descriptor + 6) & 0x3fff;
}

u8 bcmbca_runner_reduced_tx_counter(struct bcmbca_runner *runner, u16 offset)
{
	return readb(runner->base + runner->data->sram + offset);
}
