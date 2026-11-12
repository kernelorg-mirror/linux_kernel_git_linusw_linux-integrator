// SPDX-License-Identifier: GPL-2.0-only
/* Translate cfg80211 AP settings to the RT2880 firmware profile. */
#include <linux/ctype.h>
#include <linux/module.h>
#include <linux/if_vlan.h>
#include <linux/of.h>
#include <linux/unaligned.h>
#include <linux/wireless.h>

#include "inic.h"

/* Firmware ieee80211req_key layout. */
#define INIC_KEY_REQUEST_SIZE	60
#define INIC_KEY_INDEX_PAIRWISE	0xffff /* Look up the station by MAC address. */

/* Firmware ieee80211req_key cipher and flags. */
#define INIC_CIPHER_CCMP	4
#define INIC_KEY_TX		BIT(0)
#define INIC_KEY_RX		BIT(1)
#define INIC_KEY_DEFAULT	BIT(7) /* Default transmit key. */

static struct ieee80211_rate inic_rates[] = {
	{ .bitrate = 10, .hw_value = 0 },
	{ .bitrate = 20, .hw_value = 1 },
	{ .bitrate = 55, .hw_value = 2 },
	{ .bitrate = 110, .hw_value = 3 },
	{ .bitrate = 60, .hw_value = 4 },
	{ .bitrate = 90, .hw_value = 5 },
	{ .bitrate = 120, .hw_value = 6 },
	{ .bitrate = 180, .hw_value = 7 },
	{ .bitrate = 240, .hw_value = 8 },
	{ .bitrate = 360, .hw_value = 9 },
	{ .bitrate = 480, .hw_value = 10 },
	{ .bitrate = 540, .hw_value = 11 },
};

static const u32 inic_ciphers[] = { WLAN_CIPHER_SUITE_CCMP };
static const u32 inic_akms[] = { WLAN_AKM_SUITE_PSK };

static struct inic_station *inic_find_station(struct inic *inic, const u8 *mac)
{
	int i;

	for (i = 0; i < INIC_MAX_STATIONS; i++)
		if (inic->stations[i].used && ether_addr_equal(inic->stations[i].mac, mac))
			return &inic->stations[i];
	return NULL;
}

bool inic_port_allowed(struct inic *inic, const struct ethhdr *eth, bool tx)
{
	struct inic_station *station;
	unsigned long flags;
	bool allowed;
	u8 group_key;

	if (!inic->control_port || eth->h_proto == htons(ETH_P_PAE))
		return true;
	if (tx && is_multicast_ether_addr(eth->h_dest)) {
		if (!inic->secure)
			return true;
		group_key = READ_ONCE(inic->group_key);
		return group_key < 4 && (READ_ONCE(inic->group_keys_installed) & BIT(group_key));
	}
	spin_lock_irqsave(&inic->lock, flags);
	station = inic_find_station(inic, tx ? eth->h_dest : eth->h_source);
	allowed = station && station->authorized && (!inic->secure || station->key_installed);
	spin_unlock_irqrestore(&inic->lock, flags);
	return allowed;
}

static int inic_deauth(struct inic *inic, const u8 *mac, u16 reason)
{
	u8 request[8];

	put_unaligned_le16(reason, request);
	ether_addr_copy(request + 2, mac);
	return inic_auth_command(inic, false, INIC_AUTH_DEAUTH, request, sizeof(request), NULL);
}

static void inic_abort_ap(struct inic *inic)
{
	inic_hw_stop(inic);
	cfg80211_stop_iface(inic->wiphy, &inic->wdev, GFP_KERNEL);
}

/*
 * FCC report RF970821H05, pp12 and 105: legacy b/g uses one TX chain;
 * maximum measured power is 24.81 dBm conducted, with 3.20 dBi net gain.
 * Round these up to 25 dBm and 4 dBi, add a 3 dB engineering margin,
 * and credit only the coarse 12 dB BBP reduction: 20 dBm EIRP.
 * This bound applies to the original DIR-685 module and antennas only.
 * The firmware startup fix applies the reduction before beaconing;
 * TxPower=5 preserves it, with per-rate powers fixed to EEPROM values.
 */
static bool inic_ap_allowed(struct wiphy *wiphy, struct cfg80211_chan_def *chandef)
{
	return chandef->chan->max_power >= INIC_MAX_EIRP_DBM &&
		cfg80211_reg_can_beacon(wiphy, chandef, NL80211_IFTYPE_AP);
}

