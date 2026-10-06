// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: our group owner (GO) for the Windows mobile hotspot: what mac80211's cfg.c
 * (start_ap/stop_ap), tx.c/rx.c (AP data path, power save buffering) and hostapd (authentication, association,
 * inactivity) do under Linux (read for reference only).
 *  - Interface: a second rtw89 interface of type AP (not P2P_GO: no MCC, no NoA), only while the GO runs, on the
 *    station's channel context when the station has one (same channel only), else on a context of its own. Order of
 *    the driver calls like cfg.c ieee80211_start_ap: add_interface, channel context, conf_tx, link_conf, start_ap,
 *    link_info_changed(BEACON ...), which makes rtw89 fetch the beacon template here (beacon_get_tim) and hand it to
 *    the firmware, which sends the beacons. Stopping goes the other way (cfg.c ieee80211_stop_ap).
 *  - Frames: probe responses (rate limited), open system authentication, (re)association requests checked here
 *    (SSID, basic rates, RSN PSK/CCMP) and then decided by the OS (RTW_AP_IND_ASSOC_REQ ->
 *    rtw_linux_ap_assoc_resp), the association response with a tx status, deauthentication/disassociation both ways,
 *    a deauthentication for data from strangers (class 3, rate limited), block ack requests of clients (agg.c). They
 *    run in the AP work with wiphy->mtx held; rx.c only queues them.
 *  - Clients: struct mac_sta with ap_client set, on the station list like mac80211's sta_info: NONE -> AUTH on
 *    authentication, ASSOC once the accepted association response was acknowledged, AUTHORIZED with the pairwise key
 *    (or at once on an open GO). The OS's authenticator runs the 4-way handshake over the data path.
 *  - Power save: a client's PM bit (rx) decides; while it sleeps its unicast frames wait in its buffer (64 frames, 10 s)
 *    and its TIM bit goes into the beacon (set_tim makes rtw89 fetch the template again); awake, the buffer goes out
 *    first; a PS-Poll releases one frame (More Data set while more wait). Group frames go after the DTIM (the hardware
 *    high queue, IEEE80211_TX_CTL_SEND_AFTER_DTIM) while any client sleeps.
 *  - Data path: rtw_linux_ap_tx_data/rx_data like the station's (tx.c, rx.c), with the GO port's exemption list
 *    (key.c), the clients' pairwise keys and the GO's group key; CCMP header and PN here, the hardware encrypts and
 *    decrypts.
 *  - A hardware restart (SER) parks the GO's interface and clients like the station's (iface.c) and tells the front
 *    end it stopped (RTW_AP_IND_STOPPED) when the restart is done; nothing restarts the GO by itself.
 * Locks: wiphy->mtx for the state, the clients' lifetimes and every driver call; the data path and rx.c read in
 * read-side sections (rcu_read_lock), clients are freed after list_del_rcu() and synchronize_rcu(); mac_ap.ps_lock
 * guards the power save buffers.
 */
#include "mac.h"

#ifndef EXDEV
#define EXDEV                           18      /* Linux errno: the station uses another channel */
#endif

#define AP_RXQ_MAX                     32      /* management frames waiting for the AP work */
#define AP_RXQ_PROBE_MAX                16      /* ... of which probe requests */
#define AP_INDS_MAX                     32
#define AP_TX_INFLIGHT_MAX              64
#define AP_TX_INFLIGHT_LOW              32
#define AP_MGMT_INFLIGHT_MAX            32
#define AP_RX_FIFO_MAX                  256
#define AP_PS_Q_MAX                     64
#define AP_PS_TTL_MS                    10000   /* a sleeping client's buffer not served this long is dropped
						   (mac80211 STA_TX_BUFFER_EXPIRE, its minimum) */
#define AP_TICK_MS                      1000
#define AP_PENDING_MS                   30000   /* authenticated, or waiting for the OS's decision, at most this long */
#define AP_RESP_WAIT_MS                 2000    /* the association response's tx status */
#define AP_UNAUTH_MS                    30000   /* associated on an RSN GO without a pairwise key (hostapd: EAPOL
						   timeouts) */
#define AP_INACTIVE_MS                  300000  /* hostapd ap_max_inactivity */
#define AP_PROBE_WAIT_MS                10000   /* ... then the Null frame's answer (a sleeping client wakes for its TIM) */
#define AP_PRESP_PER_S                  30
#define AP_CLASS3_GAP_MS                1000
#define AP_BODY_MAX                     1024    /* beacon, probe and association response bodies */
#define MAC_HDR_LEN                     24

#define STYPE_ASSOC_REQ                 0x00
#define STYPE_ASSOC_RESP                0x10
#define STYPE_REASSOC_REQ               0x20
#define STYPE_REASSOC_RESP              0x30
#define STYPE_PROBE_REQ                 0x40
#define STYPE_PROBE_RESP                0x50
#define STYPE_BEACON                    0x80
#define STYPE_DISASSOC                  0xa0
#define STYPE_AUTH                      0xb0
#define STYPE_DEAUTH                    0xc0
#define STYPE_ACTION                    0xd0
#define STYPE_CTL_BAR                   0x80
#define STYPE_CTL_PSPOLL                0xa0
#define FTYPE_MGMT                      0x00
#define FTYPE_CTL                       0x04
#define FTYPE_DATA                      0x08
#define FC_STYPE_NODATA                 0x40    /* Null, QoS Null, CF-only */

#define AUTH_ALG_OPEN                   0
#define REASON_UNSPECIFIED              1
#define REASON_PREV_AUTH_NOT_VALID      2
#define REASON_LEAVING                  3
#define REASON_INACTIVITY               4
#define REASON_CLASS2                   6
#define REASON_CLASS3                   7
#define REASON_4WAY_TIMEOUT             15
#define STATUS_SUCCESS                  0
#define STATUS_UNSPECIFIED              1
#define STATUS_AUTH_ALG                 13
#define STATUS_AP_FULL                  17
#define CATEGORY_BACK                   3

_Static_assert(sizeof(struct rtw_ap_start) == 48, "rtw_ap_start layout");
_Static_assert(sizeof(struct rtw_ap_ind) == 28, "rtw_ap_ind layout");
_Static_assert(sizeof(struct rtw_ap_client) == 24, "rtw_ap_client layout");
_Static_assert(sizeof(struct rtw_ap_stats) == 340, "rtw_ap_stats layout");
_Static_assert(offsetof(struct rtw_ap_stats, clients) == 148, "rtw_ap_stats layout");
_Static_assert(RTW_AP_MAX_CLIENTS < 32, "AIDs in a u32 map");

/* the indications for the front end, under wiphy->mtx */
struct mac_ap_ind {
	struct list_head list;
	struct rtw_ap_ind ind;
	u32 len;                        /* data[] that follows */
	u8 data[];
};

static u32 wfd_mode = RTW_WFD_OFF;
static struct rtw_ap_stats ap_st;       /* the counters (the state fields are filled in on read) */
#define AP_COUNT(field)                 __atomic_fetch_add(&ap_st.field, 1, __ATOMIC_RELAXED)

/* the front end's data path callbacks of the GO port: read in read-side sections, detached with synchronize_rcu() */
static struct rtw_dp_ops ap_dp_ops;
static bool ap_dp_on;

static const u8 ap_rfc1042[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
static const u8 ap_bridge_tunnel[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8 };
static const u8 ap_tid_to_ac[8] = {
	IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_BK, IEEE80211_AC_BE,
	IEEE80211_AC_VI, IEEE80211_AC_VI, IEEE80211_AC_VO, IEEE80211_AC_VO,
};

static void ap_work (struct wiphy *wiphy, struct wiphy_work *work);
static void ap_timer (struct wiphy *wiphy, struct wiphy_work *work);

void mac_ap_init (struct mac_hw *mh)
{
	struct mac_ap *ap = &mh->ap;

	skb_queue_head_init(&ap->rxq);
	skb_queue_head_init(&ap->rx_fifo);
	spin_lock_init(&ap->ps_lock);
	INIT_LIST_HEAD(&ap->inds);
	atomic_set(&ap->n_ps, 0);
	atomic_set(&ap->tx_inflight, 0);
	atomic_set(&ap->mgmt_inflight, 0);
	wiphy_work_init(&ap->work, ap_work);
	wiphy_delayed_work_init(&ap->timer, ap_timer);
}

/* ---- the setting ---- */
void rtw_linux_set_wfd (uint32_t mode)
{
	WRITE_ONCE(wfd_mode, mode == RTW_WFD_ON ? RTW_WFD_ON : RTW_WFD_OFF);
	pr_info("mac: Wi-Fi Direct (mobile hotspot GO) %s", mode == RTW_WFD_ON ? "on" : "off");
}

uint32_t rtw_linux_get_wfd (void)
{
	return READ_ONCE(wfd_mode);
}

/* ---- helpers ---- */
/* A GO beacons only where the rules allow initiating radiation: not DISABLED, NO_IR or RADAR (we do no DFS), and not
   channel 14 (CCK only, while the GO sends OFDM) */
static bool ap_chan_ok (const struct ieee80211_channel *c)
{
	return c && !(c->flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR |
				  IEEE80211_CHAN_NO_OFDM)) && c->hw_value != 14;
}

static struct mac_sta *ap_find (struct mac_hw *mh, const u8 *addr)
{
	struct mac_sta *ms;

	list_for_each_entry_rcu(ms, &mh->sta_list, list)
		if (ms->ap_client && ether_addr_equal(ms->sta.addr, addr))
			return ms;
	return NULL;
}

static u32 ap_count_clients (struct mac_hw *mh, u8 min_state)
{
	struct mac_sta *ms;
	u32 n = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(ms, &mh->sta_list, list)
		n += ms->ap_client && READ_ONCE(ms->ap.state) >= min_state;
	rcu_read_unlock();
	return n;
}

static u16 ap_ethertype (const u8 *body, unsigned int len)
{
	if (len < 8 || (memcmp(body, ap_rfc1042, 6) && memcmp(body, ap_bridge_tunnel, 6)))
		return 0;
	return get_unaligned_be16(body + 6);
}

/* mac80211 ieee80211_frame_allowed: 802.1X frames from a client go up only when they are for us or the PAE group
   address, protected or not (FragAttacks CVE-2020-26139) */
static const u8 ap_pae_group[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x03 };

static bool ap_pae_da_ok (const struct mac_ap *ap, const u8 *da)
{
	return ether_addr_equal(da, ap->bss.bssid) || ether_addr_equal(da, ap_pae_group);
}

/* A client's A-MSDU (802.11-2020 9.3.2.2.2: subframes of DA, SA, length (big endian), MSDU, padded to 4 bytes but
   the last) is checked whole before any of it goes up: the lengths add up, every subframe comes from the client
   itself (mac80211 passes check_sa for an AP), none has the RFC 1042 header as its DA (cfg80211's mitigation of the
   A-MSDU injection of FragAttacks, CVE-2020-24588: our RSN capabilities do not ask for SPP A-MSDUs, so the A-MSDU bit
   is not protected), and 802.1X only to us. */
static bool ap_amsdu_ok (const struct mac_ap *ap, const u8 *sa, const u8 *body, unsigned int len)
{
	for (;;) {
		unsigned int sub, total;

		if (len < 14)
			return false;
		sub = get_unaligned_be16(body + 12);
		if (sub > len - 14)
			return false;
		if (!ether_addr_equal(body + 6, sa) || !memcmp(body, ap_rfc1042, sizeof(ap_rfc1042)) ||
		    (ap_ethertype(body + 14, sub) == ETH_P_PAE && !ap_pae_da_ok(ap, body)))
			return false;
		total = 14 + sub;
		if (total < len)
			total = ALIGN(total, 4);
		if (total >= len)
			return true;
		body += total;
		len -= total;
	}
}

/* An indication for the front end: the indication, then d1 and d2 (frames). wiphy->mtx held. */
static void ap_ind (struct mac_hw *mh, const struct rtw_ap_ind *ind, const u8 *d1, u32 l1, const u8 *d2, u32 l2)
{
	struct mac_ap *ap = &mh->ap;
	struct mac_ap_ind *n;

	lockdep_assert_wiphy(mh->hw.wiphy);
	/* only requests may be dropped when the queue is long (the client retries or times out): an ASSOC_DONE completes
	   the OS's association response task, LEFT and STOPPED keep the OS's client list right */
	if (!ap->event_fn || (ap->n_inds >= AP_INDS_MAX && ind->type == RTW_AP_IND_ASSOC_REQ)) {
		AP_COUNT(inds_dropped);
		return;
	}
	n = kmalloc(sizeof(*n) + l1 + l2, GFP_KERNEL);
	if (!n) {
		AP_COUNT(inds_dropped);
		return;
	}
	n->ind = *ind;
	n->len = l1 + l2;
	if (l1)
		memcpy(n->data, d1, l1);
	if (l2)
		memcpy(n->data + l1, d2, l2);
	list_add_tail(&n->list, &ap->inds);
	ap->n_inds++;
	ap->event_fn(ap->event_ctx, RTW_AP_EV_IND);
}

static void ap_inds_free (struct mac_ap *ap)
{
	struct mac_ap_ind *n, *n2;

	list_for_each_entry_safe(n, n2, &ap->inds, list) {
		list_del(&n->list);
		kfree(n);
	}
	ap->n_inds = 0;
}

/* ---- the BSS ---- */
/* The rates, HT and the Country element of our BSS on chan (apframe.h). wiphy->mtx held. */
static void ap_bss_setup (struct mac_hw *mh, const struct rtw_ap_start *req, struct ieee80211_channel *chan)
{
	struct mac_ap *ap = &mh->ap;
	struct apf_bss *b = &ap->bss;
	struct ieee80211_supported_band *sb = mh->hw.wiphy->bands[chan->band];
	bool band5 = chan->band == NL80211_BAND_5GHZ, b11 = !band5 && (req->flags & RTW_AP_F_11B);
	u32 basic = 0;
	int i;

	memset(b, 0, sizeof(*b));
	ether_addr_copy(b->bssid, req->addr);
	memcpy(b->ssid, req->ssid, req->ssid_len);
	b->ssid_len = req->ssid_len;
	b->channel = (u8)chan->hw_value;
	b->band5 = band5;
	b->rsn = req->flags & RTW_AP_F_RSN;
	b->beacon_int = req->beacon_int;
	b->dtim_period = req->dtim_period;
	/* rates: 2.4 GHz with 802.11b the CCK rates basic, else 6, 12, 24 Mbit/s; CCK left out without 802.11b */
	for (i = 0; sb && i < sb->n_bitrates && b->n_rates < APF_RATES_MAX; i++) {
		u8 r = (u8)(sb->bitrates[i].bitrate / 5);
		bool cck = r == 2 || r == 4 || r == 11 || r == 22;

		if (cck && !b11)
			continue;
		if (b11 ? cck : (r == 12 || r == 24 || r == 48)) {
			r |= 0x80;
			basic |= BIT(i);
		}
		b->rates[b->n_rates++] = r;
	}
	ap->vif->vif.bss_conf.basic_rates = basic;
	/* HT20: our capabilities without 40 MHz, SM power save disabled (like the station's association request) */
	if (sb && sb->ht_cap.ht_supported) {
		struct ieee80211_sta_ht_cap ht = sb->ht_cap;
		u8 el[2 + APF_HT_CAP_LEN];

		ht.cap &= ~(IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_DSSSCCK40 |
			    IEEE80211_HT_CAP_MAX_AMSDU | IEEE80211_HT_CAP_SM_PS);
		ht.cap |= WLAN_HT_CAP_SM_PS_DISABLED << IEEE80211_HT_CAP_SM_PS_SHIFT;
		mac_put_ht_cap(el, &ht);
		memcpy(b->ht_cap, el + 2, APF_HT_CAP_LEN);
		b->ht = true;
	}
	/* Country: the domain in force (none for the world rules), environment "any", the channels of the band that may
	   beacon, in runs of the same maximum power */
	if (mh->reg.cur[0] && mh->reg.cur[0] != '0') {
		u8 step = band5 ? 4 : 1;

		b->country[0] = (u8)mh->reg.cur[0];
		b->country[1] = (u8)mh->reg.cur[1];
		b->country[2] = ' ';
		for (i = 0; sb && i < sb->n_channels; i++) {
			const struct ieee80211_channel *c = &sb->channels[i];
			u8 *t;

			if (!ap_chan_ok(c))
				continue;
			t = b->n_triplets ? b->triplets[b->n_triplets - 1] : NULL;
			if (t && c->hw_value == t[0] + t[1] * step && t[2] == (u8)c->max_power) {
				t[1]++;
				continue;
			}
			if (b->n_triplets == APF_TRIPLETS_MAX)
				break;
			t = b->triplets[b->n_triplets++];
			t[0] = (u8)c->hw_value;
			t[1] = 1;
			t[2] = (u8)c->max_power;
		}
	}
}

