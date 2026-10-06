// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: the frames of our group owner's BSS as pure functions (apframe.h). No locks, no allocation, no
 * kernel calls: tools-dev\apframe-test.c builds this file on the PC as well (RTW_APFRAME_HOST).
 */
#include "apframe.h"

#define RSN_SEL(t)                      (0x000fac00u | (t))
#define RSN_CIPHER_CCMP                 RSN_SEL(4)
#define RSN_AKM_PSK                     RSN_SEL(2)
#define RSN_CAP_MFPR                    0x0040
/* RSN capabilities we advertise: 16 PTKSA replay counters (the hardware queues reorder frames of different TIDs, so
   the client must keep a replay counter per TID; hostapd does the same with WMM) */
#define RSN_CAPS_OURS                   0x000c
#define HT_OPER_LEN                     22

static void put16 (u8 *p, u16 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static u16 get16 (const u8 *p)
{
	return (u16)(p[0] | p[1] << 8);
}

static u32 get_sel (const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/* a writer that stops (and remembers it) at the end of the buffer */
struct apf_w {
	u8 *p, *end;
	bool full;
};

static void w_bytes (struct apf_w *w, const void *d, size_t n)
{
	if (w->full || (size_t)(w->end - w->p) < n) {
		w->full = true;
		return;
	}
	memcpy(w->p, d, n);
	w->p += n;
}

static void w_u8 (struct apf_w *w, u8 v)
{
	w_bytes(w, &v, 1);
}

static void w_u16 (struct apf_w *w, u16 v)
{
	u8 b[2];

	put16(b, v);
	w_bytes(w, b, 2);
}

static void w_ie (struct apf_w *w, u8 id, const void *d, u8 len)
{
	w_u8(w, id);
	w_u8(w, len);
	w_bytes(w, d, len);
}

u16 apf_capability (const struct apf_bss *b)
{
	u16 cap = APF_CAP_ESS;

	if (b->rsn)
		cap |= APF_CAP_PRIVACY;
	if (!b->band5)
		cap |= APF_CAP_SHORT_PREAMBLE | APF_CAP_SHORT_SLOT;
	return cap;
}

size_t apf_put_tim (u8 *out, u8 dtim_count, u8 dtim_period, bool group, u32 aid_bits)
{
	u8 bm[4];
	int first = -1, last = -1, n1, i;
	size_t len;

	for (i = 0; i < 4; i++) {
		bm[i] = (u8)(aid_bits >> (8 * i));
		if (bm[i] && first < 0)
			first = i;
		if (bm[i])
			last = i;
	}
	/* 802.11-2020 9.4.2.5.1: N1 the largest even number with no bit below octet N1, N2 the last octet with a bit;
	   none set: one octet of 0 at offset 0 */
	n1 = first < 0 ? 0 : first & ~1;
	if (last < 0)
		last = 0;
	out[0] = APF_EID_TIM;
	out[2] = dtim_count;
	out[3] = dtim_period;
	out[4] = (u8)(n1 | (group ? 1 : 0));
	len = 0;
	for (i = n1; i <= last; i++)
		out[5 + len++] = bm[i];
	out[1] = (u8)(3 + len);
	return 2 + 3 + len;
}

const u8 *apf_find_ie (const u8 *p, size_t len, u8 id)
{
	while (len >= 2 && (size_t)p[1] + 2 <= len) {
		if (p[0] == id)
			return p;
		len -= (size_t)p[1] + 2;
		p += p[1] + 2;
	}
	return NULL;
}

bool apf_ies_ok (const u8 *p, size_t len)
{
	while (len) {
		if (len < 2 || (size_t)p[1] + 2 > len)
			return false;
		len -= (size_t)p[1] + 2;
		p += p[1] + 2;
	}
	return true;
}

/* Supported Rates (the first 8) and, after the elements between them, Extended Supported Rates (the rest) */
static void w_rates (struct apf_w *w, const struct apf_bss *b)
{
	u8 n = b->n_rates < 8 ? b->n_rates : 8;

	w_ie(w, APF_EID_RATES, b->rates, n);
}

static void w_ext_rates (struct apf_w *w, const struct apf_bss *b)
{
	if (b->n_rates > 8)
		w_ie(w, APF_EID_EXT_RATES, b->rates + 8, (u8)(b->n_rates - 8));
}

static void w_ht (struct apf_w *w, const struct apf_bss *b)
{
	u8 oper[HT_OPER_LEN];

	if (!b->ht)
		return;
	w_ie(w, APF_EID_HT_CAP, b->ht_cap, APF_HT_CAP_LEN);
	/* HT Operation: primary channel, 20 MHz only (no secondary channel, STA channel width 0), no protection, no
	   basic MCS */
	memset(oper, 0, sizeof(oper));
	oper[0] = b->channel;
	w_ie(w, APF_EID_HT_OPER, oper, sizeof(oper));
}

/* WMM Parameter element (WMM spec 2.2.2): the EDCA parameters the clients use, hostapd's defaults (BE AIFSN 3,
   ECWmin 4, ECWmax 10; BK 7, 4, 10; VI 2, 3, 4, TXOP 94; VO 2, 2, 3, TXOP 47); no U-APSD */
static void w_wmm (struct apf_w *w)
{
	static const u8 wmm[24] = {
		0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x00, 0x00,
		0x03, 0xa4, 0x00, 0x00,         /* ACI 0 (BE) */
		0x27, 0xa4, 0x00, 0x00,         /* ACI 1 (BK) */
		0x42, 0x43, 0x5e, 0x00,         /* ACI 2 (VI) */
		0x62, 0x32, 0x2f, 0x00,         /* ACI 3 (VO) */
	};

	w_ie(w, APF_EID_VENDOR, wmm, sizeof(wmm));
}

static void w_country (struct apf_w *w, const struct apf_bss *b)
{
	u8 d[3 + 3 * APF_TRIPLETS_MAX + 1];
	u8 n = 3, i;

	if (!b->country[0] || !b->n_triplets)
		return;
	memcpy(d, b->country, 3);
	for (i = 0; i < b->n_triplets && i < APF_TRIPLETS_MAX; i++) {
		memcpy(d + n, b->triplets[i], 3);
		n += 3;
	}
	if (n & 1)
		d[n++] = 0;     /* pad to an even length (802.11-2020 9.4.2.8) */
	w_ie(w, APF_EID_COUNTRY, d, n);
}

static void w_rsn (struct apf_w *w)
{
	static const u8 rsn[20] = {
		0x01, 0x00,                     /* version 1 */
		0x00, 0x0f, 0xac, 0x04,         /* group CCMP */
		0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,     /* pairwise CCMP */
		0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,     /* AKM PSK */
		RSN_CAPS_OURS & 0xff, RSN_CAPS_OURS >> 8,
	};

	w_ie(w, APF_EID_RSN, rsn, sizeof(rsn));
}

size_t apf_build_bss_body (const struct apf_bss *b, int kind, const u8 *tim, size_t tim_len, const u8 *extra,
	size_t extra_len, u8 *out, size_t cap, size_t *tim_off)
{
	struct apf_w w = { out, out + cap, false };
	static const u8 zero8[8] = { 0 };
	u8 ds = b->channel, erp = 0;

	*tim_off = 0;
	if (extra && !apf_ies_ok(extra, extra_len))
		extra_len = 0;
	w_bytes(&w, zero8, 8);                  /* timestamp: the hardware fills it in */
	w_u16(&w, b->beacon_int);
	w_u16(&w, apf_capability(b));
	w_ie(&w, APF_EID_SSID, b->ssid, b->ssid_len);
	w_rates(&w, b);
	w_ie(&w, APF_EID_DS, &ds, 1);
	if (kind == APF_BEACON && tim && tim_len) {
		*tim_off = (size_t)(w.p - out);
		w_bytes(&w, tim, tim_len);
	}
	w_country(&w, b);
	if (!b->band5)
		w_ie(&w, APF_EID_ERP, &erp, 1); /* no non-ERP stations, no protection */
	w_ext_rates(&w, b);
	if (b->rsn && !(extra_len && apf_find_ie(extra, extra_len, APF_EID_RSN)))
		w_rsn(&w);
	w_ht(&w, b);
	w_wmm(&w);
	if (extra_len)
		w_bytes(&w, extra, extra_len);
	return w.full ? 0 : (size_t)(w.p - out);
}

size_t apf_build_assoc_resp (const struct apf_bss *b, u16 status, u16 aid, bool wmm, const u8 *extra,
	size_t extra_len, u8 *out, size_t cap)
{
	struct apf_w w = { out, out + cap, false };

	if (extra && !apf_ies_ok(extra, extra_len))
		extra_len = 0;
	w_u16(&w, apf_capability(b));
	w_u16(&w, status);
	w_u16(&w, status == APF_STATUS_SUCCESS ? (u16)(aid | 0xc000) : 0);
	w_rates(&w, b);
	w_ext_rates(&w, b);
	if (status == APF_STATUS_SUCCESS) {
		w_ht(&w, b);
		if (wmm)
			w_wmm(&w);
	}
	if (extra_len)
		w_bytes(&w, extra, extra_len);
	return w.full ? 0 : (size_t)(w.p - out);
}

/* RSN element body: version, group cipher, pairwise ciphers, AKMs, capabilities (the rest is not needed here) */
static void parse_rsn (struct apf_assoc_req *r, const u8 *d, size_t len)
{
	size_t n, i;

	r->rsn = true;
	r->rsn_bad = true;
	r->rsn_group = RSN_CIPHER_CCMP;
	r->rsn_ccmp = true;
	r->rsn_psk = false;     /* the default AKM is 802.1X */
	r->rsn_caps = 0;
	if (len < 2 || get16(d) != 1)
		return;
	d += 2;
	len -= 2;
	if (len >= 4) {
		r->rsn_group = get_sel(d);
		d += 4;
		len -= 4;
	} else if (len) {
		return;
	}
	if (len >= 2) {
		n = get16(d);
		if (len < 2 + 4 * n)
			return;
		r->rsn_ccmp = false;
		for (i = 0; i < n; i++)
			r->rsn_ccmp |= get_sel(d + 2 + 4 * i) == RSN_CIPHER_CCMP;
		d += 2 + 4 * n;
		len -= 2 + 4 * n;
	} else if (len) {
		return;
	}
	if (len >= 2) {
		n = get16(d);
		if (len < 2 + 4 * n)
			return;
		for (i = 0; i < n; i++)
			r->rsn_psk |= get_sel(d + 2 + 4 * i) == RSN_AKM_PSK;
		d += 2 + 4 * n;
		len -= 2 + 4 * n;
	} else if (len) {
		return;
	}
	if (len >= 2)
		r->rsn_caps = get16(d);
	r->rsn_bad = false;
}

int apf_parse_assoc_req (const u8 *body, size_t len, bool reassoc, struct apf_assoc_req *r)
{
	static const u8 oui_ms[3] = { 0x00, 0x50, 0xf2 };
	size_t fixed = reassoc ? 10 : 4;        /* capability, listen interval (, current AP address) */
	const u8 *p;

	memset(r, 0, sizeof(*r));
	if (len < fixed)
		return -1;
	r->capability = get16(body);
	r->listen_int = get16(body + 2);
	p = body + fixed;
	len -= fixed;
	while (len >= 2) {
		const u8 *d = p + 2;
		u8 id = p[0], l = p[1];

		if ((size_t)l + 2 > len)
			return -1;
		switch (id) {
		case APF_EID_SSID:
			if (!r->has_ssid && l <= 32) {
				r->has_ssid = true;
				r->ssid = d;
				r->ssid_len = l;
			}
			break;
		case APF_EID_RATES:
		case APF_EID_EXT_RATES:
			for (u8 i = 0; i < l && r->n_rates < sizeof(r->rates); i++)
				r->rates[r->n_rates++] = d[i] & 0x7f;
			break;
		case APF_EID_RSN:
			if (!r->rsn)
				parse_rsn(r, d, l);
			break;
		case APF_EID_HT_CAP:
			if (l >= APF_HT_CAP_LEN) {
				r->ht = true;
				memcpy(r->ht_cap, d, APF_HT_CAP_LEN);
			}
			break;
		case APF_EID_VENDOR:
			/* WMM Information (subtype 0) or Parameter (1) element: OUI 00:50:f2, type 2 */
			if (l >= 7 && !memcmp(d, oui_ms, 3) && d[3] == 2 && (d[4] == 0 || d[4] == 1)) {
				r->wmm = true;
				r->wmm_qos_info = d[6];
			}
			break;
		default:
			break;
		}
		len -= (size_t)l + 2;
		p += l + 2;
	}
	return len ? -1 : 0;
}

u16 apf_check_assoc_req (const struct apf_bss *b, const struct apf_assoc_req *r)
{
	u8 i, j;

	if (!r->has_ssid || r->ssid_len != b->ssid_len || memcmp(r->ssid, b->ssid, b->ssid_len))
		return APF_STATUS_UNSPECIFIED;
	/* every basic rate of ours must be among the client's */
	for (i = 0; i < b->n_rates; i++) {
		bool found = false;

		if (!(b->rates[i] & 0x80))
			continue;
		for (j = 0; j < r->n_rates && !found; j++)
			found = r->rates[j] == (b->rates[i] & 0x7f);
		if (!found)
			return APF_STATUS_RATES;
	}
	if (!b->rsn)
		return APF_STATUS_SUCCESS;
	if (!r->rsn || r->rsn_bad)
		return APF_STATUS_INVALID_IE;
	if (r->rsn_group != RSN_CIPHER_CCMP)
		return APF_STATUS_GROUP_CIPHER;
	if (!r->rsn_ccmp)
		return APF_STATUS_PAIRWISE_CIPHER;
	if (!r->rsn_psk)
		return APF_STATUS_AKMP;
	if (r->rsn_caps & RSN_CAP_MFPR)
		return APF_STATUS_MFP_POLICY;       /* it requires management frame protection, which the GO lacks */
	return APF_STATUS_SUCCESS;
}