static void inic_reg_notifier(struct wiphy *wiphy, struct regulatory_request *request)
{
	struct inic *inic = wiphy_priv(wiphy);

	/* The wiphy mutex serializes this with AP startup and shutdown. */
	if (!inic->ap_started || inic_ap_allowed(wiphy, &inic->chandef))
		return;
	/* Reset the autonomous transmitter before notifying cfg80211. */
	inic_abort_ap(inic);
	dev_warn(&inic->pdev->dev, "regulatory change forced AP shutdown\n");
}

static int inic_set_tx_power(struct wiphy *wiphy, struct wireless_dev *wdev,
			     int radio_idx, enum nl80211_tx_power_setting type, int mbm)
{
	/* A bound is available, not an exact output level or a finer power step. */
	switch (type) {
	case NL80211_TX_POWER_AUTOMATIC:
		return 0;
	case NL80211_TX_POWER_LIMITED:
		return mbm >= DBM_TO_MBM(INIC_MAX_EIRP_DBM) ? 0 : -ERANGE;
	case NL80211_TX_POWER_FIXED:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}

void inic_station_event(struct wiphy *wiphy, struct wiphy_work *work)
{
	struct inic *inic = wiphy_priv(wiphy);
	struct sk_buff *skb;

	if (READ_ONCE(inic->events_lost)) {
		/* Without a complete event stream, neither station table is reliable. */
		inic_abort_ap(inic);
		skb_queue_purge(&inic->events);
		return;
	}
	while ((skb = skb_dequeue(&inic->events))) {
		const struct inic_msg *msg = (const void *)(skb->data + ETH_HLEN);
		const u8 *mac = msg->data + 2;
		struct inic_station *station = inic_find_station(inic, mac);
		struct station_info sinfo = {};
		unsigned long flags;
		u8 ies[256] = {};
		u16 length = 0;
		int i, ret;

		if (!READ_ONCE(inic->ap_started))
			goto next;
		if (le32_to_cpu(msg->status) == IWEVEXPIRED) {
			if (station) {
				spin_lock_irqsave(&inic->lock, flags);
				memzero_explicit(station, sizeof(*station));
				spin_unlock_irqrestore(&inic->lock, flags);
				inic->station_generation++;
				cfg80211_del_sta(&inic->wdev, mac, GFP_KERNEL);
			}
			goto next;
		}
		if (!station) {
			for (i = 0; i < INIC_MAX_STATIONS; i++) {
				if (!inic->stations[i].used) {
					station = &inic->stations[i];
					break;
				}
			}
		}
		if (!station) {
			inic_deauth(inic, mac, WLAN_REASON_DISASSOC_AP_BUSY);
			goto next;
		}
		/* Close the previous association's port before querying new IEs. */
		spin_lock_irqsave(&inic->lock, flags);
		station->authorized = false;
		station->key_installed = false;
		spin_unlock_irqrestore(&inic->lock, flags);
		if (inic->secure) {
			ether_addr_copy(ies, mac);
			ret = inic_auth_command(inic, true, INIC_AUTH_RSN_IE, ies, sizeof(ies),
						&length);
			if (ret || length < 2 || ies[0] != WLAN_EID_RSN || ies[1] + 2 != length) {
				inic_deauth(inic, mac, WLAN_REASON_INVALID_IE);
				goto next;
			}
			sinfo.assoc_req_ies = ies;
			sinfo.assoc_req_ies_len = length;
		}
		spin_lock_irqsave(&inic->lock, flags);
		memzero_explicit(station, sizeof(*station));
		station->used = true;
		station->connected = jiffies;
		station->authorized = !inic->control_port;
		ether_addr_copy(station->mac, mac);
		spin_unlock_irqrestore(&inic->lock, flags);
		sinfo.generation = ++inic->station_generation;
		cfg80211_new_sta(&inic->wdev, mac, &sinfo, GFP_KERNEL);
next:
		kfree_skb(skb);
	}
}

static int inic_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			int link_id, u8 index, enum nl80211_key_type type,
			const u8 *mac, struct key_params *params)
{
	struct inic *inic = wiphy_priv(wiphy);
	struct inic_station *station = NULL;
	unsigned long flags;
	u8 request[INIC_KEY_REQUEST_SIZE] = {};
	int ret;

	if (!inic->ap_started || !inic->secure)
		return -ENETDOWN;
	if (link_id > 0 || params->cipher != WLAN_CIPHER_SUITE_CCMP ||
	    params->key_len != WLAN_KEY_LEN_CCMP || index > 3 ||
	    params->mode != NL80211_KEY_RX_TX || params->vlan_id || params->ltf_keyseed_len ||
	    (type != NL80211_KEYTYPE_PAIRWISE && type != NL80211_KEYTYPE_GROUP) ||
	    (params->seq_len && (params->seq_len != 6 ||
				memchr_inv(params->seq, 0, params->seq_len))))
		return -EOPNOTSUPP;
	if (type == NL80211_KEYTYPE_PAIRWISE) {
		if (!mac || index)
			return -EINVAL;
		station = inic_find_station(inic, mac);
		if (!station)
			return -ENOENT;
		if (station->key_installed && !memcmp(station->key, params->key, params->key_len))
			return 0;
		put_unaligned_le16(INIC_KEY_INDEX_PAIRWISE, request + 2);
		ether_addr_copy(request + 6, mac);
		request[5] = INIC_KEY_TX | INIC_KEY_RX;
	} else {
		int i;

		if ((inic->group_keys_valid & BIT(index)) &&
		    !memcmp(inic->group_keys[index], params->key, params->key_len))
			return 0;
		/* Reusing an existing GTK in another slot would reset its PN. */
		for (i = 0; i < ARRAY_SIZE(inic->group_keys); i++)
			if ((inic->group_keys_valid & BIT(i)) &&
			    !memcmp(inic->group_keys[i], params->key, params->key_len))
				return -EOPNOTSUPP;
		/*
		 * The firmware always selects and resets a GTK when installing
		 * it, ignoring the default-key flag. AP group keys protect TX;
		 * stage them here until set_default_key() activates the new key.
		 * Client-to-AP data is protected by the client's pairwise key.
		 * A fresh key may also replace the active slot; clearing its
		 * installed bit blocks group TX until the replacement is selected.
		 */
		memcpy(inic->group_keys[index], params->key, params->key_len);
		inic->group_keys_valid |= BIT(index);
		WRITE_ONCE(inic->group_keys_installed, inic->group_keys_installed & ~BIT(index));
		return 0;
	}
	/* Firmware's fixed 60-byte ieee80211req_key, including two zero counters. */
	request[0] = INIC_CIPHER_CCMP;
	request[4] = WLAN_KEY_LEN_CCMP;
	memcpy(request + 28, params->key, WLAN_KEY_LEN_CCMP);
	ret = inic_auth_command(inic, false, INIC_AUTH_SET_KEY, request, sizeof(request), NULL);
	memzero_explicit(request, sizeof(request));
	if (ret) {
		/* An ambiguous key update must not leave an active, uncontrolled AP. */
		inic_abort_ap(inic);
		return ret;
	}
	spin_lock_irqsave(&inic->lock, flags);
	station->key_installed = true;
	memcpy(station->key, params->key, WLAN_KEY_LEN_CCMP);
	spin_unlock_irqrestore(&inic->lock, flags);
	return 0;
}