/* The beacon body (from the Timestamp on) into out; *tim_off, *tim_len of its TIM element. wiphy->mtx held (or the
   caller otherwise keeps the GO and its clients). */
static size_t ap_beacon_body (struct mac_hw *mh, u8 *out, size_t cap, size_t *tim_off, size_t *tim_len)
{
	struct mac_ap *ap = &mh->ap;
	u8 tim[APF_TIM_MAX];
	struct mac_sta *ms;
	u32 aids = 0;

	/* clients asleep with frames waiting (their TIM bit) */
	rcu_read_lock();
	list_for_each_entry_rcu(ms, &mh->sta_list, list)
		if (ms->ap_client && ms->sta.aid && ms->sta.aid < 32 && READ_ONCE(ms->ap.ps) &&
		    skb_queue_len_lockless(&ms->ap.ps_q))
			aids |= BIT(ms->sta.aid);
	rcu_read_unlock();
	/* DTIM count 0 and the group bit 0: the hardware keeps both up to date (rtw89 B_AX_UPD_TIMIE/UPD_HGQMD) */
	*tim_len = apf_put_tim(tim, 0, ap->bss.dtim_period, false, aids);
	return apf_build_bss_body(&ap->bss, APF_BEACON, tim, *tim_len, ap->beacon_ies, ap->beacon_ies_len, out, cap,
				  tim_off);
}

static void ap_put_hdr (u8 *p, u16 stype, const u8 *da, const u8 *bssid)
{
	put_unaligned_le16(IEEE80211_FTYPE_MGMT | stype, p);
	put_unaligned_le16(0, p + 2);
	ether_addr_copy(p + 4, da);
	ether_addr_copy(p + 10, bssid);
	ether_addr_copy(p + 16, bssid);
	put_unaligned_le16(0, p + 22);
}

/*
 * mac80211 ieee80211_beacon_get_tim() for rtw89_fw_h2c_update_beacon(): the template the firmware sends. rtw89 reads
 * the frame control of the skb it gets at the skb structure itself (fw.c "hdr = (struct ieee80211_hdr *)skb_beacon"),
 * so the skb must be freshly allocated (its first field, next, NULL: frame control 0, a 24-byte management header),
 * which dev_alloc_skb() gives. *tim_offset counts from the start of the frame. wiphy->mtx held (rtw89's
 * link_info_changed or update_beacon_work).
 */
struct sk_buff *linuxkpi_ieee80211_beacon_get_tim (struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	uint16_t *tim_offset, uint16_t *tim_len, uint32_t link_id)
{
	struct mac_hw *mh = mac_hw(hw);
	size_t len, off = 0, tl = 0;
	struct sk_buff *skb;
	u8 *p;

	(void)link_id;
	if (tim_offset)
		*tim_offset = 0;
	if (tim_len)
		*tim_len = 0;
	if (!mac_ap_vif(mh, vif))
		return NULL;
	skb = dev_alloc_skb(MAC_HDR_LEN + AP_BODY_MAX);
	if (!skb)
		return NULL;
	p = skb_put_zero(skb, MAC_HDR_LEN + AP_BODY_MAX);
	ap_put_hdr(p, STYPE_BEACON, (const u8 *)"\xff\xff\xff\xff\xff\xff", mh->ap.bss.bssid);
	len = ap_beacon_body(mh, p + MAC_HDR_LEN, AP_BODY_MAX, &off, &tl);
	if (!len || WARN_ON(skb->next)) {
		pr_err("mac: GO beacon does not fit");
		kfree_skb(skb);
		return NULL;
	}
	skb_trim(skb, (unsigned int)(MAC_HDR_LEN + len));
	if (tim_offset)
		*tim_offset = (u16)(MAC_HDR_LEN + off);
	if (tim_len)
		*tim_len = (u16)tl;
	AP_COUNT(beacons);
	return skb;
}

/* mac80211 ieee80211_proberesp_get(): rtw89 asks only for its probe response offload, which it does not use on the
   8852B; answered all the same (broadcast DA) */
struct sk_buff *linuxkpi_ieee80211_proberesp_get (struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mac_hw *mh = mac_hw(hw);
	struct sk_buff *skb;
	size_t len, off;
	u8 *p;

	if (!mac_ap_vif(mh, vif))
		return NULL;
	skb = dev_alloc_skb(MAC_HDR_LEN + AP_BODY_MAX);
	if (!skb)
		return NULL;
	p = skb_put_zero(skb, MAC_HDR_LEN + AP_BODY_MAX);
	ap_put_hdr(p, STYPE_PROBE_RESP, (const u8 *)"\xff\xff\xff\xff\xff\xff", mh->ap.bss.bssid);
	len = apf_build_bss_body(&mh->ap.bss, APF_PROBE_RESP, NULL, 0, mh->ap.presp_ies, mh->ap.presp_ies_len,
				 p + MAC_HDR_LEN, AP_BODY_MAX, &off);
	if (!len) {
		kfree_skb(skb);
		return NULL;
	}
	skb_trim(skb, (unsigned int)(MAC_HDR_LEN + len));
	return skb;
}

/* ---- management frames ---- */
static struct sk_buff *ap_frame (struct mac_hw *mh, u16 stype, const u8 *da, unsigned int body_len, u8 **body)
{
	struct sk_buff *skb;
	u8 *p;

	skb = dev_alloc_skb(mh->hw.extra_tx_headroom + MAC_HDR_LEN + body_len);
	if (!skb)
		return NULL;
	skb_reserve(skb, mh->hw.extra_tx_headroom);
	p = skb_put_zero(skb, MAC_HDR_LEN + body_len);
	ap_put_hdr(p, stype, da, mh->ap.bss.bssid);
	*body = p + MAC_HDR_LEN;
	return skb;
}

/* Sends a management frame of the GO (ms: the client's station while it is in the driver, else NULL); takes the
   skb. status: the association response, whose tx status (cookie) the AP work waits for. Not through the station's
   pending table (tx.c): the GO's frames are counted apart, so a burst of them never keeps the station's MLME frames
   out. wiphy->mtx held. */
static int ap_tx_mgmt (struct mac_hw *mh, struct sk_buff *skb, struct mac_sta *ms, bool status, u16 cookie)
{
	struct mac_ap *ap = &mh->ap;
	struct ieee80211_tx_control ctl = { 0 };
	struct ieee80211_chanctx_conf *conf;
	struct ieee80211_tx_info *info;
	struct ieee80211_vif *vif;

	lockdep_assert_wiphy(mh->hw.wiphy);
	vif = ap->vif ? &ap->vif->vif : NULL;
	conf = vif ? vif->bss_conf.chanctx_conf : NULL;
	if (!conf || READ_ONCE(mh->restarting) || atomic_read(&ap->mgmt_inflight) >= AP_MGMT_INFLIGHT_MAX) {
		AP_COUNT(mgmt_dropped);
		kfree_skb(skb);
		return -EBUSY;
	}
	memset(skb->cb, 0, sizeof(skb->cb));
	info = IEEE80211_SKB_CB(skb);
	info->flags = status ? IEEE80211_TX_CTL_REQ_TX_STATUS : 0;
	if (is_multicast_ether_addr(skb->data + 4))
		info->flags |= IEEE80211_TX_CTL_NO_ACK;
	info->band = conf->def.chan->band;
	info->hw_queue = vif->hw_queue[IEEE80211_AC_VO];
	info->control.vif = vif;
	skb->priority = 7;
	skb_set_queue_mapping(skb, IEEE80211_AC_VO);
	skb->mac_tag = MAC_SKB_AP | MAC_SKB_MGMT | cookie;
	atomic_inc(&ap->mgmt_inflight);
	AP_COUNT(mgmt_tx);
	ctl.sta = ms && ms->state != IEEE80211_STA_NOTEXIST ? &ms->sta : NULL;
	mac_read_lock(mh);
	mh->ops->tx(&mh->hw, &ctl, skb);        /* a failure comes back through free_txskb */
	mac_read_unlock(mh);
	return 0;
}

static void ap_send_deauth (struct mac_hw *mh, const u8 *da, struct mac_sta *ms, u16 reason)
{
	struct sk_buff *skb;
	u8 *b;

	skb = ap_frame(mh, STYPE_DEAUTH, da, 2, &b);
	if (!skb)
		return;
	put_unaligned_le16(reason, b);
	if (!ap_tx_mgmt(mh, skb, ms, false, 0))
		AP_COUNT(deauth_tx);
}

static void ap_send_auth (struct mac_hw *mh, const u8 *da, struct mac_sta *ms, u16 alg, u16 status)
{
	struct sk_buff *skb;
	u8 *b;

	skb = ap_frame(mh, STYPE_AUTH, da, 6, &b);
	if (!skb)
		return;
	put_unaligned_le16(alg, b);
	put_unaligned_le16(2, b + 2);           /* transaction sequence 2 */
	put_unaligned_le16(status, b + 4);
	ap_tx_mgmt(mh, skb, ms, false, 0);
}

/* An action frame (agg.c: ADDBA response, DELBA) to a client. wiphy->mtx held. */
void mac_ap_send_action (struct mac_hw *mh, struct mac_sta *ms, const u8 *body, unsigned int len)
{
	struct sk_buff *skb;
	u8 *b;

	skb = ap_frame(mh, STYPE_ACTION, ms->sta.addr, len, &b);
	if (!skb)
		return;
	memcpy(b, body, len);
	ap_tx_mgmt(mh, skb, ms, false, 0);
}

/* ---- tx status (any context: record and queue only) ---- */
void mac_ap_tx_done (struct mac_hw *mh, struct sk_buff *skb, bool acked)
{
	struct mac_ap *ap = &mh->ap;
	u32 tag = skb->mac_tag;
	u16 cookie = (u16)(tag & 0xffff);
	struct mac_sta *ms;
	bool found = false;

	if (tag & MAC_SKB_DATA) {
		if (atomic_dec_return(&ap->tx_inflight) == AP_TX_INFLIGHT_LOW)
			mac_ap_dp_tx_ready();
		return;
	}
	atomic_dec(&ap->mgmt_inflight);
	if (!cookie)
		return;
	rcu_read_lock();
	list_for_each_entry_rcu(ms, &mh->sta_list, list) {
		if (!ms->ap_client)
			continue;
		if (READ_ONCE(ms->ap.resp_cookie) == cookie) {
			WRITE_ONCE(ms->ap.resp_txs, acked ? 1 : 2);
			found = true;
			break;
		}
		if (READ_ONCE(ms->ap.probe_cookie) == cookie) {
			/* our Null frame to an inactive client: an acknowledgement shows it is still there */
			if (acked)
				WRITE_ONCE(ms->ap.last_rx_ns, ktime_get_ns());
			break;
		}
	}
	rcu_read_unlock();
	if (found)
		wiphy_work_queue(mh->hw.wiphy, &ap->work);
}

/* ---- clients ---- */
/* What the client can do with us, from its association request (mlme.c sta_set_caps, the other way round). */
static void ap_sta_caps (struct mac_hw *mh, struct mac_sta *ms, const struct apf_assoc_req *r)
{
	struct mac_ap *ap = &mh->ap;
	struct ieee80211_supported_band *sb = mh->hw.wiphy->bands[ap->chan->band];
	struct ieee80211_link_sta *ls = &ms->sta.deflink;
	u32 rates = 0;
	int i, nss = 0;

	for (i = 0; i < sb->n_bitrates && i < 32; i++)
		for (u8 j = 0; j < r->n_rates; j++)
			if (sb->bitrates[i].bitrate / 5 == r->rates[j])
				rates |= BIT(i);
	ls->supp_rates[ap->chan->band] = rates ? rates : ap->vif->vif.bss_conf.basic_rates;
	ls->bandwidth = IEEE80211_STA_RX_BW_20;
	ls->smps_mode = IEEE80211_SMPS_OFF;
	ls->rx_nss = 1;
	ls->agg.max_amsdu_len = 3839;
	ls->agg.max_rc_amsdu_len = 3839;
	memset(&ls->ht_cap, 0, sizeof(ls->ht_cap));
	memset(&ls->vht_cap, 0, sizeof(ls->vht_cap));
	memset(&ls->he_cap, 0, sizeof(ls->he_cap));
	memset(&ls->eht_cap, 0, sizeof(ls->eht_cap));
	ms->sta.wme = r->wmm && mh->hw.queues >= IEEE80211_NUM_ACS;
	ms->sta.uapsd_queues = 0;               /* we never advertise U-APSD */
	ms->sta.max_sp = 0;
	ms->ap.wmm = ms->sta.wme;
	if (r->ht && ap->bss.ht && sb->ht_cap.ht_supported) {
		u16 cap = get_unaligned_le16(r->ht_cap), own = sb->ht_cap.cap, c = 0;

		c |= cap & own & (IEEE80211_HT_CAP_LDPC_CODING | IEEE80211_HT_CAP_SGI_20);
		c |= cap & IEEE80211_HT_CAP_SM_PS;
		if ((cap & IEEE80211_HT_CAP_RX_STBC) && (own & IEEE80211_HT_CAP_TX_STBC))
			c |= cap & IEEE80211_HT_CAP_RX_STBC;
		if ((cap & IEEE80211_HT_CAP_TX_STBC) && (own & IEEE80211_HT_CAP_RX_STBC))
			c |= IEEE80211_HT_CAP_TX_STBC;
		ls->ht_cap.ht_supported = true;
		ls->ht_cap.cap = c;
		ls->ht_cap.ampdu_factor = r->ht_cap[2] & IEEE80211_HT_AMPDU_PARM_FACTOR;
		ls->ht_cap.ampdu_density = (r->ht_cap[2] & IEEE80211_HT_AMPDU_PARM_DENSITY) >>
					   IEEE80211_HT_AMPDU_PARM_DENSITY_SHIFT;
		for (i = 0; i < IEEE80211_HT_MCS_MASK_LEN; i++)
			ls->ht_cap.mcs.rx_mask[i] = r->ht_cap[3 + i] & sb->ht_cap.mcs.rx_mask[i];
		ls->ht_cap.mcs.rx_highest = get_unaligned_le16(r->ht_cap + 3 + IEEE80211_HT_MCS_MASK_LEN);
		for (i = 0; i < 4; i++)
			nss += ls->ht_cap.mcs.rx_mask[i] != 0;
		ls->rx_nss = (u8)clamp(nss, 1, 2);
		switch ((cap & IEEE80211_HT_CAP_SM_PS) >> IEEE80211_HT_CAP_SM_PS_SHIFT) {
		case WLAN_HT_CAP_SM_PS_STATIC:
			ls->smps_mode = IEEE80211_SMPS_STATIC;
			break;
		case WLAN_HT_CAP_SM_PS_DYNAMIC:
			ls->smps_mode = IEEE80211_SMPS_DYNAMIC;
			break;
		default:
			break;
		}
	}
}

