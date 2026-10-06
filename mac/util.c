// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: stateless cfg80211/mac80211 helpers (channel numbers, bit rates, TIDs).
 */
#include "mac.h"

uint32_t linuxkpi_ieee80211_channel_to_frequency (uint32_t chan, enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_2GHZ:
		if (chan == 14)
			return 2484;
		return chan && chan < 14 ? 2407 + chan * 5 : 0;
	case NL80211_BAND_5GHZ:
		if (chan >= 182 && chan <= 196)         /* 4.9 GHz (Japan) */
			return 4000 + chan * 5;
		return chan ? 5000 + chan * 5 : 0;
	case NL80211_BAND_6GHZ:
		if (chan == 2)
			return 5935;
		return chan && chan <= 233 ? 5950 + chan * 5 : 0;
	default:
		return 0;
	}
}

/*
 * Bit rate in 100 kbit/s. HT/VHT/HE data rate = data subcarriers * coded bits per subcarrier * NSS / symbol time.
 * Coded bits per subcarrier for MCS 0..11, times 12 so they are integers (BPSK 1/2 = 0.5 ... 1024-QAM 5/6 = 8.33).
 */
static const uint8_t mcs_bits12[12] = { 6, 12, 18, 24, 36, 48, 54, 60, 72, 80, 90, 100 };

static uint32_t ofdm_rate (uint32_t nsd, uint32_t mcs, uint32_t nss, uint32_t tsym_100ns)
{
	if (mcs >= ARRAY_SIZE(mcs_bits12) || !nss)
		return 0;
	return (uint32_t)((uint64_t)nsd * mcs_bits12[mcs] * nss * 100 / (12 * tsym_100ns));
}

uint32_t linuxkpi_cfg80211_calculate_bitrate (struct rate_info *r)
{
	static const uint16_t nsd_ht[] = { 52, 108, 234, 468 };         /* 20/40/80/160 MHz */
	static const uint16_t nsd_he[] = { 234, 468, 980, 1960 };
	static const uint8_t he_gi[] = { 8, 16, 32 };                   /* 0.8/1.6/3.2 us, in 100 ns */
	int bw;

	switch (r->bw) {
	case RATE_INFO_BW_20:  bw = 0; break;
	case RATE_INFO_BW_40:  bw = 1; break;
	case RATE_INFO_BW_80:  bw = 2; break;
	case RATE_INFO_BW_160: bw = 3; break;
	default:               bw = -1; break;
	}
	if (r->flags & RATE_INFO_FLAGS_MCS) {
		if (bw < 0 || bw > 1)
			return 0;
		return ofdm_rate(nsd_ht[bw], r->mcs % 8, r->mcs / 8 + 1,
				 (r->flags & RATE_INFO_FLAGS_SHORT_GI) ? 36 : 40);
	}
	if (r->flags & RATE_INFO_FLAGS_VHT_MCS) {
		if (bw < 0)
			return 0;
		return ofdm_rate(nsd_ht[bw], r->mcs, r->nss, (r->flags & RATE_INFO_FLAGS_SHORT_GI) ? 36 : 40);
	}
	if (r->flags & RATE_INFO_FLAGS_HE_MCS) {
		if (bw < 0 || r->he_gi >= ARRAY_SIZE(he_gi))
			return 0;
		return ofdm_rate(nsd_he[bw], r->mcs, r->nss, 128 + he_gi[r->he_gi]);
	}
	return r->legacy;
}

/* TID of a QoS data frame; IEEE80211_NUM_TIDS for other frames (only asked for when nonqos_ok). */
uint8_t linuxkpi_ieee80211_get_tid (struct ieee80211_hdr *hdr, bool nonqos_ok)
{
	if (!ieee80211_is_data_qos(hdr->frame_control)) {
		WARN_ONCE(!nonqos_ok, "mac: get_tid on a non-QoS frame");
		return IEEE80211_NUM_TIDS;
	}
	return *ieee80211_get_qos_ctl(hdr) & IEEE80211_QOS_CTL_TID_MASK;
}
