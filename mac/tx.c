// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: transmit. Frames go to the driver the way mac80211 hands over frames that
 * have no txq: ops->tx() with an ieee80211_tx_info that names our interface, inside a read-side section (the station
 * pointer). rtw89 picks the queue, the rate (lowest basic rate for management frames) and, for management frames,
 * the sequence number; it needs a channel context of the interface (the chip is powered off without one).
 *  - Management frames (MLME, TXPROBE) go through mac_tx_mgmt() with wiphy->mtx held. They wait in a small pending
 *    table for their tx status; the skb carries its slot in sk_buff.mac_tag, because rtw89 owns the skb's cb.
 *  - Data frames (rtw_linux_tx_data(), the front end's TX queue) are counted in flight and limited to 64. On an
 *    RSN connection they carry a CCMP header and go to rtw89 with control.hw_key (key.c decides).
 * The tx status comes back in ieee80211_tx_status() from rtw89's NAPI poll, at DISPATCH_LEVEL with its trx_lock
 * held, or in ieee80211_free_txskb() when the driver drops a frame (possibly inside ops->tx()). Both only do the
 * bookkeeping and signal: never send from there (trx_lock is not recursive).
 * Also here: the queue stop/wake calls, the PS-Poll and Null templates rtw89 asks for after association, and the
 * diagnostic TXPROBE.
 * The GO's frames (ap.c, MAC_SKB_AP in the tag) get their tx status in ap.c; they never use the pending table.
 */
#include "mac.h"

#define TX_INFLIGHT_MAX                 64      /* each AC ring has 256 BDs: leaves room for management frames */
#define TX_INFLIGHT_LOW                 32      /* wake the front end when this many are left */
#define TXPROBE_BUDGET                  32      /* probe requests per driver load */
#define TXPROBE_MAX_BSS                 16
#define MAC_HDR_LEN                     24

_Static_assert(sizeof(struct rtw_txprobe_param) == 12, "rtw_txprobe_param layout");
_Static_assert(sizeof(struct rtw_txprobe_result) == 40, "rtw_txprobe_result layout");
_Static_assert(sizeof(struct rtw_link_stats) == 288, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, phy) == 256, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, rx_amsdu) == 264, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, tx_kbps) == 284, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, mfp) == 192, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, mgmt_protected_tx) == 196, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, sae_params) == 252, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, tx_protected) == 112, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, auth) == 152, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, restarts) == 156, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, rx_reordered) == 160, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, tx_ampdu) == 184, "rtw_link_stats layout");
_Static_assert(offsetof(struct rtw_link_stats, ba_rx_tids) == 188, "rtw_link_stats layout");
_Static_assert(sizeof(struct rtw_frag) == 16, "rtw_frag layout");
_Static_assert(sizeof(struct rtw_conn_cand) == 16, "rtw_conn_cand layout");
_Static_assert(sizeof(struct rtw_connect_req) == 128, "rtw_connect_req layout");
_Static_assert(offsetof(struct rtw_connect_req, bodies) == 104, "rtw_connect_req layout");
_Static_assert(offsetof(struct rtw_connect_req, ies) == 120, "rtw_connect_req layout");
_Static_assert(sizeof(struct rtw_conn_info) == 48, "rtw_conn_info layout");
_Static_assert(offsetof(struct rtw_conn_info, mgmt_group) == 43, "rtw_conn_info layout");
_Static_assert(offsetof(struct rtw_connect_req, auths) == 116, "rtw_connect_req layout");
_Static_assert(MAC_TXP_MAX <= 256, "slot fits into mac_tag");

struct rtw_link_stats mac_st;

/* the front end's data path callbacks: read in read-side sections, detached with synchronize_rcu() */
static struct rtw_dp_ops dp_ops;
static bool dp_on;

/* TXPROBE: frames left for this driver load; the counters of the running probe (under mac_hw.txp_lock) */
static u32 txprobe_budget = TXPROBE_BUDGET;
static struct {
	bool active;
	u64 since_ns;
	u64 first_presp_ns;
	u32 done, failed, presp;
	u32 n_bss;
	u8 bss[TXPROBE_MAX_BSS][ETH_ALEN];
} txprobe;

void mac_tx_init (struct mac_hw *mh)
{
	spin_lock_init(&mh->txp_lock);
	atomic_set(&mh->tx_inflight, 0);
}