static int inic_get_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			int link_id, u8 index, enum nl80211_key_type type,
			const u8 *mac, void *cookie,
			void (*callback)(void *cookie, struct key_params *params))
{
	struct inic *inic = wiphy_priv(wiphy);
	u8 sequence[6] = {};
	struct key_params params = {
		.cipher = WLAN_CIPHER_SUITE_CCMP,
		.key_len = WLAN_KEY_LEN_CCMP,
		.seq = sequence,
		.seq_len = sizeof(sequence),
	};
	int ret;

	if (!inic->ap_started || !inic->secure)
		return -ENETDOWN;
	if (link_id > 0 || index > 3 || type != NL80211_KEYTYPE_GROUP || mac)
		return -EOPNOTSUPP;
	if (!(inic->group_keys_valid & BIT(index)))
		return -ENOENT;
	if (inic->group_keys_installed & BIT(index)) {
		/* The ASIC has one live group counter per BSSID, not per slot. */
		if (index != inic->group_key)
			return -EOPNOTSUPP;
		ret = inic_group_sequence(inic, index, sequence);
		if (ret)
			return ret;
	}
	/* A staged key has never transmitted, so its PN is still zero. */
	params.key = inic->group_keys[index];
	callback(cookie, &params);
	return 0;
}

static int inic_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			int link_id, u8 index, enum nl80211_key_type type, const u8 *mac)
{
	struct inic *inic = wiphy_priv(wiphy);
	struct inic_station *station = NULL;
	unsigned long flags;
	u8 request[INIC_KEY_REQUEST_SIZE] = {};
	int ret;

	if (!inic->ap_started)
		return 0;
	if (link_id > 0 || index > 3)
		return -EINVAL;
	if (type == NL80211_KEYTYPE_PAIRWISE && mac) {
		station = inic_find_station(inic, mac);
		if (!station || !station->key_installed)
			return 0;
		spin_lock_irqsave(&inic->lock, flags);
		station->key_installed = false;
		station->authorized = false;
		spin_unlock_irqrestore(&inic->lock, flags);
		put_unaligned_le16(INIC_KEY_INDEX_PAIRWISE, request + 2);
		ether_addr_copy(request + 6, mac);
	} else if (type == NL80211_KEYTYPE_GROUP) {
		if (!(inic->group_keys_valid & BIT(index)))
			return 0;
		inic->group_keys_valid &= ~BIT(index);
		memzero_explicit(inic->group_keys[index], WLAN_KEY_LEN_CCMP);
		/* A staged replacement may still have an older key in its ASIC slot. */
		WRITE_ONCE(inic->group_keys_installed, inic->group_keys_installed & ~BIT(index));
		put_unaligned_le16(index, request + 2);
	} else {
		return -EINVAL;
	}
	ret = inic_auth_command(inic, false, INIC_AUTH_DEL_KEY, request, sizeof(request), NULL);
	if (ret) {
		inic_abort_ap(inic);
		return ret;
	}
	if (station) {
		spin_lock_irqsave(&inic->lock, flags);
		memzero_explicit(station->key, sizeof(station->key));
		spin_unlock_irqrestore(&inic->lock, flags);
	}
	return 0;
}

