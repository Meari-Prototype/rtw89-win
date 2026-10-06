/*
 * rtw89win: 802.11 / mac80211 definitions that rtw89 (Linux v7.2) uses and the FreeBSD LinuxKPI headers do not
 * have. Values are from IEEE Std 802.11-2024 and the radiotap specification; the interfaces follow mac80211.
 * Included at the end of net/mac80211.h.
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_IEEE80211_EXTRA_H
#define RTW_IEEE80211_EXTRA_H

/* Sequence Control: sequence number in bits 4..15 */
#ifndef IEEE80211_SN_MASK
#define IEEE80211_SN_MASK                               (IEEE80211_SCTL_SEQ >> 4)
#endif
static inline u16 ieee80211_get_sn (struct ieee80211_hdr *hdr)
{
	return (u16)((le16_to_cpu(hdr->seq_ctrl) & IEEE80211_SCTL_SEQ) >> 4);
}

/* HT Block Ack window (net80211/ieee80211_ht.h; linux/ieee80211.h's IEEE80211_MAX_AMPDU_BUF_HT refers to it) */
#ifndef IEEE80211_AGGR_BAWMAX
#define IEEE80211_AGGR_BAWMAX                           64
#endif

/* radiotap HE data1: PPDU format in bits 0..1 */
#define IEEE80211_RADIOTAP_HE_DATA1_FORMAT_MASK         0x0003

/* EHT Multi-Link element, EML Capabilities: EMLSR padding delay in bits 1..3 */
#ifndef IEEE80211_EML_CAP_EML_PADDING_DELAY
#define IEEE80211_EML_CAP_EML_PADDING_DELAY             0x000e
#define IEEE80211_EML_CAP_EML_PADDING_DELAY_256US       4
#endif

/* for_each_station(sta, hw): all stations known to the driver, under the wiphy lock (mac\iface.c). The iterator
 * returns the station after prev (the first one for NULL), NULL at the end. */
struct ieee80211_sta *__ieee80211_iterate_stations (struct ieee80211_hw *, struct ieee80211_sta *);
#define for_each_station(sta, hw) \
	for (sta = __ieee80211_iterate_stations(hw, NULL); sta != NULL; sta = __ieee80211_iterate_stations(hw, sta))

#endif