/* A new client after its open system authentication: on the list first, then NONE -> AUTH in the driver (rtw89
   gives it a MAC ID). wiphy->mtx held. */
static struct mac_sta *ap_client_new (struct mac_hw *mh, const u8 *addr)
{
	struct mac_sta *ms;

	ms = mac_sta_alloc(mh, mh->ap.vif, addr);
	if (!ms)
		return NULL;
	ms->ap_client = true;
	skb_queue_head_init(&ms->ap.ps_q);
	ms->ap.state = MAC_APC_AUTH;
	ms->ap.since_ns = ms->ap.last_rx_ns = ktime_get_ns();
	ms->sta.deflink.supp_rates[mh->ap.chan->band] = mh->ap.vif->vif.bss_conf.basic_rates;
	smp_wmb();
	list_add_tail_rcu(&ms->list, &mh->sta_list);
	if (mac_sta_move(mh, ms, IEEE80211_STA_AUTH)) {
		list_del_rcu(&ms->list);
		synchronize_rcu();
		mac_sta_move(mh, ms, IEEE80211_STA_NOTEXIST);
		mac_sta_free(ms);
		return NULL;
	}
	return ms;
}

/* frames buffered for it go (their keys may be going) */
static void ap_ps_purge (struct mac_ap *ap, struct mac_sta *ms)
{
	struct sk_buff_head q;

	__skb_queue_head_init(&q);
	spin_lock_bh(&ap->ps_lock);
	skb_queue_splice_init(&ms->ap.ps_q, &q);
	ms->ap.ps_flush = false;
	spin_unlock_bh(&ap->ps_lock);
	__skb_queue_purge(&q);
}

/* Its TIM bit off in the beacon template (mac80211 sta_info_recalc_tim when a station goes): set_tim makes rtw89
   fetch the beacon again, which no longer counts the client (its buffer is empty, or it is off the list). The station
   must still be in the driver. wiphy->mtx held, the driver not being reset. */
static void ap_tim_clear (struct mac_hw *mh, struct mac_sta *ms)
{
	clear_bit(MAC_APC_F_TIM, &ms->ap.flags);
	if (!ms->ap.tim || !mh->ops->set_tim || ms->state == IEEE80211_STA_NOTEXIST || READ_ONCE(mh->restarting))
		return;
	ms->ap.tim = false;
	mh->ops->set_tim(&mh->hw, &ms->sta, false);
}

/* Takes its pairwise key out of use: unpublished, readers gone, out of rtw89 (unless call_driver is false), freed. */
static void ap_ptk_remove (struct mac_hw *mh, struct mac_sta *ms, bool call_driver)
{
	struct mac_key *k = ms->ap.ptk;
	int ret = 0;

	if (!k)
		return;
	rcu_assign_pointer(ms->ap.ptk, NULL);
	synchronize_rcu();
	ap_ps_purge(&mh->ap, ms);       /* buffered frames point at the key */
	if (call_driver && k->hw && !READ_ONCE(k->lost) && mh->ap.vif && mh->ap.vif->vif.bss_conf.chanctx_conf)
		ret = mh->ops->set_key(&mh->hw, DISABLE_KEY, &mh->ap.vif->vif, &ms->sta, &k->conf);
	if (ret)
		pr_warn("mac: GO: drv set_key(DISABLE) of %pM's pairwise key failed: %d", ms->sta.addr, ret);
	mac_key_free(k);
	AP_COUNT(key_dels);
}

/*
 * A client goes: deauthenticated first when tx (reason tx_reason), off the list (readers gone), its buffered frames,
 * block ack sessions and pairwise key, then down to NOTEXIST in the driver (mac80211 __sta_info_destroy). With
 * indicate and associated: RTW_AP_IND_LEFT (status, code, frame). wiphy->mtx held; the driver is not being reset.
 */
static void ap_client_remove (struct mac_hw *mh, struct mac_sta *ms, bool tx, u16 tx_reason, bool indicate,
	s32 status, u16 code, const u8 *frame, u32 frame_len)
{
	struct mac_ap *ap = &mh->ap;
	struct ieee80211_hw *hw = &mh->hw;
	bool assoc = ms->ap.state == MAC_APC_ASSOC;
	struct rtw_ap_ind ind;

	lockdep_assert_wiphy(hw->wiphy);
	if (tx) {
		ap_send_deauth(mh, ms->sta.addr, ms, tx_reason);
		if (mh->ops->flush)
			mh->ops->flush(hw, &ap->vif->vif, (u32)(BIT(hw->queues) - 1), false);
	}
	list_del_rcu(&ms->list);
	synchronize_rcu();
	spin_lock_bh(&ap->ps_lock);
	if (ms->ap.ps)
		atomic_dec(&ap->n_ps);
	ms->ap.ps = false;
	spin_unlock_bh(&ap->ps_lock);
	ap_ps_purge(ap, ms);
	ap_tim_clear(mh, ms);
	mac_agg_stop_client(mh, ms, true);
	if (ms->state == IEEE80211_STA_AUTHORIZED)
		mac_sta_move(mh, ms, IEEE80211_STA_ASSOC);
	ap_ptk_remove(mh, ms, true);
	mac_sta_move(mh, ms, IEEE80211_STA_NOTEXIST);
	if (ms->sta.aid && ms->sta.aid < 32)
		ap->aid_map &= ~BIT(ms->sta.aid);
	if (ms->ap.state == MAC_APC_RESP) {
		/* the OS's association response task waits for this client: it ends (failed) */
		memset(&ind, 0, sizeof(ind));
		ind.type = RTW_AP_IND_ASSOC_DONE;
		ether_addr_copy(ind.peer, ms->sta.addr);
		ind.reassoc = ms->ap.reassoc;
		ind.status = -ENOLINK;
		ind.band = (u8)ap->chan->band;
		ap_ind(mh, &ind, NULL, 0, NULL, 0);
	}
	if (indicate && assoc) {
		memset(&ind, 0, sizeof(ind));
		ind.type = RTW_AP_IND_LEFT;
		ether_addr_copy(ind.peer, ms->sta.addr);
		ind.status = status;
		ind.code = code;
		ind.frame_len = (u16)min_t(u32, frame_len, MAC_FRAME_MAX);
		ind.band = (u8)ap->chan->band;
		ap_ind(mh, &ind, frame, ind.frame_len, NULL, 0);
	}
	pr_info("mac: GO: client %pM gone (state %u, status %d, reason %u%s)", ms->sta.addr, ms->ap.state, status, code,
		tx ? ", deauthenticated" : "");
	mac_sta_free(ms);
}

/* An associated client starts over (it reassociates): keys and buffers go, the driver's station back to AUTH. */
static void ap_client_unassoc (struct mac_hw *mh, struct mac_sta *ms)
{
	struct mac_ap *ap = &mh->ap;
	struct rtw_ap_ind ind;

	spin_lock_bh(&ap->ps_lock);
	if (ms->ap.ps)
		atomic_dec(&ap->n_ps);
	ms->ap.ps = false;
	spin_unlock_bh(&ap->ps_lock);
	ap_ps_purge(ap, ms);
	ap_tim_clear(mh, ms);
	mac_agg_stop_client(mh, ms, true);
	if (ms->state == IEEE80211_STA_AUTHORIZED)
		mac_sta_move(mh, ms, IEEE80211_STA_ASSOC);
	ap_ptk_remove(mh, ms, true);
	mac_sta_move(mh, ms, IEEE80211_STA_AUTH);
	if (ms->sta.aid && ms->sta.aid < 32)
		ap->aid_map &= ~BIT(ms->sta.aid);
	ms->sta.aid = 0;
	WRITE_ONCE(ms->ap.state, MAC_APC_AUTH);
	memset(&ind, 0, sizeof(ind));
	ind.type = RTW_AP_IND_LEFT;
	ether_addr_copy(ind.peer, ms->sta.addr);
	ind.status = RTW_LOST_PEER_DISASSOC;
	ind.band = (u8)ap->chan->band;
	ap_ind(mh, &ind, NULL, 0, NULL, 0);
}

/* the clients, for removal loops (list_for_each_entry_safe cannot be used: removal waits for readers) */
static struct mac_sta *ap_first_client (struct mac_hw *mh)
{
	struct mac_sta *ms;

	list_for_each_entry(ms, &mh->sta_list, list)
		if (ms->ap_client)
			return ms;
	return NULL;
}

/* ---- received management frames (the AP work, wiphy->mtx held; f = the frame, FCS removed) ---- */
static void ap_rx_probe_req (struct mac_hw *mh, const u8 *f, unsigned int len)
{
	struct mac_ap *ap = &mh->ap;
	const u8 *ies = f + MAC_HDR_LEN, *ssid, *ds;
	unsigned int ies_len = len - MAC_HDR_LEN;
	struct sk_buff *skb;
	size_t blen, off;
	u64 now = ktime_get_ns();
	u8 *b;

	AP_COUNT(probe_req);
	if (!is_valid_ether_addr(f + 10))
		return;
	/* only for us: the wildcard or our SSID, and (DS Parameter Set) our channel */
	ssid = apf_find_ie(ies, ies_len, APF_EID_SSID);
	if (!ssid || (ssid[1] && (ssid[1] != ap->bss.ssid_len || memcmp(ssid + 2, ap->bss.ssid, ssid[1]))))
		return;
	ds = apf_find_ie(ies, ies_len, APF_EID_DS);
	if (ds && ds[1] >= 1 && ds[2] != ap->bss.channel)
		return;
	if (now - ap->presp_window_ns >= NSEC_PER_SEC) {
		ap->presp_window_ns = now;
		ap->presp_count = 0;
	}
	if (ap->presp_count >= AP_PRESP_PER_S)
		return;
	ap->presp_count++;
	skb = ap_frame(mh, STYPE_PROBE_RESP, f + 10, AP_BODY_MAX, &b);
	if (!skb)
		return;
	blen = apf_build_bss_body(&ap->bss, APF_PROBE_RESP, NULL, 0, ap->presp_ies, ap->presp_ies_len, b, AP_BODY_MAX,
				  &off);
	if (!blen) {
		kfree_skb(skb);
		return;
	}
	skb_trim(skb, (unsigned int)(MAC_HDR_LEN + blen));
	if (!ap_tx_mgmt(mh, skb, NULL, false, 0))
		AP_COUNT(probe_resp);
}

static void ap_rx_auth (struct mac_hw *mh, const u8 *f, unsigned int len)
{
	const u8 *sa = f + 10;
	struct mac_sta *ms;
	u16 alg, seq;

	if (len < MAC_HDR_LEN + 6 || !is_valid_ether_addr(sa))
		return;
	alg = get_unaligned_le16(f + MAC_HDR_LEN);
	seq = get_unaligned_le16(f + MAC_HDR_LEN + 2);
	AP_COUNT(auth_rx);
	if (seq != 1)
		return;
	if (alg != AUTH_ALG_OPEN) {
		ap_send_auth(mh, sa, NULL, alg, STATUS_AUTH_ALG);
		return;
	}
	/* authenticating again: the old association is over (hostapd handle_auth -> the station starts afresh) */
	ms = ap_find(mh, sa);
	if (ms)
		ap_client_remove(mh, ms, false, 0, true, RTW_LOST_PEER_DEAUTH, REASON_PREV_AUTH_NOT_VALID, NULL, 0);
	if (ap_count_clients(mh, MAC_APC_AUTH) >= RTW_AP_MAX_CLIENTS) {
		ap_send_auth(mh, sa, NULL, AUTH_ALG_OPEN, STATUS_AP_FULL);
		return;
	}
	ms = ap_client_new(mh, sa);
	if (!ms) {
		ap_send_auth(mh, sa, NULL, AUTH_ALG_OPEN, STATUS_UNSPECIFIED);
		return;
	}
	pr_info("mac: GO: %pM authenticated (open system)", sa);
	ap_send_auth(mh, sa, ms, AUTH_ALG_OPEN, STATUS_SUCCESS);
}

/* an association response refused here, without the OS (the request does not fit our BSS) */
static void ap_send_refusal (struct mac_hw *mh, struct mac_sta *ms, bool reassoc, u16 status)
{
	struct sk_buff *skb;
	size_t blen;
	u8 *b;

	skb = ap_frame(mh, reassoc ? STYPE_REASSOC_RESP : STYPE_ASSOC_RESP, ms->sta.addr, AP_BODY_MAX, &b);
	if (!skb)
		return;
	blen = apf_build_assoc_resp(&mh->ap.bss, status, 0, false, NULL, 0, b, AP_BODY_MAX);
	if (!blen) {
		kfree_skb(skb);
		return;
	}
	skb_trim(skb, (unsigned int)(MAC_HDR_LEN + blen));
	ap_tx_mgmt(mh, skb, ms, false, 0);
	AP_COUNT(assoc_refused);
}

static void ap_rx_assoc_req (struct mac_hw *mh, const u8 *f, unsigned int len, bool reassoc)
{
	const u8 *sa = f + 10, *body = f + MAC_HDR_LEN;
	unsigned int blen = len - MAC_HDR_LEN;
	struct apf_assoc_req r;
	struct rtw_ap_ind ind;
	struct mac_sta *ms;
	u16 status;

	AP_COUNT(assoc_req);
	ms = ap_find(mh, sa);
	if (!ms) {
		/* class 2 frame from a station not authenticated */
		if (is_valid_ether_addr(sa))
			ap_send_deauth(mh, sa, NULL, REASON_CLASS2);
		return;
	}
	if (ms->ap.state == MAC_APC_RESP || ms->ap.state == MAC_APC_WAIT_OS)
		return;                         /* a retransmission: the OS's answer or its tx status is pending */
	if (ms->ap.state == MAC_APC_ASSOC) {
		pr_info("mac: GO: %pM reassociates", sa);
		ap_client_unassoc(mh, ms);
	}
	if (apf_parse_assoc_req(body, blen, reassoc, &r)) {
		ap_send_refusal(mh, ms, reassoc, STATUS_UNSPECIFIED);
		return;
	}
	status = apf_check_assoc_req(&mh->ap.bss, &r);
	if (status) {
		pr_info("mac: GO: %pM association refused here: status %u (RSN %u%s, PSK %u, CCMP %u, %u rates)", sa,
			status, r.rsn, r.rsn_bad ? " malformed" : "", r.rsn_psk, r.rsn_ccmp, r.n_rates);
		ap_send_refusal(mh, ms, reassoc, status);
		return;
	}
	kfree(ms->ap.req);
	ms->ap.req = kmemdup(body, blen, GFP_KERNEL);
	if (!ms->ap.req) {
		ap_send_refusal(mh, ms, reassoc, STATUS_UNSPECIFIED);
		return;
	}
	ms->ap.req_len = (u16)blen;
	ms->ap.reassoc = reassoc;
	ms->ap.since_ns = ktime_get_ns();
	WRITE_ONCE(ms->ap.state, MAC_APC_WAIT_OS);
	memset(&ind, 0, sizeof(ind));
	ind.type = RTW_AP_IND_ASSOC_REQ;
	ether_addr_copy(ind.peer, sa);
	ind.reassoc = reassoc;
	ind.frame_len = (u16)blen;
	ind.ht = r.ht;
	ind.wmm = r.wmm;
	ind.rsn = r.rsn;
	ind.band = (u8)mh->ap.chan->band;
	pr_info("mac: GO: %pM asks to %sassociate (%s%s%s); the OS decides", sa, reassoc ? "re" : "",
		r.ht ? "HT" : "legacy", r.wmm ? ", WMM" : "", r.rsn ? ", RSN" : "");
	ap_ind(mh, &ind, body, blen, NULL, 0);
}

