/*
 * rtw89win mac: the frames of our group owner's BSS, as pure functions on byte buffers, so that
 * tools-dev\apframe-test.c can check them on the PC: the beacon and probe response bodies, the TIM element, the
 * association response body, and the parsing and checking of a (re)association request. mac\ap.c fills struct
 * apf_bss from the channel, the band's rates and HT capabilities and the regulatory domain.
 * Element order: 802.11-2020 Table 9-34 (beacon), 9-40 (probe response), 9-37 (association response).
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_MAC_APFRAME_H
#define RTW_MAC_APFRAME_H

#ifdef RTW_APFRAME_HOST
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

#define APF_EID_SSID                    0
#define APF_EID_RATES                   1
#define APF_EID_DS                      3
#define APF_EID_TIM                     5
#define APF_EID_COUNTRY                 7
#define APF_EID_ERP                     42
#define APF_EID_HT_CAP                  45
#define APF_EID_RSN                     48
#define APF_EID_EXT_RATES               50
#define APF_EID_HT_OPER                 61
#define APF_EID_VENDOR                  221

#define APF_CAP_ESS                     0x0001
#define APF_CAP_PRIVACY                 0x0010
#define APF_CAP_SHORT_PREAMBLE          0x0020
#define APF_CAP_SHORT_SLOT              0x0400

#define APF_RATES_MAX                   12
#define APF_TRIPLETS_MAX                8
#define APF_HT_CAP_LEN                  26
#define APF_FIXED_LEN                   12      /* timestamp, beacon interval, capability */
#define APF_TIM_MAX                     (2 + 3 + 32)    /* TIM element with AIDs up to 255 */

/* 802.11 status codes of the association response */
#define APF_STATUS_SUCCESS              0
#define APF_STATUS_UNSPECIFIED          1
#define APF_STATUS_RATES                18      /* the client does not support all basic rates */
#define APF_STATUS_MFP_POLICY           31      /* robust management frame policy violation (it requires MFP) */
#define APF_STATUS_INVALID_IE           40
#define APF_STATUS_GROUP_CIPHER         41
#define APF_STATUS_PAIRWISE_CIPHER      42
#define APF_STATUS_AKMP                 43

/* what our BSS advertises */
struct apf_bss {
	u8 bssid[6];
	u8 ssid[32];
	u8 ssid_len;
	u8 channel;
	bool band5;                     /* 5 GHz, else 2.4 GHz */
	bool rsn;                       /* WPA2-Personal: privacy, RSN element (PSK, CCMP) */
	u16 beacon_int;
	u8 dtim_period;
	u8 n_rates;
	u8 rates[APF_RATES_MAX];        /* 500 kbit/s units; 0x80 set = basic */
	bool ht;
	u8 ht_cap[APF_HT_CAP_LEN];      /* HT Capabilities element body */
	u8 country[3];                  /* Country string; country[0] == 0: no Country element */
	u8 n_triplets;
	u8 triplets[APF_TRIPLETS_MAX][3];       /* first channel, number of channels, maximum power (dBm) */
};

/* A (re)association request, parsed. The pointers point into the body. */
struct apf_assoc_req {
	u16 capability;
	u16 listen_int;
	const u8 *ssid;
	u8 ssid_len;
	bool has_ssid;
	u8 n_rates;
	u8 rates[16];                   /* 500 kbit/s units, basic bit cleared */
	bool rsn;                       /* RSN element present */
	bool rsn_bad;                   /* ... but malformed */
	u32 rsn_group;                  /* suite selectors, big endian (00-0F-AC:4 = 0x000fac04) */
	bool rsn_ccmp;                  /* CCMP among the pairwise ciphers */
	bool rsn_psk;                   /* PSK among the AKMs */
	u16 rsn_caps;
	bool ht;
	u8 ht_cap[APF_HT_CAP_LEN];
	bool wmm;                       /* WMM Information or Parameter element */
	u8 wmm_qos_info;
};

#define APF_BEACON                      0
#define APF_PROBE_RESP                  1

u16 apf_capability (const struct apf_bss *b);
/* The TIM element: DTIM count and period, the group bit, the partial virtual bitmap of aid_bits (bit n = AID n,
   n < 32). Returns its length (with the 2-byte header). */
size_t apf_put_tim (u8 *out, u8 dtim_count, u8 dtim_period, bool group, u32 aid_bits);
/* A beacon (tim = its TIM element) or probe response body (tim NULL) from the Timestamp on, extra = the OS's
   additional elements (whole elements; an RSN element there replaces ours). Returns the length, 0 if it does not fit;
   *tim_off = offset of the TIM element in the body. */
size_t apf_build_bss_body (const struct apf_bss *b, int kind, const u8 *tim, size_t tim_len, const u8 *extra,
	size_t extra_len, u8 *out, size_t cap, size_t *tim_off);
/* An association response body from the Capability field on: status, AID (0 when refused), rates, HT, WMM
   (wmm: the client uses WMM), extra (the OS's vendor elements). Returns the length, 0 if it does not fit. */
size_t apf_build_assoc_resp (const struct apf_bss *b, u16 status, u16 aid, bool wmm, const u8 *extra,
	size_t extra_len, u8 *out, size_t cap);
/* Whole elements only (no truncated one at the end). */
bool apf_ies_ok (const u8 *p, size_t len);
/* The element with this ID in p, NULL if none (or the elements run out of p). */
const u8 *apf_find_ie (const u8 *p, size_t len, u8 id);
/* A (re)association request body (from the Capability field on). 0, or -1 if malformed. */
int apf_parse_assoc_req (const u8 *body, size_t len, bool reassoc, struct apf_assoc_req *r);
/* What we answer it ourselves before the OS is asked: APF_STATUS_SUCCESS, or the status to refuse it with. */
u16 apf_check_assoc_req (const struct apf_bss *b, const struct apf_assoc_req *r);

#endif
