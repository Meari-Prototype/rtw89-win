// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: received frames. rtw89 hands every frame to
 * ieee80211_rx_napi() from its NAPI poll (a work queue thread, PASSIVE_LEVEL), with the FCS still on and
 * sta = NULL; the skb is ours.
 *  - Beacons and probe responses fill the BSS table (bss.c); probe responses to us also count for TXPROBE.
 *  - While a connection is being made or exists, the MLME frames from the BSS (authentication, association
 *    response, deauthentication, disassociation, action, including the block ack action frames) go to the MLME
 *    (mlme.c), which takes the skb.
 *  - A BlockAckReq control frame from our AP pushes the matching reorder window on (agg.c).
 *  - Data frames go up only while connected (mac80211 rx.c order): from our BSS (From DS) to us or to a group,
 *    not our own multicast coming back, not fragmented, no duplicate; QoS control (and HT control) removed,
 *    A-MSDUs split. A unicast QoS data frame on a TID with a block ack session goes through that TID's reorder
 *    buffer (agg.c), which hands each frame back here in sequence order; every other data frame is delivered at
 *    once. mac_rx_deliver() owns the skb from then on: it re-parses the header, does the key and PN check
 *    and builds one 24-byte data header (From DS: DA, BSSID, SA), the LLC/SNAP header and the payload, copied
 *    into a FIFO the front end empties (rtw_linux_rx_data()).
 *  - While our GO runs, frames addressed to its BSSID and probe requests go to ap.c first (mac_ap_rx).
 *  - An A-MSDU is checked whole before its subframes go up (rx_amsdu_ok, rx_amsdu); the rate of the last data
 *    frame and frames with +HTC are recorded for the diagnostics.
 *  - A protected frame must have been decrypted by the hardware (RX_FLAG_DECRYPTED; rtw89 leaves the CCMP
 *    header and the MIC in the frame, no IV_STRIPPED/MIC_STRIPPED) and pass key.c's key and PN check (per TID, the
 *    group key's own counters for group frames), which happens here, after reordering, in sequence order; then
 *    both come off and the frame goes up without the Protected bit. An unprotected frame on an RSN connection
 *    goes up only if the exemption list lets it (EAPOL before the key).
 * Ownership: every data frame reaching rx_data() is taken over here, so linuxkpi_ieee80211_rx() frees the skb
 * only on the paths that did not pass it on. A frame held in a reorder buffer is freed later (agg.c), either when
 * it is delivered or when the buffer is dropped.
 */
#include "mac.h"

#define RX_FIFO_MAX                     256
#define MAC_HDR_LEN                     24
#define FC_DATA_FROMDS                  (IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA | IEEE80211_FCTL_FROMDS)