/* After the driver stopped (all its frames came back or were dropped): forget the pending table. A tx status that
   still comes for a forgotten frame finds no slot and only frees the skb. */
void mac_tx_reset (struct mac_hw *mh)
{
	spin_lock_bh(&mh->txp_lock);
	memset(mh->txp, 0, sizeof(mh->txp));
	spin_unlock_bh(&mh->txp_lock);
	atomic_set(&mh->tx_inflight, 0);
	WRITE_ONCE(mh->queues_stopped, false);
}

/* IOCTL_RTW_LINK: a snapshot without locks (counters may be a frame apart) */
void rtw_linux_link_stats (struct rtw_link_stats *st)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_conn *c;
	s32 rssi;

	*st = mac_st;
	st->state = RTW_CONN_IDLE;
	memset(st->bssid, 0, sizeof(st->bssid));
	st->band = st->channel = 0;
	st->aid = 0;
	st->rssi = 0;
	st->flags = 0;
	st->tx_inflight = 0;
	st->auth = 0;
	st->keys = 0;
	st->phy = st->width = st->center = 0;
	st->bw_setting = (u8)rtw_linux_get_wide();       /* Parameters\Bandwidth */
	st->tx_kbps = 0;
	st->exemptions = mac_exempt_count();
	mac_pmf_stats(mh, st);          /* the MFP/SAE state fields */
	if (!mh)
		return;
	c = &mh->conn;
	st->tx_inflight = (u32)atomic_read(&mh->tx_inflight);
	st->state = READ_ONCE(c->state);
	st->keys = mac_link_keys(mh);
	mac_agg_stats(mh, st);          /* the restart counter and the per-TID session bitmaps */
	if (st->state == RTW_CONN_IDLE)
		return;
	st->auth = c->auth;
	ether_addr_copy(st->bssid, c->bssid);
	st->band = c->info.band;
	st->channel = c->info.channel;
	st->aid = c->info.aid;
	rssi = READ_ONCE(c->rssi_x16);
	st->rssi = (s8)(rssi / 16);
	st->flags = (c->info.wmm ? RTW_LINK_F_WMM : 0) | (c->info.ht ? RTW_LINK_F_HT : 0);
	/* connected, what the AP's station uses; before, what the attempt asked for */
	st->phy = st->state == RTW_CONN_CONNECTED ? c->info.phy : c->wide.mode;
	st->width = st->state == RTW_CONN_CONNECTED ? c->info.width_mhz : c->wide.width;
	st->center = c->wide.center;
	st->tx_kbps = c->info.tx_kbps;
}

/* ---- the front end's callbacks ---- */
void rtw_linux_dp_attach (const struct rtw_dp_ops *ops)
{
	if (ops) {
		WRITE_ONCE(dp_on, false);
		synchronize_rcu();
		dp_ops = *ops;
		smp_wmb();
		WRITE_ONCE(dp_on, true);
		return;
	}
	WRITE_ONCE(dp_on, false);
	synchronize_rcu();              /* no callback runs any more */
	memset(&dp_ops, 0, sizeof(dp_ops));
}

void mac_dp_tx_ready (void)
{
	rcu_read_lock();
	if (READ_ONCE(dp_on) && dp_ops.tx_ready)
		dp_ops.tx_ready(dp_ops.ctx);
	rcu_read_unlock();
}

void mac_dp_rx_ready (void)
{
	rcu_read_lock();
	if (READ_ONCE(dp_on) && dp_ops.rx_ready)
		dp_ops.rx_ready(dp_ops.ctx);
	rcu_read_unlock();
}

/* ---- management frames ---- */
/*
 * Sends a frame the mac layer built (management or Null); takes the skb in every case. With wiphy->mtx held
 * (the channel context and ms cannot go away), never from a tx status. -ENETDOWN without a channel context,
 * -EBUSY when the pending table is full, -EACCES when it had to be protected and could not be.
 * With management frame protection a robust frame to the AP goes out protected (pmf.c, CCMP in software).
 */
