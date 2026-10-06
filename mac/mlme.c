// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: the station MLME: authentication and association with one of the
 * BSSs the OS listed, the connection, and its end (deauthentication by us or the AP, beacon loss).
 * The order of the driver calls follows mac80211 mlme.c (ieee80211_prep_connection, ieee80211_assoc_success,
 * ieee80211_set_disassoc; read for reference only):
 *   channel context -> WMM defaults -> link_info_changed(BSSID, BASIC_RATES, BEACON_INT) -> station on the list,
 *   then NOTEXIST -> NONE -> authentication -> AUTH -> association -> ASSOC -> AUTHORIZED (open network, no
 *   control port) -> WMM parameters -> vif_cfg_changed(ASSOC) -> link_info_changed(QOS, ERP, HT, ...).
 * Leaving goes the other way (flush, deauthentication, station off the list, states down to NOTEXIST,
 * vif_cfg_changed(ASSOC), BSSID cleared, WMM defaults, channel context released).
 * rtw89 v7.2 has no mgd_prepare_tx/mgd_complete_tx, so they are not called; it reports beacon loss from its
 * firmware beacon filter (CONNECTION_MONITOR), so beacons are not counted here.
 * Everything runs with wiphy->mtx held: rtw_linux_connect/cancel/disconnect, and conn.work/conn.timer, which take
 * the frames rx.c queues and the tx status tx.c records. Block ack agreements are agg.c's.
 * Only rtw_linux_connect() leaves IDLE, and the front end calls it only for a TASK_CONNECT it accepted (and not while
 * the optional Parameters\AllowConnect says 0, wificx\connect.cpp).
 * WPA2-Personal: a candidate must carry the SSID the OS asked for and offer what it asked for (open, or RSN with PSK
 * and CCMP as pairwise and group cipher, management frame protection not required); the association request then
 * carries our RSN element (PSK, CCMP, no MFPC). The OS does the 4-way handshake over the data path; the AP's station
 * stays ASSOC until the pairwise key comes (key.c), and the keys go before the station on leaving.
 * Power save goes off before leaving and while the AP is probed (ps.c); the Country element of the BSS is a
 * regulatory hint from the connection's success to its end (reg.c).
 * WPA3-Personal: a candidate may also use SAE (sae.c) and management frame protection (pmf.c): the RSN element is
 * then built for the attempt (AKM PSK or SAE, MFPC/MFPR), every frame from the BSS goes through pmf.c before it is
 * looked at, an association the AP refuses for now (status 30, it is checking an older association of ours with an
 * SA Query) is tried again after the comeback time it gives, and the AP's SA Query requests get their answer.
 * Channel width: on 5 GHz the connection is as wide as the BSS, Parameters\Bandwidth (rtw_linux_set_wide) and the
 * regulatory rules allow, up to VHT80 (wide.c chooses): the channel context gets that chandef, the association
 * request our HT (with 40 MHz) and VHT capabilities, the AP's station the intersected HT/VHT capabilities, width and
 * streams; an Operating Mode Notification of the AP narrows them. 2.4 GHz, a BSS without HT and Bandwidth 20 give an
 * HT20 connection (no VHT element, HT without 40 MHz, an HT20 context).
 */
#include "mac.h"

#define MLME_RXQ_MAX                    8
#define MLME_TRIES                      3       /* authentication and association requests */
#define MLME_TX_WAIT_MS                 500     /* a request's tx status, or the response when no status comes */
#define MLME_RESP_WAIT_MS               100     /* the response after the request was acknowledged */
#define MLME_HEARD_MS                   10000   /* a NO_IR/RADAR channel: the BSS must have been heard this recently */
#define MLME_PROBE_TRIES                3       /* Null frames after a beacon loss */
#define MLME_PROBE_WAIT_MS              500
#define MLME_TICK_MS                    1000    /* connected: link report check */
#define MLME_LINK_REPORT_MS             5000
#define MLME_DEAUTH_WAIT_MS             200
#define MLME_TIMER_SLACK_MS             5

#define CAP_ESS                         0x0001
#define CAP_PRIVACY                     0x0010
#define CAP_SHORT_PREAMBLE              0x0020
#define CAP_SHORT_SLOT                  0x0400
#define EID_ERP                         42
#define EID_TIMEOUT_INTERVAL            56
#define EID_RSNX                        244
#define TIMEOUT_ASSOC_COMEBACK          3       /* Timeout Interval type: association comeback time (TUs) */
#define REASON_PREV_AUTH_NOT_VALID      2       /* WLAN_REASON_PREV_AUTH_NOT_VALID */
#define REASON_INACTIVITY               4       /* WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY */
#define STATUS_REFUSED_TEMPORARILY      30      /* WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY */
#define STATUS_REQUEST_DECLINED         37
#define ACTION_CAT_BACK                 3
#define ACTION_CAT_SA_QUERY             8
#define ACTION_CAT_VHT                  21
#define ACTION_VHT_OPMODE_NOTIF         2
#define ACTION_ADDBA_REQ                0
#define ACTION_ADDBA_RESP               1
#define AUTH_ALG_OPEN                   0
#define AUTH_ALG_SAE                    3
#define MAC_HDR_LEN                     24
#define RSN_SEL(type)                   (0x000fac00u | (type))  /* suite selector 00-0F-AC:type, big endian */
#define RSN_CIPHER_CCMP                 RSN_SEL(4)
#define RSN_CIPHER_BIP                  RSN_SEL(6)              /* BIP-CMAC-128 */
#define RSN_AKM_PSK                     RSN_SEL(2)
#define RSN_AKM_SAE                     RSN_SEL(8)
#define RSN_CAP_MFPR                    0x0040
#define RSN_CAP_MFPC                    0x0080
#define RSNX_SAE_H2E                    0x20    /* RSNXE: field length 1 (bits 0-3 = 0), SAE hash-to-element */
#define RSN_IE_LEN                      22      /* our RSN element */
#define MLME_IES_MAX                    256     /* the OS's elements for the association request */
#define MLME_COMEBACKS                  3       /* association comebacks per candidate */
#define MLME_COMEBACK_MIN_MS            100
#define MLME_COMEBACK_MAX_MS            10000

static const u8 oui_ms[3] = { 0x00, 0x50, 0xf2 };
static u32 wide_mhz = RTW_WIDE_DEFAULT;         /* Parameters\Bandwidth (rtw_linux_set_wide) */

static void mlme_work (struct wiphy *wiphy, struct wiphy_work *work);
static void mlme_timer (struct wiphy *wiphy, struct wiphy_work *work);

void mac_mlme_init (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;

	wiphy_work_init(&c->work, mlme_work);
	wiphy_delayed_work_init(&c->timer, mlme_timer);
	spin_lock_init(&c->txs_lock);
	skb_queue_head_init(&c->rxq);
	mac_agg_init(mh);
	mac_pmf_init(mh);
}

/* ---- helpers ---- */
static void mlme_events (struct mac_hw *mh, u32 events)
{
	struct mac_conn *c = &mh->conn;

	if (c->event_fn && events)
		c->event_fn(c->event_ctx, events);
}

/* The step timer. A run of conn.timer that was already queued when the timer was armed again (or disarmed) finds
   a later deadline (it waits for the rest) or none (it does nothing). */
static void mlme_arm (struct mac_hw *mh, u32 ms)
{
	clear_bit(MAC_CONN_F_TIMEOUT, &mh->conn.flags);
	mh->conn.deadline_ns = ktime_get_ns() + (u64)ms * NSEC_PER_MSEC;
	wiphy_delayed_work_queue(mh->hw.wiphy, &mh->conn.timer, msecs_to_jiffies(ms));
}

static void mlme_disarm (struct mac_hw *mh)
{
	mh->conn.deadline_ns = 0;
	wiphy_delayed_work_cancel(mh->hw.wiphy, &mh->conn.timer);
	clear_bit(MAC_CONN_F_TIMEOUT, &mh->conn.flags);
}

static struct ieee80211_vif *mlme_vif (struct mac_hw *mh)
{
	return &mh->sta_vif->vif;
}

/* the same for sae.c and pmf.c */
void mac_mlme_arm (struct mac_hw *mh, u32 ms)
{
	mlme_arm(mh, ms);
}

void mac_mlme_disarm (struct mac_hw *mh)
{
	mlme_disarm(mh);
}

void mac_mlme_events (struct mac_hw *mh, u32 events)
{
	mlme_events(mh, events);
}

/* a management frame to the BSS: header filled in, body_len bytes of body to fill (also agg.c's action frames) */
struct sk_buff *mac_mlme_frame (struct mac_hw *mh, u16 stype, unsigned int body_len, u8 **body)
{
	struct sk_buff *skb;
	u8 *p;

	skb = dev_alloc_skb(mh->hw.extra_tx_headroom + MAC_HDR_LEN + body_len);
	if (!skb)
		return NULL;
	skb_reserve(skb, mh->hw.extra_tx_headroom);
	p = skb_put_zero(skb, MAC_HDR_LEN + body_len);
	put_unaligned_le16(IEEE80211_FTYPE_MGMT | stype, p);
	ether_addr_copy(p + 4, mh->conn.bssid);
	ether_addr_copy(p + 10, mlme_vif(mh)->addr);
	ether_addr_copy(p + 16, mh->conn.bssid);
	*body = p + MAC_HDR_LEN;
	return skb;
}

/* ---- elements of the BSS's frames ---- */
static void parse_rates (const struct ieee80211_supported_band *sb, const u8 *d, u8 l, u32 *rates, u32 *basic)
{
	for (u8 j = 0; j < l; j++) {
		for (int i = 0; i < sb->n_bitrates && i < 32; i++) {
			if (sb->bitrates[i].bitrate / 5 != (d[j] & 0x7f))
				continue;
			*rates |= BIT(i);
			if (d[j] & 0x80)
				*basic |= BIT(i);
		}
	}
}

static u32 rsn_sel (const u8 *d)
{
	return (u32)d[0] << 24 | (u32)d[1] << 16 | (u32)d[2] << 8 | d[3];
}

/* RSN element body: version, group cipher, pairwise ciphers, AKMs, capabilities, PMKIDs, group management cipher; a
   field may be missing only with all the following ones (then the defaults: CCMP, CCMP, 802.1X, 0, none,
   BIP-CMAC-128) */
static void parse_rsn (struct mac_bss_params *b, const u8 *d, int len)
{
	int n, i;

	b->rsn = true;
	b->rsn_bad = true;
	b->rsn_group = RSN_CIPHER_CCMP;
	b->rsn_ccmp = true;
	b->rsn_psk = false;
	b->rsn_sae = false;
	b->rsn_caps = 0;
	b->rsn_mgmt_group = RSN_CIPHER_BIP;
	if (len < 2 || get_unaligned_le16(d) != 1)
		return;
	d += 2;
	len -= 2;
	if (len >= 4) {
		b->rsn_group = rsn_sel(d);
		d += 4;
		len -= 4;
	} else if (len) {
		return;
	}
	if (len >= 2) {
		n = get_unaligned_le16(d);
		if (len < 2 + 4 * n)
			return;
		b->rsn_ccmp = false;
		for (i = 0; i < n; i++)
			b->rsn_ccmp |= rsn_sel(d + 2 + 4 * i) == RSN_CIPHER_CCMP;
		d += 2 + 4 * n;
		len -= 2 + 4 * n;
	} else if (len) {
		return;
	}
	if (len >= 2) {
		n = get_unaligned_le16(d);
		if (len < 2 + 4 * n)
			return;
		for (i = 0; i < n; i++) {
			b->rsn_psk |= rsn_sel(d + 2 + 4 * i) == RSN_AKM_PSK;
			b->rsn_sae |= rsn_sel(d + 2 + 4 * i) == RSN_AKM_SAE;
		}
		d += 2 + 4 * n;
		len -= 2 + 4 * n;
	} else if (len) {
		return;
	}
	if (len >= 2) {
		b->rsn_caps = get_unaligned_le16(d);
		d += 2;
		len -= 2;
	}
	/* PMKIDs (none in a beacon normally), then the group management cipher; both are optional extras here, so a
	   short PMKID list only ends the parsing there and does not make the element malformed */
	b->rsn_bad = false;
	if (len >= 2) {
		n = get_unaligned_le16(d);
		if (len < 2 + 16 * n)
			return;
		d += 2 + 16 * n;
		len -= 2 + 16 * n;
	}
	if (len >= 4)
		b->rsn_mgmt_group = rsn_sel(d);
}

