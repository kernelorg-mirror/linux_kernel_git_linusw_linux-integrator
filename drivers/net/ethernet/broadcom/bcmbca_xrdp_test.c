// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>
#include <linux/etherdevice.h>

#include "bcmbca_xrdp.h"

static const struct bcmbca_xrdp_enet_variant bcm6846_variant = {
	.num_buffers = 1536,
	.num_ports = 5,
};

static void bcmbca_xrdp_rx_valid_test(struct kunit *test)
{
	const struct bcmbca_xrdp_enet_variant *variant = &bcm6846_variant;
	struct bcmbca_xrdp_rx_info info;
	unsigned int port;

	for (port = 0; port < bcm6846_variant.num_ports; port++) {
		KUNIT_ASSERT_TRUE(test,
				  bcmbca_xrdp_rx_decode(variant, 0x017fc000, 0x000017b8,
							port << 25, &info));
		KUNIT_EXPECT_EQ(test, info.buffer, 1535);
		KUNIT_EXPECT_EQ(test, info.length, 1518);
		KUNIT_EXPECT_EQ(test, info.offset, 0);
		KUNIT_EXPECT_EQ(test, info.port, port);
	}
	KUNIT_ASSERT_TRUE(test, bcmbca_xrdp_rx_decode(variant, 0, 56, 109 << 6, &info));
	KUNIT_EXPECT_EQ(test, info.length, 14);
	KUNIT_EXPECT_EQ(test, info.offset, 109);
}

static void bcmbca_xrdp_rx_invalid_test(struct kunit *test)
{
	const struct bcmbca_xrdp_enet_variant *variant = &bcm6846_variant;
	struct bcmbca_xrdp_rx_info info;

	KUNIT_EXPECT_FALSE(test, bcmbca_xrdp_rx_decode(variant, 0, 0, 0, &info));
	KUNIT_EXPECT_FALSE(test, bcmbca_xrdp_rx_decode(variant, 0, 52, 0, &info));
	KUNIT_EXPECT_FALSE(test, bcmbca_xrdp_rx_decode(variant, 0, 8192, 0, &info));
	KUNIT_EXPECT_FALSE(test, bcmbca_xrdp_rx_decode(variant, 0, 0x100f0, 0, &info));
	KUNIT_EXPECT_FALSE(test, bcmbca_xrdp_rx_decode(variant, 0x01800000, 240, 0, &info));
	/* Do not truncate a corrupt 18-bit buffer number to 16 bits. */
	KUNIT_EXPECT_FALSE(test, bcmbca_xrdp_rx_decode(variant, 0x40000000, 240, 0, &info));
	KUNIT_EXPECT_FALSE(test, bcmbca_xrdp_rx_decode(variant, 0, 240, 5 << 25, &info));
	KUNIT_EXPECT_FALSE(test, bcmbca_xrdp_rx_decode(variant, 0, 240, 110 << 6, &info));
}

/* Descriptor validity must follow the matched pool and port limits. */
static void bcmbca_xrdp_rx_variant_test(struct kunit *test)
{
	const struct bcmbca_xrdp_enet_variant variant = {
		.num_buffers = 1024,
		.num_ports = 2,
	};
	struct bcmbca_xrdp_rx_info info;

	KUNIT_EXPECT_TRUE(test,
			  bcmbca_xrdp_rx_decode(&variant, 1023 << 14, 240, 1 << 25, &info));
	KUNIT_EXPECT_FALSE(test,
			   bcmbca_xrdp_rx_decode(&variant, 1024 << 14, 240, 0, &info));
	KUNIT_EXPECT_FALSE(test,
			   bcmbca_xrdp_rx_decode(&variant, 0, 240, 2 << 25, &info));
	KUNIT_EXPECT_TRUE(test,
			  bcmbca_xrdp_rx_decode(&bcm6846_variant, 1024 << 14,
						240, 2 << 25, &info));
}