int mac_tx_mgmt (struct mac_hw *mh, struct sk_buff *skb, struct mac_sta *ms, u32 flags, enum mac_txt txt)
{
	const struct ieee80211_hdr *hdr;
	struct ieee80211_tx_control ctl = { 0 };
	struct ieee80211_chanctx_conf *conf;
	struct ieee80211_tx_info *info;
	struct ieee80211_vif *vif;
	int slot;

	lockdep_assert_wiphy(mh->hw.wiphy);
	vif = mh->sta_vif ? &mh->sta_vif->vif : NULL;
	conf = vif ? vif->bss_conf.chanctx_conf : NULL;
	if (!mh->started || !conf || skb->len < MAC_HDR_LEN) {
		kfree_skb(skb);
		return -ENETDOWN;
	}
	skb = mac_pmf_tx(mh, skb);
	if (!skb)
		return -EACCES;
	hdr = (const struct ieee80211_hdr *)skb->data;

	memset(skb->cb, 0, sizeof(skb->cb));
	info = IEEE80211_SKB_CB(skb);
	info->flags = flags | IEEE80211_TX_CTL_REQ_TX_STATUS;
	if (is_multicast_ether_addr(hdr->addr1))
		info->flags |= IEEE80211_TX_CTL_NO_ACK;
	info->band = conf->def.chan->band;
	info->hw_queue = vif->hw_queue[IEEE80211_AC_VO];
	info->control.vif = vif;
	skb->priority = 7;
	skb_set_queue_mapping(skb, IEEE80211_AC_VO);

	spin_lock_bh(&mh->txp_lock);
	for (slot = 0; slot < MAC_TXP_MAX && mh->txp[slot].skb; slot++)
		;
	if (slot == MAC_TXP_MAX) {
		spin_unlock_bh(&mh->txp_lock);
		pr_warn_once("mac: management frame not sent: %d frames wait for their tx status", MAC_TXP_MAX);
		kfree_skb(skb);
		return -EBUSY;
	}
	mh->txp[slot].skb = skb;
	mh->txp[slot].txt = (u8)txt;
	skb->mac_tag = MAC_SKB_MGMT | (u32)slot << 8 | (u32)txt;
	spin_unlock_bh(&mh->txp_lock);
	MAC_COUNT(mgmt_tx);

	ctl.sta = ms ? &ms->sta : NULL;
	mac_read_lock(mh);
	mh->ops->tx(&mh->hw, &ctl, skb);        /* a failure comes back through free_txskb */
	mac_read_unlock(mh);
	return 0;
}

/* frames of this kind still waiting for their tx status */
int mac_txt_pending (struct mac_hw *mh, enum mac_txt txt)
{
	int n = 0;

	spin_lock_bh(&mh->txp_lock);
	for (int i = 0; i < MAC_TXP_MAX; i++)
		n += mh->txp[i].skb && mh->txp[i].txt == txt;
	spin_unlock_bh(&mh->txp_lock);
	return n;
}

/* ---- tx status (DISPATCH_LEVEL inside rtw89's trx_lock, or the sender's thread) ---- */
static void tx_done (struct mac_hw *mh, struct sk_buff *skb, bool acked)
{
	u32 tag = skb->mac_tag;

	if (tag & MAC_SKB_AP) {
		mac_ap_tx_done(mh, skb, acked);         /* our GO's frames (counters, association response) */
	} else if (tag & MAC_SKB_DATA) {
		if (!acked)
			MAC_COUNT(tx_failed);
		if (atomic_dec_return(&mh->tx_inflight) == TX_INFLIGHT_LOW)
			mac_dp_tx_ready();
	} else if (tag & MAC_SKB_MGMT) {
		u32 slot = MAC_SKB_SLOT(tag);
		enum mac_txt txt = MAC_SKB_TXT(tag);
		bool mine = false;

		spin_lock_bh(&mh->txp_lock);
		if (slot < MAC_TXP_MAX && mh->txp[slot].skb == skb) {
			mh->txp[slot].skb = NULL;
			mine = true;
			if (txt == MAC_TXT_PROBE && txprobe.active) {
				if (acked)
					txprobe.done++;
				else
					txprobe.failed++;
			}
		}
		spin_unlock_bh(&mh->txp_lock);
		if (mine) {
			if (acked)
				MAC_COUNT(mgmt_acked);
			else
				MAC_COUNT(mgmt_failed);
			if (txt != MAC_TXT_PROBE)
				mac_mlme_tx_status(mh, txt, acked);
		}
	}
	kfree_skb(skb);
}