static void ap_rx_leave (struct mac_hw *mh, const u8 *f, unsigned int len, bool deauth)
{
	struct mac_sta *ms = ap_find(mh, f + 10);
	u16 reason = len >= MAC_HDR_LEN + 2 ? get_unaligned_le16(f + MAC_HDR_LEN) : 0;

	if (!ms)
		return;
	AP_COUNT(deauth_rx);
	ap_client_remove(mh, ms, false, 0, true, deauth ? RTW_LOST_PEER_DEAUTH : RTW_LOST_PEER_DISASSOC, reason,
			 f + MAC_HDR_LEN, len - MAC_HDR_LEN);
}

static void ap_rx_mgmt (struct mac_hw *mh, struct sk_buff *skb)
{
	const u8 *f = skb->data;
	unsigned int len = skb->len;
	struct mac_sta *ms;
	u16 stype = get_unaligned_le16(f) & IEEE80211_FCTL_STYPE;

	if (len < MAC_HDR_LEN)
		return;
	/* a protected management frame needs MFP, which the GO lacks: dropped */
	if (get_unaligned_le16(f) & IEEE80211_FCTL_PROTECTED)
		return;
	switch (stype) {
	case STYPE_PROBE_REQ:
		ap_rx_probe_req(mh, f, len);
		break;
	case STYPE_AUTH:
		ap_rx_auth(mh, f, len);
		break;
	case STYPE_ASSOC_REQ:
	case STYPE_REASSOC_REQ:
		ap_rx_assoc_req(mh, f, len, stype == STYPE_REASSOC_REQ);
		break;
	case STYPE_DEAUTH:
	case STYPE_DISASSOC:
		ap_rx_leave(mh, f, len, stype == STYPE_DEAUTH);
		break;
	case STYPE_ACTION:
		ms = ap_find(mh, f + 10);
		if (ms && ms->ap.state == MAC_APC_ASSOC && len >= MAC_HDR_LEN + 2 && f[MAC_HDR_LEN] == CATEGORY_BACK)
			mac_agg_rx_action_sta(mh, ms, f + MAC_HDR_LEN, len - MAC_HDR_LEN);
		break;
	default:
		break;
	}
}

/* ---- the OS's answer to an association request ---- */
static void ap_resp_done (struct mac_hw *mh, struct mac_sta *ms, bool acked)
{
	struct mac_ap *ap = &mh->ap;
	struct rtw_ap_ind ind;
	size_t blen = 0, off, tl;
	u8 *beacon;
	int ret = 0;

	memset(&ind, 0, sizeof(ind));
	ind.type = RTW_AP_IND_ASSOC_DONE;
	ether_addr_copy(ind.peer, ms->sta.addr);
	ind.reassoc = ms->ap.reassoc;
	ind.code = ms->ap.resp_status;
	ind.band = (u8)ap->chan->band;
	WRITE_ONCE(ms->ap.resp_cookie, 0);
	if (ms->ap.resp_accept && acked) {
		/* hostapd: the station becomes associated once the response was acknowledged */
		ret = mac_sta_move(mh, ms, IEEE80211_STA_ASSOC);
		if (!ret && !(ap->flags & RTW_AP_F_RSN))
			ret = mac_sta_move(mh, ms, IEEE80211_STA_AUTHORIZED);   /* open: no control port */
	}
	if (ms->ap.resp_accept && acked && !ret) {
		ms->ap.since_ns = ms->ap.last_rx_ns = ktime_get_ns();
		WRITE_ONCE(ms->ap.state, MAC_APC_ASSOC);
		ind.accepted = true;
		ind.ht = ms->sta.deflink.ht_cap.ht_supported;
		ind.wmm = ms->sta.wme;
		ind.rsn = !!(ap->flags & RTW_AP_F_RSN);
		AP_COUNT(assoc_ok);
		pr_info("mac: GO: %pM associated, AID %u, %s%s", ms->sta.addr, ms->sta.aid, ind.ht ? "HT20" : "legacy",
			ind.wmm ? ", WMM" : "");
	} else {
		if (ms->ap.resp_accept) {
			ind.status = ret ? ret : -EIO;
			AP_COUNT(assoc_tx_failed);
			pr_info("mac: GO: association response to %pM %s", ms->sta.addr,
				ret ? "sent, but the driver refused the station" : "not acknowledged");
			if (ms->state > IEEE80211_STA_AUTH)
				mac_sta_move(mh, ms, IEEE80211_STA_AUTH);
			if (ms->sta.aid && ms->sta.aid < 32)
				ap->aid_map &= ~BIT(ms->sta.aid);
			ms->sta.aid = 0;
		} else {
			AP_COUNT(assoc_refused);
			if (!acked)
				ind.status = -EIO;
		}
		ms->ap.since_ns = ktime_get_ns();
		WRITE_ONCE(ms->ap.state, MAC_APC_AUTH);
	}
	/* the beacon's body from the Timestamp field on, as the WDI sample's WDI_TLV_BEACON_IES (WDI_Cmds.c
	   MMPDU_BODY(beaconframe)): the OS's authenticator takes our RSN element from it, and given the elements alone
	   (no fixed fields) it gives up on the client at once (observed on Windows 11 26100) */
	beacon = kmalloc(AP_BODY_MAX, GFP_KERNEL);
	if (beacon) {
		blen = ap_beacon_body(mh, beacon, AP_BODY_MAX, &off, &tl);
		if (blen < APF_FIXED_LEN)
			blen = 0;
	}
	ind.frame_len = ms->ap.resp_len;
	ind.ies_len = (u16)blen;
	ap_ind(mh, &ind, ms->ap.resp, ms->ap.resp_len, beacon, ind.ies_len);
	kfree(beacon);
	kfree(ms->ap.resp);
	ms->ap.resp = NULL;
	ms->ap.resp_len = 0;
	kfree(ms->ap.req);
	ms->ap.req = NULL;
	ms->ap.req_len = 0;
}

int rtw_linux_ap_assoc_resp (const uint8_t *peer, int accept, uint16_t status, const uint8_t *ies, uint32_t ie_len)
{
	struct mac_hw *mh = mac_the_hw;
	struct apf_assoc_req r;
	struct sk_buff *skb;
	struct mac_sta *ms;
	struct mac_ap *ap;
	size_t blen;
	u16 aid = 0, cookie;
	u8 *b;
	int ret = 0;

	if (!mh)
		return -ENODEV;
	if ((ie_len && (!ies || ie_len > RTW_AP_IES_MAX || !apf_ies_ok(ies, ie_len))) || (!accept && !status))
		return -EINVAL;
	ap = &mh->ap;
	if (!mac_lock_driver(mh) || !ap->running) {
		wiphy_unlock(mh->hw.wiphy);
		return -ENOTCONN;
	}
	ms = ap_find(mh, peer);
	if (!ms || ms->ap.state != MAC_APC_WAIT_OS || !ms->ap.req ||
	    apf_parse_assoc_req(ms->ap.req, ms->ap.req_len, ms->ap.reassoc, &r)) {
		wiphy_unlock(mh->hw.wiphy);
		return -ENOENT;
	}
	if (accept) {
		for (aid = 1; aid <= RTW_AP_MAX_CLIENTS && (ap->aid_map & BIT(aid)); aid++)
			;
		if (aid > RTW_AP_MAX_CLIENTS) {
			accept = 0;
			status = STATUS_AP_FULL;
			aid = 0;
		} else {
			ap_sta_caps(mh, ms, &r);
			ms->sta.aid = aid;
			ap->aid_map |= BIT(aid);
			status = STATUS_SUCCESS;
		}
	}
	ms->ap.resp = kmalloc(AP_BODY_MAX, GFP_KERNEL);
	skb = ms->ap.resp ? ap_frame(mh, ms->ap.reassoc ? STYPE_REASSOC_RESP : STYPE_ASSOC_RESP, peer, AP_BODY_MAX, &b) :
	      NULL;
	blen = skb ? apf_build_assoc_resp(&ap->bss, status, aid, accept && ms->sta.wme, ies, ie_len, b, AP_BODY_MAX) : 0;
	if (!blen) {
		kfree_skb(skb);
		kfree(ms->ap.resp);
		ms->ap.resp = NULL;
		if (aid) {
			ap->aid_map &= ~BIT(aid);
			ms->sta.aid = 0;
		}
		wiphy_unlock(mh->hw.wiphy);
		return -ENOMEM;
	}
	skb_trim(skb, (unsigned int)(MAC_HDR_LEN + blen));
	memcpy(ms->ap.resp, b, blen);
	ms->ap.resp_len = (u16)blen;
	ms->ap.resp_accept = accept;
	ms->ap.resp_status = status;
	cookie = ++ap->cookie ? ap->cookie : ++ap->cookie;
	WRITE_ONCE(ms->ap.resp_txs, 0);
	WRITE_ONCE(ms->ap.resp_cookie, cookie);
	ms->ap.since_ns = ktime_get_ns();
	WRITE_ONCE(ms->ap.state, MAC_APC_RESP);
	pr_info("mac: GO: %sassociation response to %pM: %s (status %u, AID %u)", ms->ap.reassoc ? "re" : "", peer,
		accept ? "accepted" : "refused", status, aid);
	if (ap_tx_mgmt(mh, skb, ms, true, cookie))
		ap_resp_done(mh, ms, false);    /* not sent: the front end hears it at once */
	wiphy_unlock(mh->hw.wiphy);
	return ret;
}

int rtw_linux_ap_disconnect (const uint8_t *peer, uint16_t reason)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_sta *ms;
	int ret = -ENOENT;

	if (!mh)
		return -ENODEV;
	if (!mac_lock_driver(mh) || !mh->ap.running) {
		wiphy_unlock(mh->hw.wiphy);
		return -ENOTCONN;
	}
	if (!reason)
		reason = REASON_LEAVING;
	if (is_broadcast_ether_addr(peer)) {
		while ((ms = ap_first_client(mh))) {
			ap_client_remove(mh, ms, true, reason, false, RTW_LOST_BY_HOST, reason, NULL, 0);
			ret = 0;
		}
	} else {
		ms = ap_find(mh, peer);
		if (ms) {
			ap_client_remove(mh, ms, true, reason, false, RTW_LOST_BY_HOST, reason, NULL, 0);
			ret = 0;
		}
	}
	wiphy_unlock(mh->hw.wiphy);
	return ret;
}

/* ---- keys ---- */
int rtw_linux_ap_add_key (const struct rtw_key *in)
{
	struct mac_hw *mh = mac_the_hw;
	bool pairwise = in->type == RTW_KEY_PAIRWISE;
	struct mac_key *k, **slot;
	struct mac_sta *ms = NULL;
	struct mac_ap *ap;
	struct ieee80211_vif *vif;
	bool again = false;
	int ret;

	if (!mh)
		return -ENODEV;
	if (in->type != RTW_KEY_PAIRWISE && in->type != RTW_KEY_GROUP)
		return -EOPNOTSUPP;
	k = mac_key_alloc(in);
	if (!k)
		return in->cipher != RTW_CIPHER_CCMP || in->len != 16 ? -EOPNOTSUPP : -EINVAL;
	ap = &mh->ap;
	if (!mac_lock_driver(mh)) {
		ret = -EBUSY;
		goto out;
	}
	if (!ap->running || !(ap->flags & RTW_AP_F_RSN)) {
		ret = -ENOTCONN;
		goto out;
	}
	vif = &ap->vif->vif;
	if (pairwise) {
		ms = ap_find(mh, in->peer);
		if (!ms || ms->ap.state != MAC_APC_ASSOC || ms->state < IEEE80211_STA_ASSOC || in->index) {
			ret = -EINVAL;
			goto out;
		}
		slot = &ms->ap.ptk;
	} else {
		slot = &ap->gtk[in->index];
	}
	if (*slot && mac_key_identical(*slot, k)) {
		ret = 0;                /* installed again (a replayed handshake message): keep its PN state */
		again = true;
		goto out;
	}
	if (*slot) {
		/* rekey: the old key with this ID goes first (rtw89 matches by key ID) */
		if (pairwise) {
			ap_ptk_remove(mh, ms, true);
		} else {
			struct mac_key *old = *slot;

			rcu_assign_pointer(*slot, NULL);
			synchronize_rcu();
			if (old->hw && !READ_ONCE(old->lost))
				mh->ops->set_key(&mh->hw, DISABLE_KEY, vif, NULL, &old->conf);
			mac_key_free(old);
			AP_COUNT(key_dels);
		}
	}
	ret = mh->ops->set_key(&mh->hw, SET_KEY, vif, pairwise ? &ms->sta : NULL, &k->conf);
	k->hw = !ret;
	if (ret) {
		pr_err("mac: GO: drv set_key of the %s key %u failed: %d", pairwise ? "pairwise" : "group", in->index, ret);
		ret = -EIO;
		goto out;
	}
	if (pairwise) {
		/* frames held for reordering were decrypted with an older key: they go as the new one is published */
		spin_lock_bh(&mh->rx_lock);
		rcu_assign_pointer(*slot, k);
		mac_agg_rx_drop_held(mh, ms);
		spin_unlock_bh(&mh->rx_lock);
	} else {
		rcu_assign_pointer(*slot, k);
		WRITE_ONCE(ap->gtk_tx, in->index);
	}
	k = NULL;
	AP_COUNT(key_adds);
	if (pairwise && ms->state == IEEE80211_STA_ASSOC && !mac_sta_move(mh, ms, IEEE80211_STA_AUTHORIZED))
		pr_info("mac: GO: %pM authorized", ms->sta.addr);
	pr_info("mac: GO: %s key %u installed (CCMP)%s", pairwise ? "pairwise" : "group", in->index,
		pairwise ? "" : ", used for group frames");
out:
	wiphy_unlock(mh->hw.wiphy);
	if (again)
		pr_info("mac: GO: %s key %u installed again with the same value; kept", pairwise ? "pairwise" : "group",
			in->index);
	mac_key_free(k);
	return ret;
}

int rtw_linux_ap_del_key (uint8_t type, uint8_t index, const uint8_t *peer)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_sta *ms;
	struct mac_key *old;
	int ret = 0;

	if (!mh)
		return -ENODEV;
	if (!mac_lock_driver(mh)) {
		wiphy_unlock(mh->hw.wiphy);
		return -EBUSY;
	}
	if (!mh->ap.running) {
		ret = -ENOENT;
	} else if (type == RTW_KEY_PAIRWISE) {
		ms = peer ? ap_find(mh, peer) : NULL;
		if (!ms || !ms->ap.ptk)
			ret = -ENOENT;
		else
			ap_ptk_remove(mh, ms, true);
	} else if (type == RTW_KEY_GROUP && index < MAC_AP_GTKS && mh->ap.gtk[index]) {
		old = mh->ap.gtk[index];
		rcu_assign_pointer(mh->ap.gtk[index], NULL);
		synchronize_rcu();
		if (old->hw && !READ_ONCE(old->lost))
			mh->ops->set_key(&mh->hw, DISABLE_KEY, &mh->ap.vif->vif, NULL, &old->conf);
		mac_key_free(old);
		AP_COUNT(key_dels);
	} else {
		ret = -ENOENT;
	}
	wiphy_unlock(mh->hw.wiphy);
	return ret;
}

int rtw_linux_ap_set_tx_key (uint8_t index)
{
	struct mac_hw *mh = mac_the_hw;

	if (!mh || index >= MAC_AP_GTKS)
		return -EINVAL;
	WRITE_ONCE(mh->ap.gtk_tx, index);
	return 0;
}

void rtw_linux_ap_set_exemptions (const struct rtw_exemption *list, uint32_t n)
{
	mac_exempt_set(MAC_PORT_AP, list, n);
}