static const u8 rx_rfc1042[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
static const u8 rx_bridge_tunnel[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8 };

/* the EtherType of an MSDU that starts with LLC/SNAP, 0 otherwise */
static u16 rx_ethertype (const u8 *body, unsigned int len)
{
	if (len < 8 || (memcmp(body, rx_rfc1042, 6) && memcmp(body, rx_bridge_tunnel, 6)))
		return 0;
	return get_unaligned_be16(body + 6);
}

void mac_rx_init (struct mac_hw *mh)
{
	skb_queue_head_init(&mh->rx_fifo);
}

void mac_rx_flush (struct mac_hw *mh)
{
	skb_queue_purge(&mh->rx_fifo);
}

/* One MSDU for the OS: 24-byte header with From DS (A1 = DA, A2 = BSSID, A3 = SA), then body (LLC/SNAP...). */
static void rx_fifo_put (struct mac_hw *mh, const u8 *da, const u8 *bssid, const u8 *sa, __le16 seq_ctrl,
	const u8 *body, unsigned int len, u32 info)
{
	struct sk_buff *skb;
	u8 *p;

	if (len > MAC_FRAME_MAX) {
		MAC_COUNT(rx_dropped_toolarge);
		return;
	}
	if (skb_queue_len_lockless(&mh->rx_fifo) >= RX_FIFO_MAX) {
		MAC_COUNT(rx_dropped_full);
		return;
	}
	skb = alloc_skb(MAC_HDR_LEN + len, GFP_ATOMIC);
	if (!skb) {
		MAC_COUNT(rx_dropped_full);
		return;
	}
	p = skb_put_zero(skb, MAC_HDR_LEN);
	put_unaligned_le16(FC_DATA_FROMDS, p);
	ether_addr_copy(p + 4, da);
	ether_addr_copy(p + 10, bssid);
	ether_addr_copy(p + 16, sa);
	memcpy(p + 22, &seq_ctrl, 2);
	skb_put_data(skb, body, len);
	skb->mac_tag = info;            /* RTW_RX_* for rtw_linux_rx_data() */
	skb_queue_tail(&mh->rx_fifo, skb);
}

/*
 * A-MSDU (802.11-2020 9.3.2.2.2): subframes of DA, SA, length (big endian), MSDU, padded to 4 bytes but the last.
 * Checked whole before any subframe goes up, like the GO's (ap.c ap_amsdu_ok) and
 * cfg80211 ieee80211_amsdu_to_8023s: every length fits, and the first subframe's DA is not the RFC 1042 header (the
 * A-MSDU injection of FragAttacks, CVE-2020-24588: a plain MSDU whose A-MSDU bit was flipped).
 */
static bool rx_amsdu_ok (const u8 *p, unsigned int len)
{
	bool first = true;

	while (len) {
		unsigned int sub, total;

		if (len < 14)
			return false;
		sub = get_unaligned_be16(p + 12);
		if (sub > len - 14)
			return false;
		if (first && !memcmp(p, rx_rfc1042, 6))
			return false;
		first = false;
		total = 14 + sub;
		if (total < len)
			total = ALIGN(total, 4);
		if (total >= len)
			break;
		p += total;
		len -= total;
	}
	return true;
}

/* Hands up the subframes of an A-MSDU rx_amsdu_ok() accepted. A subframe goes up only with a DA of ours or a group
   (cfg80211 check_da; the SA may be anyone behind the AP), not our own multicast echoed by the AP, and an 802.1X one
   only to us or the PAE group address (mac80211 ieee80211_frame_allowed). */
static void rx_amsdu (struct mac_hw *mh, const u8 *bssid, const u8 *own, __le16 seq_ctrl, const u8 *p,
	unsigned int len, u32 info)
{
	static const u8 pae_group[ETH_ALEN] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x03 };

	while (len >= 14) {
		unsigned int sub = get_unaligned_be16(p + 12), total = 14 + sub;
		bool group = is_multicast_ether_addr(p);

		if (sub > len - 14)
			break;                  /* not after rx_amsdu_ok() */
		if (group && ether_addr_equal(p + 6, own))
			;                       /* our own multicast frame echoed by the AP */
		else if ((!group && !ether_addr_equal(p, own)) ||
			 (rx_ethertype(p + 14, sub) == ETH_P_PAE && !ether_addr_equal(p, own) &&
			  !ether_addr_equal(p, pae_group)))
			MAC_COUNT(rx_amsdu_skipped);
		else
			rx_fifo_put(mh, p, bssid, p + 6, seq_ctrl, p + 14, sub, info);
		if (total < len)
			total = ALIGN(total, 4);
		if (total >= len)
			break;
		p += total;
		len -= total;
	}
}

/* What the last data frame from the AP came with (rtwdiag link) */
static void rx_note_rate (const struct ieee80211_rx_status *rs)
{
	static const u8 bw_mhz[] = { [RATE_INFO_BW_20] = 20, [RATE_INFO_BW_40] = 40, [RATE_INFO_BW_80] = 80,
				     [RATE_INFO_BW_160] = 160 };
	u8 enc = rs->encoding, nss = rs->nss;

	if (enc == RX_ENC_HT)
		nss = (u8)(rs->rate_idx / 8 + 1);
	else if (enc == RX_ENC_LEGACY)
		nss = 1;
	WRITE_ONCE(mac_st.rx_last_enc, enc);
	WRITE_ONCE(mac_st.rx_last_bw, rs->bw < ARRAY_SIZE(bw_mhz) ? bw_mhz[rs->bw] : 0);
	WRITE_ONCE(mac_st.rx_last_mcs, rs->rate_idx);
	WRITE_ONCE(mac_st.rx_last_nss, nss);
}