/* elements of a beacon/probe response (fresh) or of the association response (updates what it carries) */
static void parse_ies (struct mac_bss_params *b, const struct ieee80211_supported_band *sb, const u8 *p, int len,
	bool resp)
{
	bool rates_seen = false, cap = false, oper = false;

	while (len >= 2 && p[1] + 2 <= len) {
		const u8 *d = p + 2;
		u8 id = p[0], l = p[1];

		switch (id) {
		case WLAN_EID_SSID:
			if (!resp && !b->ssid && l <= IEEE80211_MAX_SSID_LEN) {
				b->ssid = d;
				b->ssid_len = l;
			}
			break;
		case WLAN_EID_RSN:
			if (!resp && !b->rsn)
				parse_rsn(b, d, l);
			break;
		case WLAN_EID_SUPP_RATES:
		case WLAN_EID_EXT_SUPP_RATES:
			if (resp && !rates_seen)
				b->rates = b->basic_rates = 0;
			rates_seen = true;
			parse_rates(sb, d, l, &b->rates, &b->basic_rates);
			break;
		case WLAN_EID_DS_PARAMS:
			if (l >= 1)
				b->ds_channel = d[0];
			break;
		case WLAN_EID_TIM:
			if (l >= 2 && d[1])
				b->dtim_period = d[1];
			if (!resp)
				b->tim = true;
			break;
		case WLAN_EID_COUNTRY:
			/* kept for the regulatory hint once connected (reg.c checks it) */
			if (!resp && !b->country_len && l) {
				b->country_len = l;
				memcpy(b->country, d, min_t(u8, l, sizeof(b->country)));
			}
			break;
		case EID_ERP:
			if (l >= 1) {
				b->erp = true;
				b->erp_protection = d[0] & 0x02;
				b->erp_barker = d[0] & 0x04;
			}
			break;
		case WLAN_EID_HT_CAPABILITY:
			if (l >= sizeof(b->ht_cap)) {
				memcpy(b->ht_cap, d, sizeof(b->ht_cap));
				cap = true;
			}
			break;
		case WLAN_EID_HT_OPERATION:
			if (l >= sizeof(b->ht_oper)) {
				memcpy(b->ht_oper, d, sizeof(b->ht_oper));
				oper = true;
			}
			break;
		case WLAN_EID_VENDOR_SPECIFIC:
			/* WPA (pre-RSN): OUI 00:50:f2, type 1; not supported, but it means "not open" */
			if (!resp && l >= 4 && !memcmp(d, oui_ms, 3) && d[3] == 1)
				b->wpa = true;
			/* WMM (Wi-Fi Multimedia): OUI 00:50:f2, type 2, subtype 0 information / 1 parameter */
			if (l >= 7 && !memcmp(d, oui_ms, 3) && d[3] == 2 && (d[4] == 0 || d[4] == 1)) {
				b->wmm = true;
				if (d[4] == 1 && l >= 24) {
					b->wmm_params = true;
					memcpy(b->wmm_ac, d + 8, sizeof(b->wmm_ac));
				}
			}
			break;
		default:
			break;
		}
		wide_parse_ie(&b->w, id, d, l, resp);   /* HT/VHT for the width, VHT, Operating Mode Notification */
		len -= l + 2;
		p += l + 2;
	}
	/* a response without HT elements keeps what the beacon said; one with them has just replaced its copies */
	if (!resp)
		b->ht = cap && oper;
}

/* ---- WMM (conf_tx) ---- */
static bool mlme_use_11b (struct mac_hw *mh)
{
	/* mac80211 operating_11g_mode: a 2.4 GHz BSS with OFDM rates */
	const struct ieee80211_supported_band *sb = mh->hw.wiphy->bands[NL80211_BAND_2GHZ];

	if (mh->conn.chan->band != NL80211_BAND_2GHZ || !sb)
		return false;
	for (int i = 0; i < sb->n_bitrates && i < 32; i++)
		if ((mh->conn.bss.rates & BIT(i)) && sb->bitrates[i].bitrate > 110)
			return false;
	return true;
}

/* util.c ieee80211_set_wmm_default(); the values are the EDCA defaults of 802.11-2020 Table 9-155 */
static void mlme_wmm_default (struct mac_hw *mh, bool qos)
{
	struct ieee80211_vif *vif = mlme_vif(mh);
	bool b11 = mlme_use_11b(mh);
	u16 cw_min = b11 ? 31 : 15;
	int ac;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		struct ieee80211_tx_queue_params p = { 0 };

		p.cw_max = 1023;
		p.cw_min = cw_min;
		p.aifs = 2;
		if (qos) {
			switch (ac) {
			case IEEE80211_AC_BK:
				p.aifs = 7;
				break;
			case IEEE80211_AC_VI:
				p.cw_max = cw_min;
				p.cw_min = (cw_min + 1) / 2 - 1;
				p.txop = b11 ? 6016 / 32 : 3008 / 32;
				break;
			case IEEE80211_AC_VO:
				p.cw_max = (cw_min + 1) / 2 - 1;
				p.cw_min = (cw_min + 1) / 4 - 1;
				p.txop = b11 ? 3264 / 32 : 1504 / 32;
				break;
			default:                /* BE */
				p.aifs = 3;
				break;
			}
		}
		mh->ops->conf_tx(&mh->hw, vif, 0, (u16)ac, &p);
	}
	vif->bss_conf.qos = qos;
}

/* mlme.c ieee80211_sta_wmm_params(): the AP's WMM Parameter element */
static void mlme_wmm_params (struct mac_hw *mh)
{
	static const u8 aci_to_ac[4] = { IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_VI, IEEE80211_AC_VO };
	struct ieee80211_vif *vif = mlme_vif(mh);

	for (int i = 0; i < 4; i++) {
		const u8 *r = mh->conn.bss.wmm_ac[i];
		struct ieee80211_tx_queue_params p = { 0 };

		p.aifs = r[0] & 0x0f;
		p.acm = !!(r[0] & 0x10);
		p.cw_min = (u16)((1 << (r[1] & 0x0f)) - 1);
		p.cw_max = (u16)((1 << (r[1] >> 4)) - 1);
		p.txop = get_unaligned_le16(r + 2);
		if (p.aifs < 2)
			p.aifs = 2;
		mh->ops->conf_tx(&mh->hw, vif, 0, aci_to_ac[(r[0] >> 5) & 3], &p);
	}
	vif->bss_conf.qos = true;
}

/* ---- the AP's station ---- */
static const u8 tid_to_ac[8] = {
	IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_BK, IEEE80211_AC_BE,
	IEEE80211_AC_VI, IEEE80211_AC_VI, IEEE80211_AC_VO, IEEE80211_AC_VO,
};

/* Frees a station. Drops any frames its reorder buffers still hold and stops its reorder timer first (agg.c), so
   the timer cannot run against freed memory. PASSIVE_LEVEL, after the station is off the lists and out of the
   driver (its readers gone). */
void mac_sta_free (struct mac_sta *ms)
{
	if (!ms)
		return;
	if (ms->mv)
		mac_agg_sta_free(ms->mv->mh, ms);
	if (ms->ap_client) {
		/* a client of our GO (ap.c): what it kept */
		skb_queue_purge(&ms->ap.ps_q);
		kfree(ms->ap.req);
		kfree(ms->ap.resp);
	}
	kfree(ms->txq_mem);
	kfree(ms);
}

/* sta_info_alloc(): the station and its txqs (rtw89 initialises the txqs' private part in sta_add); on interface mv
   (the station's, or the GO's for a client) */
struct mac_sta *mac_sta_alloc (struct mac_hw *mh, struct mac_vif *mv, const u8 *addr)
{
	size_t stride = ALIGN(sizeof(struct ieee80211_txq) + mh->hw.txq_data_size, CACHE_LINE_SIZE);
	struct mac_sta *ms;
	int i;

	ms = kzalloc(sizeof(*ms) + mh->hw.sta_data_size, GFP_KERNEL);
	if (!ms)
		return NULL;
	ms->txq_mem = kzalloc(stride * ARRAY_SIZE(ms->sta.txq), GFP_KERNEL);
	if (!ms->txq_mem) {
		kfree(ms);
		return NULL;
	}
	INIT_LIST_HEAD(&ms->list);
	ms->mv = mv;
	ms->state = IEEE80211_STA_NOTEXIST;
	memset(ms->rx_seq, 0xff, sizeof(ms->rx_seq));
	mac_agg_sta_init(ms);           /* the reorder timer and the block ack state */
	for (i = 0; i < (int)ARRAY_SIZE(ms->sta.txq); i++) {
		struct ieee80211_txq *txq = (struct ieee80211_txq *)((u8 *)ms->txq_mem + stride * i);

		txq->sta = &ms->sta;
		txq->vif = &mv->vif;
		txq->tid = (u8)i;
		txq->ac = i < IEEE80211_NUM_TIDS ? tid_to_ac[i & 7] : IEEE80211_AC_VO;
		ms->sta.txq[i] = txq;
	}
	ether_addr_copy(ms->sta.addr, addr);
	ms->sta.deflink.sta = &ms->sta;
	ether_addr_copy(ms->sta.deflink.addr, addr);
	ms->sta.deflink.link_id = 0;
	ms->sta.link[0] = &ms->sta.deflink;
	return ms;
}

/* What the station can do with us, from the BSS's elements: rates, HT (mac80211 ieee80211_ht_cap_ie_to_sta_ht_cap,
   reduced), one or two streams; the 40 MHz bits at 40 MHz and more; VHT in VHT mode (wide.c, vht.c
   ieee80211_vht_cap_ie_to_sta_vht_cap); the width (ieee80211_sta_cap_rx_bw within the channel) and the streams
   (ieee80211_sta_set_rx_nss); an Operating Mode Notification of the association response narrows them (mlme.c
   ieee80211_assoc_config_link). The rate adaptation reads these when the station goes to ASSOC (phy.c). */
static void sta_set_caps (struct mac_hw *mh, struct mac_sta *ms)
{
	struct mac_conn *c = &mh->conn;
	const struct mac_bss_params *b = &c->bss;
	struct ieee80211_supported_band *sb = mh->hw.wiphy->bands[c->chan->band];
	struct ieee80211_link_sta *ls = &ms->sta.deflink;
	u16 ap_cap, own = sb->ht_cap.cap, cap = 0;
	u8 bw = IEEE80211_STA_RX_BW_20, nss;
	struct wide_vht v;
	int i, n = 0;

	BUILD_BUG_ON(IEEE80211_STA_RX_BW_40 != 1 || IEEE80211_STA_RX_BW_80 != 2);
	ls->supp_rates[c->chan->band] = b->rates;
	ls->bandwidth = IEEE80211_STA_RX_BW_20;
	ls->smps_mode = IEEE80211_SMPS_OFF;
	ls->rx_nss = 1;
	ls->agg.max_amsdu_len = 3839;
	ls->agg.max_rc_amsdu_len = 3839;
	memset(&ls->ht_cap, 0, sizeof(ls->ht_cap));
	memset(&ls->vht_cap, 0, sizeof(ls->vht_cap));
	memset(&ls->he_cap, 0, sizeof(ls->he_cap));
	memset(&ls->eht_cap, 0, sizeof(ls->eht_cap));
	c->sta_bw = IEEE80211_STA_RX_BW_20;
	c->sta_nss = 1;
	if (!b->ht || !sb->ht_cap.ht_supported)
		return;

	ap_cap = get_unaligned_le16(b->ht_cap);
	cap |= ap_cap & own & (IEEE80211_HT_CAP_LDPC_CODING | IEEE80211_HT_CAP_SGI_20);
	cap |= ap_cap & IEEE80211_HT_CAP_SM_PS;
	if ((ap_cap & IEEE80211_HT_CAP_RX_STBC) && (own & IEEE80211_HT_CAP_TX_STBC))
		cap |= ap_cap & IEEE80211_HT_CAP_RX_STBC;
	if ((ap_cap & IEEE80211_HT_CAP_TX_STBC) && (own & IEEE80211_HT_CAP_RX_STBC))
		cap |= IEEE80211_HT_CAP_TX_STBC;
	/* 40 MHz and its short guard interval only on a 40/80 MHz channel (as advertised, wide_ht_cap_ours) */
	if (c->wide.width >= 40)
		cap |= ap_cap & own & (IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_40);
	ls->ht_cap.ht_supported = true;
	ls->ht_cap.cap = cap;                   /* never DSSS/CCK in 40 MHz */
	ls->ht_cap.ampdu_factor = b->ht_cap[2] & IEEE80211_HT_AMPDU_PARM_FACTOR;
	ls->ht_cap.ampdu_density = (b->ht_cap[2] & IEEE80211_HT_AMPDU_PARM_DENSITY) >> IEEE80211_HT_AMPDU_PARM_DENSITY_SHIFT;
	for (i = 0; i < IEEE80211_HT_MCS_MASK_LEN; i++)
		ls->ht_cap.mcs.rx_mask[i] = b->ht_cap[3 + i] & sb->ht_cap.mcs.rx_mask[i];
	ls->ht_cap.mcs.rx_highest = get_unaligned_le16(b->ht_cap + 3 + IEEE80211_HT_MCS_MASK_LEN);
	for (i = 0; i < 4; i++)
		n += ls->ht_cap.mcs.rx_mask[i] != 0;
	nss = (u8)clamp(n, 1, 2);
	if (cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40)
		bw = IEEE80211_STA_RX_BW_40;

	/* VHT (the AP's VHT Capabilities: the association response's if it had them, else the beacon's) */
	if (c->wide.mode == WIDE_MODE_VHT && b->w.vht_cap && sb->vht_cap.vht_supported) {
		wide_vht_sta(c->vht_ours, sb->vht_cap.vht_mcs.rx_mcs_map, sb->vht_cap.vht_mcs.tx_mcs_map, b->w.vht_cap_ie,
			     &v);
		if (v.ok) {
			ls->vht_cap.vht_supported = true;
			ls->vht_cap.cap = v.cap;
			ls->vht_cap.vht_mcs.rx_mcs_map = v.rx_mcs_map;
			ls->vht_cap.vht_mcs.rx_highest = v.rx_highest;
			ls->vht_cap.vht_mcs.tx_mcs_map = v.tx_mcs_map;
			ls->vht_cap.vht_mcs.tx_highest = v.tx_highest;
			ls->agg.max_amsdu_len = wide_vht_amsdu_len(v.cap);
			bw = IEEE80211_STA_RX_BW_80;    /* a VHT station has 80 MHz */
			nss = (u8)clamp_t(int, max_t(int, nss, wide_vht_nss(v.rx_mcs_map)), 1, 2);
		}
	}
	/* within the channel */
	if (c->wide.width < 80 && bw > IEEE80211_STA_RX_BW_40)
		bw = IEEE80211_STA_RX_BW_40;
	if (c->wide.width < 40)
		bw = IEEE80211_STA_RX_BW_20;
	c->sta_bw = bw;
	c->sta_nss = nss;
	if (b->w.opmode && wide_opmode(b->w.opmode_val, &bw, &nss))
		pr_info("mac: connect: the AP's operating mode notification: %u MHz, %u streams", 20u << bw, nss);
	ls->bandwidth = bw;
	ls->rx_nss = nss;
}