/* SER (key.c linuxkpi_ieee80211_iterate_keys): rtw89 drops the GO's keys from its CAM; they must never be used
   again. Returns whether there were any. wiphy->mtx held or a read-side section. */
bool mac_ap_iterate_keys (struct mac_hw *mh, struct ieee80211_vif *vif,
	void (*fn)(struct ieee80211_hw *, struct ieee80211_vif *, struct ieee80211_sta *, struct ieee80211_key_conf *,
		   void *), void *arg)
{
	struct mac_ap *ap = &mh->ap;
	struct ieee80211_vif *gv;
	struct mac_sta *ms;
	struct mac_key *k;
	bool any = false;

	if (!ap->vif || (vif && vif != &ap->vif->vif))
		return false;
	gv = &ap->vif->vif;
	rcu_read_lock();
	list_for_each_entry_rcu(ms, &mh->sta_list, list) {
		if (!ms->ap_client)
			continue;
		k = rcu_dereference(ms->ap.ptk);
		if (k && k->hw && !READ_ONCE(k->lost)) {
			fn(&mh->hw, gv, &ms->sta, &k->conf, arg);
			WRITE_ONCE(k->lost, true);
			any = true;
		}
	}
	for (int i = 0; i < MAC_AP_GTKS; i++) {
		k = rcu_dereference(ap->gtk[i]);
		if (k && k->hw && !READ_ONCE(k->lost)) {
			fn(&mh->hw, gv, NULL, &k->conf, arg);
			WRITE_ONCE(k->lost, true);
			any = true;
		}
	}
	rcu_read_unlock();
	return any;
}

/* ---- power save ---- */
/* The client's PM bit changed (rx.c via mac_ap_rx, or rtw89's PWR_INT_NOTIFY): any context but inside the GO's
   lifetime (read-side section or wiphy->mtx). */
static void ap_ps_set (struct mac_hw *mh, struct mac_sta *ms, bool sleep)
{
	struct mac_ap *ap = &mh->ap;
	bool work = false;

	if (sleep == READ_ONCE(ms->ap.ps))
		return;
	spin_lock_bh(&ap->ps_lock);
	if (sleep != ms->ap.ps) {
		ms->ap.ps = sleep;
		if (sleep) {
			atomic_inc(&ap->n_ps);
		} else {
			atomic_dec(&ap->n_ps);
			if (!skb_queue_empty(&ms->ap.ps_q)) {
				ms->ap.ps_flush = true;
				work = true;
			}
		}
	}
	spin_unlock_bh(&ap->ps_lock);
	if (work) {
		set_bit(MAC_APC_F_TIM, &ms->ap.flags);
		wiphy_work_queue(mh->hw.wiphy, &ap->work);
	}
}

void linuxkpi_ieee80211_sta_ps_transition (struct ieee80211_sta *sta, bool sleeping)
{
	struct mac_sta *ms = container_of(sta, struct mac_sta, sta);

	if (ms->ap_client && ms->mv)
		ap_ps_set(ms->mv->mh, ms, sleeping);
}

/* hands a frame built by the data path to the driver; rcu_read_lock held */
static void ap_tx_hand (struct mac_hw *mh, struct mac_sta *ms, struct sk_buff *skb)
{
	struct ieee80211_tx_control ctl = { 0 };

	ctl.sta = ms ? &ms->sta : NULL;
	/* counted the way mac_ap_tx_done uncounts it (a buffered Null probe is tagged like a management frame) */
	if (skb->mac_tag & MAC_SKB_MGMT)
		atomic_inc(&mh->ap.mgmt_inflight);
	else
		atomic_inc(&mh->ap.tx_inflight);
	mh->ops->tx(&mh->hw, &ctl, skb);        /* a failure comes back through free_txskb */
}

static void ap_ps_run (struct mac_hw *mh, struct mac_sta *ms);

/* A From DS Null frame to the client (A2 = A3 = BSSID), tagged like a management frame (mac_ap_tx_done); with a
   tx status when cookie is not 0. wiphy->mtx held. */
static struct sk_buff *ap_null_frame (struct mac_hw *mh, struct mac_sta *ms, u16 cookie)
{
	struct mac_ap *ap = &mh->ap;
	struct ieee80211_tx_info *info;
	struct ieee80211_vif *vif = &ap->vif->vif;
	struct sk_buff *skb;
	u8 *p;

	skb = dev_alloc_skb(mh->hw.extra_tx_headroom + MAC_HDR_LEN);
	if (!skb)
		return NULL;
	skb_reserve(skb, mh->hw.extra_tx_headroom);
	p = skb_put_zero(skb, MAC_HDR_LEN);
	put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_FC0_SUBTYPE_NODATA | IEEE80211_FCTL_FROMDS, p);
	ether_addr_copy(p + 4, ms->sta.addr);
	ether_addr_copy(p + 10, ap->bss.bssid);
	ether_addr_copy(p + 16, ap->bss.bssid);
	memset(skb->cb, 0, sizeof(skb->cb));
	info = IEEE80211_SKB_CB(skb);
	info->flags = cookie ? IEEE80211_TX_CTL_REQ_TX_STATUS : 0;
	info->band = ap->chan->band;
	info->hw_queue = vif->hw_queue[IEEE80211_AC_VO];
	info->control.vif = vif;
	skb->priority = 7;
	skb_set_queue_mapping(skb, IEEE80211_AC_VO);
	skb->mac_tag = MAC_SKB_AP | MAC_SKB_MGMT | cookie;
	return skb;
}

/* hostapd's check of an inactive station: a Null frame with a tx status (buffered while it sleeps: the TIM wakes it,
   and whatever it sends counts as well). wiphy->mtx held. */
static void ap_probe (struct mac_hw *mh, struct mac_sta *ms)
{
	struct mac_ap *ap = &mh->ap;
	struct sk_buff *skb, *old = NULL;
	bool buffered = false;
	u16 cookie;

	cookie = ++ap->cookie ? ap->cookie : ++ap->cookie;
	skb = ap_null_frame(mh, ms, cookie);
	if (!skb)
		return;
	WRITE_ONCE(ms->ap.probe_cookie, cookie);
	spin_lock_bh(&ap->ps_lock);
	if (ms->ap.ps || ms->ap.ps_flush) {
		if (skb_queue_len(&ms->ap.ps_q) >= AP_PS_Q_MAX)
			old = __skb_dequeue(&ms->ap.ps_q);
		if (skb_queue_empty(&ms->ap.ps_q))
			ms->ap.ps_ns = ktime_get_ns();
		__skb_queue_tail(&ms->ap.ps_q, skb);
		buffered = true;
	}
	spin_unlock_bh(&ap->ps_lock);
	kfree_skb(old);
	if (buffered) {
		set_bit(MAC_APC_F_TIM, &ms->ap.flags);
		ap_ps_run(mh, ms);
		return;
	}
	rcu_read_lock();
	ap_tx_hand(mh, ms, skb);
	rcu_read_unlock();
}

/* The AP work: an awake client's buffer goes out (unless it falls asleep again meanwhile), a PS-Poll releases one
   frame with More Data while more wait, and the TIM bits follow the buffers. wiphy->mtx held. */
static void ap_ps_run (struct mac_hw *mh, struct mac_sta *ms)
{
	struct mac_ap *ap = &mh->ap;
	struct sk_buff *skb;
	bool more, want, asleep;

	if (test_and_clear_bit(MAC_APC_F_PSPOLL, &ms->ap.flags)) {
		spin_lock_bh(&ap->ps_lock);
		asleep = ms->ap.ps;
		skb = asleep ? __skb_dequeue(&ms->ap.ps_q) : NULL;
		more = !skb_queue_empty(&ms->ap.ps_q);
		if (skb)
			ms->ap.ps_ns = ktime_get_ns();
		spin_unlock_bh(&ap->ps_lock);
		if (skb) {
			if (more)
				put_unaligned_le16(get_unaligned_le16(skb->data) | IEEE80211_FCTL_MOREDATA, skb->data);
			rcu_read_lock();
			ap_tx_hand(mh, ms, skb);
			rcu_read_unlock();
			AP_COUNT(ps_released);
			set_bit(MAC_APC_F_TIM, &ms->ap.flags);
		} else if (asleep) {
			/* nothing buffered (expired after the TIM bit was seen): mac80211 answers with a Null frame
			   (ieee80211_sta_ps_deliver_response -> ieee80211_send_null_response), so the client need not
			   stay awake until it gives up; the TIM bit goes as well */
			skb = ap_null_frame(mh, ms, 0);
			if (skb) {
				rcu_read_lock();
				ap_tx_hand(mh, ms, skb);
				rcu_read_unlock();
			}
			set_bit(MAC_APC_F_TIM, &ms->ap.flags);
		}
	}
	for (;;) {
		spin_lock_bh(&ap->ps_lock);
		skb = NULL;
		if (ms->ap.ps_flush) {
			if (!ms->ap.ps)
				skb = __skb_dequeue(&ms->ap.ps_q);
			if (!skb)
				ms->ap.ps_flush = false;
		}
		spin_unlock_bh(&ap->ps_lock);
		if (!skb)
			break;
		rcu_read_lock();
		ap_tx_hand(mh, ms, skb);
		rcu_read_unlock();
		AP_COUNT(ps_released);
	}
	if (test_and_clear_bit(MAC_APC_F_TIM, &ms->ap.flags)) {
		want = READ_ONCE(ms->ap.ps) && skb_queue_len_lockless(&ms->ap.ps_q);
		if (want != ms->ap.tim && mh->ops->set_tim) {
			ms->ap.tim = want;
			mh->ops->set_tim(&mh->hw, &ms->sta, want);      /* rtw89 fetches the beacon again */
		}
	}
}

/* ---- the AP work and timer ---- */
static void ap_class3_run (struct mac_hw *mh)
{
	struct mac_ap *ap = &mh->ap;
	u8 list[4][ETH_ALEN];
	u64 now = ktime_get_ns();
	u8 n, i;

	spin_lock_bh(&ap->ps_lock);
	n = ap->n_class3;
	memcpy(list, ap->class3, sizeof(list));
	ap->n_class3 = 0;
	spin_unlock_bh(&ap->ps_lock);
	for (i = 0; i < n; i++) {
		if (ap_find(mh, list[i]))
			continue;               /* it authenticated meanwhile */
		if (ether_addr_equal(list[i], ap->class3_last) &&
		    now - ap->class3_ns < (u64)AP_CLASS3_GAP_MS * NSEC_PER_MSEC)
			continue;
		ether_addr_copy(ap->class3_last, list[i]);
		ap->class3_ns = now;
		AP_COUNT(class3);
		ap_send_deauth(mh, list[i], NULL, REASON_CLASS3);
	}
}

static void ap_run (struct mac_hw *mh)
{
	struct mac_ap *ap = &mh->ap;
	struct mac_sta *ms, *next;
	struct sk_buff *skb;
	u8 txs;

	lockdep_assert_wiphy(mh->hw.wiphy);
	if (READ_ONCE(mh->restarting) || !ap->running) {
		skb_queue_purge(&ap->rxq);
		return;
	}
	while (ap->running && (skb = skb_dequeue(&ap->rxq))) {
		ap_rx_mgmt(mh, skb);
		kfree_skb(skb);
	}
	if (!ap->running)
		return;
	ap_class3_run(mh);
	/* the clients: association response status, power save (a client may go while we look: take the next one
	   first; only the current one is ever removed here) */
	list_for_each_entry_safe(ms, next, &mh->sta_list, list) {
		if (!ms->ap_client)
			continue;
		if (ms->ap.state == MAC_APC_RESP) {
			txs = READ_ONCE(ms->ap.resp_txs);
			if (txs)
				ap_resp_done(mh, ms, txs == 1);
		}
		ap_ps_run(mh, ms);
	}
}

static void ap_work (struct wiphy *wiphy, struct wiphy_work *work)
{
	(void)wiphy;
	ap_run(container_of(work, struct mac_hw, ap.work));
}

/* once a second: overdue clients, the response nobody acknowledged, old buffered frames */
static void ap_tick (struct mac_hw *mh)
{
	struct mac_ap *ap = &mh->ap;
	u64 now = ktime_get_ns();
	struct mac_sta *ms;
	bool again;

	do {
		again = false;
		list_for_each_entry(ms, &mh->sta_list, list) {
			u64 age;

			if (!ms->ap_client)
				continue;
			age = now - ms->ap.since_ns;
			if ((ms->ap.state == MAC_APC_AUTH || ms->ap.state == MAC_APC_WAIT_OS) &&
			    age > (u64)AP_PENDING_MS * NSEC_PER_MSEC) {
				pr_info("mac: GO: %pM never completed its association; forgotten", ms->sta.addr);
				ap_client_remove(mh, ms, true, REASON_PREV_AUTH_NOT_VALID, false, 0, 0, NULL, 0);
				again = true;
				break;
			}
			if (ms->ap.state == MAC_APC_RESP && age > (u64)AP_RESP_WAIT_MS * NSEC_PER_MSEC) {
				ap_resp_done(mh, ms, false);
				continue;
			}
			if (ms->ap.state != MAC_APC_ASSOC)
				continue;
			if ((ap->flags & RTW_AP_F_RSN) && ms->state < IEEE80211_STA_AUTHORIZED &&
			    age > (u64)AP_UNAUTH_MS * NSEC_PER_MSEC) {
				AP_COUNT(unauthorized);
				pr_info("mac: GO: %pM got no pairwise key in %u s; deauthenticated", ms->sta.addr,
					AP_UNAUTH_MS / 1000);
				ap_client_remove(mh, ms, true, REASON_4WAY_TIMEOUT, true, RTW_LOST_UNREACHABLE,
						 REASON_4WAY_TIMEOUT, NULL, 0);
				again = true;
				break;
			}
			/* inactive: a Null frame first (hostapd ap_max_inactivity); still nothing after AP_PROBE_WAIT_MS
			   (no acknowledgement, no frame): gone */
			if (now - READ_ONCE(ms->ap.last_rx_ns) <= (u64)AP_INACTIVE_MS * NSEC_PER_MSEC) {
				ms->ap.probe_ns = 0;
			} else if (!ms->ap.probe_ns) {
				ms->ap.probe_ns = now;
				ap_probe(mh, ms);
			} else if (now - ms->ap.probe_ns > (u64)AP_PROBE_WAIT_MS * NSEC_PER_MSEC) {
				AP_COUNT(inactive);
				pr_info("mac: GO: %pM inactive for %u s and no answer to a Null frame; deauthenticated",
					ms->sta.addr, AP_INACTIVE_MS / 1000);
				ap_client_remove(mh, ms, true, REASON_INACTIVITY, true, RTW_LOST_UNREACHABLE,
						 REASON_INACTIVITY, NULL, 0);
				again = true;
				break;
			}
			/* a sleeping client's buffer not served for a while goes */
			if (skb_queue_len_lockless(&ms->ap.ps_q) &&
			    now - READ_ONCE(ms->ap.ps_ns) > (u64)AP_PS_TTL_MS * NSEC_PER_MSEC) {
				u32 n = skb_queue_len_lockless(&ms->ap.ps_q);

				ap_ps_purge(ap, ms);
				__atomic_fetch_add(&ap_st.ps_dropped, n, __ATOMIC_RELAXED);
				set_bit(MAC_APC_F_TIM, &ms->ap.flags);
				ap_ps_run(mh, ms);
			}
		}
	} while (again && ap->running);
}

static void ap_timer (struct wiphy *wiphy, struct wiphy_work *work)
{
	struct mac_hw *mh = container_of(work, struct mac_hw, ap.timer.work);

	(void)wiphy;
	if (READ_ONCE(mh->restarting) || !mh->ap.running)
		return;
	ap_tick(mh);
	if (mh->ap.running)
		wiphy_delayed_work_queue(wiphy, &mh->ap.timer, msecs_to_jiffies(AP_TICK_MS));
}