/*
 * Hands one received data frame to the OS: re-parses the frame (it may have waited in a reorder buffer), does the
 * key and PN check and the privacy exemption check, then puts the MSDU(s) into the FIFO. Takes the skb in
 * every case. The caller holds rcu_read_lock() (the connection's keys), is connected, and - for frames of a TID
 * with a block ack session - holds mac_hw.rx_lock, so each key's PN slot has one writer. Does not signal the front
 * end; the caller does (mac_dp_rx_ready) after releasing rx_lock.
 */
void mac_rx_deliver (struct mac_hw *mh, struct sk_buff *skb)
{
	const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)skb->data;
	const struct ieee80211_rx_status *rs = IEEE80211_SKB_RXCB(skb);
	const u8 *own = mh->hw.wiphy->perm_addr;
	__le16 seq_ctrl = hdr->seq_ctrl;
	u16 fc = le16_to_cpu(hdr->frame_control);
	unsigned int hdrlen, slot, len;
	bool qos, group, amsdu = false;
	const u8 *body;
	u32 info = 0;
	int action;
	u16 qc = 0;

	group = is_multicast_ether_addr(hdr->addr1);
	hdrlen = ieee80211_hdrlen(hdr->frame_control);
	qos = ieee80211_is_data_qos(hdr->frame_control);
	if (hdrlen < MAC_HDR_LEN || skb->len < hdrlen) {
		MAC_COUNT(rx_dropped_other);
		goto drop;
	}
	if (qos) {
		qc = get_unaligned_le16(skb->data + MAC_HDR_LEN);
		amsdu = qc & IEEE80211_QOS_CTL_A_MSDU_PRESENT;
	}
	/* no A-MSDU to a group address (mac80211 ieee80211_rx_h_amsdu) */
	if (amsdu) {
		MAC_COUNT(rx_amsdu);
		if (group) {
			MAC_COUNT(rx_dropped_amsdu);
			goto drop;
		}
	}
	slot = qos ? (qc & IEEE80211_QOS_CTL_TID_MASK) : MAC_SEQ_NONQOS;
	if (slot > MAC_SEQ_NONQOS)              /* never the management slot (a 4-bit TID always has one of its own) */
		slot = MAC_SEQ_NONQOS;

	/* the CCMP header and the MIC come off a frame the hardware decrypted and key.c accepted (PN); an
	   unprotected one must be allowed by the connection's rules (key.c) */
	body = skb->data + hdrlen;
	len = skb->len - hdrlen;
	if (fc & IEEE80211_FCTL_PROTECTED) {
		if (!(rs->flag & RX_FLAG_DECRYPTED) || len < IEEE80211_CCMP_HDR_LEN + IEEE80211_CCMP_MIC_LEN) {
			MAC_COUNT(rx_dropped_protected);
			goto drop;
		}
		if (mac_key_rx(mh, body, group, slot))
			goto drop;              /* counted there */
		body += IEEE80211_CCMP_HDR_LEN;
		len -= IEEE80211_CCMP_HDR_LEN + IEEE80211_CCMP_MIC_LEN;
		info = RTW_RX_PROTECTED;
	} else if (mac_conn_rsn(&mh->conn)) {
		/* an A-MSDU would need the check per subframe: none of the exempt frames (EAPOL) comes that way */
		action = amsdu ? -1 : mac_key_rx_clear(mh, rx_ethertype(body, len), group);
		if (action < 0) {
			if (amsdu)
				MAC_COUNT(rx_dropped_unencrypted);
			goto drop;
		}
		info = RTW_RX_EXEMPT | (u32)action << 4;
	}

	if (!amsdu)
		rx_fifo_put(mh, hdr->addr1, hdr->addr2, hdr->addr3, seq_ctrl, body, len, info);
	else if (!rx_amsdu_ok(body, len))
		MAC_COUNT(rx_dropped_amsdu);    /* nothing of it goes up */
	else
		rx_amsdu(mh, hdr->addr2, own, seq_ctrl, body, len, info);
drop:
	kfree_skb(skb);
}

