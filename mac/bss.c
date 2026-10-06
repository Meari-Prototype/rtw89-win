// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: the BSS table. Beacons and probe responses (every received frame comes through mac_bss_rx() from
 * rx.c, which owns the skb) fill a BSS table (BSSID, SSID, channel, signal, capabilities, security, PHY) that
 * rtw_linux_bss_get() copies out for the driver (rtwdiag.ps1 bss).
 * Each entry also keeps the body of the last beacon and of the last probe response (from the Timestamp field
 * on), which rtw_linux_bss_export() hands to the WiFiCx front end for its BSS list indications.
 * The table is static, so the driver can read it at any time, also without a hw. It is cleared when a scan or
 * listening starts. Called from the driver's NAPI poll (a work queue thread, PASSIVE_LEVEL) or its interrupt
 * thread; the table lock is a spin lock, so DISPATCH_LEVEL callers are fine too.
 */
#include "mac.h"

#define BSS_MAX                         128
#define BSS_LOG_NEW                     24      /* new BSSs logged one by one per scan/listening session */
#define BSS_BODY_MAX                    2304    /* frame body limit (802.11-2020 9.2.4.7) */
#define BSS_HDR_LEN                     24      /* management frame MAC header */

#define EID_HT_OPERATION                61
#define EID_EXTENSION                   255
#define EID_EXT_HE_CAPABILITY           35
#define EID_EXT_EHT_CAPABILITY          108
#define CAPABILITY_PRIVACY              0x0010

struct bss_entry {
	struct rtw_bss pub;
	u64 last_ns;
	bool used;
	u8 *body[2];                    /* last beacon [0] and probe response [1] body, kmalloc'ed */
	u16 body_len[2];
};

static struct {
	spinlock_t lock;                /* zero = unlocked, valid before rtw_linux_init() */
	struct bss_entry e[BSS_MAX];
	u32 used;
	struct rtw_bss_list st;         /* counters; count/total/state/channel are filled in on read */
	u32 logged;                     /* new BSSs logged in this session */
	u32 unlogged;
	u64 session_ns;
	bool first_beacon_logged;
} bss;

_Static_assert(sizeof(struct rtw_bss) == 64, "rtw_bss layout");
_Static_assert(sizeof(struct rtw_bss_list) == 40, "rtw_bss_list layout");
_Static_assert(sizeof(struct rtw_bss_rec) == 32, "rtw_bss_rec layout");
_Static_assert(BSS_MAX == RTW_BSS_EXPORT_END, "export cursor end");

/* ---- elements ---- */
struct bss_ies {
	const u8 *ssid;
	u8 ssid_len;
	u8 ds_chan;
	u8 ht_chan;
	u8 phy;
	u8 security;
	u8 akm;
	u8 pairwise;
	u8 group;
	bool rsn;
};

static const u8 oui_rsn[3] = { 0x00, 0x0f, 0xac };
static const u8 oui_ms[3] = { 0x00, 0x50, 0xf2 };

static u8 cipher_bit (const u8 *s, const u8 *oui)
{
	if (memcmp(s, oui, 3))
		return RTW_BSS_CIPHER_OTHER;
	switch (s[3]) {
	case 1: case 5: return RTW_BSS_CIPHER_WEP;
	case 2:         return RTW_BSS_CIPHER_TKIP;
	case 4:         return RTW_BSS_CIPHER_CCMP;
	case 8:         return RTW_BSS_CIPHER_GCMP;
	case 9:         return RTW_BSS_CIPHER_GCMP256;
	case 10:        return RTW_BSS_CIPHER_CCMP256;
	default:        return RTW_BSS_CIPHER_OTHER;
	}
}