/* ---- received frames (rx.c, NAPI thread) ---- */
static void ap_rxq (struct mac_hw *mh, struct sk_buff *skb, bool probe)
{
	struct mac_ap *ap = &mh->ap;
	u32 n = skb_queue_len_lockless(&ap->rxq);

	if (n >= AP_RXQ_MAX || (probe && n >= AP_RXQ_PROBE_MAX)) {
		kfree_skb(skb);
		return;
	}
	skb_queue_tail(&ap->rxq, skb);
	wiphy_work_queue(mh->hw.wiphy, &ap->work);
}

/* data from a station that is not our client: deauthenticate it (the AP work, rate limited) */
static void ap_class3 (struct mac_hw *mh, const u8 *sa)
{
	struct mac_ap *ap = &mh->ap;
	bool queue = false;
	u8 i;

	spin_lock_bh(&ap->ps_lock);
	for (i = 0; i < ap->n_class3 && !ether_addr_equal(ap->class3[i], sa); i++)
		;
	if (i == ap->n_class3 && i < ARRAY_SIZE(ap->class3)) {
		ether_addr_copy(ap->class3[ap->n_class3++], sa);
		queue = true;
	}
	spin_unlock_bh(&ap->ps_lock);
	if (queue)
		wiphy_work_queue(mh->hw.wiphy, &ap->work);
}

/* One MSDU for the OS: 24-byte header with To DS (A1 = BSSID, A2 = SA, A3 = DA), then the body (LLC/SNAP...). */
static void ap_fifo_put (struct mac_hw *mh, const u8 *sa, const u8 *da, __le16 seq_ctrl, const u8 *body,
	unsigned int len, u32 info)
{
	struct mac_ap *ap = &mh->ap;
	struct sk_buff *skb;
	u8 *p;

	if (len > MAC_FRAME_MAX || skb_queue_len_lockless(&ap->rx_fifo) >= AP_RX_FIFO_MAX) {
		AP_COUNT(rx_dropped);
		return;
	}
	skb = alloc_skb(MAC_HDR_LEN + len, GFP_ATOMIC);
	if (!skb) {
		AP_COUNT(rx_dropped);
		return;
	}
	p = skb_put_zero(skb, MAC_HDR_LEN);
	put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA | IEEE80211_FCTL_TODS, p);
	ether_addr_copy(p + 4, ap->bss.bssid);
	ether_addr_copy(p + 10, sa);
	ether_addr_copy(p + 16, da);
	memcpy(p + 22, &seq_ctrl, 2);
	skb_put_data(skb, body, len);
	skb->mac_tag = info;
	skb_queue_tail(&ap->rx_fifo, skb);
}

/*
 * Hands one received data frame of a client to the OS (rx.c mac_rx_deliver for the GO): its pairwise key and PN check
 * or the GO port's exemption rules, then the MSDU(s) into the FIFO. Takes the skb. The caller holds rcu_read_lock()
 * and, for a TID with a block ack session, mac_hw.rx_lock (agg.c), so each PN slot has one writer.
 */
void mac_ap_rx_deliver (struct mac_hw *mh, struct sk_buff *skb)
{
	struct mac_ap *ap = &mh->ap;
	const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)skb->data;
	const struct ieee80211_rx_status *rs = IEEE80211_SKB_RXCB(skb);
	u16 fc = le16_to_cpu(hdr->frame_control);
	__le16 seq_ctrl = hdr->seq_ctrl;
	unsigned int hdrlen, slot, len;
	struct mac_sta *ms;
	struct mac_key *k;
	bool qos, amsdu = false;
	const u8 *body;
	u32 info = 0;
	u16 qc = 0;
	int action;

	ms = ap_find(mh, hdr->addr2);
	hdrlen = ieee80211_hdrlen(hdr->frame_control);
	qos = ieee80211_is_data_qos(hdr->frame_control);
	if (!ms || hdrlen < MAC_HDR_LEN || skb->len < hdrlen) {
		AP_COUNT(rx_dropped);
		goto drop;
	}
	if (qos) {
		qc = get_unaligned_le16(skb->data + MAC_HDR_LEN);
		amsdu = qc & IEEE80211_QOS_CTL_A_MSDU_PRESENT;
	}
	slot = qos ? (qc & IEEE80211_QOS_CTL_TID_MASK) : MAC_SEQ_NONQOS;
	if (slot > MAC_SEQ_NONQOS)
		slot = MAC_SEQ_NONQOS;
	body = skb->data + hdrlen;
	len = skb->len - hdrlen;
	if (fc & IEEE80211_FCTL_PROTECTED) {
		u8 keyid;
		u64 pn;

		if (!(rs->flag & RX_FLAG_DECRYPTED) || len < IEEE80211_CCMP_HDR_LEN + IEEE80211_CCMP_MIC_LEN ||
		    !(body[3] & 0x20)) {
			AP_COUNT(rx_dropped);
			goto drop;
		}
		keyid = body[3] >> 6;
		k = rcu_dereference(ms->ap.ptk);
		if (!k || k->conf.keyidx != keyid) {
			AP_COUNT(rx_dropped);
			goto drop;
		}
		pn = (u64)body[0] | (u64)body[1] << 8 | (u64)body[4] << 16 | (u64)body[5] << 24 | (u64)body[6] << 32 |
		     (u64)body[7] << 40;
		if (pn <= k->rx_pn[slot]) {
			AP_COUNT(rx_dropped_replay);
			goto drop;
		}
		k->rx_pn[slot] = pn;
		AP_COUNT(rx_decrypted);
		body += IEEE80211_CCMP_HDR_LEN;
		len -= IEEE80211_CCMP_HDR_LEN + IEEE80211_CCMP_MIC_LEN;
		info = RTW_RX_PROTECTED;
	} else if (ap->flags & RTW_AP_F_RSN) {
		/* in clear: ALWAYS exempt, ON_KEY_MAPPING_KEY_UNAVAILABLE while the client has no pairwise key (EAPOL of
		   the 4-way handshake), nothing else. The OS starts a WPA2 GO with ExcludeUnencrypted FALSE (observed on
		   Windows 11 26100), but other data in clear comes from a client before its handshake or after it has a
		   key, which is what a stranger sends: mac80211 drops it as well (ieee80211_frame_allowed: 802.1X port
		   control, then ieee80211_drop_unencrypted) */
		action = amsdu ? -1 : mac_exempt_lookup(MAC_PORT_AP, ap_ethertype(body, len),
							   is_multicast_ether_addr(hdr->addr3));  /* To DS: A3 = DA */
		if (action == RTW_EXEMPT_ALWAYS || (action == RTW_EXEMPT_NO_KEY && !rcu_dereference(ms->ap.ptk))) {
			info = RTW_RX_EXEMPT | (u32)(action < 0 ? RTW_EXEMPT_NONE : action) << 4;
		} else {
			AP_COUNT(rx_dropped_unencrypted);
			goto drop;
		}
	}
	if (!amsdu) {
		if (ap_ethertype(body, len) == ETH_P_PAE && !ap_pae_da_ok(ap, hdr->addr3)) {
			AP_COUNT(rx_dropped);
			goto drop;
		}
		ap_fifo_put(mh, hdr->addr2, hdr->addr3, seq_ctrl, body, len, info);
	} else {
		if (!ap_amsdu_ok(ap, hdr->addr2, body, len)) {
			AP_COUNT(rx_dropped);
			goto drop;
		}
		/* A-MSDU subframes: DA, SA, length (big endian), MSDU, padded to 4 bytes but the last */
		while (len >= 14) {
			unsigned int sub = get_unaligned_be16(body + 12), total = 14 + sub;

			if (sub > len - 14)
				break;
			ap_fifo_put(mh, body + 6, body, seq_ctrl, body + 14, sub, info);
			if (total < len)
				total = ALIGN(total, 4);
			if (total >= len)
				break;
			body += total;
			len -= total;
		}
	}
	ms->ap.rx_data++;
	AP_COUNT(rx_data);
drop:
	kfree_skb(skb);
}

/* a data frame for our BSSID (FCS removed). Takes the skb. */
static void ap_rx_data (struct mac_hw *mh, struct sk_buff *skb)
{
	const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)skb->data;
	u16 fc = le16_to_cpu(hdr->frame_control);
	__le16 seq_ctrl = hdr->seq_ctrl;
	unsigned int hdrlen, slot, tid;
	struct mac_sta *ms;
	bool qos;
	u16 qc = 0;

	rcu_read_lock();
	ms = ap_find(mh, hdr->addr2);
	if (!ms) {
		/* class 3 frame from a station that is not authenticated */
		if (is_valid_ether_addr(hdr->addr2))
			ap_class3(mh, hdr->addr2);
		AP_COUNT(rx_dropped);
		goto drop;
	}
	if (READ_ONCE(ms->ap.state) < MAC_APC_RESP || (fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) !=
	    IEEE80211_FCTL_TODS) {
		AP_COUNT(rx_dropped);
		goto drop;
	}
	WRITE_ONCE(ms->ap.last_rx_ns, ktime_get_ns());
	ap_ps_set(mh, ms, fc & IEEE80211_FCTL_PM);
	if (fc & FC_STYPE_NODATA)
		goto drop;                      /* Null, QoS Null: only the PM bit counts */
	hdrlen = ieee80211_hdrlen(hdr->frame_control);
	qos = ieee80211_is_data_qos(hdr->frame_control);
	if (hdrlen < MAC_HDR_LEN || skb->len < hdrlen || (fc & IEEE80211_FCTL_MOREFRAGS) ||
	    (le16_to_cpu(seq_ctrl) & IEEE80211_SCTL_FRAG)) {
		AP_COUNT(rx_dropped);
		goto drop;
	}
	if (qos)
		qc = get_unaligned_le16(skb->data + MAC_HDR_LEN);
	tid = qos ? (qc & IEEE80211_QOS_CTL_TID_MASK) : IEEE80211_NUM_TIDS;
	slot = qos ? tid : MAC_SEQ_NONQOS;
	if (slot < ARRAY_SIZE(ms->rx_seq)) {
		if ((fc & (IEEE80211_FC1_RETRY << 8)) && ms->rx_seq[slot] == le16_to_cpu(seq_ctrl)) {
			AP_COUNT(rx_dup);
			goto drop;
		}
		ms->rx_seq[slot] = le16_to_cpu(seq_ctrl);
	}
	if (qos && tid < MAC_BA_TIDS && mac_agg_rx_frame(mh, ms, tid, skb)) {
		rcu_read_unlock();              /* agg.c took it (held or delivered) and signals */
		return;
	}
	mac_ap_rx_deliver(mh, skb);
	rcu_read_unlock();
	mac_ap_dp_rx_ready();
	return;
drop:
	rcu_read_unlock();
	kfree_skb(skb);
}

/* rx.c, NAPI thread, while the GO runs: takes the frames for our BSS (true), leaves the rest to the station's path. */
bool mac_ap_rx (struct mac_hw *mh, struct sk_buff *skb)
{
	struct mac_ap *ap = &mh->ap;
	const u8 *f = skb->data;
	const u8 *bssid = ap->bss.bssid;
	u16 fc = get_unaligned_le16(f);
	u16 ftype = fc & IEEE80211_FCTL_FTYPE, stype = fc & IEEE80211_FCTL_STYPE;
	struct mac_sta *ms;

	switch (ftype) {
	case FTYPE_MGMT:
		if (stype == STYPE_PROBE_REQ) {
			if ((is_broadcast_ether_addr(f + 4) || ether_addr_equal(f + 4, bssid)) &&
			    (is_broadcast_ether_addr(f + 16) || ether_addr_equal(f + 16, bssid))) {
				ap_rxq(mh, skb, true);
				return true;
			}
			return false;
		}
		if (!ether_addr_equal(f + 4, bssid) || stype == STYPE_BEACON || stype == STYPE_PROBE_RESP)
			return false;
		rcu_read_lock();
		ms = ap_find(mh, f + 10);
		if (ms)
			WRITE_ONCE(ms->ap.last_rx_ns, ktime_get_ns());
		rcu_read_unlock();
		ap_rxq(mh, skb, false);
		return true;
	case FTYPE_CTL:
		if (skb->len < 16 || !ether_addr_equal(f + 4, bssid))
			return false;
		rcu_read_lock();
		ms = ap_find(mh, f + 10);
		if (ms && stype == STYPE_CTL_PSPOLL) {
			AP_COUNT(pspoll);
			WRITE_ONCE(ms->ap.last_rx_ns, ktime_get_ns());
			set_bit(MAC_APC_F_PSPOLL, &ms->ap.flags);
			wiphy_work_queue(mh->hw.wiphy, &ap->work);
		} else if (ms && stype == STYPE_CTL_BAR && READ_ONCE(ms->ap.state) == MAC_APC_ASSOC) {
			mac_agg_rx_bar_sta(mh, ms, f, skb->len);
		}
		rcu_read_unlock();
		kfree_skb(skb);
		return true;
	case FTYPE_DATA:
		if (!ether_addr_equal(f + 4, bssid))
			return false;
		ap_rx_data(mh, skb);
		return true;
	default:
		return false;
	}
}

/* ---- the data path (the front end's TX queue of the role port, IRQL <= DISPATCH_LEVEL) ---- */
void rtw_linux_ap_dp_attach (const struct rtw_dp_ops *ops)
{
	if (ops) {
		WRITE_ONCE(ap_dp_on, false);
		synchronize_rcu();
		ap_dp_ops = *ops;
		smp_wmb();
		WRITE_ONCE(ap_dp_on, true);
		return;
	}
	WRITE_ONCE(ap_dp_on, false);
	synchronize_rcu();              /* no callback runs any more */
	memset(&ap_dp_ops, 0, sizeof(ap_dp_ops));
}

void mac_ap_dp_tx_ready (void)
{
	rcu_read_lock();
	if (READ_ONCE(ap_dp_on) && ap_dp_ops.tx_ready)
		ap_dp_ops.tx_ready(ap_dp_ops.ctx);
	rcu_read_unlock();
}

void mac_ap_dp_rx_ready (void)
{
	rcu_read_lock();
	if (READ_ONCE(ap_dp_on) && ap_dp_ops.rx_ready)
		ap_dp_ops.rx_ready(ap_dp_ops.ctx);
	rcu_read_unlock();
}

int rtw_linux_ap_tx_ready (void)
{
	struct mac_hw *mh = mac_the_hw;

	if (!mh || !READ_ONCE(mh->ap.running) || READ_ONCE(mh->restarting))
		return -ENOTCONN;
	if (READ_ONCE(mh->queues_stopped) || atomic_read(&mh->ap.tx_inflight) >= AP_TX_INFLIGHT_MAX)
		return -EBUSY;
	return 0;
}

void rtw_linux_ap_tx_drop (uint32_t n)
{
	__atomic_fetch_add(&ap_st.tx_dropped, n, __ATOMIC_RELAXED);
}

/* copies len bytes from offset off of the fragment list (tx.c frags_copy) */
static void ap_frags_copy (const struct rtw_frag *frags, u32 n, u32 off, u8 *to, u32 len)
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

/* How a frame goes out on an RSN GO (key.c mac_key_tx for the GO port): *key = the client's pairwise key (unicast)
   or the group key, NULL = in clear. 0, or -EACCES (must be protected, no key). rcu_read_lock held. */
static int ap_key_tx (struct mac_ap *ap, struct mac_sta *ms, u16 ethertype, bool group, u32 flags,
	struct mac_key **key)
{
	struct mac_key *k;
	int action;

