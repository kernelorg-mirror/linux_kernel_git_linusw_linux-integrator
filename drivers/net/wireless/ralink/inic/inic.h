/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RALINK_INIC_H
#define _RALINK_INIC_H

#include <linux/completion.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <net/cfg80211.h>

#define INIC_FIRMWARE		"ralink/rt2880-inic-ap.bin"
/* Qualified DIR-685 configuration; TxPower is a vendor percentage. */
#define INIC_TX_POWER_PERCENT	5
#define INIC_MAX_EIRP_DBM	20
#define INIC_TX_COUNT		128
#define INIC_RX_COUNT		64
/* Full PCI frame size, including the firmware's four-byte BSSID tag. */
#define INIC_BUFFER_SIZE	1536
#define INIC_CHUNK_SIZE		1024
#define INIC_MAGIC		0x18142880
#define INIC_BOOT		1
#define INIC_NOTIFY		1
#define INIC_INITCFG		2
#define INIC_UPLOAD		3
#define INIC_STARTUP		4
#define INIC_ASYNC		0x8005
#define INIC_GET_MAC		0x8009
#define INIC_IWREQ		7
#define INIC_COPY_REPLY		0x8006
#define INIC_IWREQ_REPLY	0x8007
#define INIC_STATUS_REPLY	0x8008
#define INIC_AUTH_SET		0x8bfe
#define INIC_AUTH_GET		0x8bff
#define INIC_AUTH_EVENTS	5
#define INIC_AUTH_RSN_IE	7
#define INIC_AUTH_SET_KEY	10
#define INIC_AUTH_DEL_KEY	11
#define INIC_AUTH_DEAUTH	12
#define INIC_MAX_STATIONS	32

struct inic_station {
	u8 mac[ETH_ALEN];
	unsigned long connected;
	u8 key[WLAN_KEY_LEN_CCMP];
	bool used;
	bool key_installed;
	bool authorized;
};

struct inic_desc {
	__le32 address;
	__le32 control;
	__le32 address1;
	__le32 info;
};

struct inic_msg {
	__le32 magic;
	__le16 type;
	__le16 id;
	__le16 length;
	__le16 sequence;
	__le16 dev_id;
	__le16 dev_type;
	__le16 command_sequence;
	__le16 flags;
	__le32 status;
	__le32 reserved[2];
	u8 data[];
} __packed;

struct inic_buffer {
	struct sk_buff *skb;
	dma_addr_t dma;
	bool sensitive;
};

struct inic {
	struct pci_dev *pdev;
	void __iomem *regs;
	struct wiphy *wiphy;
	struct wireless_dev wdev;
	struct net_device *netdev;
	struct napi_struct napi;
	/* Serializes hardware startup, teardown and permanent shutdown. */
	struct mutex hw_mutex;
	bool shutting_down;
	/* Serializes TX producer/reclaimer and interrupt mask updates. */
	spinlock_t lock;
	struct inic_desc *rx_desc;
	struct inic_desc *tx_desc;
	dma_addr_t desc_dma;
	struct inic_buffer rx[INIC_RX_COUNT];
	struct inic_buffer tx[INIC_TX_COUNT];
	u32 tx_head;
	u32 tx_tail;
	u32 rx_head;
	/* Transport active; the shared IRQ handler may access registers. */
	bool running;
	bool ap_started;
	/* A single bootstrap transaction; protected against NAPI by lock. */
	struct completion reply;
	struct completion notify;
	struct completion ready;
	bool pending;
	u16 expected_id;
	u16 expected_seq;
	u16 expected_len;
	u8 expected[INIC_CHUNK_SIZE];
	int reply_error;
	/* Runtime commands and station state are serialized by the wiphy mutex. */
	struct completion command_reply;
	u16 command_sequence;
	u16 command_id;
	u16 command_copy_sequence;
	u16 command_length;
	u16 command_capacity;
	u16 command_copied;
	bool command_pending;
	bool command_decoded;
	int command_error;
	u8 command_data[256];
	struct sk_buff_head events;
	bool events_lost;
	struct wiphy_work event_work;
	struct inic_station stations[INIC_MAX_STATIONS];
	u32 station_generation;
	bool secure;
	bool control_port;
	u8 group_key;
	u8 group_keys[4][WLAN_KEY_LEN_CCMP];
	u8 group_keys_valid;
	u8 group_keys_installed;
	struct cfg80211_chan_def chandef;
	struct ieee80211_channel channels[11];
	struct ieee80211_supported_band band;
};

int inic_hw_start(struct inic *inic);
void inic_hw_stop(struct inic *inic);
int inic_send(struct inic *inic, struct sk_buff *skb, bool control);
void inic_receive_control(struct inic *inic, struct sk_buff *skb);
int inic_boot(struct inic *inic, const u8 *profile, size_t len);
int inic_auth_command(struct inic *inic, bool query, u16 parameter,
		      void *data, u16 length, u16 *result_length);
int inic_group_sequence(struct inic *inic, u8 index, u8 *sequence);
void inic_station_event(struct wiphy *wiphy, struct wiphy_work *work);
bool inic_port_allowed(struct inic *inic, const struct ethhdr *eth, bool tx);
int inic_wireless_register(struct inic *inic);
void inic_wireless_unregister(struct inic *inic);
extern const struct net_device_ops inic_netdev_ops;
extern const struct cfg80211_ops inic_cfg_ops;

#endif