static int inic_default_key(struct wiphy *wiphy, struct net_device *dev, int link_id,
			    u8 index, bool unicast, bool multicast)
{
	struct inic *inic = wiphy_priv(wiphy);
	u8 request[INIC_KEY_REQUEST_SIZE] = {};
	int ret;

	if (!inic->ap_started || !inic->secure)
		return -ENETDOWN;
	if (link_id > 0 || index > 3 || unicast || !multicast)
		return -EOPNOTSUPP;
	if (!(inic->group_keys_valid & BIT(index)))
		return -ENOENT;
	if (inic->group_keys_installed & BIT(index)) {
		/* Reinstallation of an old GTK would reset its packet number. */
		return index == inic->group_key ? 0 : -EOPNOTSUPP;
	}
	request[0] = INIC_CIPHER_CCMP;
	put_unaligned_le16(index, request + 2);
	request[4] = WLAN_KEY_LEN_CCMP;
	request[5] = INIC_KEY_TX | INIC_KEY_RX | INIC_KEY_DEFAULT;
	eth_broadcast_addr(request + 6);
	memcpy(request + 28, inic->group_keys[index], WLAN_KEY_LEN_CCMP);
	ret = inic_auth_command(inic, false, INIC_AUTH_SET_KEY, request, sizeof(request), NULL);
	memzero_explicit(request, sizeof(request));
	if (ret) {
		inic_abort_ap(inic);
		return ret;
	}
	WRITE_ONCE(inic->group_key, index);
	WRITE_ONCE(inic->group_keys_installed, inic->group_keys_installed | BIT(index));
	return 0;
}