	*key = NULL;
	if (!(ap->flags & RTW_AP_F_RSN))
		return 0;
	action = (flags & RTW_TX_EXEMPT_VALID) ? (int)RTW_TX_EXEMPT_GET(flags) : -1;
	if (action <= RTW_EXEMPT_NONE || action > RTW_EXEMPT_NO_KEY)
		action = mac_exempt_lookup(MAC_PORT_AP, ethertype, group);
	k = group ? rcu_dereference(ap->gtk[READ_ONCE(ap->gtk_tx) & (MAC_AP_GTKS - 1)]) : rcu_dereference(ms->ap.ptk);
	if (k && READ_ONCE(k->lost))
		k = NULL;
	switch (action) {
	case RTW_EXEMPT_ALWAYS:
		return 0;
	case RTW_EXEMPT_NO_KEY:
		*key = k;
		return 0;
	default:
		/* never in clear on an RSN GO, whatever ExcludeUnencrypted says (the receive side, mac_ap_rx_deliver) */
		*key = k;
		return k ? 0 : -EACCES;
	}
}

/*
 * A native 802.11 data frame from the OS for the GO's BSS (From DS: A1 = DA, A2 = BSSID, A3 = SA; or without DS bits:
 * A2 = SA, A3 = BSSID) becomes a From DS (QoS) data frame to the client (DA) or to the group: sequence number, CCMP
 * header with the client's pairwise key or the group key, like tx.c rtw_linux_tx_data for the station. A sleeping
 * client's frames wait in its buffer; group frames go after the DTIM while any client sleeps.
 */
int rtw_linux_ap_tx_data (const struct rtw_frag *frags, uint32_t n, uint8_t prio, uint32_t flags)
{
	struct mac_hw *mh = mac_the_hw;
	struct ieee80211_tx_info *info;
	struct ieee80211_vif *vif;
	struct mac_sta *ms = NULL;
	struct sk_buff *skb, *old = NULL;
	struct mac_key *key;
	struct mac_ap *ap;
	u8 hdr[MAC_HDR_LEN], snap[8], da[ETH_ALEN], sa[ETH_ALEN];
	u32 total = 0, body, slot;
	u16 fc, seq, ethertype = 0;
	u8 tid, ac, qos_len;
	bool qos, group, buffered = false, first = false;
	u8 *p;

	for (u32 i = 0; i < n; i++)
		total += frags[i].len;
	if (!mh)
		return -ENOTCONN;
	ap = &mh->ap;
	if (!n || total < MAC_HDR_LEN + 8 || total > MAC_HDR_LEN + MAC_FRAME_MAX) {
		AP_COUNT(tx_dropped);
		return -EINVAL;
	}
	rcu_read_lock();
	vif = ap->vif ? &ap->vif->vif : NULL;
	if (!READ_ONCE(ap->running) || !vif || READ_ONCE(mh->restarting)) {
		rcu_read_unlock();
		AP_COUNT(tx_dropped);
		return -ENOTCONN;
	}
	if (READ_ONCE(mh->queues_stopped) || atomic_read(&ap->tx_inflight) >= AP_TX_INFLIGHT_MAX) {
		rcu_read_unlock();
		return -EBUSY;
	}
	ap_frags_copy(frags, n, 0, hdr, MAC_HDR_LEN);
	fc = get_unaligned_le16(hdr);
	if ((fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) != (IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA) ||
	    (fc & IEEE80211_FCTL_MOREFRAGS))
		goto bad;
	if ((fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) == IEEE80211_FCTL_FROMDS &&
	    ether_addr_equal(hdr + 10, ap->bss.bssid)) {
		ether_addr_copy(sa, hdr + 16);
	} else if (!(fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) && ether_addr_equal(hdr + 16, ap->bss.bssid)) {
		ether_addr_copy(sa, hdr + 10);
	} else {
		pr_warn_once("mac: GO: TX frame of an unexpected form dropped (frame control %#06x)", fc);
		goto bad;
	}
	ether_addr_copy(da, hdr + 4);
	group = is_multicast_ether_addr(da);
	if (!group) {
		ms = ap_find(mh, da);
		if (!ms || READ_ONCE(ms->ap.state) < MAC_APC_RESP) {
			rcu_read_unlock();
			AP_COUNT(tx_dropped);
			return -ENOENT;
		}
	}
	ap_frags_copy(frags, n, MAC_HDR_LEN, snap, sizeof(snap));
	if (!memcmp(snap, ap_rfc1042, sizeof(ap_rfc1042)))
		ethertype = get_unaligned_be16(snap + 6);
	if (ethertype == ETH_P_PAE)
		flags |= RTW_TX_EAPOL;
	if (ap_key_tx(ap, ms, ethertype, group, flags, &key)) {
		rcu_read_unlock();
		AP_COUNT(tx_nokey);
		return -EACCES;
	}

	qos = ms && ms->sta.wme;
	tid = qos ? ((flags & RTW_TX_EAPOL) ? 7 : (prio & 7)) : 0;
	ac = ap_tid_to_ac[tid];
	qos_len = qos ? 2 : 0;
	body = MAC_HDR_LEN + qos_len + (key ? IEEE80211_CCMP_HDR_LEN : 0);
	skb = dev_alloc_skb(mh->hw.extra_tx_headroom + body + (total - MAC_HDR_LEN));
	if (!skb) {
		rcu_read_unlock();
		AP_COUNT(tx_dropped);
		return -ENOMEM;
	}
	skb_reserve(skb, mh->hw.extra_tx_headroom);
	p = skb_put(skb, body);
	put_unaligned_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_FROMDS |
			   (qos ? IEEE80211_STYPE_QOS_DATA : IEEE80211_STYPE_DATA) | (key ? IEEE80211_FCTL_PROTECTED : 0), p);
	put_unaligned_le16(0, p + 2);
	ether_addr_copy(p + 4, da);
	ether_addr_copy(p + 10, ap->bss.bssid);
	ether_addr_copy(p + 16, sa);
	if (ms) {
		slot = qos ? tid : MAC_SEQ_NONQOS;
		seq = ms->tx_seq[slot];
		ms->tx_seq[slot] = (seq + 1) & 0xfff;
	} else {
		seq = ap->mc_seq;
		ap->mc_seq = (seq + 1) & 0xfff;
	}
	put_unaligned_le16((u16)(seq << 4), p + 22);
	if (qos)
		put_unaligned_le16(tid, p + MAC_HDR_LEN);
	if (key)
		mac_key_ccmp_hdr(key, p + MAC_HDR_LEN + qos_len);
	ap_frags_copy(frags, n, MAC_HDR_LEN, skb_put(skb, total - MAC_HDR_LEN), total - MAC_HDR_LEN);
	mac_tx_set_protocol(skb, body);

	memset(skb->cb, 0, sizeof(skb->cb));
	info = IEEE80211_SKB_CB(skb);
	/* group frames: no ACK (mac80211 ieee80211_tx_prepare), after the DTIM while a client sleeps */
	info->flags = group ? IEEE80211_TX_CTL_NO_ACK | (atomic_read(&ap->n_ps) ? IEEE80211_TX_CTL_SEND_AFTER_DTIM : 0) : 0;
	info->band = ap->chan->band;
	info->hw_queue = vif->hw_queue[ac];
	info->control.vif = vif;
	info->control.hw_key = key ? &key->conf : NULL;        /* a buffered frame's key goes only after it (ap.c) */
	if (flags & RTW_TX_EAPOL)
		info->control.flags |= IEEE80211_TX_CTRL_PORT_CTRL_PROTO;
	skb->priority = tid;
	skb_set_queue_mapping(skb, ac);
	skb->mac_tag = MAC_SKB_AP | MAC_SKB_DATA;
	AP_COUNT(tx_data);
	if (ms) {
		ms->ap.tx_data++;
		/* a sleeping client (or one whose buffer still drains): its buffer */
		spin_lock_bh(&ap->ps_lock);
		if (ms->ap.ps || ms->ap.ps_flush) {
			if (skb_queue_len(&ms->ap.ps_q) >= AP_PS_Q_MAX)
				old = __skb_dequeue(&ms->ap.ps_q);
			first = skb_queue_empty(&ms->ap.ps_q);
			if (first)
				ms->ap.ps_ns = ktime_get_ns();
			__skb_queue_tail(&ms->ap.ps_q, skb);
			buffered = true;
		}
		spin_unlock_bh(&ap->ps_lock);
	}
	if (buffered) {
		AP_COUNT(ps_buffered);
		if (old) {
			kfree_skb(old);
			AP_COUNT(ps_dropped);
		}
		if (first) {
			set_bit(MAC_APC_F_TIM, &ms->ap.flags);
			wiphy_work_queue(mh->hw.wiphy, &ap->work);
		}
	} else {
		ap_tx_hand(mh, ms, skb);
	}
	rcu_read_unlock();
	return 0;
bad:
	rcu_read_unlock();
	AP_COUNT(tx_dropped);
	return -EINVAL;
}

uint32_t rtw_linux_ap_rx_data (void *buf, uint32_t cap, uint32_t *info)
{
	struct mac_hw *mh = mac_the_hw;
	struct sk_buff *skb;
	uint32_t len;

	*info = 0;
	if (!mh)
		return 0;
	if (!READ_ONCE(mh->ap.running)) {
		skb_queue_purge(&mh->ap.rx_fifo);
		return 0;
	}
	while ((skb = skb_dequeue(&mh->ap.rx_fifo))) {
		if (skb->len > cap) {
			AP_COUNT(rx_dropped);
			kfree_skb(skb);
			continue;
		}
		len = skb->len;
		memcpy(buf, skb->data, len);
		*info = skb->mac_tag;
		kfree_skb(skb);
		return len;
	}
	return 0;
}

uint32_t rtw_linux_ap_rx_pending (void)
{
	return mac_the_hw ? skb_queue_len_lockless(&mac_the_hw->ap.rx_fifo) : 0;
}

void rtw_linux_ap_rx_flush (void)
{
	if (mac_the_hw)
		skb_queue_purge(&mac_the_hw->ap.rx_fifo);
}

uint32_t rtw_linux_ap_clients (void)
{
	struct mac_hw *mh = mac_the_hw;

	return mh && READ_ONCE(mh->ap.running) ? ap_count_clients(mh, MAC_APC_ASSOC) : 0;
}

/* ---- start and stop ---- */
uint32_t rtw_linux_ap_channels (uint8_t band, uint8_t *out, uint32_t max)
{
	struct mac_hw *mh = mac_the_hw;
	const struct ieee80211_supported_band *sb;
	uint32_t n = 0;

	if (!mh || band > 1)
		return 0;
	wiphy_lock(mh->hw.wiphy);
	sb = mh->hw.wiphy->bands[band ? NL80211_BAND_5GHZ : NL80211_BAND_2GHZ];
	for (int i = 0; sb && i < sb->n_channels && n < max; i++)
		if (ap_chan_ok(&sb->channels[i]))
			out[n++] = (u8)sb->channels[i].hw_value;
	wiphy_unlock(mh->hw.wiphy);
	return n;
}

int rtw_linux_sta_channel (uint8_t *band, uint8_t *channel)
{
	struct mac_hw *mh = mac_the_hw;
	int ret = 0;

	if (!mh)
		return 0;
	wiphy_lock(mh->hw.wiphy);
	if (mh->conn.state != RTW_CONN_IDLE && mh->conn.ctx && mh->conn.chan) {
		*band = (u8)mh->conn.chan->band;
		*channel = (u8)mh->conn.chan->hw_value;
		ret = 1;
	}
	wiphy_unlock(mh->hw.wiphy);
	return ret;
}

/* hostapd's EDCA parameters for the AP's own transmissions (tx_queue_data0..3): VO AIFS 1, CWmin 3, CWmax 7, TXOP 1.5
   ms; VI 1, 7, 15, 3 ms; BE 3, 15, 63; BK 7, 15, 1023 */
static void ap_conf_tx (struct mac_hw *mh)
{
	static const struct { u8 aifs; u16 cw_min, cw_max, txop; } edca[IEEE80211_NUM_ACS] = {
		[IEEE80211_AC_VO] = { 1, 3, 7, 47 },
		[IEEE80211_AC_VI] = { 1, 7, 15, 94 },
		[IEEE80211_AC_BE] = { 3, 15, 63, 0 },
		[IEEE80211_AC_BK] = { 7, 15, 1023, 0 },
	};
	struct ieee80211_vif *vif = &mh->ap.vif->vif;

	for (int ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		struct ieee80211_tx_queue_params p = { 0 };

		p.aifs = edca[ac].aifs;
		p.cw_min = edca[ac].cw_min;
		p.cw_max = edca[ac].cw_max;
		p.txop = edca[ac].txop;
		mh->ops->conf_tx(&mh->hw, vif, 0, (u16)ac, &p);
	}
	vif->bss_conf.qos = true;
}

/* the GO's interface leaves the driver (and our lists); the caller frees mv after mac_sync_readers() */
static void ap_vif_remove (struct mac_hw *mh, struct mac_vif *mv)
{
	list_del_rcu(&mv->list);
	mac_sync_readers(mh);
	mh->ops->remove_interface(&mh->hw, &mv->vif);
	mh->ap.vif = NULL;
	mac_configure_filter(mh);
}

