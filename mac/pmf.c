// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: management frame protection (802.11w) - mac80211's part of it (wpa.c CCMP
 * and BIP, rx.c ieee80211_drop_unencrypted_mgmt and the SA Query answer) and wpa_supplicant's (the SA Query after an
 * unprotected disconnection); both read for reference only.
 * The 8852B neither encrypts management frames (hw_mgmt_tx_encrypt = false: rtw89 gives CCMP keys
 * IEEE80211_KEY_FLAG_SW_MGMT_TX) nor decrypts or checks them outside WoWLAN (R_AX_SEC_ENG_CTRL UC/BMC_MGNT_DEC stay
 * off), so with MFP negotiated (mlme.c mlme_check_target, conn.mfp) this file does it with the kernel's AES
 * (driver\crypto.c):
 *  - TX (mac_tx_mgmt): a robust frame to the AP (deauthentication, disassociation, action frames other than public,
 *    HT, ... - block ack and SA Query included) goes out with CCMP under the pairwise key: Protected bit, CCMP header
 *    with the key's next PN (the data path's counter, so every PN is used once), MIC at the end. rtw89 sends it as it
 *    is (no hw_key; its management path would use one only with hw_mgmt_tx_encrypt). Without the pairwise key yet a
 *    deauthentication/disassociation goes in clear and a robust action frame not at all (mac80211).
 *  - RX (mlme.c, before the MLME looks at a frame): a protected unicast frame is decrypted with the pairwise key, its
 *    PN checked against the key's management slot (MAC_PN_MGMT), the CCMP header and MIC removed; a group-addressed
 *    robust frame needs a valid MMIE (BIP-CMAC-128 with the IGTK of its key ID, IPN rising), which then comes off;
 *    an unprotected unicast robust frame is dropped, except a deauthentication/disassociation before the pairwise key
 *    (the 4-way handshake).
 *  - An unprotected deauthentication/disassociation saying the AP lost our state (reason 6 or 7, e.g. it restarted)
 *    starts an SA Query: protected requests every 201 ms for 1 s; an answer keeps the connection, silence ends it
 *    (IEEE 802.11-2020 11.13; wpa_supplicant sme_event_unprot_disconnect), at most once in 10 s like wpa_supplicant.
 *    Other unprotected ones are just dropped. The log says so at most once in 10 s.
 *  - The AP's SA Query requests get a protected response with the same transaction ID.
 * Everything runs with wiphy->mtx held at PASSIVE_LEVEL (the CNG calls need it). Key bytes never go to the log.
 * Parameters\Pmf (rtw_linux_set_pmf, read at every probe): 0 no MFP and no SAE (the default, and what any other value
 * than 0-2 means), 1 MFP, 2 MFP and SAE. It only takes effect if the crypto self-tests passed: driver\crypto.c's at
 * load and the frame-level ones below.
 */
#include "mac.h"

#define MAC_HDR_LEN                     24
#define FC_TYPE                         0x000c
#define FC_TYPE_MGMT                    0x0000
#define FC_SUBTYPE_DATA_BITS            0x0070  /* masked in the AAD of data frames only */
#define FC_RETRY                        0x0800
#define FC_PM                           0x1000
#define FC_MOREDATA                     0x2000
#define FC_PROTECTED                    0x4000
#define CCMP_HDR_LEN                    8
#define CCMP_MIC_LEN                    8
#define CCMP_EXT_IV                     0x20
#define CCMP_AAD_LEN                    22      /* no A4, no QoS control */
#define CCMP_NONCE_LEN                  13
#define BIP_AAD_LEN                     20
#define BIP_MIC_LEN                     8
#define EID_MMIE                        76
#define MMIE_LEN                        18      /* element ID, length, key ID, IPN, MIC */
#define CAT_PUBLIC                      4
#define CAT_HT                          7
#define CAT_SA_QUERY                    8
#define CAT_WNM_UNPROTECTED             11
#define CAT_SELF_PROTECTED              15
#define CAT_UNPROT_DMG                  20
#define CAT_VHT                         21
#define CAT_S1G                         22
#define CAT_VENDOR                      127
#define SAQ_REQUEST                     0
#define SAQ_RESPONSE                    1
#define SAQ_LEN                         4       /* category, action, transaction ID */
#define SAQ_RETRY_MS                    201     /* dot11AssociationSAQueryRetryTimeout */
#define SAQ_TIMEOUT_MS                  1000    /* dot11AssociationSAQueryMaximumTimeout */
#define SAQ_MIN_INTERVAL_MS             10000   /* between the starts of two SA Queries (wpa_supplicant) */
#define SAQ_UNPROT_LOG_MS               10000   /* between two log lines about dropped unprotected disconnections */
#define REASON_CLASS2_FRAME             6       /* class 2 frame received from a nonauthenticated STA */
#define REASON_CLASS3_FRAME             7       /* class 3 frame received from a nonassociated STA */
#define SAQ_LOG_ANSWERS                 4