static int inic_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			       const u8 *mac, struct station_parameters *params)
{
	struct inic *inic = wiphy_priv(wiphy);
	struct inic_station *station = inic_find_station(inic, mac);
	unsigned long flags;
	u32 mask;
	bool authorized;
	int ret;

	if (!station)
		return -ENOENT;
	ret = cfg80211_check_station_change(wiphy, params, CFG80211_STA_AP_MLME_CLIENT);
	if (ret)
		return ret;
	mask = params->sta_flags_mask;
	authorized = params->sta_flags_set & BIT(NL80211_STA_FLAG_AUTHORIZED);
	if (mask & ~(BIT(NL80211_STA_FLAG_AUTHORIZED) | BIT(NL80211_STA_FLAG_WME) |
		     BIT(NL80211_STA_FLAG_SHORT_PREAMBLE) | BIT(NL80211_STA_FLAG_AUTHENTICATED) |
		     BIT(NL80211_STA_FLAG_ASSOCIATED)))
		return -EOPNOTSUPP;
	if (!(mask & BIT(NL80211_STA_FLAG_AUTHORIZED)))
		return 0;
	if (authorized && inic->secure && !station->key_installed)
		return -EINVAL;
	/* Key installation opens the firmware's port. Revoke it by disconnecting. */
	if (!authorized && station->authorized) {
		spin_lock_irqsave(&inic->lock, flags);
		station->authorized = false;
		spin_unlock_irqrestore(&inic->lock, flags);
		return inic_deauth(inic, mac, WLAN_REASON_PREV_AUTH_NOT_VALID);
	}
	spin_lock_irqsave(&inic->lock, flags);
	station->authorized = authorized;
	spin_unlock_irqrestore(&inic->lock, flags);
	return 0;
}

static int inic_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			    struct station_del_parameters *params)
{
	struct inic *inic = wiphy_priv(wiphy);
	unsigned long flags;
	int i, ret;

	if (!inic->ap_started)
		return 0;
	if (params->mac && !is_broadcast_ether_addr(params->mac)) {
		struct inic_station *station = inic_find_station(inic, params->mac);

		if (!station)
			return 0;
		spin_lock_irqsave(&inic->lock, flags);
		station->authorized = false;
		spin_unlock_irqrestore(&inic->lock, flags);
		return inic_deauth(inic, params->mac, params->reason_code);
	}
	for (i = 0; i < INIC_MAX_STATIONS; i++) {
		if (!inic->stations[i].used)
			continue;
		spin_lock_irqsave(&inic->lock, flags);
		inic->stations[i].authorized = false;
		spin_unlock_irqrestore(&inic->lock, flags);
		ret = inic_deauth(inic, inic->stations[i].mac, params->reason_code);
		if (ret)
			return ret;
	}
	return 0;
}

static int inic_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			    const u8 *mac, struct station_info *sinfo)
{
	struct inic *inic = wiphy_priv(wiphy);
	struct inic_station *station = inic_find_station(inic, mac);

	if (!inic->ap_started || !station)
		return -ENOENT;
	sinfo->generation = inic->station_generation;
	sinfo->connected_time = (jiffies - station->connected) / HZ;
	sinfo->filled = BIT_ULL(NL80211_STA_INFO_CONNECTED_TIME);
	sinfo->sta_flags.mask = BIT(NL80211_STA_FLAG_AUTHORIZED);
	if (station->authorized)
		sinfo->sta_flags.set = BIT(NL80211_STA_FLAG_AUTHORIZED);
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_STA_FLAGS);
	return 0;
}

static int inic_dump_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			     int index, u8 *mac, struct station_info *sinfo)
{
	struct inic *inic = wiphy_priv(wiphy);
	int i;

	for (i = 0; i < INIC_MAX_STATIONS; i++) {
		if (!inic->stations[i].used || index--)
			continue;
		ether_addr_copy(mac, inic->stations[i].mac);
		return inic_get_station(wiphy, wdev, mac, sinfo);
	}
	return -ENOENT;
}

/* The card generates a fixed legacy beacon. Accept only matching security IEs. */
static bool inic_beacon_supported(const struct cfg80211_beacon_data *beacon, bool secure)
{
	static const u8 rsn[] = {
		1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4,
		1, 0, 0, 0x0f, 0xac, 2, 0, 0,
	};
	static const u8 extended_rates[] = { 48, 72, 96, 108 };
	const struct element *element;
	bool found = false;

	for_each_element(element, beacon->tail, beacon->tail_len) {
		switch (element->id) {
		case WLAN_EID_RSN:
			if (!secure || found || element->datalen != sizeof(rsn) ||
			    memcmp(element->data, rsn, sizeof(rsn)))
				return false;
			found = true;
			break;
		case WLAN_EID_ERP_INFO:
			/* The legacy profile uses long preambles. */
			if (element->datalen != 1 ||
			    (element->data[0] & ~WLAN_ERP_BARKER_PREAMBLE))
				return false;
			break;
		case WLAN_EID_EXT_SUPP_RATES:
			if (element->datalen != sizeof(extended_rates) ||
			    memcmp(element->data, extended_rates, sizeof(extended_rates)))
				return false;
			break;
		case WLAN_EID_EXT_CAPABILITY:
			if (memchr_inv(element->data, 0, element->datalen))
				return false;
			break;
		default:
			return false;
		}
	}
	return for_each_element_completed(element, beacon->tail, beacon->tail_len) &&
		(!secure || found);
}