/* a data frame (FCS already removed): with the connection's station held (read-side section). Takes the skb. */
static void rx_data (struct mac_hw *mh, struct sk_buff *skb, const struct ieee80211_rx_status *rs)
{
	const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)skb->data;
	const u8 *own = mh->hw.wiphy->perm_addr;
	u16 fc = le16_to_cpu(hdr->frame_control);
	__le16 seq_ctrl = hdr->seq_ctrl;
	unsigned int hdrlen, slot;
	struct mac_sta *ap;
	bool qos;
	u16 qc = 0;
	unsigned int tid;

	rcu_read_lock();
	ap = READ_ONCE(mh->conn.ap);
	if (READ_ONCE(mh->conn.state) != RTW_CONN_CONNECTED || !ap) {
		MAC_COUNT(rx_dropped_notours);  /* not connected (listening, scanning) */
		goto drop;
	}
	/* from the AP to us (or a group): From DS only, A2 = BSSID, A1 = us or group */
	if ((fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) != IEEE80211_FCTL_FROMDS ||
	    !ether_addr_equal(hdr->addr2, mh->conn.bssid) ||
	    (!ether_addr_equal(hdr->addr1, own) && !is_multicast_ether_addr(hdr->addr1)) ||
	    (is_multicast_ether_addr(hdr->addr1) && ether_addr_equal(hdr->addr3, own))) {
		MAC_COUNT(rx_dropped_notours);
		goto drop;
	}
	MAC_COUNT(rx_data);
	mac_mlme_rx_from_ap(mh, rs);
	if (fc & IEEE80211_FC0_SUBTYPE_NODATA) {
		MAC_COUNT(rx_dropped_other);    /* Null, QoS Null, CF-only: no MSDU */
		goto drop;
	}
	rx_note_rate(rs);
	hdrlen = ieee80211_hdrlen(hdr->frame_control);
	qos = ieee80211_is_data_qos(hdr->frame_control);
	if (qos && (fc & IEEE80211_FCTL_ORDER))
		MAC_COUNT(rx_htc);              /* +HTC (hdrlen counts it); we advertise no +HTC support */
	if (hdrlen < MAC_HDR_LEN || skb->len < hdrlen) {
		MAC_COUNT(rx_dropped_other);
		goto drop;
	}
	if ((fc & IEEE80211_FCTL_MOREFRAGS) || (le16_to_cpu(seq_ctrl) & IEEE80211_SCTL_FRAG)) {
		MAC_COUNT(rx_dropped_frag);
		goto drop;
	}
	if (qos)
		qc = get_unaligned_le16(skb->data + MAC_HDR_LEN);

	/* ieee80211_rx_h_check_dup(): unicast only; a retry with the sequence control seen last is a duplicate.
	   Done before reordering (mac80211 does the duplicate filter per MPDU, ahead of the reorder buffer). */
	tid = qos ? (qc & IEEE80211_QOS_CTL_TID_MASK) : IEEE80211_NUM_TIDS;
	if (!is_multicast_ether_addr(hdr->addr1)) {
		slot = qos ? tid : MAC_SEQ_NONQOS;
		if (slot < ARRAY_SIZE(ap->rx_seq)) {
			if ((fc & (IEEE80211_FC1_RETRY << 8)) && ap->rx_seq[slot] == le16_to_cpu(seq_ctrl)) {
				MAC_COUNT(rx_dropped_dup);
				goto drop;
			}
			ap->rx_seq[slot] = le16_to_cpu(seq_ctrl);
		}

		/* a unicast QoS data frame on a TID with a block ack session goes through its reorder buffer */
		if (qos && tid < MAC_BA_TIDS && mac_agg_rx_frame(mh, ap, tid, skb)) {
			rcu_read_unlock();      /* agg.c took the skb (held or delivered) and signals if it delivered */
			return;
		}
	}

	mac_rx_deliver(mh, skb);        /* no block ack session: hand it over now */
	rcu_read_unlock();
	mac_dp_rx_ready();
	return;
drop:
	rcu_read_unlock();
	kfree_skb(skb);
}