static u8 pmf_mode = RTW_PMF_OFF;       /* RTW_PMF_* in effect (rtw_linux_set_pmf) */
static u8 pmf_crypto = RTW_CRYPTO_UNTESTED;

static void saq_timer_fn (struct wiphy *wiphy, struct wiphy_work *w);

static u64 ccmp_pn (const u8 *h)
{
	return (u64)h[0] | (u64)h[1] << 8 | (u64)h[4] << 16 | (u64)h[5] << 24 | (u64)h[6] << 32 | (u64)h[7] << 40;
}

/* constant time: the MICs of frames an attacker may send */
static bool pmf_memneq (const u8 *a, const u8 *b, unsigned int n)
{
	u8 d = 0;

	while (n--)
		d |= *a++ ^ *b++;
	return d != 0;
}

/* include/linux/ieee80211.h _ieee80211_is_robust_mgmt_frame(): deauthentication, disassociation, and every action
   frame but the public, HT, unprotected WNM, self-protected, unprotected DMG, VHT, S1G and vendor-specific ones (a
   protected action frame is robust whatever its encrypted category). f is a management frame of len >= 24. */
static bool pmf_robust (const u8 *f, unsigned int len)
{
	u16 fc = get_unaligned_le16(f);

	switch (fc & IEEE80211_FCTL_STYPE) {
	case IEEE80211_STYPE_DEAUTH:
	case IEEE80211_STYPE_DISASSOC:
		return true;
	case IEEE80211_STYPE_ACTION:
		if (fc & FC_PROTECTED)
			return true;
		if (len < MAC_HDR_LEN + 1)
			return false;
		switch (f[MAC_HDR_LEN]) {
		case CAT_PUBLIC:
		case CAT_HT:
		case CAT_WNM_UNPROTECTED:
		case CAT_SELF_PROTECTED:
		case CAT_UNPROT_DMG:
		case CAT_VHT:
		case CAT_S1G:
		case CAT_VENDOR:
			return false;
		default:
			return true;
		}
	default:
		return false;
	}
}

/*
 * AAD and nonce of a CCMP MPDU with a 24-byte header (management, or data without QoS control and A4; mac80211
 * ccmp_gcmp_aad and ccmp_special_blocks, 802.11-2020 12.5.3.3.3-4): FC with Retry, PwrMgt and MoreData cleared - for
 * a data frame also subtype bits 4-6 - and Protected set; A1, A2, A3; the sequence control's fragment number only.
 * Nonce: priority 0 | management << 4, A2, PN most significant octet first.
 */
static void pmf_ccmp_aad (const u8 *hdr, u64 pn, u8 *aad, u8 *nonce)
{
	u16 fc = get_unaligned_le16(hdr);
	bool mgmt = (fc & FC_TYPE) == FC_TYPE_MGMT;
	int i;

	fc &= ~(FC_RETRY | FC_PM | FC_MOREDATA);
	if (!mgmt)
		fc &= ~FC_SUBTYPE_DATA_BITS;
	fc |= FC_PROTECTED;
	put_unaligned_le16(fc, aad);
	memcpy(aad + 2, hdr + 4, 3 * ETH_ALEN);
	aad[20] = hdr[22] & 0x0f;
	aad[21] = 0;
	nonce[0] = mgmt ? 0x10 : 0;
	memcpy(nonce + 1, hdr + 10, ETH_ALEN);
	for (i = 0; i < 6; i++)
		nonce[7 + i] = (u8)(pn >> (8 * (5 - i)));
}

/* f: 24-byte header, CCMP header, body_len bytes of body, room for the MIC. Encrypts the body in place and writes
   the MIC after it. 0 or -errno. */