static int inic_change_beacon(struct wiphy *wiphy, struct net_device *dev,
			      struct cfg80211_ap_update *update)
{
	struct inic *inic = wiphy_priv(wiphy);

	if (!inic->ap_started)
		return -ENETDOWN;
	/* The firmware owns the beacon and cannot apply userspace templates. */
	return -EOPNOTSUPP;
}

static int inic_change_bss(struct wiphy *wiphy, struct net_device *dev,
			   struct bss_parameters *params)
{
	static const u8 basic_rates[] = { 2, 4, 11, 22 };

	/* Match the legacy profile; firmware maintains ERP protection itself. */
	if (params->link_id > 0 || params->ap_isolate == 0 ||
	    params->use_short_preamble > 0 || params->ht_opmode >= 0 ||
	    params->p2p_ctwindow > 0 || params->p2p_opp_ps > 0)
		return -EOPNOTSUPP;
	if (params->basic_rates &&
	    (params->basic_rates_len != sizeof(basic_rates) ||
	     memcmp(params->basic_rates, basic_rates, sizeof(basic_rates))))
		return -EOPNOTSUPP;
	return 0;
}

static int inic_set_wiphy_params(struct wiphy *wiphy, int radio_idx, u32 changed)
{
	if (changed & ~(WIPHY_PARAM_RTS_THRESHOLD | WIPHY_PARAM_FRAG_THRESHOLD))
		return -EOPNOTSUPP;
	if ((changed & WIPHY_PARAM_RTS_THRESHOLD) &&
	    wiphy->rts_threshold != (u32)-1 && wiphy->rts_threshold != 2347)
		return -EOPNOTSUPP;
	if ((changed & WIPHY_PARAM_FRAG_THRESHOLD) &&
	    wiphy->frag_threshold != (u32)-1 && wiphy->frag_threshold != 2346)
		return -EOPNOTSUPP;
	return 0;
}

static int inic_start_ap(struct wiphy *wiphy, struct net_device *dev,
			 struct cfg80211_ap_settings *settings)
{
	struct inic *inic = wiphy_priv(wiphy);
	struct cfg80211_crypto_settings *crypto = &settings->crypto;
	struct ieee80211_channel *channel = settings->chandef.chan;
	char *profile;
	bool secure = crypto->wpa_versions != 0;
	__le32 auth[] = {
		cpu_to_le32(secure ? BIT(3) | BIT(4) : BIT(1)), /* WPA2 + PSK / open */
		0, /* No IEEE8021X. */
		cpu_to_le32(secure ? BIT(2) : BIT(0)), /* CCMP / no encryption */
		cpu_to_le32(1), /* Association events. */
	};
	int len, ret, i;