void linuxkpi_ieee80211_tx_status (struct ieee80211_hw *hw, struct sk_buff *skb)
{
	tx_done(mac_hw(hw), skb, !!(IEEE80211_SKB_CB(skb)->flags & IEEE80211_TX_STAT_ACK));
}

void linuxkpi_ieee80211_free_txskb (struct ieee80211_hw *hw, struct sk_buff *skb, int status)
{
	(void)status;
	tx_done(mac_hw(hw), skb, false);
}

/* rtw89 stops all queues while a scan is off the operating channel, around SER and in some power states */
void linuxkpi_ieee80211_stop_queues (struct ieee80211_hw *hw)
{
	struct mac_hw *mh = mac_hw(hw);

	if (!READ_ONCE(mh->queues_stopped)) {
		WRITE_ONCE(mh->queues_stopped, true);
		MAC_COUNT(queues_stopped);
	}
}

void linuxkpi_ieee80211_wake_queues (struct ieee80211_hw *hw)
{
	struct mac_hw *mh = mac_hw(hw);

	if (READ_ONCE(mh->queues_stopped)) {
		WRITE_ONCE(mh->queues_stopped, false);
		mac_dp_tx_ready();
		mac_ap_dp_tx_ready();   /* the GO's port waits for the same queues */
	}
}

/* ---- data frames from the front end ---- */
int rtw_linux_tx_ready (void)
{
	struct mac_hw *mh = mac_the_hw;

	if (!mh || READ_ONCE(mh->conn.state) != RTW_CONN_CONNECTED)
		return -ENOTCONN;
	if (READ_ONCE(mh->queues_stopped) || atomic_read(&mh->tx_inflight) >= TX_INFLIGHT_MAX)
		return -EBUSY;
	return 0;
}

void rtw_linux_tx_drop (uint32_t n)
{
	__atomic_fetch_add(&mac_st.tx_dropped_disconnected, n, __ATOMIC_RELAXED);
}

/* copies len bytes from offset off of the fragment list */
static void frags_copy (const struct rtw_frag *frags, u32 n, u32 off, u8 *to, u32 len)
{
	for (u32 i = 0; i < n && len; i++) {
		u32 c;

		if (off >= frags[i].len) {
			off -= frags[i].len;
			continue;
		}
		c = min(frags[i].len - off, len);
		memcpy(to, (const u8 *)frags[i].p + off, c);
		to += c;
		len -= c;
		off = 0;
	}
}

/* skb->protocol and the network/transport headers of an IPv4 (UDP) frame, which rtw89 reads for its BT
   coexistence notifications; body = LLC/SNAP and on */
static const u8 rfc1042[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };     /* LLC/SNAP for Ethernet types */

static void tx_set_protocol (struct sk_buff *skb, unsigned int body)
{
	const u8 *p = skb->data + body;
	unsigned int len = skb->len - body;
	u16 type;

	if (len < 8 || memcmp(p, rfc1042, sizeof(rfc1042)))
		return;
	type = get_unaligned_be16(p + 6);
	if (type == ETH_P_IP) {
		const struct iphdr *ip = (const struct iphdr *)(p + 8);
		unsigned int ihl;

		if (len < 8 + sizeof(*ip))
			return;
		ihl = ip->ihl * 4u;
		if (ihl < sizeof(*ip) || len < 8 + ihl + (ip->protocol == IPPROTO_UDP ? sizeof(struct udphdr) : 0))
			return;
		skb_set_network_header(skb, (int)(body + 8));
		skb_set_transport_header(skb, (int)(body + 8 + ihl));
	}
	skb->protocol = cpu_to_be16(type);
}

/* the same for the GO's data frames (ap.c) */
void mac_tx_set_protocol (struct sk_buff *skb, unsigned int body)
{
	tx_set_protocol(skb, body);
}

/* 802.1D priority (TID) to access category (mac80211 ieee802_1d_to_ac) */
static const u8 tid_to_ac[8] = {
	IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_BK, IEEE80211_AC_BE,
	IEEE80211_AC_VI, IEEE80211_AC_VI, IEEE80211_AC_VO, IEEE80211_AC_VO,
};