/* drv_sta_state() one step at a time, like sta_info_move_state(); with wiphy->mtx held (also key.c) */
int mac_sta_move (struct mac_hw *mh, struct mac_sta *ms, enum ieee80211_sta_state to)
{
	int ret = 0;

	while (ms->state != to) {
		enum ieee80211_sta_state next = ms->state < to ? ms->state + 1 : ms->state - 1;

		ret = mh->ops->sta_state(&mh->hw, &ms->mv->vif, &ms->sta, ms->state, next);
		if (ret && next > ms->state) {
			pr_err("mac: drv sta_state %d -> %d failed: %d", ms->state, next, ret);
			return ret;
		}
		if (ret)
			pr_warn("mac: drv sta_state %d -> %d failed: %d (going on)", ms->state, next, ret);
		WRITE_ONCE(ms->state, next);    /* the iterators show the driver only stations it knows */
	}
	return 0;
}

/* ---- leaving (ieee80211_set_disassoc / ieee80211_destroy_assoc_data) ---- */
static void mlme_send_deauth (struct mac_hw *mh, u16 reason)
{
	struct sk_buff *skb;
	u64 start;
	u8 *body;

	skb = mac_mlme_frame(mh, IEEE80211_STYPE_DEAUTH, 2, &body);
	if (!skb)
		return;
	put_unaligned_le16(reason, body);
	if (mac_tx_mgmt(mh, skb, READ_ONCE(mh->conn.ap), 0, MAC_TXT_DEAUTH))
		return;
	/* wait for it to go out before the station and channel go away (the status comes from the NAPI thread,
	   which does not need wiphy->mtx) */
	start = ktime_get_ns();
	while (mac_txt_pending(mh, MAC_TXT_DEAUTH) && ktime_get_ns() - start < MLME_DEAUTH_WAIT_MS * NSEC_PER_MSEC)
		msleep(5);
}

/* Takes the attempt or connection apart; tx: send a deauthentication first (we were authenticated). Back in
   IDLE afterwards; no events. With wiphy->mtx held. */
static void mlme_teardown (struct mac_hw *mh, u16 reason, bool tx)
{
	struct mac_conn *c = &mh->conn;
	struct ieee80211_hw *hw = &mh->hw;
	struct ieee80211_vif *vif = mlme_vif(mh);
	struct mac_sta *ms = c->ap;
	struct mac_chanctx *ctx = c->ctx;
	bool was_assoc = vif->cfg.assoc;

	lockdep_assert_wiphy(hw->wiphy);
	/* out of power save first, so the deauthentication is not sent from it (ieee80211_set_disassoc); no SA
	   Query or SAE exchange outlives the attempt (the deauthentication below is still protected with MFP) */
	mac_ps_leave(mh, "leaving the BSS");
	mac_pmf_conn_end(mh);
	mac_sae_reset(mh);
	WRITE_ONCE(c->state, RTW_CONN_DISCONNECTING);
	mlme_disarm(mh);
	synchronize_rcu();              /* the data path saw the state change */
	if (ctx) {
		if (mh->ops->flush)
			mh->ops->flush(hw, vif, (u32)(BIT(hw->queues) - 1), true);
		if (tx) {
			mlme_send_deauth(mh, reason);
			if (mh->ops->flush)
				mh->ops->flush(hw, vif, (u32)(BIT(hw->queues) - 1), false);
		}
	}

	/* end the block ack sessions while the station and channel context are still in the driver: the held receive
	   frames go away with the connection, and rtw89 clears its BA CAM (unless this is a hardware restart) */
	if (ms)
		mac_agg_stop_sta(mh, ms, ctx && !mh->restarting);

	/* the station: off the lists, readers gone; the keys out of rtw89 while it is still ASSOC there (mac80211
	   __sta_info_destroy_part2: AUTHORIZED -> ASSOC, ieee80211_free_sta_keys, then down), the group keys with
	   them; then down to NOTEXIST */
	eth_zero_addr(vif->cfg.ap_addr);
	vif->cfg.ssid_len = 0;
	rcu_assign_pointer(c->ap, NULL);
	if (ms) {
		list_del_rcu(&ms->list);
		synchronize_rcu();
		if (ms->state == IEEE80211_STA_AUTHORIZED)
			mac_sta_move(mh, ms, IEEE80211_STA_ASSOC);
	}
	mac_keys_clear(mh, ms);
	if (ms)
		mac_sta_move(mh, ms, IEEE80211_STA_NOTEXIST);

	if (ctx) {
		if (was_assoc) {
			vif->cfg.assoc = false;
			vif->cfg.aid = 0;
			mh->ops->vif_cfg_changed(hw, vif, BSS_CHANGED_ASSOC);
		}
		vif->bss_conf.qos = false;
		eth_zero_addr(mh->sta_vif->bssid);
		vif->bss_conf.basic_rates = 0;
		vif->bss_conf.use_cts_prot = false;
		vif->bss_conf.use_short_preamble = false;
		vif->bss_conf.use_short_slot = false;
		vif->bss_conf.dtim_period = 0;
		vif->bss_conf.ht_operation_mode = 0;
		mh->ops->link_info_changed(hw, vif, &vif->bss_conf, BSS_CHANGED_BSSID | BSS_CHANGED_QOS |
					   BSS_CHANGED_HT | BSS_CHANGED_BASIC_RATES | BSS_CHANGED_ERP_CTS_PROT |
					   BSS_CHANGED_ERP_PREAMBLE | BSS_CHANGED_ERP_SLOT);
		mlme_wmm_default(mh, false);
		/* a context the GO still uses stays (and is not ours to free) */
		if (!mac_chanctx_release(mh, mh->sta_vif, ctx))
			ctx = NULL;
		c->ctx = NULL;
	}
	if (ms || ctx)
		synchronize_rcu();
	mac_sta_free(ms);
	kfree(ctx);
	mac_rx_flush(mh);
	skb_queue_purge(&c->rxq);
	c->authed = false;
	c->mfp = false;
	c->flags = 0;
	WRITE_ONCE(c->state, RTW_CONN_IDLE);
	mac_reg_disconnected(mh);       /* a Country element of the BSS no longer counts */
}

/*
 * A hardware restart (SER level 2, iface.c): rtw89 has already stopped and reset itself, so the connection is torn
 * down WITHOUT any driver calls. The station and channel context are parked (iface.c frees them only when the hw is
 * freed, because rtw89 may still hold a stale pointer to them), the keys are dropped (memory only), and the
 * connection is reported lost so the front end lets Wlansvc reconnect; an attempt still under way is reported
 * failed (the front end's connect task waits for that event). wiphy->mtx held.
 */
static void mlme_fill_info (struct mac_hw *mh, struct rtw_conn_info *i, int status, u16 code);

void mac_mlme_restart (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	struct mac_sta *ms;
	struct mac_chanctx *ctx;
	bool connected, connecting;

	lockdep_assert_wiphy(mh->hw.wiphy);
	mac_agg_cancel(mh);
	mlme_disarm(mh);
	wiphy_work_cancel(mh->hw.wiphy, &c->work);
	mac_pmf_conn_end(mh);           /* memory and timers only */
	mac_sae_reset(mh);
	if (READ_ONCE(c->state) == RTW_CONN_IDLE) {
		skb_queue_purge(&c->rxq);
		return;
	}
	connected = c->state == RTW_CONN_CONNECTED;
	connecting = c->state == RTW_CONN_PREP || c->state == RTW_CONN_AUTH || c->state == RTW_CONN_ASSOC;
	if (connecting && c->cur < c->n_cands && c->n_attempts < ARRAY_SIZE(c->attempts))
		mlme_fill_info(mh, &c->attempts[c->n_attempts++], RTW_ASSOC_FAILURE, 0);
	WRITE_ONCE(c->state, RTW_CONN_DISCONNECTING);
	synchronize_rcu();              /* the data path saw the state change: no more use of the station or keys */
	ms = c->ap;
	ctx = c->ctx;
	rcu_assign_pointer(c->ap, NULL);
	c->ctx = NULL;
	if (ms)
		list_del_rcu(&ms->list);
	if (ctx)
		list_del_rcu(&ctx->list);
	synchronize_rcu();              /* the lock-free iterators no longer see them */
	if (ms) {
		mac_agg_stop_sta(mh, ms, false);        /* drop held frames; do not touch rtw89 (it is reset) */
		list_add_tail(&ms->list, &mh->parked_sta);
	}
	if (ctx)
		list_add_tail(&ctx->list, &mh->parked_ctx);
	mac_keys_drop(mh);              /* free the key memory; rtw89 forgot its security CAM */
	eth_zero_addr(mlme_vif(mh)->cfg.ap_addr);
	mlme_vif(mh)->cfg.ssid_len = 0;
	mlme_vif(mh)->cfg.assoc = false;
	mlme_vif(mh)->cfg.aid = 0;
	c->info.status = RTW_LOST_UNREACHABLE;
	c->info.status_code = WLAN_REASON_UNSPECIFIED;
	c->info.lost_frame_len = 0;
	c->authed = false;
	c->mfp = false;
	c->flags = 0;
	mac_rx_flush(mh);
	skb_queue_purge(&c->rxq);
	WRITE_ONCE(c->state, RTW_CONN_IDLE);
	mac_reg_disconnected(mh);       /* applied (by reg_work) once the restart is done */
	if (connected) {
		MAC_COUNT(disconnects);
		pr_info("mac: connection dropped by a hardware restart; reporting it lost");
		mlme_events(mh, RTW_CONN_EV_LOST);
	} else if (connecting) {
		pr_info("mac: connect: abandoned for a hardware restart; reporting it failed");
		mlme_events(mh, RTW_CONN_EV_ATTEMPT | RTW_CONN_EV_FAILED);
	}
}

/* ---- results ---- */
static void mlme_fill_security (const struct mac_conn *c, struct rtw_conn_info *i)
{
	bool rsn = mac_conn_rsn(c);

	i->auth = c->auth;
	i->pairwise = rsn ? RTW_CIPHER_CCMP : RTW_CIPHER_NONE;
	i->group = rsn ? RTW_CIPHER_CCMP : RTW_CIPHER_NONE;
	i->mfp = c->mfp;
	i->mgmt_group = c->mfp ? RTW_CIPHER_BIP : RTW_CIPHER_NONE;
}

static void mlme_fill_info (struct mac_hw *mh, struct rtw_conn_info *i, int status, u16 code)
{
	struct mac_conn *c = &mh->conn;
	struct mac_cand *cand = &c->cand[c->cur];

	memset(i, 0, sizeof(*i));
	i->status = status;
	i->status_code = code;
	ether_addr_copy(i->bssid, cand->bssid);
	i->band = cand->band;
	i->channel = cand->channel;
	i->wmm = c->bss.wmm;
	i->ht = c->bss.ht;
	i->phy = c->wide.mode;          /* what the attempt asked for */
	i->width_mhz = c->wide.width;
	i->center = c->wide.center;
	mlme_fill_security(c, i);
}

