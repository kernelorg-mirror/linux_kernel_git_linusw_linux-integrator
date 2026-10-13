// SPDX-License-Identifier: GPL-2.0-or-later
#include <kunit/test.h>

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
		word = (word & 0xffff0000) | bcmbca_xrdp_tx_length_word(buffers, len);
		KUNIT_EXPECT_EQ(test, word & 0x3fff, len);
		KUNIT_EXPECT_EQ(test, (word >> 14) & 0x7f, buffers);
		KUNIT_EXPECT_TRUE(test, word & BIT(31));
	}
}

static struct kunit_case bcmbca_xrdp_enet_cases[] = {
	KUNIT_CASE(bcmbca_xrdp_rx_valid_test),
	KUNIT_CASE(bcmbca_xrdp_rx_invalid_test),
	KUNIT_CASE(bcmbca_xrdp_rx_variant_test),
	KUNIT_CASE(bcmbca_xrdp_tx_credit_test),
	KUNIT_CASE(bcmbca_xrdp_tx_descriptor_test),
	{}
};

static struct kunit_suite bcmbca_xrdp_enet_suite = {
	.name = "bcmbca-xrdp-enet",
	.test_cases = bcmbca_xrdp_enet_cases,
};

kunit_test_suite(bcmbca_xrdp_enet_suite);

MODULE_LICENSE("GPL");