static int pmf_ccmp_seal (const u8 *tk, u8 *f, unsigned int body_len)
{
	u8 aad[CCMP_AAD_LEN], nonce[CCMP_NONCE_LEN];
	u8 *body = f + MAC_HDR_LEN + CCMP_HDR_LEN;

	pmf_ccmp_aad(f, ccmp_pn(f + MAC_HDR_LEN), aad, nonce);
	return rtwk_aes_ccm_encrypt(tk, nonce, aad, sizeof(aad), body, body_len, body + body_len);
}

/* f: the received MPDU of len bytes (header, CCMP header, body, MIC). Decrypts the body in place if the MIC
   matches: 0, -EBADMSG, -EINVAL (too short) or another -errno. */
static int pmf_ccmp_open (const u8 *tk, u8 *f, unsigned int len)
{
	u8 aad[CCMP_AAD_LEN], nonce[CCMP_NONCE_LEN];
	u8 *body = f + MAC_HDR_LEN + CCMP_HDR_LEN;
	unsigned int body_len;

	if (len < MAC_HDR_LEN + CCMP_HDR_LEN + CCMP_MIC_LEN)
		return -EINVAL;
	body_len = len - MAC_HDR_LEN - CCMP_HDR_LEN - CCMP_MIC_LEN;
	pmf_ccmp_aad(f, ccmp_pn(f + MAC_HDR_LEN), aad, nonce);
	return rtwk_aes_ccm_decrypt(tk, nonce, aad, sizeof(aad), body, body_len, body + body_len);
}

/* BIP-CMAC-128 MIC of a group-addressed management frame f of len bytes whose body ends with the MMIE (mac80211
   bip_aad, ieee80211_aes_cmac): AES-CMAC over FC with Retry, PwrMgt and MoreData cleared, A1, A2, A3 and the body
   with the MMIE's MIC as zeros, cut to 8 octets. Not for beacons (their Timestamp would be masked). */
static int pmf_bip_mic (const u8 *igtk, const u8 *f, unsigned int len, u8 *mic)
{
	unsigned int n = BIP_AAD_LEN + len - MAC_HDR_LEN;
	u8 mac[16];
	u8 *buf;
	int ret;

	buf = kmalloc(n, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	put_unaligned_le16(get_unaligned_le16(f) & ~(FC_RETRY | FC_PM | FC_MOREDATA), buf);
	memcpy(buf + 2, f + 4, 3 * ETH_ALEN);
	memcpy(buf + BIP_AAD_LEN, f + MAC_HDR_LEN, len - MAC_HDR_LEN);
	memset(buf + n - BIP_MIC_LEN, 0, BIP_MIC_LEN);
	ret = rtwk_aes_cmac(igtk, buf, n, mac);
	kfree(buf);
	if (!ret)
		memcpy(mic, mac, BIP_MIC_LEN);
	return ret;
}

/* ---- frame-level known-answer tests (the AES primitives have theirs in driver\crypto.c) ---- */
/* IEEE 802.11-2020 J.6.4: a data MPDU (FC with Retry set) with its CCMP header, the expected body and MIC */
static const u8 kat_tk[16] = {
	0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85, 0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd5, 0x2f,
};
static const u8 kat_data_hdr[32] = {
	0x08, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84, 0x44, 0x08,
	0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33, 0x0c, 0xe7, 0x00, 0x20, 0x76, 0x97, 0x03, 0xb5,
};
static const u8 kat_data_plain[20] = {
	0xf8, 0xba, 0x1a, 0x55, 0xd0, 0x2f, 0x85, 0xae, 0x96, 0x7b, 0xb6, 0x2f, 0xb6, 0xcd, 0xa8, 0xeb,
	0x7e, 0x78, 0xa0, 0x50,
};
static const u8 kat_data_out[28] = {
	0xf3, 0xd0, 0xa2, 0xfe, 0x9a, 0x3d, 0xbf, 0x23, 0x42, 0xa6, 0x43, 0xe4, 0x32, 0x46, 0xe8, 0x0c,
	0x3c, 0x04, 0xd0, 0x19, 0x78, 0x45, 0xce, 0x0b, 0x16, 0xf9, 0x76, 0x23,
};
/* a protected deauthentication (FC with Retry and PwrMgt set, fragment number 3, PN 0x102, key ID 0) under the same
   TK; the expected body and MIC were computed apart from this code (.NET AesCcm, the AAD and nonce made by hand
   after the rules above) */
static const u8 kat_mgmt_hdr[32] = {
	0xc0, 0x58, 0x3a, 0x01, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x3c, 0xb7, 0xef, 0xb9, 0xea,
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x13, 0x2a, 0x02, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
};
static const u8 kat_mgmt_plain[20] = {
	0x03, 0x00, 0xdd, 0x10, 0x00, 0x10, 0x18, 0x01, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b,
};
static const u8 kat_mgmt_out[28] = {
	0xe8, 0x9e, 0x7f, 0x4a, 0x3b, 0xd3, 0x4c, 0xf1, 0x25, 0x4a, 0x3f, 0xae, 0xb6, 0xa6, 0x53, 0x8a,
	0x06, 0xfd, 0xec, 0xcf, 0x38, 0x46, 0xc7, 0x3b, 0x7e, 0x05, 0x91, 0xe3,
};
/* a broadcast deauthentication with an MMIE (key ID 4, IPN 5), IGTK = the RFC 4493 key; the MIC computed apart */
static const u8 kat_igtk[16] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
};
static const u8 kat_bip_frame[44] = {
	0xc0, 0x18, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
	0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x20, 0x30, 0x07, 0x00, 0x4c, 0x10, 0x04, 0x00, 0x05, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x84, 0x6f, 0xba, 0x0f, 0x07, 0x2c, 0x61, 0xda,
};