/* The current candidate failed: record it, tear down, go on with the next one (or report the end). */
static void mlme_attempt_failed (struct mac_hw *mh, int status, u16 code)
{
	struct mac_conn *c = &mh->conn;
	u32 events = RTW_CONN_EV_ATTEMPT;

	pr_info("mac: connect: %pM failed, status %d (802.11 status %u)", c->cand[c->cur].bssid, status, code);
	if (c->n_attempts < ARRAY_SIZE(c->attempts))
		mlme_fill_info(mh, &c->attempts[c->n_attempts++], status, code);
	mlme_teardown(mh, WLAN_REASON_DEAUTH_LEAVING, c->authed);
	c->cur++;
	if (c->cur < c->n_cands) {
		WRITE_ONCE(c->state, RTW_CONN_PREP);
		wiphy_work_queue(mh->hw.wiphy, &c->work);
	} else {
		events |= RTW_CONN_EV_FAILED;
	}
	mlme_events(mh, events);
}

/* The connection ended without rtw_linux_disconnect(): tell the front end why (status, reason). tx_reason: the reason
   code of the deauthentication we send, 0 = none. */
static void mlme_lost_tx (struct mac_hw *mh, int status, u16 reason, const u8 *frame, unsigned int len, u16 tx_reason)
{
	struct mac_conn *c = &mh->conn;

	pr_info("mac: connection to %pM lost: status %d, reason %u", c->bssid, status, reason);
	len = min_t(unsigned int, len, sizeof(c->lost_frame));
	if (frame && len)
		memcpy(c->lost_frame, frame, len);
	else
		len = 0;
	c->info.lost_frame_len = (u16)len;
	c->info.status = status;
	c->info.status_code = reason;
	mlme_teardown(mh, tx_reason, tx_reason != 0);
	MAC_COUNT(disconnects);
	mlme_events(mh, RTW_CONN_EV_LOST);
}

/* the same, with our deauthentication (tx) giving the reason the front end gets */
static void mlme_lost (struct mac_hw *mh, int status, u16 reason, const u8 *frame, unsigned int len, bool tx)
{
	mlme_lost_tx(mh, status, reason, frame, len, tx ? reason : 0);
}

/* for sae.c */
void mac_mlme_attempt_failed (struct mac_hw *mh, int status, u16 code)
{
	mlme_attempt_failed(mh, status, code);
}

/* For pmf.c: an unprotected deauthentication/disassociation (stype, reason: the AP's, for the front end) that the SA
   Query showed to be real; leaving with a deauthentication of our own, reason 2 like wpa_supplicant after its SA
   Query timed out (sme_check_sa_query_timeout: WLAN_REASON_PREV_AUTH_NOT_VALID) */
void mac_mlme_peer_left (struct mac_hw *mh, u16 stype, u16 reason)
{
	u8 body[2];

	if (mh->conn.state != RTW_CONN_CONNECTED)
		return;
	put_unaligned_le16(reason, body);
	mlme_lost_tx(mh, stype == IEEE80211_STYPE_DEAUTH ? RTW_LOST_PEER_DEAUTH : RTW_LOST_PEER_DISASSOC, reason, body,
		     sizeof(body), REASON_PREV_AUTH_NOT_VALID);
}

/* ---- authentication and association ---- */
static int mlme_send_auth (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	struct sk_buff *skb;
	u8 *body;

	skb = mac_mlme_frame(mh, IEEE80211_STYPE_AUTH, 6, &body);
	if (!skb)
		return -ENOMEM;
	put_unaligned_le16(AUTH_ALG_OPEN, body);
	put_unaligned_le16(1, body + 2);        /* transaction sequence 1 */
	put_unaligned_le16(0, body + 4);
	c->tries++;
	c->wait_txt = MAC_TXT_AUTH;
	c->resp_wait_ms = MLME_RESP_WAIT_MS;
	pr_info("mac: connect: authenticating with %pM (try %u)", c->bssid, c->tries);
	mlme_arm(mh, MLME_TX_WAIT_MS);
	return mac_tx_mgmt(mh, skb, c->ap, 0, MAC_TXT_AUTH);
}

/* Our RSN element (802.11-2020 9.4.2.24): version 1, group CCMP, pairwise CCMP, the AKM of the attempt (PSK
   or SAE), capabilities: one replay counter each, MFPC with management frame protection, MFPR as well for SAE (WPA3
   requires it) or when the AP requires it. The group management cipher is the default BIP-CMAC-128, so its field
   (and the PMKID count before it) is left out, as wpa_supplicant does. RSN_IE_LEN bytes. */
static u8 *mlme_put_rsn (const struct mac_conn *c, u8 *p)
{
	u16 caps = 0;

	if (c->mfp) {
		caps |= RSN_CAP_MFPC;
		if (c->auth == RTW_AUTH_SAE || (c->bss.rsn_caps & RSN_CAP_MFPR))
			caps |= RSN_CAP_MFPR;
	}
	*p++ = WLAN_EID_RSN;
	*p++ = RSN_IE_LEN - 2;
	put_unaligned_le16(1, p);
	put_unaligned_be32(RSN_CIPHER_CCMP, p + 2);
	put_unaligned_le16(1, p + 6);
	put_unaligned_be32(RSN_CIPHER_CCMP, p + 8);
	put_unaligned_le16(1, p + 12);
	put_unaligned_be32(c->auth == RTW_AUTH_SAE ? RSN_AKM_SAE : RSN_AKM_PSK, p + 14);
	put_unaligned_le16(caps, p + 18);
	return p + RSN_IE_LEN - 2;
}

/* The OS's elements for the association request, without one of the IDs we wrote ourselves (ours[n]): ours
   counts (mac80211 leaves such elements to the caller as well). Observed: Wlansvc gives none on Windows 11 26100. */
static u8 *mlme_put_os_ies (const struct mac_conn *c, u8 *p, const u8 *ours, int n)
{
	const u8 *e = c->ies;
	u32 len = c->ie_len;            /* whole elements (mlme_ies_ok) */

	while (len >= 2 && (u32)e[1] + 2 <= len) {
		bool dup = false;

		for (int i = 0; i < n; i++)
			dup = dup || e[0] == ours[i];
		if (dup) {
			pr_info("mac: connect: the OS's element %u left out (we wrote one)", e[0]);
		} else {
			memcpy(p, e, e[1] + 2);
			p += e[1] + 2;
		}
		len -= (u32)e[1] + 2;
		e += e[1] + 2;
	}
	return p;
}

/* mlme.c ieee80211_send_assoc(): capability, listen interval, SSID, rates, RSN, HT capabilities, VHT
   capabilities, RSNXE (SAE with hash-to-element), the OS's elements (vendor specific), WMM information. The
   element order is 802.11-2020 Table 9-35. */
static int mlme_send_assoc (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	const struct mac_bss_params *b = &c->bss;
	const struct ieee80211_supported_band *sb = mh->hw.wiphy->bands[c->chan->band];
	u8 rates[32], ours[4], *p, *start;
	struct sk_buff *skb;
	u16 cap = CAP_ESS;
	int n = 0, n_ours = 0, i;

	for (i = 0; i < sb->n_bitrates && i < 32; i++)
		if (b->rates & BIT(i))
			rates[n++] = (u8)(sb->bitrates[i].bitrate / 5);
	skb = mac_mlme_frame(mh, IEEE80211_STYPE_ASSOC_REQ, 4 + 2 + c->ssid_len + 2 + 8 + 2 + 32 + RSN_IE_LEN + 2 + 26 +
			 2 + WIDE_VHT_CAP_LEN + 3 + c->ie_len + 9, &start);
	if (!skb)
		return -ENOMEM;
	if (b->capability & CAP_SHORT_PREAMBLE)
		cap |= CAP_SHORT_PREAMBLE;
	if (c->chan->band == NL80211_BAND_2GHZ)
		cap |= CAP_SHORT_SLOT;
	if (b->capability & CAP_PRIVACY)
		cap |= CAP_PRIVACY;     /* like mac80211: the BSS's privacy bit */
	p = start;
	put_unaligned_le16(cap, p);
	put_unaligned_le16(mh->hw.max_listen_interval ? mh->hw.max_listen_interval : 5, p + 2);
	p += 4;
	*p++ = WLAN_EID_SSID;
	*p++ = c->ssid_len;
	memcpy(p, c->ssid, c->ssid_len);
	p += c->ssid_len;
	*p++ = WLAN_EID_SUPP_RATES;
	*p++ = (u8)min(n, 8);
	for (i = 0; i < n && i < 8; i++)
		*p++ = rates[i];
	if (n > 8) {
		*p++ = WLAN_EID_EXT_SUPP_RATES;
		*p++ = (u8)(n - 8);
		for (; i < n; i++)
			*p++ = rates[i];
	}
	if (mac_conn_rsn(c)) {
		p = mlme_put_rsn(c, p);
		ours[n_ours++] = WLAN_EID_RSN;
	}
	if (b->ht && sb->ht_cap.ht_supported) {
		struct ieee80211_sta_ht_cap ht = sb->ht_cap;

		/* 40 MHz (width and short GI) only on a 40/80 MHz channel; always no DSSS/CCK in 40 MHz, 3839-octet
		   A-MSDUs, SM power save disabled */
		BUILD_BUG_ON(WIDE_HT_SUP_WIDTH_20_40 != IEEE80211_HT_CAP_SUP_WIDTH_20_40 ||
			     WIDE_HT_SGI_40 != IEEE80211_HT_CAP_SGI_40 || WIDE_HT_DSSSCCK40 != IEEE80211_HT_CAP_DSSSCCK40 ||
			     WIDE_HT_MAX_AMSDU != IEEE80211_HT_CAP_MAX_AMSDU || WIDE_HT_SM_PS != IEEE80211_HT_CAP_SM_PS ||
			     WIDE_HT_SM_PS_DISABLED != WLAN_HT_CAP_SM_PS_DISABLED << IEEE80211_HT_CAP_SM_PS_SHIFT);
		ht.cap = wide_ht_cap_ours(sb->ht_cap.cap, c->wide.width);
		p = mac_put_ht_cap(p, &ht);
		ours[n_ours++] = WLAN_EID_HT_CAPABILITY;
	}
	/* VHT Capabilities (wide_vht_cap_ours: 80 MHz at most, 3895-octet MPDUs, no +HTC, no MU beamformee) */
	if (c->wide.mode == WIDE_MODE_VHT) {
		const struct ieee80211_sta_vht_cap *v = &sb->vht_cap;

		p += wide_put_vht_cap(p, c->vht_ours, v->vht_mcs.rx_mcs_map, v->vht_mcs.rx_highest, v->vht_mcs.tx_mcs_map,
				      v->vht_mcs.tx_highest);
		ours[n_ours++] = WIDE_EID_VHT_CAP;
	}
	if (c->auth == RTW_AUTH_SAE && c->sae.h2e) {
		*p++ = EID_RSNX;
		*p++ = 1;
		*p++ = RSNX_SAE_H2E;
		ours[n_ours++] = EID_RSNX;
	}
	if (c->ie_len)
		p = mlme_put_os_ies(c, p, ours, n_ours);
	if (b->wmm) {
		static const u8 wmm_info[9] = { WLAN_EID_VENDOR_SPECIFIC, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 };

		memcpy(p, wmm_info, sizeof(wmm_info));
		p += sizeof(wmm_info);
	}
	skb_trim(skb, (unsigned int)(p - skb->data));
	c->info.assoc_req_len = (u16)min_t(size_t, p - start, sizeof(c->assoc_req));
	memcpy(c->assoc_req, start, c->info.assoc_req_len);
	c->tries++;
	c->wait_txt = MAC_TXT_ASSOC;
	c->resp_wait_ms = MLME_RESP_WAIT_MS;
	pr_info("mac: connect: associating with %pM (try %u)", c->bssid, c->tries);
	mlme_arm(mh, MLME_TX_WAIT_MS);
	return mac_tx_mgmt(mh, skb, c->ap, 0, MAC_TXT_ASSOC);
}

/*
 * Is the candidate the network the OS asked for, offering what it asked for? (Its body was parsed into
 * conn.bss.) The SSID must be the requested one (a hidden SSID only when the OS says the network is hidden and
 * AllowSsid does not pin it: such a body does not tell which network it is); an open request needs a BSS without
 * privacy, an RSN request one with an RSN element offering CCMP as pairwise and group cipher and an AKM the OS
 * allows. Of SAE and PSK the first the BSS offers becomes c->auth (SAE only with management frame protection);
 * MFP is used when the OS lets us offer it, the self-tests passed (pmf.c), the BSS has MFPC and its group management
 * cipher is BIP-CMAC-128; a BSS requiring MFP (MFPR) needs it. 0 or RTW_ASSOC_FAILURE.
 */