	if (inic->running)
		return -EBUSY;
	if (!channel || channel->band != NL80211_BAND_2GHZ ||
	    settings->chandef.width != NL80211_CHAN_WIDTH_20_NOHT ||
	    settings->ssid_len < 1 || settings->ssid_len > IEEE80211_MAX_SSID_LEN ||
	    settings->hidden_ssid != NL80211_HIDDEN_SSID_NOT_IN_USE ||
	    (settings->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM &&
	     settings->auth_type != NL80211_AUTHTYPE_AUTOMATIC) ||
	    settings->beacon_interval < 20 || settings->beacon_interval > 1000 ||
	    settings->dtim_period < 1 || settings->dtim_period > U8_MAX ||
	    settings->privacy != secure ||
	    settings->beacon.assocresp_ies_len || settings->beacon.proberesp_ies_len ||
	    settings->beacon.beacon_ies_len ||
	    settings->p2p_ctwindow || settings->p2p_opp_ps || crypto->sae_pwe ||
	    settings->mbssid_config.tx_wdev || settings->acl || settings->pbss ||
	    settings->ht_cap || settings->vht_cap || settings->he_cap ||
	    settings->eht_cap || settings->twt_responder || settings->inactivity_timeout)
		return -EOPNOTSUPP;
	/* The vendor profile is line-oriented and cannot represent binary SSIDs. */
	for (i = 0; i < settings->ssid_len; i++)
		if (!isprint(settings->ssid[i]) || settings->ssid[i] == ';')
			return -EOPNOTSUPP;
	if (secure) {
		if (crypto->wpa_versions != NL80211_WPA_VERSION_2 || crypto->psk ||
		    crypto->cipher_group != WLAN_CIPHER_SUITE_CCMP ||
		    crypto->n_ciphers_pairwise != 1 ||
		    crypto->ciphers_pairwise[0] != WLAN_CIPHER_SUITE_CCMP ||
		    crypto->n_akm_suites != 1 || crypto->akm_suites[0] != WLAN_AKM_SUITE_PSK)
			return -EOPNOTSUPP;
	} else if (crypto->cipher_group || crypto->n_ciphers_pairwise ||
		   crypto->n_akm_suites) {
		return -EOPNOTSUPP;
	}
	/* SA Query offload support is a userspace capability, not a request. */
	if (crypto->control_port_over_nl80211 || crypto->sae_pwd ||
	    (crypto->control_port_ethertype &&
	     crypto->control_port_ethertype != htons(ETH_P_PAE)) ||
	    (settings->flags & ~NL80211_AP_SETTINGS_SA_QUERY_OFFLOAD_SUPPORT))
		return -EOPNOTSUPP;
	if (!inic_beacon_supported(&settings->beacon, secure))
		return -EOPNOTSUPP;
	if (!of_machine_is_compatible("dlink,dir-685")) {
		dev_err(&inic->pdev->dev, "no qualified transmit-power bound for this board\n");
		return -EOPNOTSUPP;
	}
	if (!inic_ap_allowed(wiphy, &settings->chandef)) {
		dev_err(&inic->pdev->dev, "AP requires a permitted channel with at least %u dBm EIRP\n",
			INIC_MAX_EIRP_DBM);
		return -EINVAL;
	}
	profile = kzalloc(2048, GFP_KERNEL);
	if (!profile)
		return -ENOMEM;
	/* Firmware owns the AP SME and encryption; hostapd owns the handshake. */
	len = scnprintf(profile, 2048,
			"Default\nCountryRegion=0\nBssidNum=1\nWirelessMode=0\n"
			"SSID1=%.*s\nChannel=%u\nAutoChannelSelect=0\n"
			"BeaconPeriod=%u\nDtimPeriod=%u\nBasicRate=15\nTxRate=0\n"
			"TxPower=%u\nBGProtection=0\nTxPreamble=0\nShortSlot=1\n"
			"RTSThreshold=2347\nFragThreshold=2346\nTxBurst=0\nPktAggregate=0\n"
			"WmmCapable=0\nAPSDCapable=0\nHideSSID=0\nIEEE8021X=0\n"
			"AuthMode=%s\nEncrypType=%s\nDefaultKeyID=1\n"
			"RekeyMethod=DISABLE\nNoForwarding=1\n"
			"NoForwardingBTNBSSID=1\nWdsEnable=0\nApCliEnable=0\n"
			"HT_BW=0\nHT_AutoBA=0\nHT_AMSDU=0\nExtEEPROM=0\n"
			"WirelessEvent=1\nWscConfMode=0\n"
			"__SkipIfNotINIC__MAC=%pM\n__SkipIfNotINIC__BUILT_IN_BRIDGE=0\n",
			(int)settings->ssid_len, settings->ssid,
			ieee80211_frequency_to_channel(channel->center_freq),
			settings->beacon_interval, settings->dtim_period,
			INIC_TX_POWER_PERCENT,
			secure ? "WPA2PSK" : "OPEN", secure ? "AES" : "NONE", dev->dev_addr);
	ret = inic_boot(inic, profile, len);
	kfree_sensitive(profile);
	if (ret)
		return ret;
	inic->secure = secure;
	inic->control_port = secure || crypto->control_port;
	WRITE_ONCE(inic->events_lost, false);
	memzero_explicit(inic->stations, sizeof(inic->stations));
	memzero_explicit(inic->group_keys, sizeof(inic->group_keys));
	inic->group_keys_valid = 0;
	inic->group_keys_installed = 0;
	inic->group_key = 0xff;
	inic->chandef = settings->chandef;
	/* Publish the AP policy before RX and TX can use it. */
	smp_store_release(&inic->ap_started, true);
	/* Match the stock authenticator's auth mode, 802.1X, cipher and event setup. */
	for (i = 0; i < ARRAY_SIZE(auth); i++) {
		ret = inic_auth_command(inic, false, i + 2, &auth[i], sizeof(auth[i]), NULL);
		if (ret) {
			inic_hw_stop(inic);
			wiphy_work_cancel(wiphy, &inic->event_work);
			skb_queue_purge(&inic->events);
			return ret;
		}
	}
	netif_carrier_on(dev);
	netif_wake_queue(dev);
	return 0;
}

