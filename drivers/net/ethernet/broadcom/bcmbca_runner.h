/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BCMBCA_RUNNER_H
#define BCMBCA_RUNNER_H

#include <linux/types.h>

/* Core register and SRAM offsets relative to the shared XRDP resource. */
struct bcmbca_runner_data {
	u32 ctrl;
	u32 stride;
	u32 sram;
	unsigned int num_cores;
	const u8 *program;
	const u8 *program_end;
};

/* SRAM offsets in the reduced program ABI, not fixed hardware registers. */
struct bcmbca_runner_reduced_data {
	u32 rx_fifo;
	u32 tx_descriptor;
	u32 tx_destination;
	unsigned int rx_fifo_size;
};

/* The caller owns the XRDP mapping, clocks, reset and access serialization. */
struct bcmbca_runner {
	void __iomem *base;
	const struct bcmbca_runner_data *data;
	const struct bcmbca_runner_reduced_data *reduced;
	unsigned int rx_index;
};

void bcmbca_runner_stop(struct bcmbca_runner *runner);
void bcmbca_runner_sync(struct bcmbca_runner *runner);
int bcmbca_runner_load_program(struct bcmbca_runner *runner, u32 offset,
			       u32 size, u32 reg_size);

/* These operations require the reduced SRAM packet interface. */
bool bcmbca_runner_reduced_receive(struct bcmbca_runner *runner, u32 *word0,
				   u32 *word1, u32 *word2);
void bcmbca_runner_reduced_transmit(struct bcmbca_runner *runner, u16 head,
				    u16 tail, unsigned int buffers,
				    unsigned int len, u16 destination);
u16 bcmbca_runner_reduced_tx_length(struct bcmbca_runner *runner);
u8 bcmbca_runner_reduced_tx_counter(struct bcmbca_runner *runner, u16 offset);

static inline u16 bcmbca_runner_reduced_tx_length_word(unsigned int buffers,
						       unsigned int len)
{
	/* BN_NUM straddles the halfword containing PACKET_LENGTH. */
	return ((buffers & 3) << 14) | len;
}

#endif