static int mlme_check_target (struct mac_hw *mh, const struct mac_cand *cand)
{
	struct mac_conn *c = &mh->conn;
	const struct mac_bss_params *b = &c->bss;
	bool hidden = true, mfp_ok, ap_mfpc, ap_mfpr;

	for (int i = 0; b->ssid && i < b->ssid_len; i++)
		hidden = hidden && !b->ssid[i];
	if (hidden ? (c->conn_flags & (RTW_CONN_F_HIDDEN | RTW_CONN_F_PINNED)) != RTW_CONN_F_HIDDEN :
	    b->ssid_len != c->ssid_len || memcmp(b->ssid, c->ssid, c->ssid_len)) {
		pr_warn("mac: connect: %pM is not the network asked for (SSID %s)", cand->bssid,
			hidden ? "hidden" : "differs");
		return RTW_ASSOC_FAILURE;
	}
	if (c->auths == RTW_AUTH_BIT(RTW_AUTH_OPEN)) {
		c->auth = RTW_AUTH_OPEN;
		if ((b->capability & CAP_PRIVACY) || b->rsn || b->wpa) {
			pr_warn("mac: connect: %pM is not an open network", cand->bssid);
			return RTW_ASSOC_FAILURE;
		}
		return 0;
	}
	if (!(b->capability & CAP_PRIVACY) || !b->rsn || b->rsn_bad) {
		pr_warn("mac: connect: %pM has no usable RSN element (privacy %u, RSN %u%s, WPA %u)", cand->bssid,
			!!(b->capability & CAP_PRIVACY), b->rsn, b->rsn_bad ? " malformed" : "", b->wpa);
		return RTW_ASSOC_FAILURE;
	}
	ap_mfpr = b->rsn_caps & RSN_CAP_MFPR;
	ap_mfpc = (b->rsn_caps & RSN_CAP_MFPC) || ap_mfpr;
	mfp_ok = (c->conn_flags & RTW_CONN_F_MFP) && mac_pmf_usable() && b->rsn_mgmt_group == RSN_CIPHER_BIP;
	if ((c->auths & RTW_AUTH_BIT(RTW_AUTH_SAE)) && b->rsn_sae && mfp_ok && ap_mfpc)
		c->auth = RTW_AUTH_SAE;
	else if ((c->auths & RTW_AUTH_BIT(RTW_AUTH_RSN_PSK)) && b->rsn_psk)
		c->auth = RTW_AUTH_RSN_PSK;
	else
		c->auth = (c->auths & RTW_AUTH_BIT(RTW_AUTH_SAE)) ? RTW_AUTH_SAE : RTW_AUTH_RSN_PSK;
	if (b->rsn_group != RSN_CIPHER_CCMP || !b->rsn_ccmp || (c->auth == RTW_AUTH_SAE ? !b->rsn_sae : !b->rsn_psk)) {
		pr_warn("mac: connect: %pM does not offer %s with CCMP (group %08x, pairwise CCMP %u, AKM PSK %u SAE %u)",
			cand->bssid, c->auth == RTW_AUTH_SAE ? "WPA3-SAE" : "WPA2-PSK", b->rsn_group, b->rsn_ccmp,
			b->rsn_psk, b->rsn_sae);
		return RTW_ASSOC_FAILURE;
	}
	if ((ap_mfpr || c->auth == RTW_AUTH_SAE) && !(mfp_ok && ap_mfpc)) {
		pr_warn("mac: connect: %pM needs management frame protection (RSN capabilities %#06x, group management "
			"cipher %08x; ours %s)", cand->bssid, b->rsn_caps, b->rsn_mgmt_group,
			!(c->conn_flags & RTW_CONN_F_MFP) ? "not allowed by the OS" : !mac_pmf_usable() ? "off" : "on");
		return RTW_ASSOC_FAILURE;
	}
	c->mfp = mfp_ok && ap_mfpc;
	pr_info("mac: connect: %pM: %s/CCMP%s", cand->bssid, c->auth == RTW_AUTH_SAE ? "WPA3-SAE" : "WPA2-PSK",
		c->mfp ? (ap_mfpr ? ", management frame protection (required)" : ", management frame protection") : "");
	return 0;
}

/* A 5 GHz channel's flags for wide.c (ctx: the band) */
static u32 mlme_chan_flags (void *ctx, u8 channel)
{
	const struct ieee80211_supported_band *sb = ctx;

	for (int i = 0; i < sb->n_channels; i++) {
		const struct ieee80211_channel *ch = &sb->channels[i];
		u32 f = 0;

		if (ch->hw_value != channel)
			continue;
		if (ch->flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_OFDM))
			f |= WIDE_CH_DISABLED;
		if (ch->flags & IEEE80211_CHAN_NO_HT40PLUS)
			f |= WIDE_CH_NO_HT40PLUS;
		if (ch->flags & IEEE80211_CHAN_NO_HT40MINUS)
			f |= WIDE_CH_NO_HT40MINUS;
		if (ch->flags & IEEE80211_CHAN_NO_80MHZ)
			f |= WIDE_CH_NO_80MHZ;
		return f;
	}
	return WIDE_CH_DISABLED;
}

/* The connection's whole channel still usable under the current rules (cfg80211 reg_wdev_chan_valid():
   cfg80211_chandef_usable(IEEE80211_CHAN_DISABLED) on the station's chandef, with the 40/80 MHz flags); with
   wiphy->mtx held (reg.c) */
bool mac_mlme_chan_ok (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;

	if (c->chan->flags & IEEE80211_CHAN_DISABLED)
		return false;
	if (c->wide.width <= 20)
		return true;
	return wide_usable((u8)c->chan->hw_value, c->wide.width, c->wide.center, mlme_chan_flags,
			   mh->hw.wiphy->bands[c->chan->band]);
}

/*
 * The channel and mode of the attempt (conn.wide) and its chandef. HT20 (or no HT) on 2.4 GHz, with a BSS without
 * HT and with Parameters\Bandwidth 20; else wide.c chooses (mac80211 ieee80211_determine_ap_chan,
 * ieee80211_chandef_usable). A channel context the GO runs in already is shared at its width only: if it is another width, the connection is the plain HT20 one (the GO's context is not widened).
 * Passive channels need nothing more: the BSS was heard on the primary (mlme_prep), and a station may use the
 * secondary channels of a BSS it follows.
 */
static void mlme_wide (struct mac_hw *mh, struct ieee80211_supported_band *sb, struct cfg80211_chan_def *def)
{
	struct mac_conn *c = &mh->conn;
	struct mac_bss_params *b = &c->bss;
	struct wide_chan *w = &c->wide;
	struct mac_chanctx *ctx;
	u8 primary = (u8)c->chan->hw_value;
	bool ht = b->ht && sb->ht_cap.ht_supported;
	u32 max = READ_ONCE(wide_mhz);

	BUILD_BUG_ON(WIDE_MODE_HT != RTW_PHY_HT || WIDE_MODE_VHT != RTW_PHY_VHT);
	BUILD_BUG_ON(WIDE_VHT_HTC_VHT != IEEE80211_VHT_CAP_HTC_VHT ||
		     WIDE_VHT_MU_BEAMFORMEE != IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE ||
		     WIDE_VHT_SU_BEAMFORMEE != IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE ||
		     WIDE_VHT_BEAMFORMEE_STS_MASK != IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK ||
		     WIDE_VHT_MAX_MPDU_MASK != IEEE80211_VHT_CAP_MAX_MPDU_MASK ||
		     WIDE_VHT_SHORT_GI_80 != IEEE80211_VHT_CAP_SHORT_GI_80);
	memset(w, 0, sizeof(*w));
	w->mode = ht ? WIDE_MODE_HT : WIDE_MODE_LEGACY;
	w->width = 20;
	w->center = primary;
	if (ht && c->chan->band == NL80211_BAND_5GHZ && max > 20)
		wide_choose(&b->w, primary, max, sb->vht_cap.vht_supported && !(c->conn_flags & RTW_CONN_F_NO_VHT),
			    mlme_chan_flags, sb, w);
	else if (ht && c->chan->band == NL80211_BAND_5GHZ)
		w->why = WIDE_WHY_SETTING;      /* Bandwidth 20: the BSS's width is not looked at (bss_width 0) */

	ctx = list_first_entry_or_null(&mh->chanctx_list, struct mac_chanctx, list);
	if (ctx && w->width > 20 && (cfg80211_chandef_get_width(&ctx->conf.def) != w->width ||
				     ctx->conf.def.center_freq1 != 5000 + 5 * (u32)w->center)) {
		w->mode = WIDE_MODE_HT;
		w->width = 20;
		w->center = primary;
		w->why = WIDE_WHY_SHARED;
	}

	if (w->width == 20) {
		cfg80211_chandef_create(def, c->chan, w->mode == WIDE_MODE_LEGACY ? NL80211_CHAN_NO_HT :
					NL80211_CHAN_HT20);
	} else {
		memset(def, 0, sizeof(*def));
		def->chan = c->chan;
		def->width = w->width == 80 ? NL80211_CHAN_WIDTH_80 : NL80211_CHAN_WIDTH_40;
		def->center_freq1 = 5000 + 5 * (u32)w->center;
	}
	c->vht_ours = w->mode == WIDE_MODE_VHT ? wide_vht_cap_ours(sb->vht_cap.cap, b->w.vht_cap_ie) : 0;
	pr_info("mac: connect: %pM: channel %u, %s%s (center %u); the BSS: %s%s%s%s%s", c->bssid, primary,
		wide_mode_str(w->mode), w->mode == WIDE_MODE_LEGACY ? "" : w->width == 80 ? "80" : w->width == 40 ? "40" :
		"20", w->center, w->bss_width >= 160 ? "160 MHz" : w->bss_width == 80 ? "80 MHz" :
		w->bss_width == 40 ? "40 MHz" : w->bss_width ? "20 MHz" : "-", b->w.vht_cap ? ", VHT" : "",
		b->w.he_cap ? ", HE (not used)" : "", w->why ? "; limited by: " : "", wide_why_str(w->why));
}

/* ieee80211_prep_connection() for the current candidate; 0 or an RTW_ASSOC_* status */
static int mlme_prep (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	struct mac_cand *cand = &c->cand[c->cur];
	struct ieee80211_hw *hw = &mh->hw;
	struct ieee80211_vif *vif = mlme_vif(mh);
	struct ieee80211_bss_conf *bc = &vif->bss_conf;
	struct ieee80211_supported_band *sb;
	struct mac_bss_params *b = &c->bss;
	struct cfg80211_chan_def def;
	struct mac_chanctx *ctx;
	struct mac_sta *ms;
	int ret, status;

	ether_addr_copy(c->bssid, cand->bssid);
	memset(b, 0, sizeof(*b));
	memset(&c->wide, 0, sizeof(c->wide));
	c->wide.width = 20;
	c->wide.center = cand->channel;
	c->vht_ours = 0;
	WRITE_ONCE(c->bcn_opmode, 0);
	clear_bit(MAC_CONN_F_OPMODE, &c->flags);
	c->tries = 0;
	c->acked = false;
	c->authed = false;
	c->mfp = false;
	c->comebacks = 0;
	c->chan = mac_find_channel(hw->wiphy, cand->band, cand->channel);
	sb = hw->wiphy->bands[cand->band];
	if (!c->chan || !sb) {
		pr_warn("mac: connect: %pM: channel %u not usable", cand->bssid, cand->channel);
		return RTW_ASSOC_NO_CHANNEL;
	}
	/* a client may answer a BSS it hears on a passive channel: NO_IR forbids initiating radiation, not answering
	   (and a client does no radar detection) */
	if ((c->chan->flags & (IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR)) &&
	    !mac_bss_heard(cand->bssid, (u64)MLME_HEARD_MS * NSEC_PER_MSEC)) {
		pr_warn("mac: connect: %pM on passive channel %u not heard in the last %u s", cand->bssid,
			cand->channel, MLME_HEARD_MS / 1000);
		return RTW_ASSOC_NO_CHANNEL;
	}
	if (cand->body_len < 12)
		return RTW_ASSOC_FAILURE;
	b->beacon_int = get_unaligned_le16(cand->body + 8);
	b->capability = get_unaligned_le16(cand->body + 10);
	b->dtim_period = 1;
	parse_ies(b, sb, cand->body + 12, cand->body_len - 12, false);
	status = mlme_check_target(mh, cand);
	if (status)
		return status;
	if (!b->rates) {
		pr_warn("mac: connect: %pM has no rate in common with us", cand->bssid);
		return RTW_ASSOC_FAILURE;
	}
	c->info.bss_body_len = (u16)min_t(u32, cand->body_len, sizeof(c->bss_body));
	memcpy(c->bss_body, cand->body, c->info.bss_body_len);

	/* the operating channel: HT20 or no HT, or 40/80 MHz on 5 GHz */
	mlme_wide(mh, sb, &def);
	ret = mac_chanctx_use_def(mh, mh->sta_vif, &def, &ctx);
	if (ret)
		return RTW_ASSOC_FAILURE;
	c->ctx = ctx;

	/* the BSS for the driver */
	ether_addr_copy(mh->sta_vif->bssid, cand->bssid);
	ether_addr_copy(vif->cfg.ap_addr, cand->bssid);
	memcpy(vif->cfg.ssid, c->ssid, c->ssid_len);
	vif->cfg.ssid_len = c->ssid_len;
	bc->beacon_int = b->beacon_int ? b->beacon_int : 100;
	bc->basic_rates = b->basic_rates ? b->basic_rates : (b->rates & -b->rates);
	bc->use_short_preamble = !!(b->capability & CAP_SHORT_PREAMBLE) && !b->erp_barker;
	bc->use_short_slot = c->chan->band != NL80211_BAND_2GHZ || (b->capability & CAP_SHORT_SLOT);
	bc->use_cts_prot = b->erp_protection;
	bc->dtim_period = b->dtim_period;
	bc->ht_operation_mode = b->ht ? get_unaligned_le16(b->ht_oper + 2) : 0;
	bc->he_support = false;
	bc->eht_support = false;
	bc->cqm_rssi_thold = 0;
	bc->cqm_rssi_hyst = 0;
	bc->nontransmitted = 0;
	mlme_wmm_default(mh, true);
	mh->ops->link_info_changed(hw, vif, bc, BSS_CHANGED_BSSID | BSS_CHANGED_BASIC_RATES | BSS_CHANGED_BEACON_INT);

	/* the AP's station: on the list first, then the driver (sta_info_insert) */
	ms = mac_sta_alloc(mh, mh->sta_vif, cand->bssid);
	if (!ms)
		return RTW_ASSOC_FAILURE;
	sta_set_caps(mh, ms);
	smp_wmb();
	list_add_tail_rcu(&ms->list, &mh->sta_list);
	ret = mac_sta_move(mh, ms, IEEE80211_STA_NONE);
	if (ret) {
		list_del_rcu(&ms->list);
		synchronize_rcu();
		mac_sta_free(ms);
		return RTW_ASSOC_FAILURE;
	}
	rcu_assign_pointer(c->ap, ms);
	return 0;
}