/* CCMP of a test frame: sealed it must be out (body and MIC), opened again the plaintext */
static bool pmf_kat_ccmp (const u8 *hdr, const u8 *plain, const u8 *out)
{
	u8 f[32 + 20 + 8];

	memcpy(f, hdr, 32);
	memcpy(f + 32, plain, 20);
	if (pmf_ccmp_seal(kat_tk, f, 20) || memcmp(f + 32, out, 28))
		return false;
	if (pmf_ccmp_open(kat_tk, f, sizeof(f)) || memcmp(f + 32, plain, 20))
		return false;
	memcpy(f + 32, out, 28);
	f[33] ^= 1;                     /* one bit changed: refused */
	return pmf_ccmp_open(kat_tk, f, sizeof(f)) == -EBADMSG;
}

static int pmf_selftest (void)
{
	u8 mic[BIP_MIC_LEN];

	if (!pmf_kat_ccmp(kat_data_hdr, kat_data_plain, kat_data_out))
		return 1;
	if (!pmf_kat_ccmp(kat_mgmt_hdr, kat_mgmt_plain, kat_mgmt_out))
		return 2;
	if (pmf_bip_mic(kat_igtk, kat_bip_frame, sizeof(kat_bip_frame), mic) ||
	    memcmp(mic, kat_bip_frame + sizeof(kat_bip_frame) - BIP_MIC_LEN, BIP_MIC_LEN))
		return 3;
	return 0;
}

/* ---- the policy ---- */
uint32_t rtw_linux_set_pmf (uint32_t mode)
{
	u8 old = READ_ONCE(pmf_mode), now;
	int failed;

	if (pmf_crypto == RTW_CRYPTO_UNTESTED) {
		/* once per driver load: the primitives' tests ran in DriverEntry, the frame-level ones run here */
		failed = rtwk_crypto_ready() ? pmf_selftest() : -1;
		pmf_crypto = failed ? RTW_CRYPTO_FAILED : RTW_CRYPTO_PASSED;
		if (failed > 0)
			pr_err("mac: management frame protection self-test %d FAILED", failed);
		else if (!failed)
			pr_info("mac: management frame protection self-test passed");
	}
	if (mode > RTW_PMF_SAE) {
		pr_warn("mac: Parameters\\Pmf %u is not 0, 1 or 2; using 0", mode);
		mode = RTW_PMF_OFF;
	}
	now = pmf_crypto == RTW_CRYPTO_PASSED ? (u8)mode : RTW_PMF_OFF;
	WRITE_ONCE(pmf_mode, now);
	if (now != old || pmf_crypto != RTW_CRYPTO_PASSED)
		pr_info("mac: management frame protection %s (Parameters\\Pmf %u%s)",
			now == RTW_PMF_SAE ? "on, with SAE" : now == RTW_PMF_ON ? "on, without SAE" : "off", mode,
			pmf_crypto != RTW_CRYPTO_PASSED ? "; crypto self-test failed" : "");
	return now;
}

