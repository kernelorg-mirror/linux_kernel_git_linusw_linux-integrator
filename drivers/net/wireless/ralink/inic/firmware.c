// SPDX-License-Identifier: GPL-2.0-only
/* Firmware bootstrap protocol, carried in Ethernet frames over PCI PDMA. */
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/hex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/wireless.h>

#include "inic.h"

static bool fw_debug;
module_param(fw_debug, bool, 0600);
MODULE_PARM_DESC(fw_debug, "Log firmware console messages");

struct inic_trailer {
	__le32 magic;
	__le32 crc;
	char version[12];
	char date[16];
	__le32 size;
} __packed;

/* APStartUp()'s MIPS16 jal AsicLockChannel(), excluding its delay slot. */
#define INIC_FW_POWER_OFFSET		0x45ffe
#define INIC_FW_LOCK_CHANNEL		0xaaca1810
#define INIC_FW_ADJUST_POWER		0xb0351810
/* AsicAdjustTxPower()'s per-rate lw a0, 68(sp), loading the TSSI delta. */
#define INIC_FW_RATE_DELTA_OFFSET	0x2b4ca
#define INIC_FW_LOAD_DELTA		0x9411
#define INIC_FW_ZERO_DELTA		0x6c00

static int inic_firmware_check(struct inic *inic, const struct firmware *fw)
{
	const struct inic_trailer *trailer;
	size_t len;
	u32 crc;

	if (fw->size != 492448)
		return -EINVAL;
	len = fw->size - sizeof(*trailer);
	trailer = (const void *)(fw->data + len);
	if (le32_to_cpu(trailer->magic) != 0x18140801 ||
	    le32_to_cpu(trailer->size) != len ||
	    memcmp(trailer->version, "\"2.0.0.0\"\0\0\0", 12) ||
	    memcmp(trailer->date, "20090310\0\0\0\0\0\0\0\0", 16))
		return -EINVAL;
	crc = crc32_le(~0, fw->data, len) ^ ~0;
	if (crc != le32_to_cpu(trailer->crc))
		return -EBADMSG;
	/*
	 * The startup power fix, power bound and runtime RX DMA bound are
	 * qualified for this exact image.
	 */
	if (crc != 0x7f1c8649 ||
	    get_unaligned_le32(fw->data + INIC_FW_POWER_OFFSET) != INIC_FW_LOCK_CHANNEL ||
	    get_unaligned_le16(fw->data + INIC_FW_RATE_DELTA_OFFSET) != INIC_FW_LOAD_DELTA)
		return -EINVAL;
	dev_info(&inic->pdev->dev, "PCI AP firmware %.12s date %.16s, %zu bytes, CRC %08x\n",
		 trailer->version, trailer->date, len, crc);
	return 0;
}

static u8 *inic_firmware_prepare(const struct firmware *fw)
{
	struct inic_trailer *trailer;
	u8 *image;
	size_t len = fw->size - sizeof(*trailer);

	image = kvmemdup(fw->data, fw->size, GFP_KERNEL);
	if (!image)
		return NULL;
	/*
	 * Replace the empty AsicLockChannel() call after AsicSwitchChannel()
	 * with AsicAdjustTxPower(), before AsicEnableBssSync() and initial
	 * RTMPEnableRxTx(). Keep the argument setup and MIPS16 delay slot.
	 * Otherwise the profile's power reduction first takes effect from
	 * the periodic MLME callback, leaving startup transmissions at full
	 * power. The uploaded profile selects the coarse 12 dB reduction.
	 */
	put_unaligned_le32(INIC_FW_ADJUST_POWER, image + INIC_FW_POWER_OFFSET);
	/*
	 * Replace the per-rate TSSI delta load with li a0, 0. Keep EEPROM
	 * rate powers unchanged: an idle startup TSSI reading or a positive
	 * temperature correction must not add power above this reference.
	 * The engineering margin covers RF variation without compensation.
	 */
	put_unaligned_le16(INIC_FW_ZERO_DELTA, image + INIC_FW_RATE_DELTA_OFFSET);
	trailer = (void *)(image + len);
	trailer->crc = cpu_to_le32(crc32_le(~0, image, len) ^ ~0);
	return image;
}