/* mlme.c ieee80211_assoc_success() and ieee80211_set_associated() */
static void mlme_assoc_success (struct mac_hw *mh, const u8 *frame, unsigned int len)
{
	struct mac_conn *c = &mh->conn;
	struct ieee80211_hw *hw = &mh->hw;
	struct ieee80211_vif *vif = mlme_vif(mh);
	struct mac_sta *ms = c->ap;
	u16 aid = get_unaligned_le16(frame + MAC_HDR_LEN + 4) & 0x3fff;

	parse_ies(&c->bss, hw->wiphy->bands[c->chan->band], frame + MAC_HDR_LEN + 6, (int)len - MAC_HDR_LEN - 6, true);
	sta_set_caps(mh, ms);
	ms->sta.aid = aid;
	ms->sta.wme = c->bss.wmm_params && hw->queues >= IEEE80211_NUM_ACS;
	/* an open network has no control port: authorized at once (as in ieee80211_assoc_success); with RSN the station waits for
	   the pairwise key the OS installs after its 4-way handshake (key.c) */
	if (mac_sta_move(mh, ms, IEEE80211_STA_ASSOC) ||
	    (c->auth == RTW_AUTH_OPEN && mac_sta_move(mh, ms, IEEE80211_STA_AUTHORIZED))) {
		mlme_attempt_failed(mh, RTW_ASSOC_FAILURE, 0);
		return;
	}
	if (ms->sta.wme)
		mlme_wmm_params(mh);
	else
		mlme_wmm_default(mh, false);
	vif->cfg.assoc = true;
	vif->cfg.aid = aid;
	mh->ops->vif_cfg_changed(hw, vif, BSS_CHANGED_ASSOC);
	mh->ops->link_info_changed(hw, vif, &vif->bss_conf, BSS_CHANGED_QOS | BSS_CHANGED_ERP_CTS_PROT |
				   BSS_CHANGED_ERP_PREAMBLE | BSS_CHANGED_ERP_SLOT | BSS_CHANGED_HT |
				   BSS_CHANGED_BEACON_INFO);

	/* the connection's info; the frame lengths (request, authentication, BSS body) were set on the way */
	c->info.status = 0;
	c->info.status_code = 0;
	ether_addr_copy(c->info.bssid, c->bssid);
	c->info.band = c->chan->band;
	c->info.channel = (u8)c->chan->hw_value;
	c->info.aid = aid;
	c->info.wmm = ms->sta.wme;
	c->info.ht = ms->sta.deflink.ht_cap.ht_supported;
	/* what the AP's station uses */
	c->info.phy = ms->sta.deflink.vht_cap.vht_supported ? RTW_PHY_VHT : c->info.ht ? RTW_PHY_HT : RTW_PHY_LEGACY;
	c->info.width_mhz = (u8)(20u << ms->sta.deflink.bandwidth);
	c->info.center = c->wide.center;
	mlme_fill_security(c, &c->info);
	c->info.tx_kbps = c->info.rx_kbps = 0;
	c->info.lost_frame_len = 0;
	c->info.assoc_resp_len = (u16)min_t(unsigned int, len - MAC_HDR_LEN, sizeof(c->assoc_resp));
	memcpy(c->assoc_resp, frame + MAC_HDR_LEN, c->info.assoc_resp_len);
	c->connected_ns = c->last_rx_ns = ktime_get_ns();
	c->link_report_ns = c->connected_ns;
	c->probe_tries = 0;
	c->info.rssi = (s8)(READ_ONCE(c->rssi_x16) / 16);
	c->link_rssi = c->info.rssi;
	WRITE_ONCE(c->state, RTW_CONN_CONNECTED);
	MAC_COUNT(connects);
	pr_info("mac: connected to %pM, aid %u, channel %u, %s%s, %u stream%s%s, %s%s", c->bssid, aid, c->chan->hw_value,
		wide_mode_str(c->info.phy), c->info.phy == RTW_PHY_LEGACY ? "" : c->info.width_mhz == 80 ? "80" :
		c->info.width_mhz == 40 ? "40" : "20", ms->sta.deflink.rx_nss, ms->sta.deflink.rx_nss > 1 ? "s" : "",
		ms->sta.wme ? ", WMM" : "",
		c->auth == RTW_AUTH_SAE ? "WPA3-SAE/CCMP" : c->auth == RTW_AUTH_RSN_PSK ? "WPA2-PSK/CCMP" : "open",
		!mac_conn_rsn(c) ? "" : c->mfp ? ", MFP (the OS's 4-way handshake follows)" :
		" (the OS's 4-way handshake follows)");
	/* the BSS's Country element (sme.c __cfg80211_connect_result), and power save if it is allowed already (an
	   open network is authorized at once; with RSN, key.c recalculates after the pairwise key) */
	mac_reg_hint_country_ie(mh, c->bss.country, c->bss.country_len);
	mac_ps_recalc(mh);
	mlme_arm(mh, MLME_TICK_MS);
	mlme_events(mh, RTW_CONN_EV_CONNECTED);
}

/* the next candidate (state PREP) */
static void mlme_start (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	int status;

	pr_info("mac: connect: candidate %u of %u, %pM channel %u", c->cur + 1, c->n_cands, c->cand[c->cur].bssid,
		c->cand[c->cur].channel);
	status = mlme_prep(mh);
	if (status) {
		mlme_attempt_failed(mh, status, 0);
		return;
	}
	WRITE_ONCE(c->state, RTW_CONN_AUTH);
	if (c->auth == RTW_AUTH_SAE)
		mac_sae_start(mh);      /* the OS gives the commit first */
	else if (mlme_send_auth(mh))
		mlme_attempt_failed(mh, RTW_ASSOC_FAILURE, 0);
}

/* the current request got no answer in time, or was not acknowledged */
static void mlme_retry (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	bool auth = c->state == RTW_CONN_AUTH;
	int ret;

	if (auth && c->auth == RTW_AUTH_SAE) {
		mac_sae_retry(mh);      /* SAE has steps of its own */
		return;
	}
	if (c->tries >= MLME_TRIES) {
		if (auth)
			mlme_attempt_failed(mh, c->acked ? RTW_ASSOC_AUTH_NO_RESP : RTW_ASSOC_AUTH_NO_ACK, 0);
		else
			mlme_attempt_failed(mh, c->acked ? RTW_ASSOC_ASSOC_NO_RESP : RTW_ASSOC_ASSOC_NO_ACK, 0);
		return;
	}
	ret = auth ? mlme_send_auth(mh) : mlme_send_assoc(mh);
	if (ret)
		mlme_attempt_failed(mh, RTW_ASSOC_FAILURE, 0);
}

/* ---- frames from the BSS ---- */
/* From rx.c (NAPI thread): an MLME frame from the BSS of the attempt or connection? */
bool mac_mlme_wants (struct mac_hw *mh, const u8 *hdr, unsigned int len)
{
	const u8 *own = mh->hw.wiphy->perm_addr;
	u16 stype = get_unaligned_le16(hdr) & IEEE80211_FCTL_STYPE;

	if (len < MAC_HDR_LEN || !ether_addr_equal(hdr + 10, mh->conn.bssid) ||
	    !ether_addr_equal(hdr + 16, mh->conn.bssid))
		return false;
	switch (stype) {
	case IEEE80211_STYPE_DEAUTH:
	case IEEE80211_STYPE_DISASSOC:
		return ether_addr_equal(hdr + 4, own) || is_broadcast_ether_addr(hdr + 4);
	case IEEE80211_STYPE_AUTH:
	case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
	case IEEE80211_FC0_SUBTYPE_REASSOC_RESP:
	case IEEE80211_STYPE_ACTION:
		return ether_addr_equal(hdr + 4, own);
	default:
		return false;
	}
}

/* takes the skb (FCS removed) */
void mac_mlme_rx (struct mac_hw *mh, struct sk_buff *skb)
{
	struct mac_conn *c = &mh->conn;

	if (skb_queue_len_lockless(&c->rxq) >= MLME_RXQ_MAX) {
		MAC_COUNT(rx_dropped_full);
		kfree_skb(skb);
		return;
	}
	skb_queue_tail(&c->rxq, skb);
	wiphy_work_queue(mh->hw.wiphy, &c->work);
}

/* any frame from the AP (rx.c, NAPI thread): signal and liveness */
void mac_mlme_rx_from_ap (struct mac_hw *mh, const struct ieee80211_rx_status *rs)
{
	struct mac_conn *c = &mh->conn;
	s32 avg;

	WRITE_ONCE(c->last_rx_ns, ktime_get_ns());
	if ((rs->flag & RX_FLAG_NO_SIGNAL_VAL) || !rs->signal)
		return;
	avg = READ_ONCE(c->rssi_x16);
	avg = avg ? avg + (rs->signal * 16 - avg) / 8 : rs->signal * 16;
	WRITE_ONCE(c->rssi_x16, avg);
}

/* A beacon of the BSS (rx.c, any context; f: the frame, len > its header). An Operating Mode Notification in it
   (mac80211 ieee80211_rx_mgmt_beacon -> ieee80211_vht_handle_opmode) goes to the work when it changes. */
void mac_mlme_beacon (struct mac_hw *mh, const u8 *f, unsigned int len)
{
	struct mac_conn *c = &mh->conn;
	unsigned int hdrlen = ieee80211_hdrlen(((const struct ieee80211_hdr *)f)->frame_control);
	const struct element *e;
	u16 v;

	if (READ_ONCE(c->state) != RTW_CONN_CONNECTED || len < hdrlen + 12)
		return;
	for_each_element_id(e, WIDE_EID_OPMODE_NOTIF, f + hdrlen + 12, len - hdrlen - 12) {
		if (e->datalen < 1)
			return;
		v = 0x100 | e->data[0];
		if (READ_ONCE(c->bcn_opmode) == v)
			return;
		WRITE_ONCE(c->bcn_opmode, v);
		set_bit(MAC_CONN_F_OPMODE, &c->flags);
		wiphy_work_queue(mh->hw.wiphy, &c->work);
		return;
	}
}


/* Authenticated by the AP (open system, or SAE once the OS said success): on to the association */
void mac_mlme_authenticated (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;

	c->authed = true;
	if (mac_sta_move(mh, c->ap, IEEE80211_STA_AUTH)) {
		mlme_attempt_failed(mh, RTW_ASSOC_FAILURE, 0);
		return;
	}
	pr_info("mac: connect: authenticated with %pM", c->bssid);
	WRITE_ONCE(c->state, RTW_CONN_ASSOC);
	c->tries = 0;
	c->acked = false;
	if (mlme_send_assoc(mh))
		mlme_attempt_failed(mh, RTW_ASSOC_FAILURE, 0);
}

/* The association comeback time (TUs) of a refused association response's Timeout Interval element, 0 = none */
static u32 mlme_comeback_tu (const u8 *p, int len)
{
	while (len >= 2 && p[1] + 2 <= len) {
		if (p[0] == EID_TIMEOUT_INTERVAL && p[1] >= 5 && p[2] == TIMEOUT_ASSOC_COMEBACK)
			return get_unaligned_le32(p + 3);
		len -= p[1] + 2;
		p += p[1] + 2;
	}
	return 0;
}

/* The AP's Operating Mode Notification action frame (vht.c ieee80211_vht_handle_opmode): the station's width and
   streams from its capabilities (sta_set_caps) narrowed to what the AP says, rtw89's rate adaptation told
   (link_sta_rc_update, as rate_control_rate_update does), the front end too (link report) */