/*
 * A native 802.11 data frame from the OS (To DS: A1 = BSSID, A2 = us, A3 = DA; no sequence number, no QoS control)
 * becomes a QoS Data frame with the TID's next sequence number if the AP does WMM, else a Data frame with the
 * non-QoS sequence number (mac80211 wme.c ieee80211_set_qos_hdr, tx.c ieee80211_tx_h_sequence). The duration
 * field stays 0 (whether the hardware fills it in has not been checked on air).
 * On an RSN connection key.c decides (exemption action, pairwise key) whether it goes protected: then the
 * Protected bit, the CCMP header after the MAC header (mac80211 wpa.c ccmp_encrypt_skb with GENERATE_IV) and
 * control.hw_key for rtw89, which has the hardware encrypt it and append the MIC.
 */
int rtw_linux_tx_data (const struct rtw_frag *frags, uint32_t n, uint8_t prio, uint32_t flags)
{
	struct mac_hw *mh = mac_the_hw;
	struct ieee80211_tx_control ctl = { 0 };
	struct ieee80211_tx_info *info;
	struct ieee80211_vif *vif;
	struct mac_key *key;
	struct sk_buff *skb;
	struct mac_sta *ap;
	u8 hdr[MAC_HDR_LEN], snap[8];
	u32 total = 0, body, slot;
	u16 fc, seq, ethertype = 0;
	u8 tid, ac, qos_len;
	bool qos, ampdu;
	u8 *p;

	for (u32 i = 0; i < n; i++)
		total += frags[i].len;
	if (!mh)
		return -ENOTCONN;
	if (!n || total < MAC_HDR_LEN + 8 || total > MAC_HDR_LEN + MAC_FRAME_MAX) {
		MAC_COUNT(tx_dropped_bad);
		return -EINVAL;
	}

	rcu_read_lock();
	ap = READ_ONCE(mh->conn.ap);
	vif = mh->sta_vif ? &mh->sta_vif->vif : NULL;
	/* associated is enough: on an RSN connection the 4-way handshake goes over this path before the AP's station
	   is authorized, and key.c keeps everything else back until the pairwise key is there */
	/* nothing for a driver being reset (SER); the restart ends the connection anyway */
	if (READ_ONCE(mh->conn.state) != RTW_CONN_CONNECTED || !ap || !vif || ap->state < IEEE80211_STA_ASSOC ||
	    READ_ONCE(mh->restarting)) {
		rcu_read_unlock();
		MAC_COUNT(tx_dropped_disconnected);
		return -ENOTCONN;
	}
	if (READ_ONCE(mh->queues_stopped) || atomic_read(&mh->tx_inflight) >= TX_INFLIGHT_MAX) {
		rcu_read_unlock();
		MAC_COUNT(tx_busy);
		return -EBUSY;
	}

	frags_copy(frags, n, 0, hdr, MAC_HDR_LEN);
	fc = get_unaligned_le16(hdr);
	if ((fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) != (IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA) ||
	    (fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) != IEEE80211_FCTL_TODS ||
	    (fc & IEEE80211_FCTL_MOREFRAGS) ||
	    !ether_addr_equal(hdr + 4, mh->conn.bssid) || !ether_addr_equal(hdr + 10, vif->addr)) {
		rcu_read_unlock();
		MAC_COUNT(tx_dropped_bad);
		return -EINVAL;
	}

	/* 802.1X (EAPOL) goes as the port control protocol, at the highest priority (the four-way handshake) */
	frags_copy(frags, n, MAC_HDR_LEN, snap, sizeof(snap));
	if (!memcmp(snap, rfc1042, sizeof(rfc1042)))
		ethertype = get_unaligned_be16(snap + 6);
	if (ethertype == ETH_P_PAE)
		flags |= RTW_TX_EAPOL;

	/* protected or in clear (A3 = DA tells unicast from group for the exemption list) */
	if (mac_key_tx(mh, ethertype, is_multicast_ether_addr(hdr + 16), flags, &key)) {
		rcu_read_unlock();
		MAC_COUNT(tx_dropped_nokey);
		return -EACCES;
	}

	qos = ap->sta.wme;
	tid = qos ? ((flags & RTW_TX_EAPOL) ? 7 : (prio & 7)) : 0;
	ac = tid_to_ac[tid];
	qos_len = qos ? 2 : 0;
	body = MAC_HDR_LEN + qos_len + (key ? IEEE80211_CCMP_HDR_LEN : 0);
	skb = dev_alloc_skb(mh->hw.extra_tx_headroom + body + (total - MAC_HDR_LEN));
	if (!skb) {
		rcu_read_unlock();
		MAC_COUNT(tx_nomem);
		return -ENOMEM;
	}
	skb_reserve(skb, mh->hw.extra_tx_headroom);
	p = skb_put(skb, body);
	memcpy(p, hdr, MAC_HDR_LEN);
	/* type Data, To DS; QoS Data with the AP's WMM; Protected with a key; no power management/retry/more data/order
	   bits from the OS (the firmware owns the PM bit in power save) */
	put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_TODS |
			   (qos ? IEEE80211_STYPE_QOS_DATA : IEEE80211_STYPE_DATA) | (key ? IEEE80211_FCTL_PROTECTED : 0),
			   p);
	put_unaligned_le16(0, p + 2);
	slot = qos ? tid : MAC_SEQ_NONQOS;
	seq = ap->tx_seq[slot];
	ap->tx_seq[slot] = (seq + 1) & 0xfff;
	put_unaligned_le16((u16)(seq << 4), p + 22);
	if (qos)
		put_unaligned_le16(tid, p + MAC_HDR_LEN);      /* normal ack, no A-MSDU */
	if (key)
		mac_key_ccmp_hdr(key, p + MAC_HDR_LEN + qos_len);
	frags_copy(frags, n, MAC_HDR_LEN, skb_put(skb, total - MAC_HDR_LEN), total - MAC_HDR_LEN);
	tx_set_protocol(skb, body);

	/* a QoS data frame on a TID with a block ack session carries IEEE80211_TX_CTL_AMPDU (rtw89 aggregates it);
	   agg.c asks for a session when there is none, and keeps EAPOL out of aggregation */
	ampdu = qos && mac_agg_tx_check(mh, ap, tid, ethertype);

	memset(skb->cb, 0, sizeof(skb->cb));
	info = IEEE80211_SKB_CB(skb);
	info->flags = ampdu ? IEEE80211_TX_CTL_AMPDU : 0;
	info->band = mh->conn.chan->band;
	info->hw_queue = vif->hw_queue[ac];
	info->control.vif = vif;
	info->control.hw_key = key ? &key->conf : NULL;        /* the key stays while we are in the read side */
	if (flags & RTW_TX_EAPOL)
		info->control.flags |= IEEE80211_TX_CTRL_PORT_CTRL_PROTO;
	skb->priority = tid;
	skb_set_queue_mapping(skb, ac);
	skb->mac_tag = MAC_SKB_DATA;
	atomic_inc(&mh->tx_inflight);
	MAC_COUNT(tx_data);
	if (ampdu)
		MAC_COUNT(tx_ampdu);
	if (key)
		MAC_COUNT(tx_protected);
	else if (mac_conn_rsn(&mh->conn))
		MAC_COUNT(tx_clear);
	ctl.sta = &ap->sta;
	mh->ops->tx(&mh->hw, &ctl, skb);        /* a failure comes back through free_txskb */
	rcu_read_unlock();
	return 0;
}