static u8 akm_bits (const u8 *s, const u8 *oui)
{
	if (memcmp(s, oui, 3))
		return RTW_BSS_AKM_OTHER;
	switch (s[3]) {
	case 1: case 5: case 11: case 12:       return RTW_BSS_AKM_8021X;
	case 3: case 13:                        return RTW_BSS_AKM_8021X | RTW_BSS_AKM_FT;
	case 2: case 6:                         return RTW_BSS_AKM_PSK;
	case 4:                                 return RTW_BSS_AKM_PSK | RTW_BSS_AKM_FT;
	case 8: case 24:                        return RTW_BSS_AKM_SAE;
	case 9: case 25:                        return RTW_BSS_AKM_SAE | RTW_BSS_AKM_FT;
	case 18:                                return RTW_BSS_AKM_OWE;
	default:                                return RTW_BSS_AKM_OTHER;
	}
}

/* RSN element body, or the WPA vendor element body after its OUI and type: version, group cipher, pairwise
   cipher list, AKM list (all but the version optional) */
static void parse_rsn (const u8 *p, int len, const u8 *oui, struct bss_ies *ie)
{
	int n, i;

	if (len < 2)
		return;
	p += 2;
	len -= 2;
	if (len < 4)
		return;
	ie->group = cipher_bit(p, oui);
	p += 4;
	len -= 4;
	if (len < 2)
		return;
	n = get_unaligned_le16(p);
	p += 2;
	len -= 2;
	for (i = 0; i < n && len >= 4; i++, p += 4, len -= 4)
		ie->pairwise |= cipher_bit(p, oui);
	if (i < n || len < 2)
		return;
	n = get_unaligned_le16(p);
	p += 2;
	len -= 2;
	for (i = 0; i < n && len >= 4; i++, p += 4, len -= 4)
		ie->akm |= akm_bits(p, oui);
}

static void parse_ies (const u8 *p, int len, struct bss_ies *ie)
{
	while (len >= 2 && p[1] + 2 <= len) {
		const u8 *d = p + 2;
		u8 id = p[0], l = p[1];

		switch (id) {
		case WLAN_EID_SSID:
			if (!ie->ssid && l <= IEEE80211_MAX_SSID_LEN) {
				ie->ssid = d;
				ie->ssid_len = l;
			}
			break;
		case WLAN_EID_DS_PARAMS:
			if (l >= 1)
				ie->ds_chan = d[0];
			break;
		case EID_HT_OPERATION:
			if (l >= 1)
				ie->ht_chan = d[0];
			break;
		case WLAN_EID_HT_CAPABILITY:
			ie->phy |= RTW_BSS_PHY_HT;
			break;
		case WLAN_EID_VHT_CAPABILITY:
			ie->phy |= RTW_BSS_PHY_VHT;
			break;
		case EID_EXTENSION:
			if (l >= 1 && d[0] == EID_EXT_HE_CAPABILITY)
				ie->phy |= RTW_BSS_PHY_HE;
			else if (l >= 1 && d[0] == EID_EXT_EHT_CAPABILITY)
				ie->phy |= RTW_BSS_PHY_EHT;
			break;
		case WLAN_EID_RSN:
			ie->security |= RTW_BSS_SEC_RSN;
			ie->rsn = true;
			ie->akm = ie->pairwise = ie->group = 0;
			parse_rsn(d, l, oui_rsn, ie);
			break;
		case WLAN_EID_VENDOR_SPECIFIC:
			if (l >= 4 && !memcmp(d, oui_ms, 3) && d[3] == 1) {
				ie->security |= RTW_BSS_SEC_WPA;
				if (!ie->rsn)
					parse_rsn(d + 4, l - 4, oui_ms, ie);
			}
			break;
		default:
			break;
		}
		len -= l + 2;
		p += l + 2;
	}
}

static u8 freq_to_channel (u32 freq)
{
	if (freq == 2484)
		return 14;
	if (freq >= 2412 && freq <= 2472)
		return (u8)((freq - 2407) / 5);
	if (freq >= 5000 && freq <= 5900)
		return (u8)((freq - 5000) / 5);
	return 0;
}

static bool ssid_hidden (const u8 *ssid, u8 len)
{
	for (int i = 0; i < len; i++)
		if (ssid[i])
			return false;
	return true;
}

/* printable copy of an SSID for the log */
static void ssid_str (char *out, const u8 *ssid, u8 len)
{
	for (int i = 0; i < len; i++)
		out[i] = ssid[i] >= 0x20 && ssid[i] < 0x7f ? (char)ssid[i] : '?';
	out[len] = '\0';
}