uint32_t rtw_linux_get_pmf (void)
{
	return READ_ONCE(pmf_mode);
}

bool mac_pmf_usable (void)
{
	return READ_ONCE(pmf_mode) != RTW_PMF_OFF;
}

/* IOCTL_RTW_LINK (tx.c) */
void mac_pmf_stats (struct mac_hw *mh, struct rtw_link_stats *st)
{
	st->pmf_mode = READ_ONCE(pmf_mode);
	st->crypto = READ_ONCE(pmf_crypto);
	st->mfp = 0;
	st->sae_state = 0;
	if (!mh || READ_ONCE(mh->conn.state) == RTW_CONN_IDLE)
		return;
	st->mfp = READ_ONCE(mh->conn.mfp);
	st->sae_state = READ_ONCE(mh->conn.sae.state);
}

/* ---- TX (mac_tx_mgmt, wiphy->mtx held) ---- */
/* Returns the frame to send: skb itself, or a protected copy (skb freed), or NULL if it may not go out (freed). */
struct sk_buff *mac_pmf_tx (struct mac_hw *mh, struct sk_buff *skb)
{
	struct mac_conn *c = &mh->conn;
	const u8 *f = skb->data;
	unsigned int len = skb->len, body_len;
	u16 fc = get_unaligned_le16(f);
	struct sk_buff *n;
	struct mac_key *k;
	u8 *p;

	if ((fc & FC_TYPE) != FC_TYPE_MGMT || !c->mfp || (fc & FC_PROTECTED) || is_multicast_ether_addr(f + 4) ||
	    !pmf_robust(f, len))
		return skb;
	k = c->ptk;
	if (k && READ_ONCE(k->lost))
		k = NULL;
	if (!k) {
		/* before the 4-way handshake ends (or after a restart lost the key): mac80211 sends a deauthentication or
		   disassociation in clear and drops a robust action frame */
		u16 stype = fc & IEEE80211_FCTL_STYPE;

		if (stype == IEEE80211_STYPE_DEAUTH || stype == IEEE80211_STYPE_DISASSOC)
			return skb;
		kfree_skb(skb);
		return NULL;
	}
	body_len = len - MAC_HDR_LEN;
	n = dev_alloc_skb(mh->hw.extra_tx_headroom + len + CCMP_HDR_LEN + CCMP_MIC_LEN);
	if (!n) {
		kfree_skb(skb);
		return NULL;
	}
	skb_reserve(n, mh->hw.extra_tx_headroom);
	p = skb_put(n, len + CCMP_HDR_LEN + CCMP_MIC_LEN);
	memcpy(p, f, MAC_HDR_LEN);
	put_unaligned_le16(fc | FC_PROTECTED, p);
	mac_key_ccmp_hdr(k, p + MAC_HDR_LEN);   /* the next PN of the pairwise key */
	memcpy(p + MAC_HDR_LEN + CCMP_HDR_LEN, f + MAC_HDR_LEN, body_len);
	kfree_skb(skb);
	if (pmf_ccmp_seal(k->conf.key, p, body_len)) {
		pr_warn_once("mac: a management frame could not be protected (CCMP); not sent");
		kfree_skb(n);
		return NULL;
	}
	MAC_COUNT(mgmt_protected_tx);
	return n;
}

/* ---- SA Query (wiphy->mtx held) ---- */
static void saq_send (struct mac_hw *mh, u8 action, const u8 *tid)
{
	struct sk_buff *skb;
	u8 *b;

	skb = mac_mlme_frame(mh, IEEE80211_STYPE_ACTION, SAQ_LEN, &b);
	if (!skb)
		return;
	b[0] = CAT_SA_QUERY;
	b[1] = action;
	b[2] = tid[0];
	b[3] = tid[1];
	mac_tx_mgmt(mh, skb, mh->conn.ap, 0, MAC_TXT_ACTION);  /* protected there (robust, MFP) */
}

