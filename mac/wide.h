/*
 * rtw89win mac: the station's channel width and VHT capabilities on 5 GHz, as pure functions on
 * byte buffers, so that tools-dev\wide-test.c can check them on the PC:
 *  - which channel to use with a BSS: its HT and VHT Operation elements, the limit of Parameters\Bandwidth and the
 *    channels' flags (mac80211 mlme.c ieee80211_determine_ap_chan, util.c ieee80211_chandef_ht_oper/vht_oper,
 *    cfg80211 chan.c cfg80211_chandef_usable; read for reference only);
 *  - the HT and VHT capabilities we advertise in the association request (mlme.c ieee80211_add_ht_ie/add_vht_ie);
 *  - what the AP's station gets (vht.c ieee80211_vht_cap_ie_to_sta_vht_cap), and an Operating Mode Notification.
 * mac\mlme.c fills struct wide_bss while it parses the BSS's elements and turns the results into the channel context
 * and the station's fields. 80 MHz at most: rtw89 v7.2 gives the 8852B 20/40/80 MHz only (rtw8852b.c).
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_MAC_WIDE_H
#define RTW_MAC_WIDE_H

#ifdef RTW_WIDE_HOST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#else
#include <rtw/linux.h>
#endif

#define WIDE_EID_HT_CAP                 45
#define WIDE_EID_HT_OPER                61
#define WIDE_EID_VHT_CAP                191
#define WIDE_EID_VHT_OPER               192
#define WIDE_EID_OPMODE_NOTIF           199
#define WIDE_EID_EXT                    255
#define WIDE_EXT_HE_CAP                 35
#define WIDE_EXT_HE_OPER                36
#define WIDE_HT_CAP_LEN                 26
#define WIDE_HT_OPER_LEN                22
#define WIDE_VHT_CAP_LEN                12
#define WIDE_VHT_OPER_LEN               5

/* HT Capability Information bits (802.11-2020 9.4.2.55.2; the values of IEEE80211_HT_CAP_*) */
#define WIDE_HT_SUP_WIDTH_20_40         0x0002
#define WIDE_HT_SM_PS                   0x000c
#define WIDE_HT_SM_PS_DISABLED          0x000c
#define WIDE_HT_SGI_40                  0x0040
#define WIDE_HT_DSSSCCK40               0x1000
#define WIDE_HT_MAX_AMSDU               0x0800
/* HT Operation Information, first byte */
#define WIDE_HT_SEC_MASK                0x03
#define WIDE_HT_SEC_ABOVE               0x01
#define WIDE_HT_SEC_BELOW               0x03

/* VHT Capabilities Information bits (802.11-2020 9.4.2.157.2; the values of IEEE80211_VHT_CAP_*) */
#define WIDE_VHT_MAX_MPDU_MASK          0x00000003      /* 0: 3895, 1: 7991, 2: 11454 octets */
#define WIDE_VHT_SUPP_WIDTH_MASK        0x0000000c
#define WIDE_VHT_SUPP_WIDTH_160         0x00000004
#define WIDE_VHT_SUPP_WIDTH_160_80P80   0x00000008
#define WIDE_VHT_RXLDPC                 0x00000010
#define WIDE_VHT_SHORT_GI_80            0x00000020
#define WIDE_VHT_SHORT_GI_160           0x00000040
#define WIDE_VHT_TXSTBC                 0x00000080
#define WIDE_VHT_RXSTBC_MASK            0x00000700
#define WIDE_VHT_SU_BEAMFORMER          0x00000800
#define WIDE_VHT_SU_BEAMFORMEE          0x00001000
#define WIDE_VHT_BEAMFORMEE_STS_MASK    0x0000e000
#define WIDE_VHT_SOUNDING_DIM_MASK      0x00070000
#define WIDE_VHT_MU_BEAMFORMER          0x00080000
#define WIDE_VHT_MU_BEAMFORMEE          0x00100000
#define WIDE_VHT_TXOP_PS                0x00200000
#define WIDE_VHT_HTC_VHT                0x00400000
#define WIDE_VHT_MAX_AMPDU_EXP_MASK     0x03800000
#define WIDE_VHT_LINK_ADAPT_MASK        0x0c000000      /* VHT link adaptation (unsolicited MFB, MRQ and MFB) */
#define WIDE_VHT_RX_ANT_PATTERN         0x10000000
#define WIDE_VHT_TX_ANT_PATTERN         0x20000000
#define WIDE_VHT_EXT_NSS_BW_MASK        0xc0000000
#define WIDE_VHT_EXT_NSS_BW_CAPABLE     0x2000          /* in the TX highest data rate field */
#define WIDE_VHT_MCS_NONE               3               /* a stream's 2 bits in an MCS map: not supported */

/* rtw_conn_info.phy (compat\include\rtw\glue.h) */
#define WIDE_MODE_LEGACY                0
#define WIDE_MODE_HT                    1
#define WIDE_MODE_VHT                   2

/* Why the channel is narrower than the BSS's (for the log) */
#define WIDE_WHY_NONE                   0
#define WIDE_WHY_SETTING                1       /* Parameters\Bandwidth */
#define WIDE_WHY_PRIMARY                2       /* the HT Operation element names another primary channel */
#define WIDE_WHY_PAIR                   3       /* secondary channel offset not a 40 MHz channel pair */
#define WIDE_WHY_VHT_OPER               4       /* the VHT Operation element does not match the channel or HT */
#define WIDE_WHY_FLAGS                  5       /* a channel of it is disabled or has no 40/80 MHz (regulatory) */
#define WIDE_WHY_NO_VHT                 6       /* the OS's ActivePhyTypeList leaves VHT out */
#define WIDE_WHY_SHARED                 7       /* the GO's channel context is another width */
#define WIDE_WHY_VHT_MCS                8       /* the AP's VHT RX MCS map supports no stream */