/* With bss.lock held: kfree() is fine at DISPATCH_LEVEL (non-paged pool). */
static void bss_free_bodies (struct bss_entry *e)
{
	for (int i = 0; i < 2; i++) {
		kfree(e->body[i]);
		e->body[i] = NULL;
		e->body_len[i] = 0;
	}
}

static struct bss_entry *bss_find_or_add (const u8 *bssid, bool *added)
{
	struct bss_entry *e, *free = NULL, *oldest = NULL;

	*added = false;
	for (e = bss.e; e < bss.e + BSS_MAX; e++) {
		if (!e->used) {
			if (!free)
				free = e;
			continue;
		}
		if (ether_addr_equal(e->pub.bssid, bssid))
			return e;
		if (!oldest || e->last_ns < oldest->last_ns)
			oldest = e;
	}
	e = free ? free : oldest;       /* full: the one heard least recently goes */
	if (free)
		bss.used++;
	bss_free_bodies(e);
	memset(e, 0, sizeof(*e));
	e->used = true;
	ether_addr_copy(e->pub.bssid, bssid);
	*added = true;
	return e;
}

/* ---- receive (every frame from rx.c; only beacons and probe responses are used here) ---- */
void mac_bss_rx (struct ieee80211_hw *hw, struct sk_buff *skb)
{
	const struct ieee80211_rx_status *rs = IEEE80211_SKB_RXCB(skb);
	const struct ieee80211_mgmt *mgmt = (const struct ieee80211_mgmt *)skb->data;
	const unsigned int hdr_len = offsetof(struct ieee80211_mgmt, u.beacon.variable);
	unsigned int len = skb->len;
	struct bss_entry *e;
	struct bss_ies ie = { 0 };
	struct rtw_bss snap;
	bool beacon, added, first_beacon = false, log_new = false;
	u8 chan, band;
	u16 freq, body_len;
	u8 *body = NULL;
	u64 now, session;

	if (ieee80211_hw_check(hw, RX_INCLUDES_FCS))
		len = len >= FCS_LEN ? len - FCS_LEN : 0;
	if ((rs->flag & (RX_FLAG_FAILED_FCS_CRC | RX_FLAG_FAILED_PLCP_CRC)) || len < 24) {
		spin_lock_bh(&bss.lock);
		bss.st.rx_frames++;
		bss.st.rx_bad++;
		spin_unlock_bh(&bss.lock);
		return;
	}
	beacon = ieee80211_is_beacon(mgmt->frame_control);
	if (!beacon && !ieee80211_is_probe_resp(mgmt->frame_control)) {
		spin_lock_bh(&bss.lock);
		bss.st.rx_frames++;
		spin_unlock_bh(&bss.lock);
		return;
	}
	if (len < hdr_len) {
		spin_lock_bh(&bss.lock);
		bss.st.rx_frames++;
		bss.st.rx_bad++;
		spin_unlock_bh(&bss.lock);
		return;
	}
	parse_ies(mgmt->u.beacon.variable, (int)(len - hdr_len), &ie);

	/* a copy of the frame body for the BSS list indications; without one only the fields are updated */
	body_len = (u16)min_t(unsigned int, len - BSS_HDR_LEN, BSS_BODY_MAX);
	if (len - BSS_HDR_LEN <= BSS_BODY_MAX) {
		body = kmalloc(body_len, GFP_ATOMIC);
		if (body)
			memcpy(body, skb->data + BSS_HDR_LEN, body_len);
	}

	/* the channel the BSS operates on: DS Parameter Set (2.4 GHz), HT Operation, else where it was heard */
	chan = ie.ds_chan ? ie.ds_chan : ie.ht_chan ? ie.ht_chan : freq_to_channel(rs->freq);
	if (chan) {
		band = chan <= 14 ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ;
		freq = (u16)ieee80211_channel_to_frequency(chan, band);
	} else {
		band = rs->band;
		freq = rs->freq;
	}

	spin_lock_bh(&bss.lock);
	now = ktime_get_ns();           /* under the lock: never before the session_ns of a concurrent clear */
	session = bss.session_ns;
	bss.st.rx_frames++;
	if (beacon) {
		bss.st.rx_beacons++;
		if (!bss.first_beacon_logged) {
			bss.first_beacon_logged = true;
			first_beacon = true;
		}
	} else {
		bss.st.rx_probe_resps++;
	}
	e = bss_find_or_add(mgmt->bssid, &added);
	e->last_ns = now;
	e->pub.channel = chan;
	e->pub.band = band;
	e->pub.freq = freq;
	e->pub.capability = get_unaligned_le16(&mgmt->u.beacon.capab_info);
	e->pub.beacon_int = get_unaligned_le16(&mgmt->u.beacon.beacon_int);
	if (!(rs->flag & RX_FLAG_NO_SIGNAL_VAL))
		e->pub.signal = rs->signal;
	e->pub.phy = ie.phy;
	e->pub.security = ie.security | ((e->pub.capability & CAPABILITY_PRIVACY) ? RTW_BSS_SEC_PRIVACY : 0);
	e->pub.akm = ie.akm;
	e->pub.pairwise = ie.pairwise;
	e->pub.group = ie.group;
	e->pub.flags |= beacon ? RTW_BSS_F_BEACON : RTW_BSS_F_PROBE_RESP;
	if (body) {
		u8 *old = e->body[!beacon];

		e->body[!beacon] = body;
		e->body_len[!beacon] = body_len;
		body = old;             /* freed below, outside the lock */
	}
	if (ie.ssid && !ssid_hidden(ie.ssid, ie.ssid_len)) {
		memset(e->pub.ssid, 0, sizeof(e->pub.ssid));
		memcpy(e->pub.ssid, ie.ssid, ie.ssid_len);
		e->pub.ssid_len = ie.ssid_len;
	} else if (beacon) {
		e->pub.flags |= RTW_BSS_F_HIDDEN;       /* keeps an SSID a probe response told us */
	}
	if (e->pub.frames != U32_MAX)
		e->pub.frames++;
	if (added) {
		if (bss.logged < BSS_LOG_NEW) {
			bss.logged++;
			log_new = true;
		} else {
			bss.unlogged++;
		}
	}
	snap = e->pub;
	spin_unlock_bh(&bss.lock);
	kfree(body);

	if (first_beacon)
		pr_info("mac: first beacon %llu ms after the scan or listening started", (now - session) / NSEC_PER_MSEC);
	if (log_new) {
		char s[IEEE80211_MAX_SSID_LEN + 1];

		ssid_str(s, snap.ssid, snap.ssid_len);
		pr_info("mac: bss %pM ch %u %d dBm sec %#x \"%s\"", snap.bssid, snap.channel, snap.signal,
			snap.security, s);
	}
}

