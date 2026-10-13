/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BCMBCA_XRDP_H
#define BCMBCA_XRDP_H

#include <linux/bitfield.h>
#include <linux/types.h>

#define BCMBCA_XRDP_FRAME_LEN	2047
#define BCMBCA_XRDP_BUFFER_SIZE	128
#define BCMBCA_XRDP_RX_HEADROOM	18
#define BCMBCA_XRDP_NULL_BUFFER	0x3fff

#define BCMBCA_XRDP_RX_BUFFER	GENMASK(31, 14)
#define BCMBCA_XRDP_RX_LENGTH	GENMASK(15, 2)
#define BCMBCA_XRDP_RX_ABSOLUTE	BIT(16)
#define BCMBCA_XRDP_RX_OFFSET	GENMASK(12, 6)
#define BCMBCA_XRDP_RX_PORT		GENMASK(29, 25)

struct bcmbca_xrdp_init_entry {
	u32 command;
	u32 value;
};

/* Register offsets are relative to the XRDP resource. */
struct bcmbca_xrdp_port_data {
	u32 mac;
	u32 mib;
	u32 mac_top;
	u32 bbh_rx;
	u32 rgmii; /* Zero for ports without an RGMII interface. */
	u16 tx_destination;
	u16 tx_counter; /* Offset within Runner SRAM. */
};

/* Layout of an XRDP implementation using the reduced SBPM packet ABI. */
struct bcmbca_xrdp_enet_variant {
	const struct bcmbca_xrdp_init_entry *init_data;
	unsigned int init_data_size;
	u32 init_base; /* Physical base used by the recorded initialization. */
	u32 reg_size;
	u32 sbpm;
	u32 psram;
	u32 runner_ctrl;
	u32 runner_stride;
	u32 runner_sram;
	/* Packet table offsets are relative to runner_sram. */
	u32 rx_fifo;
	u32 tx_descriptor;
	u32 tx_destination;
	unsigned int rx_fifo_size;
	unsigned int num_runners;
	unsigned int num_buffers;
	unsigned int num_ports;
	/* SBPM source addresses for CPU allocation and RX release commands. */
	u8 sbpm_alloc_sa;
	u8 sbpm_free_sa;
	const struct bcmbca_xrdp_port_data *ports;
};

struct bcmbca_xrdp_rx_info {
	u16 buffer;
	u16 length;
	u8 offset;
	u8 port;
};

static inline bool
bcmbca_xrdp_rx_decode(const struct bcmbca_xrdp_enet_variant *variant,
		      u32 word0, u32 word1, u32 word2,
		      struct bcmbca_xrdp_rx_info *info)
{
	u32 buffer = FIELD_GET(BCMBCA_XRDP_RX_BUFFER, word0);

	info->length = FIELD_GET(BCMBCA_XRDP_RX_LENGTH, word1);
	info->offset = FIELD_GET(BCMBCA_XRDP_RX_OFFSET, word2);
	info->port = FIELD_GET(BCMBCA_XRDP_RX_PORT, word2);
	info->buffer = buffer;

	return !(word1 & BCMBCA_XRDP_RX_ABSOLUTE) && buffer < variant->num_buffers &&
	       info->length >= 14 && info->length <= BCMBCA_XRDP_FRAME_LEN &&
	       info->port < variant->num_ports &&
	       info->offset + BCMBCA_XRDP_RX_HEADROOM < BCMBCA_XRDP_BUFFER_SIZE;
}

static inline bool bcmbca_xrdp_tx_available(u16 length, u8 producer, u8 consumer)
{
	return !length && (u8)(producer - consumer) < 8;
}

static inline u16 bcmbca_xrdp_tx_length_word(unsigned int buffers, unsigned int len)
{
	/* BN_NUM straddles the halfword containing PACKET_LENGTH. */
	return ((buffers & 3) << 14) | len;
}

#endif