static void mlme_opmode (struct mac_hw *mh, u8 opmode)
{
	struct mac_conn *c = &mh->conn;
	struct ieee80211_link_sta *ls = &c->ap->sta.deflink;
	u8 bw = c->sta_bw, nss = c->sta_nss;
	u32 changed = 0;

	if (!wide_opmode(opmode, &bw, &nss))
		return;
	if (bw != ls->bandwidth) {
		ls->bandwidth = bw;
		changed |= IEEE80211_RC_BW_CHANGED;
	}
	if (nss != ls->rx_nss) {
		ls->rx_nss = nss;
		changed |= IEEE80211_RC_NSS_CHANGED;
	}
	if (!changed)
		return;
	pr_info("mac: the AP's operating mode notification: %u MHz, %u streams", 20u << bw, nss);
	c->info.width_mhz = (u8)(20u << bw);
	if (mh->ops->link_sta_rc_update)
		mh->ops->link_sta_rc_update(&mh->hw, mlme_vif(mh), ls, changed);
	c->link_report_ns = 0;          /* the next tick reports the link (mlme_tick sees a "changed" rate) */
	c->link_kbps = ~0u;
}

static void mlme_rx (struct mac_hw *mh, struct sk_buff *skb)
{
	struct mac_conn *c = &mh->conn;
	const u8 *f;
	unsigned int len;
	u16 stype, v;
	u32 tu;

	/* protected frames decrypted, BIP checked, unprotected robust frames dropped under MFP (pmf.c) */
	if (!mac_pmf_rx(mh, skb))
		return;
	f = skb->data;
	len = skb->len;
	stype = get_unaligned_le16(f) & IEEE80211_FCTL_STYPE;
	switch (stype) {
	case IEEE80211_STYPE_DEAUTH:
	case IEEE80211_STYPE_DISASSOC:
		v = len >= MAC_HDR_LEN + 2 ? get_unaligned_le16(f + MAC_HDR_LEN) : 0;
		if (c->state == RTW_CONN_CONNECTED)
			mlme_lost(mh, stype == IEEE80211_STYPE_DEAUTH ? RTW_LOST_PEER_DEAUTH : RTW_LOST_PEER_DISASSOC, v,
				  f + MAC_HDR_LEN, len - MAC_HDR_LEN, false);
		else if (c->state == RTW_CONN_AUTH || c->state == RTW_CONN_ASSOC) {
			c->authed = false;      /* the AP dropped us already */
			mlme_attempt_failed(mh, c->state == RTW_CONN_AUTH ? RTW_ASSOC_AUTH_REFUSED :
					    RTW_ASSOC_ASSOC_REFUSED, v);
		}
		break;
	case IEEE80211_STYPE_AUTH:
		/* algorithm, transaction sequence, status */
		if (c->state != RTW_CONN_AUTH || len < MAC_HDR_LEN + 6)
			break;
		if (c->auth == RTW_AUTH_SAE) {
			mac_sae_rx(mh, f, len);
			break;
		}
		if (get_unaligned_le16(f + MAC_HDR_LEN) != AUTH_ALG_OPEN || get_unaligned_le16(f + MAC_HDR_LEN + 2) != 2)
			break;
		mlme_disarm(mh);
		v = get_unaligned_le16(f + MAC_HDR_LEN + 4);
		c->info.auth_resp_len = (u16)min_t(unsigned int, len - MAC_HDR_LEN, sizeof(c->auth_resp));
		memcpy(c->auth_resp, f + MAC_HDR_LEN, c->info.auth_resp_len);
		if (v) {
			mlme_attempt_failed(mh, RTW_ASSOC_AUTH_REFUSED, v);
			break;
		}
		mac_mlme_authenticated(mh);
		break;
	case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
	case IEEE80211_FC0_SUBTYPE_REASSOC_RESP:
		/* capability, status, AID */
		if (c->state != RTW_CONN_ASSOC || len < MAC_HDR_LEN + 6)
			break;
		mlme_disarm(mh);
		v = get_unaligned_le16(f + MAC_HDR_LEN + 2);
		/* "come back later" (an MFP AP checks an older association of ours with an SA Query first, e.g.
		   after a hardware restart here): ask again after the comeback time, like mac80211 assoc_data->comeback;
		   the refused request does not count as a try */
		tu = v == STATUS_REFUSED_TEMPORARILY ?
		     mlme_comeback_tu(f + MAC_HDR_LEN + 6, (int)len - MAC_HDR_LEN - 6) : 0;
		if (tu && c->comebacks < MLME_COMEBACKS) {
			u32 ms = clamp_t(u32, (u32)(((u64)tu * 1024 + 999) / 1000), MLME_COMEBACK_MIN_MS,
					 MLME_COMEBACK_MAX_MS);

			c->comebacks++;
			if (c->tries)
				c->tries--;
			MAC_COUNT(assoc_comebacks);
			pr_info("mac: connect: %pM refused the association for now; again in %u ms", c->bssid, ms);
			/* the request's tx status may still come (after the response): it must not re-arm the
			   response wait over the comeback time */
			c->wait_txt = 0;
			mlme_arm(mh, ms);
			break;
		}
		if (v) {
			c->info.assoc_resp_len = (u16)min_t(unsigned int, len - MAC_HDR_LEN, sizeof(c->assoc_resp));
			memcpy(c->assoc_resp, f + MAC_HDR_LEN, c->info.assoc_resp_len);
			mlme_attempt_failed(mh, RTW_ASSOC_ASSOC_REFUSED, v);
			break;
		}
		mlme_assoc_success(mh, f, len);
		break;
	case IEEE80211_STYPE_ACTION:
		/* block ack action frames (ADDBA request/response, DELBA) go to the aggregation code (agg.c), SA Query
		   to pmf.c, a VHT Operating Mode Notification to mlme_opmode() */
		if (c->state == RTW_CONN_CONNECTED && len >= MAC_HDR_LEN + 2 && f[MAC_HDR_LEN] == ACTION_CAT_BACK)
			mac_agg_rx_action(mh, f + MAC_HDR_LEN, len - MAC_HDR_LEN);
		else if (c->state == RTW_CONN_CONNECTED && len >= MAC_HDR_LEN + 2 && f[MAC_HDR_LEN] == ACTION_CAT_SA_QUERY)
			mac_pmf_rx_sa_query(mh, f + MAC_HDR_LEN, len - MAC_HDR_LEN);
		else if (c->state == RTW_CONN_CONNECTED && len >= MAC_HDR_LEN + 3 && f[MAC_HDR_LEN] == ACTION_CAT_VHT &&
			 f[MAC_HDR_LEN + 1] == ACTION_VHT_OPMODE_NOTIF)
			mlme_opmode(mh, f[MAC_HDR_LEN + 2]);
		break;
	default:
		break;
	}
}

/* ---- tx status and the work ---- */
/* From tx.c: DISPATCH_LEVEL inside rtw89's trx_lock, or the sender's thread: record and queue only. */
void mac_mlme_tx_status (struct mac_hw *mh, enum mac_txt txt, bool acked)
{
	struct mac_conn *c = &mh->conn;

	if (txt == MAC_TXT_DEAUTH || txt == MAC_TXT_ACTION)
		return;                 /* nothing waits for these in the state machine */
	spin_lock_bh(&c->txs_lock);
	c->txs_txt = (u8)txt;
	c->txs_acked = acked;
	c->txs_new = true;
	spin_unlock_bh(&c->txs_lock);
	wiphy_work_queue(mh->hw.wiphy, &c->work);
}

/* beacon loss reported by rtw89's firmware beacon filter: probe the AP with Null frames (mlme.c
   ieee80211_mgd_probe_ap); called with wiphy->mtx held (C2H work) */
void linuxkpi_ieee80211_beacon_loss (struct ieee80211_vif *vif)
{
	struct mac_hw *mh = mac_vif(vif)->mh;

	set_bit(MAC_CONN_F_BEACON_LOSS, &mh->conn.flags);
	wiphy_work_queue(mh->hw.wiphy, &mh->conn.work);
}

/* the connection cannot go on (rtw89's connection_loss, or key.c after the driver dropped the keys); any context */
void mac_mlme_conn_loss (struct mac_hw *mh)
{
	set_bit(MAC_CONN_F_CONN_LOSS, &mh->conn.flags);
	wiphy_work_queue(mh->hw.wiphy, &mh->conn.work);
}

void linuxkpi_ieee80211_connection_loss (struct ieee80211_vif *vif)
{
	mac_mlme_conn_loss(mac_vif(vif)->mh);
}

static int mlme_send_null (struct mac_hw *mh)
{
	struct ieee80211_vif *vif = mlme_vif(mh);
	struct sk_buff *skb = linuxkpi_ieee80211_nullfunc_get(&mh->hw, vif, 0, true);

	if (!skb)
		return -ENOMEM;
	mh->conn.probe_tries++;
	mlme_arm(mh, MLME_PROBE_WAIT_MS);
	return mac_tx_mgmt(mh, skb, mh->conn.ap, 0, MAC_TXT_NULL);
}

/* connected, every second: report a changed link (signal by 5 dB, TX rate) at most every 5 s */
static void mlme_tick (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	struct station_info *si;
	u64 now = ktime_get_ns();
	s8 rssi = (s8)(READ_ONCE(c->rssi_x16) / 16);
	u32 kbps = c->info.tx_kbps;

	mlme_arm(mh, MLME_TICK_MS);
	if (now - c->link_report_ns < (u64)MLME_LINK_REPORT_MS * NSEC_PER_MSEC)
		return;
	si = kzalloc(sizeof(*si), GFP_KERNEL);
	if (si && mh->ops->sta_statistics) {
		mh->ops->sta_statistics(&mh->hw, mlme_vif(mh), &c->ap->sta, si);
		if (si->filled & BIT_ULL(NL80211_STA_INFO_TX_BITRATE))
			kbps = cfg80211_calculate_bitrate(&si->txrate) * 100;
	}
	kfree(si);
	if (abs(rssi - c->link_rssi) < 5 && kbps == c->link_kbps)
		return;
	c->link_report_ns = now;
	c->link_rssi = rssi;
	c->link_kbps = kbps;
	c->info.rssi = rssi;
	c->info.tx_kbps = kbps;
	c->info.rx_kbps = kbps;
	mlme_events(mh, RTW_CONN_EV_LINK);
}

static void mlme_run (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	struct sk_buff *skb;
	bool txs, acked = false;
	u8 txt = 0;

	/* rtw89 is being reset (SER): nothing may reach the driver (no deauthentication, no sta_state); the
	   restart work drops the connection without it and cancels this work */
	if (READ_ONCE(mh->restarting))
		return;
	if (!mh->started || !mh->sta_vif || c->state == RTW_CONN_IDLE || c->state == RTW_CONN_DISCONNECTING) {
		skb_queue_purge(&c->rxq);
		return;
	}
	if (c->state == RTW_CONN_PREP) {
		mlme_start(mh);
		return;
	}

	/* the last request's tx status */
	spin_lock_bh(&c->txs_lock);
	txs = c->txs_new;
	if (txs) {
		txt = c->txs_txt;
		acked = c->txs_acked;
		c->txs_new = false;
	}
	spin_unlock_bh(&c->txs_lock);
	if (txs && (c->state == RTW_CONN_AUTH || c->state == RTW_CONN_ASSOC) && txt == c->wait_txt) {
		if (acked) {
			c->acked = true;
			mlme_arm(mh, c->resp_wait_ms);
		} else {
			mlme_retry(mh);
		}
	} else if (txs && c->state == RTW_CONN_CONNECTED && txt == MAC_TXT_NULL && c->probe_tries) {
		if (acked) {
			pr_info("mac: the AP answered after a beacon loss");
			c->probe_tries = 0;
			mlme_arm(mh, MLME_TICK_MS);
			mac_ps_recalc(mh);      /* power save may come back */
		} else if (c->probe_tries >= MLME_PROBE_TRIES) {
			mlme_lost(mh, RTW_LOST_UNREACHABLE, REASON_INACTIVITY, NULL, 0, true);
		} else if (mlme_send_null(mh)) {
			mlme_lost(mh, RTW_LOST_UNREACHABLE, REASON_INACTIVITY, NULL, 0, true);
		}
	}

	/* frames from the BSS */
	while (c->state != RTW_CONN_IDLE && c->state != RTW_CONN_PREP && (skb = skb_dequeue(&c->rxq))) {
		mlme_rx(mh, skb);
		kfree_skb(skb);
	}
	if (c->state == RTW_CONN_PREP) {
		mlme_start(mh);
		return;
	}

	/* connected: losses and the timer */
	if (c->state == RTW_CONN_CONNECTED) {
		if (test_and_clear_bit(MAC_CONN_F_CONN_LOSS, &c->flags)) {
			mlme_lost(mh, RTW_LOST_UNREACHABLE, REASON_INACTIVITY, NULL, 0, true);
			return;
		}
		if (test_and_clear_bit(MAC_CONN_F_OPMODE, &c->flags) && READ_ONCE(c->bcn_opmode))
			mlme_opmode(mh, (u8)READ_ONCE(c->bcn_opmode));  /* from a beacon */
		if (test_and_clear_bit(MAC_CONN_F_BEACON_LOSS, &c->flags) && !c->probe_tries) {
			MAC_COUNT(beacon_losses);
			pr_info("mac: beacon loss; probing the AP");
			/* no power save while probing (mac80211 CONNECTION_POLL); probe_tries keeps it off */
			mac_ps_leave(mh, "probing the AP");
			if (mlme_send_null(mh))
				mlme_lost(mh, RTW_LOST_UNREACHABLE, REASON_INACTIVITY, NULL, 0, true);
			return;
		}
	}
	if (!test_and_clear_bit(MAC_CONN_F_TIMEOUT, &c->flags))
		return;
	switch (c->state) {
	case RTW_CONN_AUTH:
	case RTW_CONN_ASSOC:
		mlme_retry(mh);
		break;
	case RTW_CONN_CONNECTED:
		if (!c->probe_tries)
			mlme_tick(mh);
		else if (c->probe_tries >= MLME_PROBE_TRIES || mlme_send_null(mh))
			mlme_lost(mh, RTW_LOST_UNREACHABLE, REASON_INACTIVITY, NULL, 0, true);
		break;
	default:
		break;
	}
}