/* MTU limits must accommodate VLAN frames and the DSA conduit overhead. */
static void bcmbca_xrdp_mtu_test(struct kunit *test)
{
	struct bcmbca_xrdp_rx_info info;
	unsigned int mtu, length;

	KUNIT_EXPECT_EQ(test, bcmbca_xrdp_frame_length(68), 90);
	KUNIT_EXPECT_EQ(test, bcmbca_xrdp_frame_length(1500), 1522);
	KUNIT_EXPECT_EQ(test, bcmbca_xrdp_frame_length(1508), 1530);
	KUNIT_EXPECT_EQ(test, bcmbca_xrdp_frame_length(BCMBCA_XRDP_MAX_MTU), 2022);

	for (mtu = ETH_MIN_MTU; mtu <= BCMBCA_XRDP_MAX_MTU; mtu++) {
		length = bcmbca_xrdp_frame_length(mtu);
		KUNIT_ASSERT_LE(test, length, BCMBCA_XRDP_FRAME_LEN);
		KUNIT_ASSERT_TRUE(test,
				  bcmbca_xrdp_rx_decode(&bcm6846_variant, 0,
							length << 2, 4 << 25, &info));
		KUNIT_EXPECT_EQ(test, info.length, length);
	}
}

static void bcmbca_xrdp_tx_credit_test(struct kunit *test)
{
	unsigned int consumer, outstanding;

	for (consumer = 0; consumer < 256; consumer++) {
		for (outstanding = 0; outstanding <= 8; outstanding++) {
			u8 producer = consumer + outstanding;

			KUNIT_EXPECT_EQ(test,
					bcmbca_xrdp_tx_available(0, producer, consumer),
					outstanding < 8);
			KUNIT_EXPECT_FALSE(test,
					   bcmbca_xrdp_tx_available(60, producer, consumer));
		}
	}
}

static void bcmbca_xrdp_tx_descriptor_test(struct kunit *test)
{
	unsigned int len;

	for (len = 60; len <= BCMBCA_XRDP_FRAME_LEN; len++) {
		unsigned int buffers = DIV_ROUND_UP(len, BCMBCA_XRDP_BUFFER_SIZE);
		u32 word = BIT(31) | (buffers << 14);

		/* The final 16-bit length write must preserve BN_NUM's low bits. */
		word = (word & 0xffff0000) | bcmbca_runner_reduced_tx_length_word(buffers, len);
		KUNIT_EXPECT_EQ(test, word & 0x3fff, len);
		KUNIT_EXPECT_EQ(test, (word >> 14) & 0x7f, buffers);
		KUNIT_EXPECT_TRUE(test, word & BIT(31));
	}
}

static void bcmbca_xrdp_tx_completion_test(struct kunit *test)
{
	unsigned int start, i;

	/* Exercise every counter origin, including wrap and ring-slot reuse. */
	for (start = 0; start < 256; start++) {
		struct bcmbca_xrdp_tx_ring ring = {
			.producer = start,
			.consumer = start,
		};
		unsigned int packets = 0;
		u8 consumer = start + 3;

		for (i = 0; i < BCMBCA_XRDP_TX_FIFO_SIZE; i++)
			bcmbca_xrdp_tx_record(&ring, 60 + i);
		KUNIT_EXPECT_FALSE(test,
				   bcmbca_xrdp_tx_available(0, ring.producer, ring.consumer));
		KUNIT_EXPECT_EQ(test, bcmbca_xrdp_tx_complete(&ring, consumer, &packets),
				183);
		KUNIT_EXPECT_EQ(test, packets, 3);
		KUNIT_EXPECT_EQ(test, bcmbca_xrdp_tx_complete(&ring, consumer, &packets), 0);
		KUNIT_EXPECT_EQ(test, packets, 3);

		bcmbca_xrdp_tx_record(&ring, 60);
		bcmbca_xrdp_tx_record(&ring, 1518);
		bcmbca_xrdp_tx_record(&ring, BCMBCA_XRDP_FRAME_LEN);
		KUNIT_EXPECT_EQ(test,
				bcmbca_xrdp_tx_complete(&ring, ring.producer, &packets), 3950);
		KUNIT_EXPECT_EQ(test, packets, 11);
		KUNIT_EXPECT_EQ(test, ring.consumer, ring.producer);
	}
}