void linuxkpi_ieee80211_rx (struct ieee80211_hw *hw, struct sk_buff *skb, struct ieee80211_sta *sta,
	struct napi_struct *napi, struct list_head *list)
{
	struct mac_hw *mh = mac_hw(hw);
	const struct ieee80211_rx_status *rs = IEEE80211_SKB_RXCB(skb);
	const struct ieee80211_hdr *hdr;
	__le16 fc;

	(void)sta;
	(void)napi;
	(void)list;
	mac_bss_rx(hw, skb);            /* the BSS table does its own FCS check and counting */
	if (rs->flag & (RX_FLAG_FAILED_FCS_CRC | RX_FLAG_FAILED_PLCP_CRC))
		goto out;
	if (ieee80211_hw_check(hw, RX_INCLUDES_FCS)) {
		if (skb->len < FCS_LEN)
			goto out;
		skb_trim(skb, skb->len - FCS_LEN);
	}
	if (skb->len < MAC_HDR_LEN)
		goto out;
	/* frames for our GO (addressed to its BSSID, or probe requests) go to ap.c, which takes them */
	if (READ_ONCE(mh->ap.running) && mac_ap_rx(mh, skb))
		return;
	hdr = (const struct ieee80211_hdr *)skb->data;
	fc = hdr->frame_control;
	if (ieee80211_is_mgmt(fc)) {
		if (ieee80211_is_probe_resp(fc))
			mac_txprobe_rx(mh, (const struct ieee80211_mgmt *)skb->data);
		if (READ_ONCE(mh->conn.state) == RTW_CONN_IDLE)
			goto out;
		if (ieee80211_is_beacon(fc) && ether_addr_equal(hdr->addr3, mh->conn.bssid)) {
			mac_mlme_rx_from_ap(mh, rs);
			mac_ps_beacon(mh);      /* the first one may allow power save */
			mac_mlme_beacon(mh, skb->data, skb->len);      /* its Operating Mode Notification */
		}
		/* +HTC on a management frame of the BSS (the MLME parses a 24-byte header; we advertise no
		   +HTC support, so none should come: counted to see) */
		if ((le16_to_cpu(fc) & IEEE80211_FCTL_ORDER) && ether_addr_equal(hdr->addr3, mh->conn.bssid))
			MAC_COUNT(rx_mgmt_htc);
		if (mac_mlme_wants(mh, skb->data, skb->len)) {
			MAC_COUNT(rx_mlme);
			mac_mlme_rx(mh, skb);   /* takes the skb */
			return;
		}
	} else if (ieee80211_is_back_req(fc)) {
		/* a BlockAckReq from our AP pushes the TID's reorder window forward (agg.c) */
		if (READ_ONCE(mh->conn.state) == RTW_CONN_CONNECTED)
			mac_agg_rx_bar(mh, skb->data, skb->len);
	} else if (ieee80211_is_data(fc)) {
		rx_data(mh, skb, rs);       /* takes the skb */
		return;
	}
out:
	kfree_skb(skb);
}

/* ---- the front end (wificx\adapter.cpp) ---- */
uint32_t rtw_linux_rx_data (void *buf, uint32_t cap, uint32_t *info)
{
	struct mac_hw *mh = mac_the_hw;
	struct sk_buff *skb;
	uint32_t len;

	*info = 0;
	if (!mh)
		return 0;
	if (READ_ONCE(mh->conn.state) != RTW_CONN_CONNECTED) {
		mac_rx_flush(mh);               /* nothing goes up while not connected */
		return 0;
	}
	while ((skb = skb_dequeue(&mh->rx_fifo))) {
		if (skb->len > cap) {
			MAC_COUNT(rx_dropped_toolarge);
			kfree_skb(skb);
			continue;
		}
		len = skb->len;
		memcpy(buf, skb->data, len);
		*info = skb->mac_tag;
		kfree_skb(skb);
		MAC_COUNT(rx_indicated);
		return len;
	}
	return 0;
}

uint32_t rtw_linux_rx_pending (void)
{
	return mac_the_hw ? skb_queue_len_lockless(&mac_the_hw->rx_fifo) : 0;
}

void rtw_linux_rx_flush (void)
{
	if (mac_the_hw)
		mac_rx_flush(mac_the_hw);
}
