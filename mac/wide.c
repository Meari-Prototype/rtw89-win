// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: the station's channel width and VHT capabilities as pure functions (wide.h). No locks, no
 * allocation, no kernel calls: tools-dev\wide-test.c builds this file on the PC as well (RTW_WIDE_HOST).
 */
#include "wide.h"

static u16 get16 (const u8 *p)
{
	return (u16)(p[0] | p[1] << 8);
}

static u32 get32 (const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void put16 (u8 *p, u16 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static void put32 (u8 *p, u32 v)
{
	put16(p, (u16)v);
	put16(p + 2, (u16)(v >> 16));
}

void wide_parse_ie (struct wide_bss *w, u8 id, const u8 *d, u8 len, bool resp)
{
	switch (id) {
	case WIDE_EID_HT_CAP:
		if (!resp && len >= WIDE_HT_CAP_LEN)
			w->ht_cap = true;
		break;
	case WIDE_EID_HT_OPER:
		/* the channel comes from the beacon/probe response only (some APs get it wrong in the association
		   response; mac80211 mlme.c ieee80211_assoc_config_link) */
		if (!resp && len >= WIDE_HT_OPER_LEN) {
			w->ht_oper = true;
			w->ht_primary = d[0];
			w->ht_param = d[1];
		}
		break;
	case WIDE_EID_VHT_CAP:
		if (len >= WIDE_VHT_CAP_LEN) {
			w->vht_cap = true;
			memcpy(w->vht_cap_ie, d, WIDE_VHT_CAP_LEN);
		}
		break;
	case WIDE_EID_VHT_OPER:
		if (!resp && len >= WIDE_VHT_OPER_LEN) {
			w->vht_oper = true;
			memcpy(w->vht_oper_ie, d, WIDE_VHT_OPER_LEN);
		}
		break;
	case WIDE_EID_OPMODE_NOTIF:
		if (resp && len >= 1) {
			w->opmode = true;
			w->opmode_val = d[0];
		}
		break;
	case WIDE_EID_EXT:
		if (!resp && len >= 1 && d[0] == WIDE_EXT_HE_CAP)
			w->he_cap = true;
		if (!resp && len >= 1 && d[0] == WIDE_EXT_HE_OPER)
			w->he_oper = true;
		break;
	default:
		break;
	}
}

/* 5 GHz 40 MHz channel pairs and 80 MHz blocks by center channel (802.11-2020 Annex E, Table E-4; 149-177 too) */
static const u8 centers40[] = { 38, 46, 54, 62, 102, 110, 118, 126, 134, 142, 151, 159, 167, 175 };
static const u8 centers80[] = { 42, 58, 106, 122, 138, 155, 171 };

u8 wide_center40 (u8 primary, bool above)
{
	int c = above ? primary + 2 : primary - 2;

	for (size_t i = 0; i < sizeof(centers40); i++)
		if (centers40[i] == c)
			return (u8)c;
	return 0;
}

u8 wide_center80 (u8 primary)
{
	for (size_t i = 0; i < sizeof(centers80); i++)
		if (primary + 6 == centers80[i] || primary + 2 == centers80[i] || primary == centers80[i] + 2 ||
		    primary == centers80[i] + 6)
			return centers80[i];
	return 0;
}

bool wide_usable (u8 primary, u8 width, u8 center, wide_flags_fn fn, void *ctx)
{
	unsigned int n = width / 20, first;
	bool seen = false;

	if (n != 1 && n != 2 && n != 4)
		return false;
	if (center < 2 * (n - 1))
		return false;
	first = center - 2 * (n - 1);           /* channel numbers are 5 MHz apart, 20 MHz channels 4 apart */
	for (unsigned int i = 0; i < n; i++) {
		u8 ch = (u8)(first + 4 * i);
		u32 f = fn(ctx, ch);

		if (f & WIDE_CH_DISABLED)
			return false;
		if (width == 80 && (f & WIDE_CH_NO_80MHZ))
			return false;
		seen = seen || ch == primary;
	}
	if (!seen)
		return false;
	if (width == 40 && (fn(ctx, primary) & (center > primary ? WIDE_CH_NO_HT40PLUS : WIDE_CH_NO_HT40MINUS)))
		return false;
	return true;
}

static void narrow (struct wide_chan *o, u8 why)
{
	if (!o->why)
		o->why = why;
}

void wide_choose (const struct wide_bss *w, u8 primary, u32 max_mhz, bool vht_ok, wide_flags_fn fn, void *ctx,
	struct wide_chan *o)
{
	u8 sec, c40 = 0, c80, chw, s0, s1;
	bool match;

	memset(o, 0, sizeof(*o));
	o->mode = WIDE_MODE_HT;
	o->width = 20;
	o->center = primary;
	o->bss_width = 20;
	if (!w->ht_cap || !w->ht_oper) {
		o->mode = WIDE_MODE_LEGACY;
		return;
	}
	/* mac80211 disables HT for a primary channel mismatch; we keep an HT20 connection on our primary */
	if (w->ht_primary != primary) {
		narrow(o, WIDE_WHY_PRIMARY);
		return;
	}

	/* HT: the secondary channel offset (ieee80211_chandef_ht_oper) */
	sec = w->ht_param & WIDE_HT_SEC_MASK;
	if (sec == WIDE_HT_SEC_ABOVE || sec == WIDE_HT_SEC_BELOW) {
		o->bss_width = 40;
		c40 = wide_center40(primary, sec == WIDE_HT_SEC_ABOVE);
		if (!c40)
			narrow(o, WIDE_WHY_PAIR);
		else if (max_mhz < 40)
			narrow(o, WIDE_WHY_SETTING);
		else if (!wide_usable(primary, 40, c40, fn, ctx))
			narrow(o, WIDE_WHY_FLAGS);
		else {
			o->width = 40;
			o->center = c40;
		}
	}

	/* VHT (ieee80211_chandef_vht_oper): 80 MHz, or the 80 MHz of a 160/80+80 MHz BSS that holds the primary */
	if (!w->vht_cap || !w->vht_oper)
		return;
	if (!vht_ok) {
		narrow(o, WIDE_WHY_NO_VHT);
		return;
	}
	if (get16(w->vht_cap_ie + 4) == 0xffff) {
		narrow(o, WIDE_WHY_VHT_MCS);     /* vht.c: a VHT station must support MCS 0-7 on one stream */
		return;
	}
	o->mode = WIDE_MODE_VHT;
	chw = w->vht_oper_ie[0];
	s0 = w->vht_oper_ie[1];
	s1 = w->vht_oper_ie[2];
	if (chw == 0) {
		/* VHT20/VHT40: the HT width; narrower than the BSS: HT (ieee80211_chandef_downgrade 40 -> 20) */
		if (o->width < o->bss_width)
			o->mode = WIDE_MODE_HT;
		return;
	}
	c80 = wide_center80(primary);
	switch (chw) {
	case 1:         /* 80 MHz; CCFS1 8 away: 160 MHz, else not zero: 80+80 MHz (both with CCFS0 = our 80 MHz) */
		match = c80 && c80 == s0;
		o->bss_width = s1 ? 160 : 80;
		break;
	case 2:         /* 160 MHz, deprecated encoding: CCFS0 is its center */
		match = c80 && (c80 == s0 + 8 || s0 == c80 + 8 || c80 == s0);
		o->bss_width = 160;
		break;
	case 3:         /* 80+80 MHz, deprecated encoding */
		match = c80 && (c80 == s0 || c80 == s1);
		o->bss_width = 160;
		break;
	default:
		match = false;
		break;
	}
	/* the VHT channel must hold the HT one (mlme.c: "AP VHT information doesn't match HT, disabling VHT") */
	if (match && o->width == 40 && (c40 + 4 != c80 && c80 + 4 != c40))
		match = false;
	if (!match) {
		o->mode = WIDE_MODE_HT;
		narrow(o, WIDE_WHY_VHT_OPER);
		return;
	}
	if (max_mhz < 80)
		narrow(o, WIDE_WHY_SETTING);
	else if (!wide_usable(primary, 80, c80, fn, ctx))
		narrow(o, WIDE_WHY_FLAGS);
	else {
		o->width = 80;
		o->center = c80;
		o->why = WIDE_WHY_NONE;         /* as wide as we go (an HT pair problem no longer matters) */
		return;
	}
	/* below the 80 MHz of a VHT80/160 BSS: HT, as ieee80211_chandef_downgrade() does from 80 MHz. Our VHT
	   Capabilities would say we receive 80 MHz (a VHT station does), and we send no Operating Mode Notification. */
	o->mode = WIDE_MODE_HT;
}

u16 wide_ht_cap_ours (u16 own, u8 width)
{
	u16 cap = own;

	cap &= (u16)~(WIDE_HT_DSSSCCK40 | WIDE_HT_MAX_AMSDU | WIDE_HT_SM_PS);
	if (width < 40)
		cap &= (u16)~(WIDE_HT_SUP_WIDTH_20_40 | WIDE_HT_SGI_40);
	return (u16)(cap | WIDE_HT_SM_PS_DISABLED);
}

u32 wide_vht_cap_ours (u32 own, const u8 *ap_vht_cap)
{
	u32 ap = get32(ap_vht_cap), cap = own;

	cap &= ~(WIDE_VHT_SHORT_GI_160 | WIDE_VHT_SUPP_WIDTH_MASK | WIDE_VHT_EXT_NSS_BW_MASK);
	if (!(ap & WIDE_VHT_SU_BEAMFORMER))
		cap &= ~(WIDE_VHT_SU_BEAMFORMEE | WIDE_VHT_MU_BEAMFORMEE);
	cap &= ~(WIDE_VHT_MU_BEAMFORMEE | WIDE_VHT_HTC_VHT | WIDE_VHT_MAX_MPDU_MASK);
	if ((ap & WIDE_VHT_BEAMFORMEE_STS_MASK) < (cap & WIDE_VHT_BEAMFORMEE_STS_MASK))
		cap = (cap & ~WIDE_VHT_BEAMFORMEE_STS_MASK) | (ap & WIDE_VHT_BEAMFORMEE_STS_MASK);
	return cap;
}

void wide_vht_sta (u32 ours, u16 own_rx, u16 own_tx, const u8 *ap_vht_cap, struct wide_vht *o)
{
	u32 ci = get32(ap_vht_cap);
	u16 peer_rx = get16(ap_vht_cap + 4), peer_tx = get16(ap_vht_cap + 8);

	memset(o, 0, sizeof(*o));
	/* some as they are */
	o->cap = ci & (WIDE_VHT_RXLDPC | WIDE_VHT_TXOP_PS | WIDE_VHT_HTC_VHT | WIDE_VHT_MAX_AMPDU_EXP_MASK |
		       WIDE_VHT_LINK_ADAPT_MASK | WIDE_VHT_RX_ANT_PATTERN | WIDE_VHT_TX_ANT_PATTERN);
	o->cap |= (ci & WIDE_VHT_MAX_MPDU_MASK) < (ours & WIDE_VHT_MAX_MPDU_MASK) ? (ci & WIDE_VHT_MAX_MPDU_MASK) :
		  (ours & WIDE_VHT_MAX_MPDU_MASK);
	/* some by our own (we advertise no 160 MHz, so none of those) */
	if ((ours & WIDE_VHT_SUPP_WIDTH_MASK) == WIDE_VHT_SUPP_WIDTH_160)
		o->cap |= ci & WIDE_VHT_SUPP_WIDTH_160;
	else if ((ours & WIDE_VHT_SUPP_WIDTH_MASK) == WIDE_VHT_SUPP_WIDTH_160_80P80)
		o->cap |= ci & WIDE_VHT_SUPP_WIDTH_MASK;
	o->cap |= ci & ours & (WIDE_VHT_SHORT_GI_80 | WIDE_VHT_SHORT_GI_160);
	if (ours & WIDE_VHT_SU_BEAMFORMEE)
		o->cap |= ci & (WIDE_VHT_SU_BEAMFORMER | WIDE_VHT_SOUNDING_DIM_MASK);
	if (ours & WIDE_VHT_SU_BEAMFORMER)
		o->cap |= ci & (WIDE_VHT_SU_BEAMFORMEE | WIDE_VHT_BEAMFORMEE_STS_MASK);
	if (ours & WIDE_VHT_MU_BEAMFORMER)
		o->cap |= ci & WIDE_VHT_MU_BEAMFORMEE;
	if (ours & WIDE_VHT_MU_BEAMFORMEE)
		o->cap |= ci & WIDE_VHT_MU_BEAMFORMER;
	if (ours & WIDE_VHT_TXSTBC)
		o->cap |= ci & WIDE_VHT_RXSTBC_MASK;
	if (ours & WIDE_VHT_RXSTBC_MASK)
		o->cap |= ci & WIDE_VHT_TXSTBC;
	o->rx_highest = get16(ap_vht_cap + 6);
	o->tx_highest = get16(ap_vht_cap + 10) & (u16)~WIDE_VHT_EXT_NSS_BW_CAPABLE;

	/* the MCS sets: what it receives limited by what we send, and the other way round */
	for (int i = 0; i < 8; i++) {
		u16 orx = (own_rx >> (2 * i)) & 3, otx = (own_tx >> (2 * i)) & 3;
		u16 prx = (peer_rx >> (2 * i)) & 3, ptx = (peer_tx >> (2 * i)) & 3;

		if (ptx != WIDE_VHT_MCS_NONE && (orx == WIDE_VHT_MCS_NONE || orx < ptx))
			ptx = orx;
		if (prx != WIDE_VHT_MCS_NONE && (otx == WIDE_VHT_MCS_NONE || otx < prx))
			prx = otx;
		o->rx_mcs_map |= (u16)(prx << (2 * i));
		o->tx_mcs_map |= (u16)(ptx << (2 * i));
	}
	o->ok = o->rx_mcs_map != 0xffff;
}

u8 wide_vht_nss (u16 map)
{
	for (int i = 7; i >= 0; i--)
		if (((map >> (2 * i)) & 3) != WIDE_VHT_MCS_NONE)
			return (u8)(i + 1);
	return 0;
}

u16 wide_vht_amsdu_len (u32 cap)
{
	switch (cap & WIDE_VHT_MAX_MPDU_MASK) {
	case 2:
		return 11454;
	case 1:
		return 7991;
	default:
		return 3895;
	}
}

bool wide_opmode (u8 opmode, u8 *bw, u8 *nss)
{
	u8 b = opmode & 0x03, n = (u8)(((opmode >> 4) & 7) + 1);

	if (opmode & 0x80)
		return false;                   /* RX NSS type 1: about beamforming only (vht.c) */
	if ((opmode & 0x04) && b == 2)
		b = 3;                          /* 160/80+80 MHz: only with 80 MHz in the width field (vht.c) */
	if (b < *bw)
		*bw = b;
	if (n < *nss)
		*nss = n;
	return true;
}

size_t wide_put_vht_cap (u8 *out, u32 cap, u16 rx_map, u16 rx_highest, u16 tx_map, u16 tx_highest)
{
	out[0] = WIDE_EID_VHT_CAP;
	out[1] = WIDE_VHT_CAP_LEN;
	put32(out + 2, cap);
	put16(out + 6, rx_map);
	put16(out + 8, rx_highest);
	put16(out + 10, tx_map);
	put16(out + 12, tx_highest);
	return 2 + WIDE_VHT_CAP_LEN;
}

const char *wide_why_str (u8 why)
{
	switch (why) {
	case WIDE_WHY_SETTING:
		return "Parameters\\Bandwidth";
	case WIDE_WHY_PRIMARY:
		return "HT Operation names another primary channel";
	case WIDE_WHY_PAIR:
		return "no 40 MHz channel pair that way";
	case WIDE_WHY_VHT_OPER:
		return "VHT Operation does not match";
	case WIDE_WHY_FLAGS:
		return "regulatory (channel flags)";
	case WIDE_WHY_NO_VHT:
		return "VHT not allowed (OS or band)";
	case WIDE_WHY_SHARED:
		return "the GO's channel context";
	case WIDE_WHY_VHT_MCS:
		return "VHT MCS map empty";
	default:
		return "";
	}
}

const char *wide_mode_str (u8 mode)
{
	return mode == WIDE_MODE_VHT ? "VHT" : mode == WIDE_MODE_HT ? "HT" : "legacy";
}