static int inic_stop_ap(struct wiphy *wiphy, struct net_device *dev,
			unsigned int link_id)
{
	struct inic *inic = wiphy_priv(wiphy);

	inic_hw_stop(inic);
	wiphy_work_cancel(wiphy, &inic->event_work);
	skb_queue_purge(&inic->events);
	WRITE_ONCE(inic->events_lost, false);
	memzero_explicit(inic->stations, sizeof(inic->stations));
	memzero_explicit(inic->group_keys, sizeof(inic->group_keys));
	inic->group_keys_valid = 0;
	inic->group_keys_installed = 0;
	return 0;
}

static int inic_change_iface(struct wiphy *wiphy, struct net_device *dev,
			     enum nl80211_iftype type, struct vif_params *params)
{
	return type == NL80211_IFTYPE_AP ? 0 : -EOPNOTSUPP;
}

const struct cfg80211_ops inic_cfg_ops = {
	.add_key = inic_add_key,
	.get_key = inic_get_key,
	.del_key = inic_del_key,
	.set_default_key = inic_default_key,
	.change_station = inic_change_station,
	.del_station = inic_del_station,
	.get_station = inic_get_station,
	.dump_station = inic_dump_station,
	.change_beacon = inic_change_beacon,
	.change_bss = inic_change_bss,
	.set_wiphy_params = inic_set_wiphy_params,
	.set_tx_power = inic_set_tx_power,
	.start_ap = inic_start_ap,
	.stop_ap = inic_stop_ap,
	.change_virtual_intf = inic_change_iface,
};

int inic_wireless_register(struct inic *inic)
{
	struct wiphy *wiphy = inic->wiphy;
	struct net_device *dev = inic->netdev;
	int i, ret;

	set_wiphy_dev(wiphy, &inic->pdev->dev);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_AP);
	wiphy->max_ap_assoc_sta = 32;
	wiphy->cipher_suites = inic_ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(inic_ciphers);
	wiphy->akm_suites = inic_akms;
	wiphy->n_akm_suites = ARRAY_SIZE(inic_akms);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME;
	wiphy->reg_notifier = inic_reg_notifier;
	for (i = 0; i < ARRAY_SIZE(inic->channels); i++) {
		inic->channels[i].band = NL80211_BAND_2GHZ;
		inic->channels[i].center_freq = 2412 + 5 * i;
		inic->channels[i].hw_value = i + 1;
		inic->channels[i].max_power = INIC_MAX_EIRP_DBM;
	}
	inic->band.channels = inic->channels;
	inic->band.n_channels = ARRAY_SIZE(inic->channels);
	inic->band.bitrates = inic_rates;
	inic->band.n_bitrates = ARRAY_SIZE(inic_rates);
	wiphy->bands[NL80211_BAND_2GHZ] = &inic->band;
	inic->wdev.wiphy = wiphy;
	inic->wdev.iftype = NL80211_IFTYPE_AP;
	inic->wdev.netdev = dev;
	dev->ieee80211_ptr = &inic->wdev;
	SET_NETDEV_DEV(dev, &inic->pdev->dev);
	dev->netdev_ops = &inic_netdev_ops;
	dev->needed_headroom = 2 * VLAN_HLEN;
	dev->max_mtu = ETH_DATA_LEN;
	strscpy(dev->name, "wlan%d", IFNAMSIZ);
	eth_hw_addr_random(dev);
	netif_carrier_off(dev);
	ret = wiphy_register(wiphy);
	if (ret)
		return ret;
	ret = register_netdev(dev);
	if (ret)
		wiphy_unregister(wiphy);
	return ret;
}

void inic_wireless_unregister(struct inic *inic)
{
	unregister_netdev(inic->netdev);
	wiphy_lock(inic->wiphy);
	wiphy_work_cancel(inic->wiphy, &inic->event_work);
	skb_queue_purge(&inic->events);
	wiphy_unlock(inic->wiphy);
	wiphy_unregister(inic->wiphy);
}