/* ---- templates for the firmware (rtw89_fw_h2c_general_pkt() after association, its own Null frames) ---- */
static const u8 *mac_vif_bssid (struct ieee80211_vif *vif)
{
	const u8 *bssid = vif->bss_conf.bssid;

	return bssid && !is_zero_ether_addr(bssid) ? bssid : NULL;
}

/* mac80211 ieee80211_pspoll_get() */
struct sk_buff *linuxkpi_ieee80211_pspoll_get (struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	const u8 *bssid = mac_vif_bssid(vif);
	struct sk_buff *skb;
	u8 *p;

	if (vif->type != NL80211_IFTYPE_STATION || !bssid)
		return NULL;
	skb = dev_alloc_skb(hw->extra_tx_headroom + 16);
	if (!skb)
		return NULL;
	skb_reserve(skb, hw->extra_tx_headroom);
	p = skb_put_zero(skb, 16);
	put_unaligned_le16(IEEE80211_FTYPE_CTL | IEEE80211_FC0_SUBTYPE_PS_POLL, p);
	put_unaligned_le16(0xc000 | vif->cfg.aid, p + 2);      /* AID with both top bits set */
	ether_addr_copy(p + 4, bssid);
	ether_addr_copy(p + 10, vif->addr);
	return skb;
}