/* A 5 GHz channel's flags, as far as the width checks care (mac\mlme.c maps IEEE80211_CHAN_*) */
#define WIDE_CH_DISABLED                0x1     /* not a channel of the band, disabled, or no OFDM */
#define WIDE_CH_NO_HT40PLUS             0x2
#define WIDE_CH_NO_HT40MINUS            0x4
#define WIDE_CH_NO_80MHZ                0x8
typedef u32 (*wide_flags_fn) (void *ctx, u8 channel);

/* the BSS's elements that decide the channel and the AP station's capabilities (wide_parse_ie) */
struct wide_bss {
	bool ht_cap, ht_oper;           /* HT Capabilities (26 bytes), HT Operation (22 bytes) */
	bool vht_cap, vht_oper;
	bool he_cap, he_oper;           /* present (HE is not used: for the log only) */
	bool opmode;                    /* Operating Mode Notification (association response only) */
	u8 ht_primary;                  /* HT Operation: primary channel */
	u8 ht_param;                    /* ... first byte of the HT Operation Information */
	u8 opmode_val;
	u8 vht_cap_ie[WIDE_VHT_CAP_LEN];
	u8 vht_oper_ie[WIDE_VHT_OPER_LEN];
};

/* the channel the station uses with a BSS (wide_choose) */
struct wide_chan {
	u8 mode;                        /* WIDE_MODE_* */
	u8 width;                       /* MHz: 20, 40, 80 */
	u8 center;                      /* channel number of the center frequency (the primary channel at 20 MHz) */
	u8 why;                         /* WIDE_WHY_*: the first reason it is narrower than the BSS (or no VHT) */
	u8 bss_width;                   /* MHz the BSS operates on, as its elements say (20/40/80/160) */
};

/* the AP's station's VHT capabilities (wide_vht_sta) */
struct wide_vht {
	bool ok;                        /* VHT usable with it */
	u32 cap;
	u16 rx_mcs_map, rx_highest;     /* what it receives (our TX), as the rate adaptation reads them */
	u16 tx_mcs_map, tx_highest;
};

/* One element of a beacon/probe response (resp false) or of the association response (resp true: the VHT
   Capabilities replace the beacon's, the Operating Mode Notification counts; operation elements are ignored). */
void wide_parse_ie (struct wide_bss *w, u8 id, const u8 *d, u8 len, bool resp);
/* The 40 MHz channel pair of a primary channel: the center channel number, 0 if there is none that way
   (above: the secondary channel is above the primary). */
u8 wide_center40 (u8 primary, bool above);
/* The 80 MHz channel block of a primary channel: its center channel number, 0 if none. */
u8 wide_center80 (u8 primary);
/* cfg80211_chandef_usable() for a station: every 20 MHz channel of it exists and is not disabled (no OFDM), the
   primary allows 40 MHz that way, every channel allows 80 MHz. */
bool wide_usable (u8 primary, u8 width, u8 center, wide_flags_fn fn, void *ctx);
/* The channel for a 5 GHz BSS with HT, at most max_mhz (40 or 80); vht_ok: the band has VHT and the OS allows it.
   out starts as HT20 on the primary; it is widened as far as the elements, the limit and the flags allow. VHT only
   at the BSS's full width (80 MHz at most): narrower than that it is HT (mac80211 ieee80211_chandef_downgrade). */
void wide_choose (const struct wide_bss *w, u8 primary, u32 max_mhz, bool vht_ok, wide_flags_fn fn, void *ctx,
	struct wide_chan *out);
/* The HT Capability Information of our association request (own: the band's): no 40 MHz below 40 MHz, no DSSS/CCK
   in 40 MHz, no 7935-octet A-MSDU, SM power save disabled. */
u16 wide_ht_cap_ours (u16 own, u8 width);
/* The VHT Capabilities Information of our association request (own: the band's, ap_vht_cap: the AP's element body):
   80 MHz at most, a 3895-octet MPDU, no +HTC, no MU beamformee (no VHT group ID management here), SU beamformee
   only with an SU beamformer AP, the beamformee STS not above the AP's field (mac80211 ieee80211_add_vht_ie). */
u32 wide_vht_cap_ours (u32 own, const u8 *ap_vht_cap);
/* ours: what we advertised (wide_vht_cap_ours); own_rx/own_tx: the band's MCS maps */
void wide_vht_sta (u32 ours, u16 own_rx, u16 own_tx, const u8 *ap_vht_cap, struct wide_vht *out);
/* The streams of an MCS map (the highest one supported), 0 if none */
u8 wide_vht_nss (u16 map);
/* The A-MSDU limit of a VHT maximum MPDU length field */
u16 wide_vht_amsdu_len (u32 cap);
/* An Operating Mode Notification (the Operating Mode field): *bw (0 20, 1 40, 2 80, 3 160 MHz = IEEE80211_STA_RX_BW_*)
   and *nss come in as the station's capabilities and go out narrowed to what it says. false: it says nothing about
   what the AP receives (RX NSS type 1, beamforming), nothing changed. */
bool wide_opmode (u8 opmode, u8 *bw, u8 *nss);
/* The VHT Capabilities element (14 bytes) */
size_t wide_put_vht_cap (u8 *out, u32 cap, u16 rx_map, u16 rx_highest, u16 tx_map, u16 tx_highest);
const char *wide_why_str (u8 why);
const char *wide_mode_str (u8 mode);

#endif