static void bcmbca_xrdp_tx_reset_test(struct kunit *test)
{
	struct bcmbca_xrdp_tx_ring ring = { .producer = 254, .consumer = 254 };
	unsigned int packets = 0, i;

	for (i = 0; i < BCMBCA_XRDP_TX_FIFO_SIZE; i++)
		bcmbca_xrdp_tx_record(&ring, 1518);
	bcmbca_xrdp_tx_reset(&ring);
	KUNIT_EXPECT_EQ(test, ring.producer, 6);
	KUNIT_EXPECT_EQ(test, ring.consumer, 254);
	KUNIT_EXPECT_FALSE(test, bcmbca_xrdp_tx_available(0, ring.producer, ring.consumer));

	/* Late completions release credits without charging the reopened queue. */
	KUNIT_EXPECT_EQ(test, bcmbca_xrdp_tx_complete(&ring, 1, &packets), 0);
	KUNIT_EXPECT_EQ(test, packets, 0);
	bcmbca_xrdp_tx_record(&ring, 60);
	bcmbca_xrdp_tx_record(&ring, 1518);
	bcmbca_xrdp_tx_record(&ring, BCMBCA_XRDP_FRAME_LEN);
	KUNIT_EXPECT_EQ(test, bcmbca_xrdp_tx_complete(&ring, 9, &packets), 3625);
	KUNIT_EXPECT_EQ(test, packets, 3);
	KUNIT_EXPECT_EQ(test, bcmbca_xrdp_tx_complete(&ring, 9, &packets), 0);
}

#if IS_ENABLED(CONFIG_BQL)
static void bcmbca_xrdp_tx_bql_test(struct kunit *test)
{
	struct bcmbca_xrdp_tx_ring ring = { .producer = 255, .consumer = 255 };
	unsigned int packets = 0, bytes;
	struct net_device *ndev;
	struct dql *dql;

	ndev = alloc_etherdev(0);
	KUNIT_ASSERT_NOT_NULL(test, ndev);
	netif_stop_queue(ndev);
	dql = &netdev_get_tx_queue(ndev, 0)->dql;

	/* Account the software queue, including one failed SBPM allocation. */
	netdev_sent_queue(ndev, 60);
	netdev_sent_queue(ndev, 1518);
	netdev_sent_queue(ndev, 2047);
	bcmbca_xrdp_tx_record(&ring, 60);
	bcmbca_xrdp_tx_record(&ring, 2047);
	bytes = bcmbca_xrdp_tx_complete(&ring, 0, &packets);
	netdev_completed_queue(ndev, packets + 1, bytes + 1518);
	KUNIT_EXPECT_EQ(test, dql->num_queued - dql->num_completed, 2047);

	/* Close with one hardware packet and one software packet outstanding. */
	netdev_sent_queue(ndev, 128);
	bcmbca_xrdp_tx_reset(&ring);
	netdev_reset_queue(ndev);
	KUNIT_EXPECT_EQ(test, dql->num_queued, 0);
	KUNIT_EXPECT_EQ(test, dql->num_completed, 0);

	netdev_sent_queue(ndev, 1518);
	bcmbca_xrdp_tx_record(&ring, 1518);
	packets = 0;
	bytes = bcmbca_xrdp_tx_complete(&ring, 2, &packets);
	netdev_completed_queue(ndev, packets, bytes);
	KUNIT_EXPECT_EQ(test, packets, 1);
	KUNIT_EXPECT_EQ(test, bytes, 1518);
	KUNIT_EXPECT_EQ(test, dql->num_queued, dql->num_completed);
	free_netdev(ndev);
}
#endif

static struct kunit_case bcmbca_xrdp_enet_cases[] = {
	KUNIT_CASE(bcmbca_xrdp_rx_valid_test),
	KUNIT_CASE(bcmbca_xrdp_rx_invalid_test),
	KUNIT_CASE(bcmbca_xrdp_rx_variant_test),
	KUNIT_CASE(bcmbca_xrdp_mtu_test),
	KUNIT_CASE(bcmbca_xrdp_tx_credit_test),
	KUNIT_CASE(bcmbca_xrdp_tx_descriptor_test),
	KUNIT_CASE(bcmbca_xrdp_tx_completion_test),
	KUNIT_CASE(bcmbca_xrdp_tx_reset_test),
#if IS_ENABLED(CONFIG_BQL)
	KUNIT_CASE(bcmbca_xrdp_tx_bql_test),
#endif
	{}
};

static struct kunit_suite bcmbca_xrdp_enet_suite = {
	.name = "bcmbca-xrdp-enet",
	.test_cases = bcmbca_xrdp_enet_cases,
};

kunit_test_suite(bcmbca_xrdp_enet_suite);

MODULE_LICENSE("GPL");