int rtw_linux_ap_start (const struct rtw_ap_start *req, rtw_ap_event_fn fn, void *ctx)
{
	struct mac_hw *mh = mac_the_hw;
	struct ieee80211_channel *chan;
	struct ieee80211_bss_conf *bc;
	struct mac_chanctx *cctx = NULL;
	struct ieee80211_vif *vif;
	struct rtw_ap_start r = *req;
	struct mac_vif *mv;
	struct mac_ap *ap;
	int ret = 0;

	if (!mh)
		return -ENODEV;
	if (!r.ssid_len || r.ssid_len > IEEE80211_MAX_SSID_LEN || !is_valid_ether_addr(r.addr) || r.band > 1)
		return -EINVAL;
	if (!r.beacon_int)
		r.beacon_int = 100;
	if (r.beacon_int < 20 || r.beacon_int > 1000)
		return -EINVAL;
	if (!r.dtim_period)
		r.dtim_period = 2;
	ap = &mh->ap;
	if (!mac_lock_driver(mh))
		ret = -EBUSY;
	else if (!mh->started || !mh->sta_vif)
		ret = -ENOTCONN;
	else if (ap->vif || mh->listen_ctx)
		ret = -EBUSY;
	if (ret)
		goto out;
	chan = mac_find_channel(mh->hw.wiphy, r.band ? NL80211_BAND_5GHZ : NL80211_BAND_2GHZ, r.channel);
	if (!ap_chan_ok(chan)) {
		ret = -ERANGE;
		goto out;
	}
	/* one channel at a time (no MCC): the station's, if it has one */
	if (mh->conn.ctx && mh->conn.ctx->conf.def.chan != chan) {
		ret = -EXDEV;
		goto out;
	}
	mac_scan_cancel(mh);                    /* rtw89 start_ap aborts a scan as well */

	/* the interface (cfg80211 brings an AP interface up first) */
	mv = mac_vif_alloc(mh, NL80211_IFTYPE_AP, r.addr);
	if (!mv) {
		ret = -ENOMEM;
		goto out;
	}
	ret = mh->ops->add_interface(&mh->hw, &mv->vif);
	if (ret) {
		pr_err("mac: GO: drv add_interface failed: %d", ret);
		kfree(mv);
		goto out;
	}
	smp_wmb();
	list_add_tail_rcu(&mv->list, &mh->vif_list);
	ap->vif = mv;
	vif = &mv->vif;
	bc = &vif->bss_conf;
	ap->chan = chan;
	ap->flags = r.flags;
	ap_bss_setup(mh, &r, chan);

	/* the channel (shared with the station's context when it has one) */
	ret = mac_chanctx_use(mh, mv, chan, ap->bss.ht ? NL80211_CHAN_HT20 : NL80211_CHAN_NO_HT, &cctx);
	if (ret) {
		pr_err("mac: GO: channel %u: %d", chan->hw_value, ret);
		goto err_vif;
	}
	ap->ctx = cctx;

	/* cfg.c ieee80211_start_ap: EDCA, link_conf, the beacon, drv_start_ap, then the changes */
	ap_conf_tx(mh);
	ether_addr_copy(mv->bssid, r.addr);
	memcpy(vif->cfg.ssid, r.ssid, r.ssid_len);
	vif->cfg.ssid_len = r.ssid_len;
	bc->beacon_int = r.beacon_int;
	bc->dtim_period = r.dtim_period;
	bc->enable_beacon = 1;
	bc->use_short_slot = true;              /* what our capability field says (apframe.c) */
	bc->use_short_preamble = chan->band == NL80211_BAND_2GHZ;
	bc->use_cts_prot = false;
	bc->ht_operation_mode = 0;
	bc->he_support = false;
	bc->eht_support = false;
	atomic_set(&ap->tx_inflight, 0);
	atomic_set(&ap->mgmt_inflight, 0);
	atomic_set(&ap->n_ps, 0);
	ap->aid_map = 0;
	ap->gtk_tx = 0;
	ap->mc_seq = 0;
	ap->n_class3 = 0;
	ap->event_fn = fn;
	ap->event_ctx = ctx;
	ret = mh->ops->start_ap(&mh->hw, vif, bc);
	if (ret) {
		pr_err("mac: GO: drv start_ap failed: %d", ret);
		goto err_ctx;
	}
	if (mh->ops->vif_cfg_changed)
		mh->ops->vif_cfg_changed(&mh->hw, vif, BSS_CHANGED_SSID);
	WRITE_ONCE(ap->running, true);          /* beacon_get_tim and rx.c may look now */
	mh->ops->link_info_changed(&mh->hw, vif, bc, BSS_CHANGED_BEACON_INT | BSS_CHANGED_BEACON_ENABLED |
				   BSS_CHANGED_BEACON | BSS_CHANGED_BASIC_RATES | BSS_CHANGED_ERP_SLOT |
				   BSS_CHANGED_ERP_PREAMBLE | BSS_CHANGED_ERP_CTS_PROT | BSS_CHANGED_HT | BSS_CHANGED_QOS);
	mac_configure_filter(mh);               /* probe requests */
	wiphy_delayed_work_queue(mh->hw.wiphy, &ap->timer, msecs_to_jiffies(AP_TICK_MS));
	AP_COUNT(starts);
	pr_info("mac: GO started: %pM, channel %u (%u MHz), %s, beacon %u TU, DTIM %u, %s%s%s", r.addr, chan->hw_value,
		chan->center_freq, ap->bss.ht ? "HT20" : "legacy", r.beacon_int, r.dtim_period,
		(r.flags & RTW_AP_F_RSN) ? "WPA2-PSK/CCMP" : "open",
		((r.flags & RTW_AP_F_11B) && chan->band == NL80211_BAND_2GHZ) ? ", 802.11b" : "",
		cctx->users > 1 ? ", channel shared with the station" : "");
	goto out;

err_ctx:
	ap->event_fn = NULL;
	ap->event_ctx = NULL;
	if (mac_chanctx_release(mh, mv, cctx)) {
		mac_sync_readers(mh);
		kfree(cctx);
	}
	ap->ctx = NULL;
err_vif:
	ap_vif_remove(mh, mv);
	kfree(mv);
	ap->chan = NULL;
out:
	wiphy_unlock(mh->hw.wiphy);
	return ret;
}

/* The GO's interface and clients park like the station's after a hardware restart (iface.c): rtw89 may still hold
   pointers to them. No driver calls. wiphy->mtx held. */
static void ap_park (struct mac_hw *mh)
{
	struct mac_ap *ap = &mh->ap;
	struct mac_vif *mv = ap->vif;
	struct mac_chanctx *cctx = ap->ctx;
	struct mac_sta *gone[2 * RTW_AP_MAX_CLIENTS];
	struct mac_key *k[MAC_AP_GTKS];
	struct mac_sta *ms;
	u32 n, i;

	WRITE_ONCE(ap->running, false);
	synchronize_rcu();
	wiphy_delayed_work_cancel(mh->hw.wiphy, &ap->timer);
	wiphy_work_cancel(mh->hw.wiphy, &ap->work);
	skb_queue_purge(&ap->rxq);
	/* the clients off the list; their list entries may be reused only once no reader can be on them */
	do {
		n = 0;
		list_for_each_entry(ms, &mh->sta_list, list)
			if (ms->ap_client && n < ARRAY_SIZE(gone))
				gone[n++] = ms;
		for (i = 0; i < n; i++)
			list_del_rcu(&gone[i]->list);
		synchronize_rcu();
		for (i = 0; i < n; i++) {
			struct mac_key *pk = gone[i]->ap.ptk;

			mac_agg_stop_client(mh, gone[i], false);
			gone[i]->ap.ptk = NULL;
			ap_ps_purge(ap, gone[i]);
			mac_key_free(pk);
			list_add_tail(&gone[i]->list, &mh->parked_sta);
		}
	} while (n == ARRAY_SIZE(gone));
	for (i = 0; i < MAC_AP_GTKS; i++) {
		k[i] = ap->gtk[i];
		rcu_assign_pointer(ap->gtk[i], NULL);
	}
	if (cctx && --cctx->users <= 0)
		list_del_rcu(&cctx->list);
	list_del_rcu(&mv->list);
	synchronize_rcu();
	for (i = 0; i < MAC_AP_GTKS; i++)
		mac_key_free(k[i]);
	if (cctx && cctx->users <= 0)
		list_add_tail(&cctx->list, &mh->parked_ctx);
	list_add_tail(&mv->list, &mh->parked_vif);
	ap->vif = NULL;
	ap->ctx = NULL;
	ap->aid_map = 0;
	atomic_set(&ap->n_ps, 0);
	skb_queue_purge(&ap->rx_fifo);
}

/*
 * cfg.c ieee80211_stop_ap for our GO, if it exists: a broadcast deauthentication (tx, the driver not being reset),
 * every client, the group keys, beacons off, drv_stop_ap, the channel (the station keeps a shared one), the interface.
 * No events. wiphy->mtx held; during a hardware restart the GO is parked instead (no driver calls).
 */
void mac_ap_stop_locked (struct mac_hw *mh, bool tx)
{
	struct mac_ap *ap = &mh->ap;
	struct ieee80211_hw *hw = &mh->hw;
	struct mac_vif *mv = ap->vif;
	struct mac_chanctx *cctx = ap->ctx;
	struct ieee80211_vif *vif;
	struct mac_key *k[MAC_AP_GTKS];
	struct mac_sta *ms;
	bool freed = false;

	lockdep_assert_wiphy(hw->wiphy);
	if (!mv)
		return;
	if (READ_ONCE(mh->restarting)) {
		pr_info("mac: GO stopped during a hardware restart: parked");
		ap_park(mh);
		AP_COUNT(stops);
		return;
	}
	vif = &mv->vif;
	if (tx && ap->running && vif->bss_conf.chanctx_conf) {
		ap_send_deauth(mh, (const u8 *)"\xff\xff\xff\xff\xff\xff", NULL, REASON_LEAVING);
		if (mh->ops->flush)
			mh->ops->flush(hw, vif, (u32)(BIT(hw->queues) - 1), false);
	}
	WRITE_ONCE(ap->running, false);
	synchronize_rcu();
	wiphy_delayed_work_cancel(hw->wiphy, &ap->timer);
	wiphy_work_cancel(hw->wiphy, &ap->work);
	skb_queue_purge(&ap->rxq);
	while ((ms = ap_first_client(mh)))
		ap_client_remove(mh, ms, false, 0, false, 0, 0, NULL, 0);
	for (int i = 0; i < MAC_AP_GTKS; i++) {
		k[i] = ap->gtk[i];
		rcu_assign_pointer(ap->gtk[i], NULL);
	}
	synchronize_rcu();
	for (int i = 0; i < MAC_AP_GTKS; i++) {
		if (!k[i])
			continue;
		if (k[i]->hw && !READ_ONCE(k[i]->lost) && vif->bss_conf.chanctx_conf)
			mh->ops->set_key(hw, DISABLE_KEY, vif, NULL, &k[i]->conf);
		mac_key_free(k[i]);
		AP_COUNT(key_dels);
	}
	vif->bss_conf.enable_beacon = 0;
	mh->ops->link_info_changed(hw, vif, &vif->bss_conf, BSS_CHANGED_BEACON_ENABLED);
	mh->ops->stop_ap(hw, vif, &vif->bss_conf);
	if (cctx)
		freed = mac_chanctx_release(mh, mv, cctx);
	ap_vif_remove(mh, mv);
	if (freed)
		kfree(cctx);
	kfree(mv);
	ap->ctx = NULL;
	ap->aid_map = 0;
	atomic_set(&ap->n_ps, 0);
	skb_queue_purge(&ap->rx_fifo);
	AP_COUNT(stops);
	pr_info("mac: GO stopped");
}

void rtw_linux_ap_stop (int tx)
{
	struct mac_hw *mh = mac_the_hw;

	if (!mh)
		return;
	(void)mac_lock_driver(mh);      /* after a restart in progress if possible; stopping goes on regardless */
	mac_ap_stop_locked(mh, tx != 0);
	ap_inds_free(&mh->ap);
	mh->ap.event_fn = NULL;
	mh->ap.event_ctx = NULL;
	mh->ap.restart_ind = false;
	wiphy_unlock(mh->hw.wiphy);
}

/* iface.c mac_restart_work, the driver reset: park the GO (the station's context is parked by mlme.c after this). */
void mac_ap_restart (struct mac_hw *mh)
{
	if (!mh->ap.vif)
		return;
	pr_info("mac: hardware restart: the GO stops");
	ap_park(mh);
	mh->ap.restart_ind = true;
}

void mac_ap_restart_done (struct mac_hw *mh)
{
	struct rtw_ap_ind ind;

	if (!mh->ap.restart_ind)
		return;
	mh->ap.restart_ind = false;
	memset(&ind, 0, sizeof(ind));
	ind.type = RTW_AP_IND_STOPPED;
	ind.status = RTW_AP_STOP_RESTART;
	ap_ind(mh, &ind, NULL, 0, NULL, 0);
}

/* reg.c reg_work, wiphy->mtx held: a GO on a channel that may no longer beacon stops (no frame goes out on it) */
void mac_ap_reg_changed (struct mac_hw *mh)
{
	struct rtw_ap_ind ind;

	if (!mh->ap.vif || !mh->ap.running || ap_chan_ok(mh->ap.chan))
		return;
	pr_warn("mac: GO: channel %u may no longer beacon under %c%c; stopping", mh->ap.chan->hw_value, mh->reg.cur[0],
		mh->reg.cur[1]);
	mac_ap_stop_locked(mh, false);
	memset(&ind, 0, sizeof(ind));
	ind.type = RTW_AP_IND_STOPPED;
	ind.status = RTW_AP_STOP_CHANNEL;
	ap_ind(mh, &ind, NULL, 0, NULL, 0);
}

/* the hw goes away (iffree) */
void mac_ap_free (struct mac_hw *mh)
{
	struct mac_ap *ap = &mh->ap;

	ap_inds_free(ap);
	skb_queue_purge(&ap->rxq);
	skb_queue_purge(&ap->rx_fifo);
	kfree(ap->beacon_ies);
	kfree(ap->presp_ies);
	ap->beacon_ies = ap->presp_ies = NULL;
	ap->beacon_ies_len = ap->presp_ies_len = 0;
}

int rtw_linux_ap_set_ies (const uint8_t *beacon, uint32_t beacon_len, const uint8_t *presp, uint32_t presp_len)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_ap *ap;
	u8 *b = NULL, *p = NULL;

	if (!mh)
		return -ENODEV;
	if (beacon_len > RTW_AP_IES_MAX || presp_len > RTW_AP_IES_MAX || (beacon_len && !apf_ies_ok(beacon, beacon_len)) ||
	    (presp_len && !apf_ies_ok(presp, presp_len)))
		return -EINVAL;
	if (beacon_len && !(b = kmemdup(beacon, beacon_len, GFP_KERNEL)))
		return -ENOMEM;
	if (presp_len && !(p = kmemdup(presp, presp_len, GFP_KERNEL))) {
		kfree(b);
		return -ENOMEM;
	}
	ap = &mh->ap;
	(void)mac_lock_driver(mh);      /* during a restart stored all the same: a later template takes them */
	swap(ap->beacon_ies, b);
	swap(ap->presp_ies, p);
	ap->beacon_ies_len = (u16)beacon_len;
	ap->presp_ies_len = (u16)presp_len;
	if (ap->running && !READ_ONCE(mh->restarting))
		mh->ops->link_info_changed(&mh->hw, &ap->vif->vif, &ap->vif->vif.bss_conf, BSS_CHANGED_BEACON);
	wiphy_unlock(mh->hw.wiphy);
	kfree(b);
	kfree(p);
	return 0;
}

/* ---- IOCTL_RTW_AP: a snapshot ---- */
void rtw_linux_ap_stats (struct rtw_ap_stats *st)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_sta *ms;
	u64 now = ktime_get_ns();

	*st = ap_st;
	st->running = 0;
	st->n_clients = 0;
	st->band = st->channel = 0;
	memset(st->bssid, 0, sizeof(st->bssid));
	st->beacon_int = 0;
	memset(st->clients, 0, sizeof(st->clients));
	if (!mh || !READ_ONCE(mh->ap.running))
		return;
	rcu_read_lock();
	st->running = 1;
	if (mh->ap.chan) {
		st->band = (u8)mh->ap.chan->band;
		st->channel = (u8)mh->ap.chan->hw_value;
	}
	ether_addr_copy(st->bssid, mh->ap.bss.bssid);
	st->beacon_int = mh->ap.bss.beacon_int;
	list_for_each_entry_rcu(ms, &mh->sta_list, list) {
		struct rtw_ap_client *c;

		if (!ms->ap_client || st->n_clients >= RTW_AP_MAX_CLIENTS)
			continue;
		c = &st->clients[st->n_clients++];
		ether_addr_copy(c->addr, ms->sta.addr);
		c->state = READ_ONCE(ms->ap.state);
		c->flags = (ms->state == IEEE80211_STA_AUTHORIZED ? RTW_APC_F_AUTHORIZED : 0) |
			   (READ_ONCE(ms->ap.ps) ? RTW_APC_F_PS : 0) |
			   (ms->sta.deflink.ht_cap.ht_supported ? RTW_APC_F_HT : 0) | (ms->sta.wme ? RTW_APC_F_WMM : 0);
		c->aid = ms->sta.aid;
		c->ps_queued = (u16)skb_queue_len_lockless(&ms->ap.ps_q);
		c->idle_ms = (u32)min_t(u64, (now - READ_ONCE(ms->ap.last_rx_ns)) / NSEC_PER_MSEC, U32_MAX);
		c->rx_data = ms->ap.rx_data;
		c->tx_data = ms->ap.tx_data;
	}
	rcu_read_unlock();
}

int rtw_linux_ap_get_ind (struct rtw_ap_ind *ind, void *buf, uint32_t len)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_ap_ind *n;
	int ret = 0;

	if (!mh)
		return 0;
	wiphy_lock(mh->hw.wiphy);
	n = list_first_entry_or_null(&mh->ap.inds, struct mac_ap_ind, list);
	if (n) {
		list_del(&n->list);
		mh->ap.n_inds--;
		*ind = n->ind;
		if (n->len > len) {
			ret = -ENOSPC;
		} else {
			if (n->len)
				memcpy(buf, n->data, n->len);
			ret = 1;
		}
		kfree(n);
	}
	wiphy_unlock(mh->hw.wiphy);
	return ret;
}