/* mac80211 ieee80211_nullfunc_get(): to the AP (To DS); QoS Null (TID 7) if the AP does WMM and qos_ok */
struct sk_buff *linuxkpi_ieee80211_nullfunc_get (struct ieee80211_hw *hw, struct ieee80211_vif *vif, int link_id,
	bool qos_ok)
{
	struct mac_hw *mh = mac_hw(hw);
	const u8 *bssid = mac_vif_bssid(vif);
	struct sk_buff *skb;
	struct mac_sta *ap;
	bool qos = false;
	u8 *p;

	(void)link_id;
	if (vif->type != NL80211_IFTYPE_STATION || !bssid)
		return NULL;
	if (qos_ok) {
		rcu_read_lock();
		ap = READ_ONCE(mh->conn.ap);
		qos = ap && ap->sta.wme;
		rcu_read_unlock();
	}
	skb = dev_alloc_skb(hw->extra_tx_headroom + MAC_HDR_LEN + 2);
	if (!skb)
		return NULL;
	skb_reserve(skb, hw->extra_tx_headroom);
	p = skb_put_zero(skb, MAC_HDR_LEN);
	put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_TODS |
			   (qos ? IEEE80211_FC0_SUBTYPE_QOS_NULL : IEEE80211_FC0_SUBTYPE_NODATA), p);
	ether_addr_copy(p + 4, bssid);
	ether_addr_copy(p + 10, vif->addr);
	ether_addr_copy(p + 16, bssid);
	if (qos) {
		skb->priority = 7;
		skb_set_queue_mapping(skb, IEEE80211_AC_VO);
		put_unaligned_le16(7, skb_put(skb, 2));
	}
	return skb;
}

/* ---- TXPROBE (diagnostic) ---- */
/* probe responses addressed to us while a TXPROBE runs; from rx.c (NAPI thread) */
void mac_txprobe_rx (struct mac_hw *mh, const struct ieee80211_mgmt *mgmt)
{
	u64 now;
	u32 i;

	if (!READ_ONCE(txprobe.active) || !ether_addr_equal(mgmt->da, mh->hw.wiphy->perm_addr))
		return;
	spin_lock_bh(&mh->txp_lock);
	now = ktime_get_ns();
	if (txprobe.active && now >= txprobe.since_ns) {
		txprobe.presp++;
		if (!txprobe.first_presp_ns)
			txprobe.first_presp_ns = now;
		for (i = 0; i < txprobe.n_bss && !ether_addr_equal(txprobe.bss[i], mgmt->bssid); i++)
			;
		if (i == txprobe.n_bss && i < TXPROBE_MAX_BSS)
			ether_addr_copy(txprobe.bss[txprobe.n_bss++], mgmt->bssid);
	}
	spin_unlock_bh(&mh->txp_lock);
}

/* A wildcard probe request like the scan's, plus the DS Parameter Set of the channel (as mac80211 adds when it
   probes on one channel), so only APs on this channel answer. */
static struct sk_buff *txprobe_frame (struct mac_hw *mh, const struct ieee80211_channel *chan)
{
	const struct ieee80211_supported_band *sb = mh->hw.wiphy->bands[chan->band];
	struct sk_buff *skb;
	u8 ies[96];
	u8 *pos = ies;

	if (!sb || sb->n_bitrates > 8 + 32)
		return NULL;
	pos = mac_put_rates(pos, sb);
	*pos++ = WLAN_EID_DS_PARAMS;
	*pos++ = 1;
	*pos++ = (u8)chan->hw_value;
	if (sb->ht_cap.ht_supported)
		pos = mac_put_ht_cap(pos, &sb->ht_cap);
	if (WARN_ON(pos - ies > (int)sizeof(ies)))
		return NULL;
	skb = linuxkpi_ieee80211_probereq_get(&mh->hw, mh->sta_vif->vif.addr, NULL, 0, pos - ies);
	if (skb)
		skb_put_data(skb, ies, (unsigned int)(pos - ies));
	return skb;
}

/* With wiphy->mtx held: the channel being listened on, if a probe may be sent there */
static int txprobe_check (struct mac_hw *mh, const struct ieee80211_channel **chan)
{
	const struct ieee80211_channel *c;

	if (!mh->started || !mh->sta_vif || !mh->listen_ctx)
		return -ENOTCONN;
	if (mh->scanning || READ_ONCE(mh->conn.state) != RTW_CONN_IDLE || READ_ONCE(mh->restarting))
		return -EBUSY;
	c = mh->listen_ctx->conf.def.chan;
	if (c->band != NL80211_BAND_2GHZ || c->hw_value < 1 || c->hw_value > 11 ||
	    (c->flags & (IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR | IEEE80211_CHAN_DISABLED)))
		return -EPERM;
	*chan = c;
	return 0;
}