static void saq_arm (struct mac_hw *mh)
{
	struct mac_saq *q = &mh->conn.saq;
	u64 now = ktime_get_ns(), at = min(q->next_ns, q->deadline_ns);
	u32 ms = at > now ? (u32)div_u64(at - now, NSEC_PER_MSEC) + 1 : 1;

	wiphy_delayed_work_queue(mh->hw.wiphy, &mh->conn.saq_timer, msecs_to_jiffies(ms));
}

/* a new request with a new transaction ID; the earlier IDs stay valid for the answer */
static void saq_request (struct mac_hw *mh)
{
	struct mac_saq *q = &mh->conn.saq;
	u16 id = get_random_u16();
	u8 *t;

	if (q->n_ids < MAC_SAQ_IDS)
		q->n_ids++;
	else
		memmove(q->ids[0], q->ids[1], sizeof(q->ids) - sizeof(q->ids[0]));
	t = q->ids[q->n_ids - 1];
	put_unaligned_le16(id, t);
	q->next_ns = ktime_get_ns() + (u64)SAQ_RETRY_MS * NSEC_PER_MSEC;
	saq_send(mh, SAQ_REQUEST, t);
}

static const char *saq_frame_name (u16 stype)
{
	return stype == IEEE80211_STYPE_DEAUTH ? "deauthentication" : "disassociation";
}

/* At most one line per SAQ_UNPROT_LOG_MS about dropped unprotected disconnections: anyone can send them, as often as
   they like (mgmt_dropped_unprot counts them all). */
static bool saq_unprot_log_ok (struct mac_conn *c, u64 now)
{
	if (c->unprot_log_ns && now - c->unprot_log_ns < (u64)SAQ_UNPROT_LOG_MS * NSEC_PER_MSEC)
		return false;
	c->unprot_log_ns = now;
	return true;
}

/* an unprotected deauthentication/disassociation was dropped; the AP may have lost our association */
static void saq_unprot_disconnect (struct mac_hw *mh, u16 stype, const u8 *f, unsigned int len)
{
	struct mac_conn *c = &mh->conn;
	struct mac_saq *q = &c->saq;
	u16 reason = len >= MAC_HDR_LEN + 2 ? get_unaligned_le16(f + MAC_HDR_LEN) : 0;
	u64 now = ktime_get_ns();

	if (c->state != RTW_CONN_CONNECTED || q->deadline_ns || !c->ptk || READ_ONCE(c->ptk->lost))
		return;
	if (reason != REASON_CLASS2_FRAME && reason != REASON_CLASS3_FRAME) {
		if (saq_unprot_log_ok(c, now))
			pr_info("mac: an unprotected %s (reason %u) was dropped", saq_frame_name(stype), reason);
		return;
	}
	/* wpa_supplicant sme_event_unprot_disconnect(): "limit SA Query procedure frequency" */
	if (c->saq_last_ns && now - c->saq_last_ns < (u64)SAQ_MIN_INTERVAL_MS * NSEC_PER_MSEC) {
		if (saq_unprot_log_ok(c, now))
			pr_info("mac: an unprotected %s (reason %u) was dropped; the last SA Query was less than %u s ago",
				saq_frame_name(stype), reason, SAQ_MIN_INTERVAL_MS / 1000);
		return;
	}
	c->saq_last_ns = now;
	memset(q, 0, sizeof(*q));
	q->stype = stype;
	q->reason = reason;
	q->deadline_ns = now + (u64)SAQ_TIMEOUT_MS * NSEC_PER_MSEC;
	MAC_COUNT(sa_query_started);
	pr_info("mac: unprotected %s (reason %u) from %pM: asking the AP with an SA Query", saq_frame_name(stype), reason,
		c->bssid);
	saq_request(mh);
	saq_arm(mh);
}

static void saq_timer_fn (struct wiphy *wiphy, struct wiphy_work *w)
{
	struct mac_hw *mh = container_of(w, struct mac_hw, conn.saq_timer.work);
	struct mac_conn *c = &mh->conn;
	struct mac_saq *q = &c->saq;
	u64 now = ktime_get_ns();
	u16 stype, reason;

	(void)wiphy;
	if (!q->deadline_ns || READ_ONCE(mh->restarting))
		return;
	if (c->state != RTW_CONN_CONNECTED) {
		memset(q, 0, sizeof(*q));
		return;
	}
	if (now >= q->deadline_ns) {
		stype = q->stype;
		reason = q->reason;
		memset(q, 0, sizeof(*q));
		MAC_COUNT(sa_query_timeouts);
		pr_info("mac: SA Query: no answer from %pM; the %s stands", c->bssid, saq_frame_name(stype));
		mac_mlme_peer_left(mh, stype, reason);
		return;
	}
	if (now >= q->next_ns)
		saq_request(mh);
	saq_arm(mh);
}