/* A beacon or probe response of this BSS arrived within max_age_ns. NO_IR forbids initiating radiation, not
   answering: a client may authenticate and associate with a BSS it currently hears on a NO_IR (or radar) channel. */
bool mac_bss_heard (const u8 *bssid, u64 max_age_ns)
{
	u64 now = ktime_get_ns();
	bool heard = false;

	spin_lock_bh(&bss.lock);
	for (struct bss_entry *e = bss.e; e < bss.e + BSS_MAX; e++) {
		if (e->used && ether_addr_equal(e->pub.bssid, bssid)) {
			heard = now - e->last_ns <= max_age_ns;
			break;
		}
	}
	spin_unlock_bh(&bss.lock);
	return heard;
}

/* ---- table management ---- */
static void session_start_locked (void)
{
	bss.st.rx_frames = bss.st.rx_beacons = bss.st.rx_probe_resps = bss.st.rx_bad = 0;
	bss.logged = bss.unlogged = 0;
	bss.first_beacon_logged = false;
	bss.session_ns = ktime_get_ns();
}

void mac_bss_clear (void)
{
	spin_lock_bh(&bss.lock);
	for (struct bss_entry *e = bss.e; e < bss.e + BSS_MAX; e++)
		bss_free_bodies(e);
	memset(bss.e, 0, sizeof(bss.e));
	bss.used = 0;
	session_start_locked();
	spin_unlock_bh(&bss.lock);
}