int rtw_linux_txprobe (const struct rtw_txprobe_param *param, struct rtw_txprobe_result *res)
{
	struct mac_hw *mh = mac_the_hw;
	const struct ieee80211_channel *chan = NULL;
	struct rtw_txprobe_param p = *param;
	u64 first_ns = 0, last_ns = 0, now;
	u32 i;
	int ret;

	memset(res, 0, sizeof(*res));
	res->budget_left = READ_ONCE(txprobe_budget);
	if (!mh)
		return res->result = -ENODEV;
	if (p.count < 1 || p.count > 4 || p.interval_ms < 50 || p.interval_ms > 1000 || p.wait_ms < 100 ||
	    p.wait_ms > 2000)
		return res->result = -EINVAL;

	wiphy_lock(mh->hw.wiphy);
	ret = txprobe_check(mh, &chan);
	if (!ret && txprobe_budget < p.count)
		ret = -EDQUOT;
	if (ret) {
		wiphy_unlock(mh->hw.wiphy);
		return res->result = ret;
	}
	res->channel = chan->hw_value;
	spin_lock_bh(&mh->txp_lock);
	memset(&txprobe, 0, sizeof(txprobe));
	txprobe.since_ns = ktime_get_ns();
	txprobe.active = true;
	spin_unlock_bh(&mh->txp_lock);
	pr_info("mac: txprobe: %u probe requests on channel %u, %u ms apart", p.count, chan->hw_value, p.interval_ms);

	for (i = 0; i < p.count; i++) {
		struct sk_buff *skb;

		if (i) {
			/* the IOCTL holds the driver's control lock: nothing else changes listening meanwhile */
			wiphy_unlock(mh->hw.wiphy);
			msleep(p.interval_ms);
			wiphy_lock(mh->hw.wiphy);
			ret = txprobe_check(mh, &chan);
			if (ret)
				break;
		}
		skb = txprobe_frame(mh, chan);
		if (!skb) {
			ret = -ENOMEM;
			break;
		}
		txprobe_budget--;
		ret = mac_tx_mgmt(mh, skb, NULL, 0, MAC_TXT_PROBE);
		if (ret)
			break;
		res->sent++;
		last_ns = ktime_get_ns();
		if (!first_ns)
			first_ns = last_ns;
	}
	wiphy_unlock(mh->hw.wiphy);

	/* until every frame has its tx status and wait_ms have passed since the last one (at most 1 s more) */
	if (!last_ns)
		last_ns = ktime_get_ns();
	for (;;) {
		now = ktime_get_ns();
		if (now - last_ns >= (u64)(p.wait_ms + 1000) * NSEC_PER_MSEC)
			break;
		if (now - last_ns >= (u64)p.wait_ms * NSEC_PER_MSEC && !mac_txt_pending(mh, MAC_TXT_PROBE))
			break;
		msleep(10);
	}

	spin_lock_bh(&mh->txp_lock);
	txprobe.active = false;
	res->tx_done = txprobe.done;
	res->tx_failed = txprobe.failed;
	res->presp_to_us = txprobe.presp;
	res->presp_bss = txprobe.n_bss;
	if (txprobe.first_presp_ns && first_ns && txprobe.first_presp_ns > first_ns)
		res->first_presp_ms = (u32)max_t(u64, (txprobe.first_presp_ns - first_ns) / NSEC_PER_MSEC, 1);
	spin_unlock_bh(&mh->txp_lock);
	res->tx_pending = res->sent > res->tx_done + res->tx_failed ? res->sent - res->tx_done - res->tx_failed : 0;
	res->budget_left = READ_ONCE(txprobe_budget);
	res->result = ret;
	pr_info("mac: txprobe: %d, sent %u, tx done %u, failed %u, pending %u; %u probe responses to us from %u BSS "
		"(first after %u ms); %u frames left", ret, res->sent, res->tx_done, res->tx_failed, res->tx_pending,
		res->presp_to_us, res->presp_bss, res->first_presp_ms, res->budget_left);
	return ret;
}