/* an SA Query action frame from the AP (mlme.c, connected; protected when MFP is on: unprotected ones never get
   here then). body starts at the category octet. */
void mac_pmf_rx_sa_query (struct mac_hw *mh, const u8 *body, unsigned int len)
{
	struct mac_conn *c = &mh->conn;
	struct mac_saq *q = &c->saq;

	if (!c->mfp || len < SAQ_LEN)
		return;
	if (body[1] == SAQ_REQUEST) {
		/* mac80211 ieee80211_process_sa_query_req(): the same transaction ID back, protected */
		MAC_COUNT(sa_query_answered);
		if (READ_ONCE(mac_st.sa_query_answered) <= SAQ_LOG_ANSWERS)
			pr_info("mac: SA Query from %pM answered", c->bssid);
		saq_send(mh, SAQ_RESPONSE, body + 2);
		return;
	}
	if (body[1] != SAQ_RESPONSE || !q->deadline_ns)
		return;
	for (int i = 0; i < q->n_ids; i++) {
		if (memcmp(q->ids[i], body + 2, 2))
			continue;
		pr_info("mac: SA Query answered by %pM; the unprotected %s is ignored", c->bssid,
			saq_frame_name(q->stype));
		memset(q, 0, sizeof(*q));
		wiphy_delayed_work_cancel(mh->hw.wiphy, &c->saq_timer);
		return;
	}
}

void mac_pmf_init (struct mac_hw *mh)
{
	wiphy_delayed_work_init(&mh->conn.saq_timer, saq_timer_fn);
}

/* the attempt or connection ends (also a hardware restart: memory and timer only) */
void mac_pmf_conn_end (struct mac_hw *mh)
{
	memset(&mh->conn.saq, 0, sizeof(mh->conn.saq));
	wiphy_delayed_work_cancel(mh->hw.wiphy, &mh->conn.saq_timer);
}

/* ---- RX (mlme.c mlme_rx, wiphy->mtx held) ---- */
/* A protected unicast frame: decrypted with the pairwise key, PN checked; CCMP header and MIC off, Protected bit
   cleared. true to go on with it. */
static bool pmf_rx_protected (struct mac_hw *mh, struct sk_buff *skb)
{
	struct mac_conn *c = &mh->conn;
	const struct ieee80211_rx_status *rs = IEEE80211_SKB_RXCB(skb);
	u8 *f = skb->data;
	unsigned int len = skb->len;
	u16 fc = get_unaligned_le16(f), stype = fc & IEEE80211_FCTL_STYPE;
	struct mac_key *k;
	const u8 *h;
	u64 pn;

	if (is_multicast_ether_addr(f + 4) || (stype != IEEE80211_STYPE_DEAUTH && stype != IEEE80211_STYPE_DISASSOC &&
	    stype != IEEE80211_STYPE_ACTION) || len < MAC_HDR_LEN + CCMP_HDR_LEN + CCMP_MIC_LEN) {
		MAC_COUNT(mgmt_dropped_mic);
		return false;           /* nothing we know to be protected this way */
	}
	k = c->mfp ? c->ptk : NULL;
	if (k && READ_ONCE(k->lost))
		k = NULL;
	h = f + MAC_HDR_LEN;
	if (!k || !(h[3] & CCMP_EXT_IV) || (h[3] >> 6) != k->conf.keyidx) {
		MAC_COUNT(mgmt_dropped_nokey);
		return false;
	}
	pn = ccmp_pn(h);
	if (pn <= k->rx_pn[MAC_PN_MGMT]) {
		/* a retransmission of the last one is no attack: dropped without counting */
		if (!((fc & FC_RETRY) && pn == k->rx_pn[MAC_PN_MGMT]))
			MAC_COUNT(mgmt_dropped_replay);
		return false;
	}
	/* rtw89 sets RX_FLAG_DECRYPTED only when the hardware decrypted (not done for management frames normally) */
	if (!(rs->flag & RX_FLAG_DECRYPTED) && pmf_ccmp_open(k->conf.key, f, len)) {
		MAC_COUNT(mgmt_dropped_mic);
		return false;
	}
	k->rx_pn[MAC_PN_MGMT] = pn;
	/* the header moves up over the CCMP header, the MIC comes off the end */
	memmove(f + CCMP_HDR_LEN, f, MAC_HDR_LEN);
	skb_pull(skb, CCMP_HDR_LEN);
	skb_trim(skb, skb->len - CCMP_MIC_LEN);
	put_unaligned_le16(fc & ~FC_PROTECTED, skb->data);
	MAC_COUNT(mgmt_protected_rx);
	return true;
}