/* A scan that keeps the table: its counters and first-beacon time start from zero all the same. */
void mac_bss_new_session (void)
{
	spin_lock_bh(&bss.lock);
	session_start_locked();
	spin_unlock_bh(&bss.lock);
}

u32 mac_bss_count (void)
{
	return READ_ONCE(bss.used);
}

/* "mac: <what> <value><unit>: <counters>" */
static void log_session (const char *what, u32 value, const char *unit)
{
	u32 frames, beacons, probe_resps, bad, used, unlogged;

	spin_lock_bh(&bss.lock);
	frames = bss.st.rx_frames;
	beacons = bss.st.rx_beacons;
	probe_resps = bss.st.rx_probe_resps;
	bad = bss.st.rx_bad;
	used = bss.used;
	unlogged = bss.unlogged;
	spin_unlock_bh(&bss.lock);
	pr_info("mac: %s %u%s: %u BSS (%u not logged), rx %u frames, %u beacons, %u probe responses, %u bad",
		what, value, unit, used, unlogged, frames, beacons, probe_resps, bad);
}

void mac_bss_scan_done (u32 ms, bool aborted)
{
	spin_lock_bh(&bss.lock);
	bss.st.scans++;
	spin_unlock_bh(&bss.lock);
	log_session(aborted ? "scan aborted after" : "scan done after", ms, " ms");
}

void mac_bss_listen_done (u32 channel)
{
	log_session("stopped listening on channel", channel, "");
}

uint32_t rtw_linux_bss_get (struct rtw_bss_list *list, struct rtw_bss *out, uint32_t max)
{
	u64 now = ktime_get_ns();
	u32 n = 0;

	spin_lock_bh(&bss.lock);
	*list = bss.st;
	for (struct bss_entry *e = bss.e; e < bss.e + BSS_MAX; e++) {
		if (!e->used || n >= max)
			continue;
		out[n] = e->pub;
		out[n].age_ms = (u32)min_t(u64, (now - e->last_ns) / NSEC_PER_MSEC, U32_MAX);
		n++;
	}
	list->total = bss.used;
	spin_unlock_bh(&bss.lock);
	list->count = n;
	list->state = READ_ONCE(mac_state);
	list->channel = READ_ONCE(mac_listen_channel);
	return n;
}

uint32_t rtw_linux_bss_export (uint64_t since_ns, uint32_t *cursor, void *buf, uint32_t len)
{
	u64 now = ktime_get_ns();
	u8 *out = buf;
	u32 used = 0;

	spin_lock_bh(&bss.lock);
	for (; *cursor < BSS_MAX; (*cursor)++) {
		const struct bss_entry *e = &bss.e[*cursor];
		struct rtw_bss_rec *r;
		u32 need;

		if (!e->used || e->last_ns < since_ns || (!e->body[0] && !e->body[1]))
			continue;
		need = ALIGN(sizeof(*r) + e->body_len[0] + e->body_len[1], RTW_BSS_REC_ALIGN);
		if (need > len - used)
			break;
		r = (struct rtw_bss_rec *)(out + used);
		memset(r, 0, sizeof(*r));
		ether_addr_copy(r->bssid, e->pub.bssid);
		r->channel = e->pub.channel;
		r->band = e->pub.band;
		r->signal = e->pub.signal;
		r->flags = e->pub.flags;
		r->freq = e->pub.freq;
		r->beacon_len = e->body_len[0];
		r->presp_len = e->body_len[1];
		r->age_ms = (u32)min_t(u64, (now - e->last_ns) / NSEC_PER_MSEC, U32_MAX);
		r->last_ns = e->last_ns;
		if (e->body_len[0])
			memcpy(r + 1, e->body[0], e->body_len[0]);
		if (e->body_len[1])
			memcpy((u8 *)(r + 1) + e->body_len[0], e->body[1], e->body_len[1]);
		used += need;
	}
	spin_unlock_bh(&bss.lock);
	return used;
}