static void mlme_work (struct wiphy *wiphy, struct wiphy_work *work)
{
	(void)wiphy;
	mlme_run(container_of(work, struct mac_hw, conn.work));
}

static void mlme_timer (struct wiphy *wiphy, struct wiphy_work *work)
{
	struct mac_hw *mh = container_of(work, struct mac_hw, conn.timer.work);
	struct mac_conn *c = &mh->conn;
	u64 now = ktime_get_ns();

	(void)wiphy;
	/* disarmed meanwhile */
	if (!c->deadline_ns)
		return;
	/* early: re-armed meanwhile (a later deadline), or the OS timer came before its due time (it may, by up to a
	   clock tick): wait for the rest, else the step would never time out */
	if (now + MLME_TIMER_SLACK_MS * NSEC_PER_MSEC < c->deadline_ns) {
		wiphy_delayed_work_queue(mh->hw.wiphy, &c->timer,
					 msecs_to_jiffies((u32)DIV_ROUND_UP(c->deadline_ns - now, NSEC_PER_MSEC)));
		return;
	}
	c->deadline_ns = 0;
	set_bit(MAC_CONN_F_TIMEOUT, &c->flags);
	mlme_run(mh);
}

/* rtw_linux_stop(), with wiphy->mtx held: leave (deauthenticate) without events */
void mac_mlme_stop (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;

	lockdep_assert_wiphy(mh->hw.wiphy);
	if (c->state != RTW_CONN_IDLE && mh->sta_vif)
		mlme_teardown(mh, WLAN_REASON_DEAUTH_LEAVING, c->authed);
	mac_agg_cancel(mh);
	mac_pmf_conn_end(mh);
	mac_sae_reset(mh);
	wiphy_work_cancel(mh->hw.wiphy, &c->work);
	mlme_disarm(mh);
	skb_queue_purge(&c->rxq);
	c->event_fn = NULL;
	c->event_ctx = NULL;
}

/* the hw goes away */
void mac_mlme_free (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;

	for (int i = 0; i < RTW_CONN_MAX_CANDS; i++) {
		kfree(c->cand[i].body);
		c->cand[i].body = NULL;
	}
	kfree(c->ies);
	c->ies = NULL;
	c->ie_len = 0;
	mac_keys_free(mh);              /* none left normally: leaving takes them out of the driver */
	mac_sae_reset(mh);
	skb_queue_purge(&c->rxq);
}

/* Parameters\Bandwidth (driver\probe.c, after a probe and before the front end reports its capabilities) */
void rtw_linux_set_wide (uint32_t mhz)
{
	WRITE_ONCE(wide_mhz, mhz == 20 || mhz == 40 || mhz == 80 ? mhz : RTW_WIDE_DEFAULT);
}

uint32_t rtw_linux_get_wide (void)
{
	return READ_ONCE(wide_mhz);
}

/* The OS's elements for the association request: whole elements only, at most MLME_IES_MAX bytes */
static bool mlme_ies_ok (const u8 *p, u32 len)
{
	if (len > MLME_IES_MAX)
		return false;
	while (len) {
		if (len < 2 || (u32)p[1] + 2 > len)
			return false;
		len -= (u32)p[1] + 2;
		p += p[1] + 2;
	}
	return true;
}

/* ---- entry points for the front end (rtw/glue.h) ---- */
int rtw_linux_connect (const struct rtw_connect_req *req, rtw_conn_event_fn fn, void *ctx)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_conn *c;
	int ret = 0;
	u8 auths;
	u32 i;

	if (!mh)
		return -ENODEV;
	auths = req->auths | RTW_AUTH_BIT(req->auth);
	if (!req->n_cands || req->n_cands > RTW_CONN_MAX_CANDS || !req->ssid_len ||
	    req->ssid_len > IEEE80211_MAX_SSID_LEN || req->auth > RTW_AUTH_SAE ||
	    (req->auth == RTW_AUTH_OPEN ? auths != RTW_AUTH_BIT(RTW_AUTH_OPEN) :
	     (auths & ~(RTW_AUTH_BIT(RTW_AUTH_RSN_PSK) | RTW_AUTH_BIT(RTW_AUTH_SAE)))) ||
	    (req->ie_len && (!req->ies || !mlme_ies_ok(req->ies, req->ie_len))))
		return -EINVAL;
	for (i = 0; i < req->n_cands; i++) {
		const struct rtw_conn_cand *cd = &req->cands[i];

		if (cd->body_len < 12 || cd->body_len > MAC_FRAME_MAX || cd->body_off > req->bodies_len ||
		    cd->body_len > req->bodies_len - cd->body_off ||
		    (cd->band != NL80211_BAND_2GHZ && cd->band != NL80211_BAND_5GHZ) ||
		    !is_valid_ether_addr(cd->bssid))
			return -EINVAL;
	}
	c = &mh->conn;
	if (!mac_lock_driver(mh))
		ret = -EBUSY;           /* a hardware restart is still under way */
	else if (!mh->started || !mh->sta_vif)
		ret = -ENOTCONN;
	else if (mh->listen_ctx || c->state != RTW_CONN_IDLE)
		ret = -EBUSY;
	if (ret) {
		wiphy_unlock(mh->hw.wiphy);
		return ret;
	}
	/* ieee80211_prep_connection() ends a running scan first */
	mac_scan_cancel(mh);
	for (i = 0; i < RTW_CONN_MAX_CANDS; i++) {
		struct mac_cand *cand = &c->cand[i];

		kfree(cand->body);
		memset(cand, 0, sizeof(*cand));
		if (i >= req->n_cands)
			continue;
		cand->body = kmemdup(req->bodies + req->cands[i].body_off, req->cands[i].body_len, GFP_KERNEL);
		if (!cand->body) {
			ret = -ENOMEM;
			break;
		}
		ether_addr_copy(cand->bssid, req->cands[i].bssid);
		cand->band = req->cands[i].band;
		cand->channel = req->cands[i].channel;
		cand->body_len = req->cands[i].body_len;
	}
	kfree(c->ies);
	c->ies = NULL;
	c->ie_len = 0;
	if (!ret && req->ie_len) {
		c->ies = kmemdup(req->ies, req->ie_len, GFP_KERNEL);
		if (!c->ies)
			ret = -ENOMEM;
		else
			c->ie_len = (u16)req->ie_len;
	}
	if (!ret) {
		c->n_cands = req->n_cands;
		c->cur = 0;
		c->n_attempts = 0;
		memcpy(c->ssid, req->ssid, req->ssid_len);
		c->ssid_len = req->ssid_len;
		c->auth = req->auth;
		c->auths = auths;
		c->conn_flags = req->flags;
		mac_sae_clear_ind(mh);  /* nothing of an earlier attempt goes to the OS any more */
		memset(&c->info, 0, sizeof(c->info));
		c->rssi_x16 = 0;
		c->link_kbps = 0;
		c->flags = 0;
		skb_queue_purge(&c->rxq);
		mac_rx_flush(mh);       /* no data frame of an earlier connection goes up on this one */
		c->event_fn = fn;
		c->event_ctx = ctx;
		WRITE_ONCE(c->state, RTW_CONN_PREP);
		wiphy_work_queue(mh->hw.wiphy, &c->work);
	}
	wiphy_unlock(mh->hw.wiphy);
	if (!ret)
		pr_info("mac: connect: %u candidates, %s%s%s, %u bytes of the OS's elements, 5 GHz up to %u MHz%s",
			req->n_cands, auths == RTW_AUTH_BIT(RTW_AUTH_OPEN) ? "open" :
			auths == RTW_AUTH_BIT(RTW_AUTH_RSN_PSK) ? "WPA2-PSK/CCMP" :
			auths == RTW_AUTH_BIT(RTW_AUTH_SAE) ? "WPA3-SAE/CCMP" : "WPA3-SAE or WPA2-PSK/CCMP",
			!(req->flags & RTW_CONN_F_MFP) ? "" : mac_pmf_usable() ? ", MFP allowed" : ", MFP allowed (off here)",
			(req->flags & RTW_CONN_F_EXCLUDE_UNENCRYPTED) ? " (exclude unencrypted)" : "", req->ie_len,
			READ_ONCE(wide_mhz), (req->flags & RTW_CONN_F_NO_VHT) ? ", no VHT (the OS's PHY list)" : "");
	return ret;
}

void rtw_linux_connect_cancel (void)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_conn *c;

	if (!mh)
		return;
	c = &mh->conn;
	/* while a hardware restart is still under way, it ends the attempt itself (and reports it failed) */
	if (mac_lock_driver(mh) &&
	    (c->state == RTW_CONN_PREP || c->state == RTW_CONN_AUTH || c->state == RTW_CONN_ASSOC)) {
		pr_info("mac: connect: cancelled");
		if (c->n_attempts < ARRAY_SIZE(c->attempts))
			mlme_fill_info(mh, &c->attempts[c->n_attempts++], RTW_ASSOC_ABORTED, 0);
		mlme_teardown(mh, WLAN_REASON_DEAUTH_LEAVING, c->authed);
		wiphy_work_cancel(mh->hw.wiphy, &c->work);
		mlme_events(mh, RTW_CONN_EV_ATTEMPT | RTW_CONN_EV_FAILED);
	}
	wiphy_unlock(mh->hw.wiphy);
}

void rtw_linux_disconnect (uint16_t reason, int tx)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_conn *c;

	if (!mh)
		return;
	c = &mh->conn;
	/* while a hardware restart is still under way, it drops the connection itself (its loss report then finds the
	   front end disconnected already) */
	if (mac_lock_driver(mh) && c->state != RTW_CONN_IDLE && mh->sta_vif) {
		pr_info("mac: disconnecting from %pM, reason %u", c->bssid, reason);
		if (c->state == RTW_CONN_CONNECTED)
			MAC_COUNT(disconnects);
		mlme_teardown(mh, reason, tx && c->authed);
		wiphy_work_cancel(mh->hw.wiphy, &c->work);
		c->info.status = RTW_LOST_BY_HOST;
		c->info.status_code = reason;
	}
	wiphy_unlock(mh->hw.wiphy);
}

int rtw_linux_conn_get (struct rtw_conn_info *info, void *buf, uint32_t len)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_conn *c;
	u32 need;
	u8 *p = buf;

	memset(info, 0, sizeof(*info));
	if (!mh)
		return -ENODEV;
	c = &mh->conn;
	wiphy_lock(mh->hw.wiphy);
	if (c->n_attempts) {
		*info = c->attempts[0];
		memmove(&c->attempts[0], &c->attempts[1], (c->n_attempts - 1) * sizeof(c->attempts[0]));
		c->n_attempts--;
		wiphy_unlock(mh->hw.wiphy);
		return 1;
	}
	*info = c->info;
	need = info->assoc_req_len + info->assoc_resp_len + info->auth_resp_len + info->bss_body_len +
	       info->lost_frame_len;
	if (buf && len < need) {
		wiphy_unlock(mh->hw.wiphy);
		return -ENOSPC;
	}
	if (buf) {
		memcpy(p, c->assoc_req, info->assoc_req_len);
		p += info->assoc_req_len;
		memcpy(p, c->assoc_resp, info->assoc_resp_len);
		p += info->assoc_resp_len;
		memcpy(p, c->auth_resp, info->auth_resp_len);
		p += info->auth_resp_len;
		memcpy(p, c->bss_body, info->bss_body_len);
		p += info->bss_body_len;
		memcpy(p, c->lost_frame, info->lost_frame_len);
	}
	wiphy_unlock(mh->hw.wiphy);
	return 0;
}