/* A group-addressed robust frame: the MMIE at its end must be valid (mac80211 ieee80211_crypto_aes_cmac_decrypt)
   with the IGTK of its key ID and an IPN above the last; the MMIE then comes off. 1 valid, 0 no MMIE (unprotected),
   -1 an MMIE that does not check out (counted). */
static int pmf_rx_bip (struct mac_hw *mh, struct sk_buff *skb)
{
	struct mac_conn *c = &mh->conn;
	const struct ieee80211_rx_status *rs = IEEE80211_SKB_RXCB(skb);
	const u8 *f = skb->data, *mmie;
	unsigned int len = skb->len;
	struct mac_key *k = NULL;
	u8 mic[BIP_MIC_LEN];
	u16 keyid;
	u64 ipn = 0;

	if (len < MAC_HDR_LEN + MMIE_LEN)
		return 0;
	mmie = f + len - MMIE_LEN;
	if (mmie[0] != EID_MMIE || mmie[1] != MMIE_LEN - 2)
		return 0;
	keyid = get_unaligned_le16(mmie + 2);
	if (keyid == 4 || keyid == 5)
		k = c->igtk[keyid - 4];
	if (!k) {
		MAC_COUNT(mgmt_dropped_nokey);
		return -1;
	}
	for (int i = 5; i >= 0; i--)
		ipn = ipn << 8 | mmie[4 + i];
	if (ipn <= k->rx_pn[0]) {
		MAC_COUNT(mgmt_dropped_replay);
		return -1;
	}
	if (!(rs->flag & RX_FLAG_DECRYPTED) &&
	    (pmf_bip_mic(k->conf.key, f, len, mic) || pmf_memneq(mic, mmie + 10, BIP_MIC_LEN))) {
		MAC_COUNT(mgmt_dropped_mic);
		return -1;
	}
	k->rx_pn[0] = ipn;
	skb_trim(skb, len - MMIE_LEN);
	MAC_COUNT(mgmt_bip_rx);
	return 1;
}

/* Every MLME frame from the BSS of the attempt or connection, before the MLME looks at it (mac80211
   ieee80211_rx_h_decrypt + ieee80211_drop_unencrypted_mgmt). true to go on with it (possibly changed), false if it
   is dropped (the caller frees it). */
bool mac_pmf_rx (struct mac_hw *mh, struct sk_buff *skb)
{
	struct mac_conn *c = &mh->conn;
	const u8 *f = skb->data;
	unsigned int len = skb->len;
	u16 fc = get_unaligned_le16(f), stype = fc & IEEE80211_FCTL_STYPE;
	bool disconnect = stype == IEEE80211_STYPE_DEAUTH || stype == IEEE80211_STYPE_DISASSOC;
	int bip;

	if (fc & FC_PROTECTED)
		return pmf_rx_protected(mh, skb);
	if (!c->mfp || !pmf_robust(f, len))
		return true;
	if (!is_multicast_ether_addr(f + 4)) {
		/* unprotected unicast: a deauthentication/disassociation passes until the pairwise key is there */
		if (disconnect && !c->ptk)
			return true;
		MAC_COUNT(mgmt_dropped_unprot);
		if (disconnect)
			saq_unprot_disconnect(mh, stype, f, len);
		return false;
	}
	bip = pmf_rx_bip(mh, skb);
	if (bip > 0)
		return true;
	if (!bip) {
		MAC_COUNT(mgmt_dropped_unprot);
		if (disconnect)
			saq_unprot_disconnect(mh, stype, f, len);
	}
	return false;
}