static int inic_command(struct inic *inic, u16 type, u16 id, u16 sequence,
			u16 command_sequence, u16 dev_type, const u8 *data, u16 len)
{
	static const u8 source[ETH_ALEN] = { 1, 0x0c, 0x43, 0, 0, 0 };
	struct sk_buff *skb;
	struct ethhdr *eth;
	struct inic_msg *msg;
	int ret;

	skb = alloc_skb(ETH_HLEN + sizeof(*msg) + len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	eth = skb_put(skb, ETH_HLEN);
	eth_broadcast_addr(eth->h_dest);
	ether_addr_copy(eth->h_source, source);
	if (type != INIC_BOOT) {
		/* Runtime PCI commands use the card MAC, with the vendor group bit. */
		ether_addr_copy(eth->h_dest, inic->netdev->dev_addr);
		ether_addr_copy(eth->h_source, inic->netdev->dev_addr);
		eth->h_dest[0] |= 1;
		eth->h_source[0] |= 1;
	}
	eth->h_proto = htons(0xffff);
	msg = skb_put_zero(skb, sizeof(*msg));
	msg->magic = cpu_to_le32(INIC_MAGIC);
	msg->type = cpu_to_le16(type);
	msg->id = cpu_to_le16(id);
	msg->sequence = cpu_to_le16(sequence);
	msg->command_sequence = cpu_to_le16(command_sequence);
	msg->dev_type = cpu_to_le16(dev_type);
	msg->length = cpu_to_le16(len);
#ifdef __BIG_ENDIAN
	if (type == INIC_BOOT)
		msg->flags = cpu_to_le16(BIT(0));
#endif
	if (len)
		skb_put_data(skb, data, len);
	ret = inic_send(inic, skb, true);
	if (ret) {
		memzero_explicit(skb->data, skb->len);
		kfree_skb(skb);
	}
	return ret;
}

static int inic_upload(struct inic *inic, u16 id, const u8 *data, size_t len)
{
	unsigned long flags;
	u16 sequence = 0;
	int ret = 0;

	while (len) {
		u16 chunk = min_t(size_t, len, INIC_CHUNK_SIZE);
		int retry;

		spin_lock_irqsave(&inic->lock, flags);
		reinit_completion(&inic->reply);
		inic->expected_id = id;
		inic->expected_seq = sequence;
		inic->expected_len = chunk;
		memcpy(inic->expected, data, chunk);
		inic->reply_error = 0;
		inic->pending = true;
		spin_unlock_irqrestore(&inic->lock, flags);
		for (retry = 0; retry < 3; retry++) {
			ret = inic_command(inic, INIC_BOOT, id, sequence, 0, 0, data, chunk);
			if (ret && ret != -ENOSPC)
				break;
			if (wait_for_completion_timeout(&inic->reply, msecs_to_jiffies(1000))) {
				ret = inic->reply_error;
				break;
			}
			ret = -ETIMEDOUT;
		}
		spin_lock_irqsave(&inic->lock, flags);
		inic->pending = false;
		memzero_explicit(inic->expected, sizeof(inic->expected));
		spin_unlock_irqrestore(&inic->lock, flags);
		if (ret) {
			dev_err(&inic->pdev->dev, "bootstrap command %u chunk %u failed: %d\n",
				id, sequence, ret);
			return ret;
		}
		/* Match the vendor pacing rather than saturating the boot ROM. */
		if (id == INIC_UPLOAD)
			msleep(30);
		sequence++;
		data += chunk;
		len -= chunk;
	}
	return 0;
}

/* The firmware uses its original private ioctl numbers on the wire only. */
static int inic_iwreq(struct inic *inic, u16 id, u16 parameter, bool query,
		      void *data, u16 length, u16 *result_length)
{
	u8 *request;
	unsigned long flags;
	u16 wire_length = 4 + ALIGN(length, 2) + 2;
	int ret;

	lockdep_assert_wiphy(inic->wiphy);
	if (!inic->running)
		return -ENETDOWN;
	if (length > sizeof(inic->command_data))
		return -EMSGSIZE;
	request = kzalloc(wire_length, GFP_KERNEL);
	if (!request)
		return -ENOMEM;
	/* IW_POINT: length, two padding bytes, inline data, even padding, flags. */
	put_unaligned_le16(length, request);
	memcpy(request + 4, data, length);
	put_unaligned_le16(parameter, request + wire_length - 2);
	spin_lock_irqsave(&inic->lock, flags);
	reinit_completion(&inic->command_reply);
	inic->command_sequence++;
	inic->command_id = id;
	inic->command_capacity = length;
	inic->command_length = 0;
	inic->command_copied = 0;
	inic->command_copy_sequence = 0;
	inic->command_decoded = false;
	inic->command_error = 0;
	inic->command_pending = true;
	spin_unlock_irqrestore(&inic->lock, flags);
	ret = inic_command(inic, INIC_IWREQ, id, 0, inic->command_sequence, 1,
			   request, wire_length);
	kfree_sensitive(request);
	/* Do not retry key installation after an ambiguous timeout. */
	if (!ret && !wait_for_completion_timeout(&inic->command_reply, 2 * HZ))
		ret = -ETIMEDOUT;
	spin_lock_irqsave(&inic->lock, flags);
	inic->command_pending = false;
	if (!ret)
		ret = inic->command_error;
	if (!ret && query) {
		if (!inic->command_decoded || inic->command_copied != inic->command_length) {
			ret = -EBADMSG;
		} else {
			memcpy(data, inic->command_data, inic->command_length);
			if (result_length)
				*result_length = inic->command_length;
		}
	}
	memzero_explicit(inic->command_data, sizeof(inic->command_data));
	spin_unlock_irqrestore(&inic->lock, flags);
	if (ret)
		dev_err_ratelimited(&inic->pdev->dev, "firmware ioctl %04x parameter %u: %d\n",
				    id, parameter, ret);
	return ret;
}

int inic_auth_command(struct inic *inic, bool query, u16 parameter,
		      void *data, u16 length, u16 *result_length)
{
	return inic_iwreq(inic, query ? INIC_AUTH_GET : INIC_AUTH_SET,
			  parameter, query, data, length, result_length);
}

/* The MAC-read ioctl's fixed response format: [0x%08x]:%08x followed by two spaces. */
static int inic_mac_word(const char *reply, u32 address, u32 *value)
{
	char prefix[14];
	u8 bytes[4];

	snprintf(prefix, sizeof(prefix), "[0x%08x]:", address);
	if (memcmp(reply, prefix, 13) || reply[21] != ' ' || reply[22] != ' ' ||
	    hex2bin(bytes, reply + 13, sizeof(bytes)))
		return -EBADMSG;
	*value = get_unaligned_be32(bytes);
	return 0;
}

int inic_group_sequence(struct inic *inic, u8 index, u8 *sequence)
{
	u32 high, low, again;
	char reply[128];
	u16 length;
	int retry, ret;

	/*
	 * BSSID 0 uses WCID 212, whose ASIC IV/EIV entry is at 0x66a0.
	 * The pinned firmware's RTMPGetTxTscFromAsic() at 0x88050128 maps
	 * CCMP PN0/PN1 from IV bytes 0/1 and PN2..PN5 from EIV bytes 0..3.
	 * Use its existing MAC-read ioctl, taking EIV/IV/EIV snapshots to
	 * detect carry into the upper 32 bits while transmission continues.
	 */
	for (retry = 0; retry < 3; retry++) {
		memset(reply, 0, sizeof(reply));
		strscpy(reply, "66a4,66a0,66a4", sizeof(reply));
		ret = inic_iwreq(inic, 0x8be5, 0, true, reply, sizeof(reply), &length);
		if (ret)
			return ret;
		if (length != 70 || reply[0] != '\n' ||
		    inic_mac_word(reply + 1, 0x66a4, &high) ||
		    inic_mac_word(reply + 24, 0x66a0, &low) ||
		    inic_mac_word(reply + 47, 0x66a4, &again))
			return -EBADMSG;
		if (!(low & BIT(29)) || (low >> 30) != index)
			return -EIO;
		if (high != again)
			continue;
		sequence[0] = low;
		sequence[1] = low >> 8;
		put_unaligned_le32(high, sequence + 2);
		return 0;
	}
	return -EAGAIN;
}

static void inic_auth_reply(struct inic *inic, const struct inic_msg *msg,
			    u16 type, u16 len)
{
	unsigned long flags;
	int status;

	spin_lock_irqsave(&inic->lock, flags);
	if (!inic->command_pending || msg->dev_id ||
	    le16_to_cpu(msg->id) != inic->command_id ||
	    le16_to_cpu(msg->command_sequence) != inic->command_sequence)
		goto unlock;
	if (type == INIC_COPY_REPLY) {
		if (le16_to_cpu(msg->sequence) != inic->command_copy_sequence ||
		    len > inic->command_capacity - inic->command_copied)
			goto malformed;
		memcpy(inic->command_data + inic->command_copied, msg->data, len);
		inic->command_copied += len;
		inic->command_copy_sequence++;
	} else if (type == INIC_IWREQ_REPLY) {
		if (le16_to_cpu(msg->dev_type) != 1 || len < 6 ||
		    get_unaligned_le16(msg->data) > inic->command_capacity)
			goto malformed;
		inic->command_length = get_unaligned_le16(msg->data);
		inic->command_decoded = true;
	} else {
		/* Vendor transport status values differ from Linux errno values. */
		status = (s32)le32_to_cpu(msg->status);
		switch (status) {
		case 0:
			break;
		case -1:
			inic->command_error = -EFAULT;
			break;
		case -2:
			inic->command_error = -EOPNOTSUPP;
			break;
		case -3:
			inic->command_error = -EINVAL;
			break;
		case -4:
			inic->command_error = -ENETDOWN;
			break;
		case -5:
			inic->command_error = -EAGAIN;
			break;
		default:
			inic->command_error = -EIO;
			break;
		}
		goto done;
	}
	goto unlock;
malformed:
	inic->command_error = -EBADMSG;
done:
	inic->command_pending = false;
	complete(&inic->command_reply);
unlock:
	spin_unlock_irqrestore(&inic->lock, flags);
}

void inic_receive_control(struct inic *inic, struct sk_buff *skb)
{
	struct inic_msg *msg;
	unsigned long flags;
	u16 type, id, len;

	if (skb->len < ETH_HLEN + sizeof(*msg))
		return;
	msg = (void *)(skb->data + ETH_HLEN);
	if (le32_to_cpu(msg->magic) != INIC_MAGIC)
		return;
	len = le16_to_cpu(msg->length);
	if (len > skb->len - ETH_HLEN - sizeof(*msg) || len > INIC_CHUNK_SIZE)
		return;
	type = le16_to_cpu(msg->type);
	id = le16_to_cpu(msg->id);
	if (type == INIC_COPY_REPLY || type == INIC_IWREQ_REPLY || type == INIC_STATUS_REPLY) {
		inic_auth_reply(inic, msg, type, len);
	} else if ((type & ~0x8000) == INIC_BOOT) {
		if (id == INIC_NOTIFY) {
			complete(&inic->notify);
			return;
		}
		spin_lock_irqsave(&inic->lock, flags);
		if (inic->pending && id == inic->expected_id &&
		    le16_to_cpu(msg->sequence) == inic->expected_seq) {
			inic->reply_error = len != inic->expected_len ||
				memcmp(msg->data, inic->expected, len) ? -EBADMSG : 0;
			inic->pending = false;
			complete(&inic->reply);
		}
		spin_unlock_irqrestore(&inic->lock, flags);
	} else if (type == INIC_GET_MAC && !id && len == 18) {
		u8 mac[ETH_ALEN];
		char text[18];

		memcpy(text, msg->data, sizeof(text));
		text[17] = '\0';
		if (!mac_pton(text, mac) || !is_valid_ether_addr(mac))
			return;
		/* Do not change a registered netdev address from softirq context. */
		dev_info(&inic->pdev->dev, "firmware ready, card MAC %pM\n", mac);
		complete(&inic->ready);
	} else if (type == INIC_ASYNC && id == 0) {
		if (fw_debug)
			dev_info_ratelimited(&inic->pdev->dev, "firmware: %.*s\n", len, msg->data);
	} else if (type == INIC_ASYNC && id == 5 && !msg->dev_id &&
		   !msg->dev_type && len >= sizeof(struct sockaddr)) {
		/* Vendor wireless_send_event2 transports an inline sockaddr. */
		const u8 *mac = msg->data + 2;
		u32 event = le32_to_cpu(msg->status);
		struct sk_buff *event_skb;

		if (!READ_ONCE(inic->ap_started) || !is_valid_ether_addr(mac))
			return;
		if (event != IWEVREGISTERED && event != IWEVEXPIRED)
			return;
		if (skb_queue_len(&inic->events) >= 128)
			goto events_lost;
		event_skb = skb_clone(skb, GFP_ATOMIC);
		if (!event_skb)
			goto events_lost;
		skb_queue_tail(&inic->events, event_skb);
		wiphy_work_queue(inic->wiphy, &inic->event_work);
	}
	return;
events_lost:
	WRITE_ONCE(inic->events_lost, true);
	wiphy_work_queue(inic->wiphy, &inic->event_work);
}

int inic_boot(struct inic *inic, const u8 *profile, size_t len)
{
	const struct firmware *fw;
	u8 *image = NULL;
	int ret;

	ret = request_firmware(&fw, INIC_FIRMWARE, &inic->pdev->dev);
	if (ret)
		return ret;
	ret = inic_firmware_check(inic, fw);
	if (ret) {
		dev_err(&inic->pdev->dev, "invalid PCI AP firmware: %d\n", ret);
		goto release;
	}
	image = inic_firmware_prepare(fw);
	if (!image) {
		ret = -ENOMEM;
		goto release;
	}
	reinit_completion(&inic->notify);
	reinit_completion(&inic->ready);
	ret = inic_hw_start(inic);
	if (ret)
		goto release;
	if (!wait_for_completion_timeout(&inic->notify, msecs_to_jiffies(5000))) {
		dev_err(&inic->pdev->dev, "no boot-ROM notification\n");
		ret = -ETIMEDOUT;
		goto stop;
	}
	dev_info(&inic->pdev->dev, "boot ROM ready; uploading AP profile\n");
	ret = inic_upload(inic, INIC_INITCFG, profile, len);
	if (ret)
		goto stop;
	dev_info(&inic->pdev->dev, "uploading firmware with startup power fix (%zu bytes)\n",
		 fw->size);
	ret = inic_upload(inic, INIC_UPLOAD, image, fw->size);
	if (ret)
		goto stop;
	ret = inic_command(inic, INIC_BOOT, INIC_STARTUP, 0, 0, 0, NULL, 0);
	if (ret)
		goto stop;
	if (!wait_for_completion_timeout(&inic->ready, msecs_to_jiffies(20000))) {
		dev_err(&inic->pdev->dev, "firmware did not report its MAC\n");
		ret = -ETIMEDOUT;
		goto stop;
	}
	goto release;
stop:
	inic_hw_stop(inic);
release:
	kvfree(image);
	release_firmware(fw);
	return ret;
}
