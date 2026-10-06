/*
 * rtw89win: entry points of the Linux-side code, called by the KMDF driver (driver\). Plain C types only; the
 * structures below have the same layout under clang and MSVC (fixed-width fields, natural alignment) and are
 * also the IOCTL layouts (driver\rtw89win_ioctl.h).
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_GLUE_H
#define RTW_GLUE_H

#include "os.h"           /* fixed-width types on both sides */

#ifdef __cplusplus
extern "C" {
#endif

struct rtw_pci_info {
	void *os_dev;                   /* passed back in rtwk_pci_cfg_* */
	void *bar_va;                   /* mapped BAR2 */
	uint64_t bar_len;
	uint16_t vendor, device, subsystem_vendor, subsystem_device;
	uint8_t revision;
	uint32_t irq;                   /* any non-zero cookie; rtw89 only passes it back */
};

int rtw_linux_init (void);                              /* PASSIVE; system work queues; 0 or -errno */
void rtw_linux_exit (void);
int rtw_linux_probe (const struct rtw_pci_info *info);  /* PASSIVE; runs the 8852BE pci_driver probe */
void rtw_linux_remove (void);                           /* PASSIVE; stops (rtw_linux_stop), pci_driver remove,
							   then frees devm memory */
int rtw_linux_probed (void);
int rtw_linux_isr (void);                               /* DIRQL; 0 not ours, 1 handled, 2 handled + DPC wanted */
void rtw_linux_irq_dpc (void);                          /* DISPATCH_LEVEL */

/*
 * The mac layer (mac\) driving rtw89 the way mac80211 does, on the probed device. PASSIVE_LEVEL; the caller
 * serializes these with probe/remove. Return 0 or a negative Linux errno (-ENODEV: not probed, -ENOTCONN: not
 * started, -EBUSY: scan or listening in progress).
 */
int rtw_linux_start (void);             /* drv_start + one station interface (efuse address); the chip then
					   idles (rtw89 inactive power save, powered off) */
void rtw_linux_stop (void);             /* cancels scan/listening, removes the interface, drv_stop */
int rtw_linux_monitor (uint32_t channel);       /* listen on a 20 MHz channel (beacons of all BSSs);
						   0 stops listening */

/* IOCTL_RTW_SCAN in/out */
#define RTW_SCAN_KEEP           0x1     /* keep the BSS table from before */
#define RTW_SCAN_PASSIVE        0x2     /* no probe requests on any channel */
struct rtw_scan_param {
	uint32_t flags;                 /* RTW_SCAN_* */
	uint32_t dwell_ms;              /* per channel, 10-250; 0 = rtw89 default (45 ms active, 105 ms passive) */
	uint32_t timeout_ms;            /* 0 = 15 s, at most 30 s; on timeout the scan is cancelled */
};
struct rtw_scan_result {
	int32_t result;                 /* 0, -ETIMEDOUT (timed out and cancelled) or -errno */
	uint32_t aborted;               /* the driver reported the scan as aborted */
	uint32_t duration_ms;
	uint32_t bss_count;             /* entries in the BSS table afterwards */
};
/* firmware (hw_scan) scan over all enabled channels with a wildcard SSID; waits for the end (the start
   path of rtw_linux_scan_start below, plus a wait and a cancel on timeout) */
int rtw_linux_scan (const struct rtw_scan_param *param, struct rtw_scan_result *res);

/* IOCTL_RTW_BSS_LIST: struct rtw_bss_list followed by count struct rtw_bss */
#define RTW_STATE_PROBED        0x1
#define RTW_STATE_STARTED       0x2
#define RTW_STATE_LISTENING     0x4
#define RTW_STATE_SCANNING      0x8
struct rtw_bss_list {
	uint32_t count;                 /* entries that follow */
	uint32_t total;                 /* entries in the table */
	uint32_t state;                 /* RTW_STATE_* */
	uint32_t channel;               /* listening channel, 0 = none */
	uint32_t rx_frames;             /* frames received since the table was cleared */
	uint32_t rx_beacons;
	uint32_t rx_probe_resps;
	uint32_t rx_bad;                /* FCS/PLCP errors, too short */
	uint32_t scans;                 /* scans completed since the driver was loaded */
	uint32_t reserved;
};                                      /* 40 bytes */

#define RTW_BSS_PHY_HT          0x01
#define RTW_BSS_PHY_VHT         0x02
#define RTW_BSS_PHY_HE          0x04
#define RTW_BSS_PHY_EHT         0x08
#define RTW_BSS_SEC_PRIVACY     0x01    /* capability: privacy */
#define RTW_BSS_SEC_WPA         0x02    /* WPA (vendor) element */
#define RTW_BSS_SEC_RSN         0x04    /* RSN element */
#define RTW_BSS_AKM_8021X       0x01
#define RTW_BSS_AKM_PSK         0x02
#define RTW_BSS_AKM_SAE         0x04
#define RTW_BSS_AKM_OWE         0x08
#define RTW_BSS_AKM_FT          0x10
#define RTW_BSS_AKM_OTHER       0x80
#define RTW_BSS_CIPHER_WEP      0x01
#define RTW_BSS_CIPHER_TKIP     0x02
#define RTW_BSS_CIPHER_CCMP     0x04
#define RTW_BSS_CIPHER_GCMP     0x08
#define RTW_BSS_CIPHER_CCMP256  0x10
#define RTW_BSS_CIPHER_GCMP256  0x20
#define RTW_BSS_CIPHER_OTHER    0x80
#define RTW_BSS_F_BEACON        0x01    /* seen in a beacon */
#define RTW_BSS_F_PROBE_RESP    0x02    /* seen in a probe response */
#define RTW_BSS_F_HIDDEN        0x04    /* beacons carry an empty SSID */
struct rtw_bss {
	uint8_t bssid[6];
	uint8_t channel;
	uint8_t band;                   /* NL80211_BAND_*: 0 = 2.4 GHz, 1 = 5 GHz, 3 = 6 GHz */
	uint16_t freq;                  /* MHz */
	uint16_t capability;            /* capability information field */
	uint16_t beacon_int;            /* TU */
	int8_t signal;                  /* dBm of the last frame, 0 = unknown */
	uint8_t phy;                    /* RTW_BSS_PHY_* */
	uint8_t security;               /* RTW_BSS_SEC_* */
	uint8_t akm;                    /* RTW_BSS_AKM_* (RSN, else WPA) */
	uint8_t pairwise;               /* RTW_BSS_CIPHER_* */
	uint8_t group;                  /* RTW_BSS_CIPHER_* */
	uint8_t ssid_len;
	uint8_t flags;                  /* RTW_BSS_F_* */
	uint16_t reserved;
	uint8_t ssid[32];
	uint32_t age_ms;                /* since the last frame */
	uint32_t frames;                /* beacons and probe responses */
};                                      /* 64 bytes */
/* any IRQL <= DISPATCH_LEVEL, at any time (the table is static); returns the number of entries stored */
uint32_t rtw_linux_bss_get (struct rtw_bss_list *list, struct rtw_bss *out, uint32_t max);

/*
 * For the WiFiCx front end (wificx\). Not IOCTL layouts, but shared by clang and MSVC like the ones above
 * (both sides check the sizes: mac\hw.c and mac\bss.c, wificx\wifi.h).
 */

/* What the probed device supports, for the WiFiCx capability calls. Bands: 0 = 2.4 GHz, 1 = 5 GHz. */
#define RTW_CHAN_PASSIVE        0x1     /* NO_IR: listen only (regulatory) */
#define RTW_CHAN_RADAR          0x2     /* DFS */
struct rtw_caps_chan {
	uint16_t freq;                  /* MHz */
	uint8_t number;                 /* IEEE channel number */
	uint8_t flags;                  /* RTW_CHAN_* */
};
struct rtw_caps {
	uint8_t addr[6];                /* permanent address (efuse) */
	uint8_t tx_streams, rx_streams;
	char fw_version[32];            /* NUL terminated */
	uint8_t ht[2], vht[2];          /* per band: supported */
	uint8_t n_rates[2];
	uint8_t rates[2][16];           /* legacy rates, 500 kbit/s units */
	uint8_t n_chans[2];
	uint8_t max_scan_ssids;
	uint8_t reserved;
	struct rtw_caps_chan chans[2][64];      /* channels not DISABLED by the regulatory rules */
};                                      /* 594 bytes */
int rtw_linux_get_caps (struct rtw_caps *caps);         /* PASSIVE, probed; 0 or -ENODEV */

/* Asynchronous firmware scan. RTW_SCAN_KEEP is implied: the BSS table is never cleared for these. */
#define RTW_SCAN_MAX_SSIDS      4
#define RTW_SCAN_MAX_CHANNELS   64
struct rtw_scan_req {
	uint32_t flags;                 /* RTW_SCAN_PASSIVE */
	uint32_t dwell_ms;              /* 0 = rtw89 default (passive channels get at least 105 ms) */
	uint32_t n_ssids;               /* 0 = the wildcard SSID only (none if RTW_SCAN_PASSIVE) */
	uint32_t n_channels;            /* 0 = all enabled channels */
	struct {
		uint8_t len;            /* 0 = wildcard */
		uint8_t ssid[32];
	} ssids[RTW_SCAN_MAX_SSIDS];
	struct {
		uint8_t band;           /* NL80211_BAND_* */
		uint8_t number;
	} channels[RTW_SCAN_MAX_CHANNELS];
};                                      /* 276 bytes */
/* Called exactly once for every scan rtw_linux_scan_start() started, with the mac layer's lock held at
   PASSIVE_LEVEL: from its work thread, or inside rtw_linux_scan_cancel()/rtw_linux_stop()/rtw_linux_remove().
   It may only record the result and signal; start_ns is rtwk_time_ns() when the scan started. */
typedef void (*rtw_scan_done_fn) (void *ctx, int32_t result, uint32_t aborted, uint64_t start_ns);
/* PASSIVE. 0, -ENODEV, -ENOTCONN (not started), -EBUSY (scan or listening), -ENOMEM, or hw_scan's error;
   done() is called only after a 0 return. */
int rtw_linux_scan_start (const struct rtw_scan_req *req, rtw_scan_done_fn done, void *ctx);
/* PASSIVE; returns after done() of the running scan, if any, has been called */
void rtw_linux_scan_cancel (void);

/* BSS table export with the last beacon and probe response bodies: records of struct rtw_bss_rec, each followed
   by beacon_len + presp_len bytes (beacon body first), then padding to a multiple of 8. A body is the 802.11
   frame body from the Timestamp field on (no MAC header, no FCS). */
#define RTW_BSS_REC_ALIGN       8
struct rtw_bss_rec {
	uint8_t bssid[6];
	uint8_t channel;                /* operating channel */
	uint8_t band;                   /* NL80211_BAND_* */
	int8_t signal;                  /* dBm of the last frame, 0 = unknown */
	uint8_t flags;                  /* RTW_BSS_F_* */
	uint16_t freq;                  /* MHz */
	uint16_t beacon_len;            /* 0 = no beacon body */
	uint16_t presp_len;             /* 0 = no probe response body */
	uint32_t age_ms;                /* since the last frame */
	uint32_t reserved;
	uint64_t last_ns;               /* rtwk_time_ns() of the last frame */
};                                      /* 32 bytes */
/* Any IRQL <= DISPATCH_LEVEL. Copies the entries heard at or after since_ns that have a body, from table slot
   *cursor on, as long as they fit into len bytes; advances *cursor (RTW_BSS_EXPORT_END = all done). Returns the
   bytes written. */
#define RTW_BSS_EXPORT_END      128
uint32_t rtw_linux_bss_export (uint64_t since_ns, uint32_t *cursor, void *buf, uint32_t len);

/*
 * Diagnostics, the data path and connections. Shared by clang and MSVC like the ones above; the IOCTL layouts are
 * checked on both sides (mac\tx.c, driver\diag.c).
 */

/* IOCTL_RTW_TXPROBE: broadcast probe requests (wildcard SSID) on the channel being listened on, to check the TX
   path. Only in the diagnostic mode, only on a 2.4 GHz channel without NO_IR/RADAR, 32 frames per driver load. */
struct rtw_txprobe_param {
	uint32_t count;                 /* 1..4 */
	uint32_t interval_ms;           /* 50..1000 */
	uint32_t wait_ms;               /* after the last frame, 100..2000 */
};                                      /* 12 bytes */
struct rtw_txprobe_result {
	int32_t result;                 /* 0 or -errno: -ENOTCONN not listening, -EPERM channel not allowed,
					   -EDQUOT budget used up, -EBUSY scanning, -EINVAL bad parameter */
	uint32_t channel;
	uint32_t sent;                  /* handed to the driver */
	uint32_t tx_done;               /* tx status "done" (no ACK is expected for broadcast frames) */
	uint32_t tx_failed;             /* tx status not done, or dropped by the driver */
	uint32_t tx_pending;            /* no tx status by the end */
	uint32_t presp_to_us;           /* probe responses with A1 = our address since the first frame */
	uint32_t presp_bss;             /* distinct BSSIDs among them (up to 16 counted) */
	uint32_t first_presp_ms;        /* from the first frame; 0 = none */
	uint32_t budget_left;           /* frames this driver load may still send */
};                                      /* 40 bytes */
int rtw_linux_txprobe (const struct rtw_txprobe_param *param, struct rtw_txprobe_result *res);      /* PASSIVE */

/* IOCTL_RTW_LINK: connection state and data path counters, any IRQL <= DISPATCH_LEVEL, at any time */
#define RTW_CONN_IDLE           0
#define RTW_CONN_PREP           1       /* channel and station being set up */
#define RTW_CONN_AUTH           2       /* waiting for the authentication response */
#define RTW_CONN_ASSOC          3       /* waiting for the association response */
#define RTW_CONN_CONNECTED      4
#define RTW_CONN_DISCONNECTING  5
#define RTW_LINK_F_WMM          0x1
#define RTW_LINK_F_HT           0x2
struct rtw_link_stats {
	uint32_t state;                 /* RTW_CONN_* */
	uint8_t bssid[6];
	uint8_t band;                   /* NL80211_BAND_* */
	uint8_t channel;
	uint16_t aid;
	int8_t rssi;                    /* dBm, average of the BSS's beacons; 0 = unknown */
	uint8_t flags;                  /* RTW_LINK_F_* */
	uint32_t tx_data;               /* data frames handed to the driver */
	uint32_t tx_dropped_disconnected;
	uint32_t tx_dropped_bad;        /* not a frame we can send (header, length, fragments) */
	uint32_t tx_busy;               /* refused for flow control (kept by the front end) */
	uint32_t tx_inflight;           /* now */
	uint32_t tx_failed;             /* tx status not done, or dropped by the driver */
	uint32_t tx_nomem;
	uint32_t rx_data;               /* data frames received while connected */
	uint32_t rx_indicated;          /* MSDUs handed to the front end */
	uint32_t rx_dropped_notours;    /* other BSS, addresses, own multicast echoed back */
	uint32_t rx_dropped_protected;
	uint32_t rx_dropped_dup;
	uint32_t rx_dropped_frag;
	uint32_t rx_dropped_full;       /* the receive FIFO was full */
	uint32_t rx_dropped_toolarge;   /* did not fit into the OS's buffer */
	uint32_t rx_dropped_other;      /* null frames, bad A-MSDU, too short */
	uint32_t rx_mlme;               /* management frames passed to the MLME */
	uint32_t mgmt_tx;               /* management frames sent by the mac layer (MLME and TXPROBE) */
	uint32_t mgmt_acked;
	uint32_t mgmt_failed;
	uint32_t queues_stopped;        /* times rtw89 stopped the TX queues */
	uint32_t connects;              /* connections made since the driver was loaded */
	uint32_t disconnects;
	uint32_t beacon_losses;
	/* security (rx_dropped_protected above: protected frames the hardware did not decrypt) */
	uint32_t tx_protected;          /* data frames sent with CCMP */
	uint32_t tx_clear;              /* data frames sent in clear on an RSN connection (exempt, e.g. EAPOL) */
	uint32_t tx_dropped_nokey;      /* had to be protected, but no pairwise key yet */
	uint32_t rx_decrypted;          /* protected frames accepted (decrypted by the hardware, PN checked) */
	uint32_t rx_exempt;             /* unprotected frames an RSN connection let up (exemption list) */
	uint32_t rx_dropped_unencrypted;        /* unprotected and not exempt */
	uint32_t rx_dropped_nokey;      /* protected with a key ID we have no key for */
	uint32_t rx_dropped_replay;     /* PN not above the last one of its key and TID */
	uint32_t key_adds;
	uint32_t key_dels;
	uint8_t auth;                   /* RTW_AUTH_* of the attempt or connection */
	uint8_t keys;                   /* RTW_LINK_KEY_* installed now */
	uint8_t exemptions;             /* entries in the privacy exemption list */
	uint8_t reserved;
	uint32_t restarts;              /* hardware restarts after a firmware/MAC error (SER) */
	/* block ack (A-MPDU) */
	uint32_t rx_reordered;          /* frames held in a reorder buffer (came before a missing one) */
	uint32_t rx_reorder_timeouts;   /* times the reorder buffer gave up waiting for missing frames */
	uint32_t rx_dropped_old;        /* behind the reorder window or held already (retransmissions) */
	uint32_t rx_bar;                /* block ack requests from the AP */
	uint32_t ba_rx_sessions;        /* receive sessions (the AP aggregates) accepted */
	uint32_t ba_tx_sessions;        /* transmit sessions (we aggregate) made operational */
	uint32_t tx_ampdu;              /* data frames handed to the driver for aggregation */
	uint8_t ba_rx_tids;             /* bit per TID 0-7: receive session now */
	uint8_t ba_tx_tids;             /* bit per TID 0-7: transmit session operational now */
	uint16_t reserved3;
	/* management frame protection (802.11w) and SAE */
	uint8_t mfp;                    /* negotiated on the attempt or connection */
	uint8_t pmf_mode;               /* RTW_PMF_* in effect (Parameters\Pmf and the crypto self-tests) */
	uint8_t crypto;                 /* RTW_CRYPTO_*: the self-tests of the driver load */
	uint8_t sae_state;              /* mac\sae.c state of the attempt, 0 = none */
	uint32_t mgmt_protected_tx;     /* robust management frames sent with CCMP */
	uint32_t mgmt_protected_rx;     /* protected unicast management frames accepted (decrypted, PN checked) */
	uint32_t mgmt_bip_rx;           /* group-addressed management frames with a valid MMIE */
	uint32_t mgmt_dropped_unprot;   /* robust management frames dropped for coming unprotected */
	uint32_t mgmt_dropped_replay;   /* PN/IPN not above the last one */
	uint32_t mgmt_dropped_mic;      /* CCMP MIC or BIP MIC wrong, or malformed */
	uint32_t mgmt_dropped_nokey;    /* protected, but no key to check it with */
	uint32_t sa_query_answered;     /* SA Query requests of the AP answered */
	uint32_t sa_query_started;      /* SA Queries we started after an unprotected disconnection */
	uint32_t sa_query_timeouts;     /* ... that got no answer (the connection ended) */
	uint32_t assoc_comebacks;       /* association refused temporarily (status 30), tried again later */
	uint32_t sae_tx;                /* SAE authentication frames sent */
	uint32_t sae_rx;                /* ... received from the candidate */
	uint32_t sae_ind;               /* SAE indications for the OS */
	uint32_t sae_params;            /* SAE parameters from the OS (WDI_SET_SAE_AUTH_PARAMS) taken */
	/* the channel width, and what comes in from the AP */
	uint8_t phy;                    /* RTW_PHY_* of the attempt or connection */
	uint8_t width;                  /* MHz the AP's station uses (20/40/80), 0 = idle */
	uint8_t center;                 /* channel number of the channel's center frequency, 0 = idle */
	uint8_t bw_setting;             /* Parameters\Bandwidth in effect (MHz) */
	uint8_t rx_last_enc;            /* last data frame from the AP: RTW_PHY_* of its encoding (HE = 3) */
	uint8_t rx_last_bw;             /* ... its width in MHz */
	uint8_t rx_last_mcs;            /* ... MCS (HT 0-31, VHT 0-9); legacy: rtw89's rate index */
	uint8_t rx_last_nss;            /* ... spatial streams */
	uint32_t rx_amsdu;              /* A-MSDUs received from the AP */
	uint32_t rx_dropped_amsdu;      /* ... dropped whole: group addressed, malformed, first DA = RFC 1042 header */
	uint32_t rx_amsdu_skipped;      /* A-MSDU subframes not passed up (DA not us nor a group; 802.1X DA) */
	uint32_t rx_htc;                /* data frames from the AP with an HT Control field (+HTC, Order bit) */
	uint32_t rx_mgmt_htc;           /* management frames from the BSS with +HTC */
	uint32_t tx_kbps;               /* TX rate rtw89 last reported (kbit/s), 0 = none yet */
};                                      /* 288 bytes */
#define RTW_LINK_KEY_PAIRWISE   0x01
#define RTW_LINK_KEY_GROUP(i)   (0x02 << (i))   /* group key ID i (0-3) */
#define RTW_LINK_KEY_IGTK(i)    (0x20 << ((i) - 4))     /* integrity group key ID i (4-5) */
#define RTW_CRYPTO_UNTESTED     0
#define RTW_CRYPTO_PASSED       1
#define RTW_CRYPTO_FAILED       2
void rtw_linux_link_stats (struct rtw_link_stats *st);

/* IOCTL_RTW_SER: have the firmware raise a CPU exception, which rtw89's SER handles as a level 2 error
   (ieee80211_restart_hw: the mac layer restarts the driver, drops the connection and reports it lost). Only on a
   started mac layer with a channel context (connected or listening: the chip is powered). PASSIVE. 0, -ENODEV,
   -ENOTCONN (no channel context), -EBUSY (scanning or restarting), -EOPNOTSUPP (firmware), or rtw89's error. */
int rtw_linux_ser_trigger (void);

/* Data path (front end: wificx\adapter.cpp). TX frames are native 802.11 data frames as the OS gives them (24-byte
   header without QoS control or sequence number, LLC/SNAP, payload); RX frames go up the same way. */
struct rtw_frag {
	const void *p;
	uint32_t len;
	uint32_t reserved;
};                                      /* 16 bytes */
struct rtw_dp_ops {                     /* called at any IRQL <= DISPATCH_LEVEL, also inside the driver's locks */
	void (*tx_ready)(void *ctx);    /* the TX queues woke, or frames in flight dropped below the low mark */
	void (*rx_ready)(void *ctx);    /* frames are waiting in rtw_linux_rx_data() */
	void *ctx;
};
void rtw_linux_dp_attach (const struct rtw_dp_ops *ops);   /* PASSIVE; NULL detaches and waits for callbacks */
int rtw_linux_tx_ready (void);          /* 0, -ENOTCONN (drop the frame), -EBUSY (keep it, wait for tx_ready) */
void rtw_linux_tx_drop (uint32_t n);    /* count frames the front end dropped while not connected */
#define RTW_TX_EAPOL            0x1     /* 802.1X frame (port control protocol): highest priority; the mac layer
					   also recognises it by its LLC/SNAP type */
#define RTW_TX_EXEMPT_VALID     0x2     /* the OS gave this frame an exemption action (RTW_TX_EXEMPT) */
#define RTW_TX_EXEMPT(a)        (((uint32_t)(a) & 3) << 4)      /* RTW_EXEMPT_* */
#define RTW_TX_EXEMPT_GET(f)    (((f) >> 4) & 3)
/* <= DISPATCH_LEVEL; copies the frame, so the OS's buffers can go back at once. Returns 0, -ENOTCONN, -EBUSY,
   -EINVAL (a frame we cannot send: dropped and counted), -EACCES (must be protected but there is no key yet;
   dropped and counted) or -ENOMEM. On an RSN connection the frame goes out with CCMP (CCMP header and PN made
   here, the hardware encrypts) unless the exemption rules let it go in clear. */
int rtw_linux_tx_data (const struct rtw_frag *frags, uint32_t n, uint8_t prio, uint32_t flags);
/* <= DISPATCH_LEVEL; copies the next received frame (24-byte data header, LLC/SNAP, payload; decrypted, CCMP header
   and MIC removed) into buf; returns its length, 0 if none is waiting. A frame longer than cap is dropped and counted
   (the next one is tried). *info: RTW_RX_* of the frame. */
#define RTW_RX_PROTECTED        0x1     /* it came protected (CCMP) */
#define RTW_RX_EXEMPT           0x2     /* it came in clear on an RSN connection, let up by RTW_RX_EXEMPT_GET */
#define RTW_RX_EXEMPT_GET(i)    (((i) >> 4) & 3)                /* RTW_EXEMPT_* of the entry that matched */
uint32_t rtw_linux_rx_data (void *buf, uint32_t cap, uint32_t *info);
uint32_t rtw_linux_rx_pending (void);   /* frames waiting; any IRQL <= DISPATCH_LEVEL */
void rtw_linux_rx_flush (void);         /* drops what is waiting */

/* Connections (front end: wificx\connect.cpp): open networks, WPA2-Personal (RSN, PSK, CCMP), WPA3-Personal (SAE)
   and management frame protection (MFP, 802.11w). */
#define RTW_CONN_MAX_CANDS      4
#define RTW_AUTH_OPEN           0       /* open system, no cipher */
#define RTW_AUTH_RSN_PSK        1       /* RSN, AKM PSK (00-0F-AC:2), pairwise and group CCMP; MFP if both allow */
#define RTW_AUTH_SAE            2       /* RSN, AKM SAE (00-0F-AC:8), pairwise and group CCMP, MFP required */
#define RTW_AUTH_BIT(a)         (1u << (a))
#define RTW_CIPHER_NONE         0       /* = WDI_CIPHER_ALGO_NONE */
#define RTW_CIPHER_CCMP         4       /* = WDI_CIPHER_ALGO_CCMP */
#define RTW_CIPHER_BIP          6       /* = WDI_CIPHER_ALGO_BIP (BIP-CMAC-128) */
#define RTW_CONN_F_EXCLUDE_UNENCRYPTED 0x1      /* dot11ExcludeUnencrypted: unprotected frames only if exempt */
#define RTW_CONN_F_HIDDEN       0x2     /* the OS connects to a hidden network: a BSS body may carry no SSID */
#define RTW_CONN_F_PINNED       0x4     /* SSID pinned by AllowSsid: a BSS body must carry it, even when hidden */
#define RTW_CONN_F_MFP          0x8     /* the OS lets us advertise MFP capability (MFPEnabled) */
#define RTW_CONN_F_NO_VHT       0x10    /* the OS's ActivePhyTypeList leaves VHT out: HT at most */
struct rtw_conn_cand {
	uint8_t bssid[6];
	uint8_t band;                   /* NL80211_BAND_* */
	uint8_t channel;
	uint16_t body_len;              /* beacon or probe response body, from the Timestamp field on */
	uint16_t reserved;
	uint32_t body_off;              /* offset of the body in rtw_connect_req.bodies */
};                                      /* 16 bytes */
struct rtw_connect_req {
	uint8_t ssid_len;
	uint8_t ssid[32];
	uint8_t n_cands;
	uint8_t auth;                   /* RTW_AUTH_* the OS asked for (every candidate must offer it, or one of auths) */
	uint8_t flags;                  /* RTW_CONN_F_* */
	struct rtw_conn_cand cands[RTW_CONN_MAX_CANDS];
	uint32_t bodies_len;
	const uint8_t *bodies;          /* read only until rtw_linux_connect() returns (the mac layer copies) */
	uint32_t ie_len;                /* elements the OS wants in the association request (vendor), 0 = none */
	uint8_t auths;                  /* RTW_AUTH_BIT()s of the RSN authentications allowed (a candidate gets the
					   first it offers of SAE, PSK); 0 = auth only */
	uint8_t reserved2[3];
	const uint8_t *ies;             /* like bodies */
};                                      /* 128 bytes */
#define RTW_CONN_EV_ATTEMPT     0x1     /* one candidate is done: rtw_linux_conn_get() has its result */
#define RTW_CONN_EV_CONNECTED   0x2
#define RTW_CONN_EV_FAILED      0x4     /* all candidates failed (or the attempt was cancelled) */
#define RTW_CONN_EV_LOST        0x8     /* the connection ended without rtw_linux_disconnect() */
#define RTW_CONN_EV_LINK        0x10    /* link quality changed (rate limited) */
#define RTW_CONN_EV_SAE         0x20    /* SAE has something for the OS: rtw_linux_sae_get() */
/* Called with the mac layer's lock held at PASSIVE_LEVEL; may only record and signal (like rtw_scan_done_fn).
   Events are bits; one call may carry several, in the order SAE, ATTEMPT, CONNECTED/FAILED. */
typedef void (*rtw_conn_event_fn) (void *ctx, uint32_t events);
/* PASSIVE. 0 (events follow), -ENODEV, -ENOTCONN (not started), -EBUSY (listening, connecting or connected),
   -EINVAL (no usable candidate), -ENOMEM */
int rtw_linux_connect (const struct rtw_connect_req *req, rtw_conn_event_fn fn, void *ctx);
void rtw_linux_connect_cancel (void);   /* PASSIVE; ATTEMPT (aborted) + FAILED follow if a connect was running */
/* PASSIVE; returns when the connection (or attempt) is torn down; sends a deauthentication if tx; no event */
void rtw_linux_disconnect (uint16_t reason, int tx);
#define RTW_ASSOC_ABORTED       5       /* WDI_ASSOC_STATUS_* the front end uses as they are */
#define RTW_ASSOC_NO_CHANNEL    31      /* candidate not usable (channel, not heard recently) */
#define RTW_ASSOC_AUTH_NO_ACK   40
#define RTW_ASSOC_AUTH_NO_RESP  41
#define RTW_ASSOC_AUTH_REFUSED  44
#define RTW_ASSOC_ASSOC_NO_ACK  50
#define RTW_ASSOC_ASSOC_NO_RESP 51
#define RTW_ASSOC_ASSOC_REFUSED 54
#define RTW_ASSOC_FAILURE       1       /* anything else (driver error) */
#define RTW_LOST_BY_HOST        7       /* WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST */
#define RTW_LOST_PEER_DEAUTH    13
#define RTW_LOST_PEER_DISASSOC  14
#define RTW_LOST_UNREACHABLE    61      /* beacon or connection loss, hardware restart (SER) */
struct rtw_conn_info {                  /* the last attempt, the connection, or how it ended */
	int32_t status;                 /* 0 or RTW_ASSOC_* / RTW_LOST_* */
	uint16_t status_code;           /* 802.11 status code (attempt) or reason code (lost) */
	uint16_t aid;
	uint8_t bssid[6];
	uint8_t band, channel;
	int8_t rssi;                    /* dBm */
	uint8_t wmm, ht;
	uint8_t phy;                    /* RTW_PHY_* in use (an attempt: the one asked for) */
	uint32_t tx_kbps, rx_kbps;
	uint16_t assoc_req_len, assoc_resp_len, auth_resp_len, bss_body_len, lost_frame_len, reserved2;
	uint8_t auth;                   /* RTW_AUTH_* negotiated */
	uint8_t pairwise, group;        /* RTW_CIPHER_* */
	uint8_t mgmt_group;             /* group management cipher: RTW_CIPHER_BIP with MFP, else NONE */
	uint8_t mfp;                    /* management frame protection negotiated */
	uint8_t width_mhz;              /* 20, 40 or 80: the width the AP's station uses */
	uint8_t center;                 /* channel number of the channel's center frequency */
	uint8_t reserved3;
	/* the caller's buffer continues with the frame bodies in this order */
};                                      /* 48 bytes */
#define RTW_PHY_LEGACY          0       /* = WIDE_MODE_* (mac\wide.h) */
#define RTW_PHY_HT              1
#define RTW_PHY_VHT             2
#define RTW_PHY_HE              3       /* rtw_link_stats.rx_last_enc only (HE is not used) */
/* PASSIVE. Takes the oldest failed attempt not taken yet (returns 1; no frames), else describes the connection
   (returns 0; with buf, its frames follow in the order of the *_len fields). -ENOSPC if buf is too small. */
int rtw_linux_conn_get (struct rtw_conn_info *info, void *buf, uint32_t len);

/* Parameters\Bandwidth, the widest channel (MHz) the station uses on 5 GHz: 20 (HT20, no VHT in the association
   request), 40 (HT40/VHT40) or 80 (VHT80, the default); 2.4 GHz and the GO stay at 20 MHz. PASSIVE, after a probe
   (driver\probe.c), before the front end reports its capabilities; kept for the driver's lifetime like
   rtw_linux_set_pmf. Another value counts as the default. */
#define RTW_WIDE_DEFAULT        80
void rtw_linux_set_wide (uint32_t mhz);
uint32_t rtw_linux_get_wide (void);     /* any IRQL: 20, 40 or 80 */

/*
 * Keys and the privacy exemption list (front end: wificx\security.cpp; mac\key.c). The OS runs the 4-way and
 * group key handshakes over the data path and hands the keys over; the mac layer installs them in rtw89 (set_key,
 * the security CAM), makes the CCMP header and PN of each frame it sends, and checks the PN of each frame the
 * hardware decrypted. Key bytes never go to the log.
 */
#define RTW_KEY_PAIRWISE        1
#define RTW_KEY_GROUP           2
#define RTW_KEY_IGTK            3       /* integrity group key (BIP-CMAC-128), with MFP */
#define RTW_KEY_F_RSC           0x1     /* rsc is valid */
struct rtw_key {
	uint8_t type;                   /* RTW_KEY_* */
	uint8_t index;                  /* key ID: pairwise 0, group 0-3, IGTK 4-5 */
	uint8_t cipher;                 /* RTW_CIPHER_CCMP; RTW_CIPHER_BIP for the IGTK */
	uint8_t len;                    /* bytes in key: 16 */
	uint8_t peer[6];                /* pairwise: the AP (all zero = the connected BSS) */
	uint8_t rsc[6];                 /* group: receive sequence counter, the PN to accept frames above; IGTK: IPN */
	uint8_t flags;                  /* RTW_KEY_F_* */
	uint8_t reserved[3];
	uint8_t key[32];
};                                      /* 52 bytes */
/* PASSIVE. Installs (or replaces) a key of the connection: 0, -ENOTCONN (not associated), -EINVAL, -EOPNOTSUPP
   (cipher, or an IGTK without MFP), -ENOMEM, -EIO (rtw89 refused it). The first pairwise key authorizes the AP's
   station. */
int rtw_linux_add_key (const struct rtw_key *key);
/* PASSIVE. 0, or -ENOENT if there is no such key. */
int rtw_linux_del_key (uint8_t type, uint8_t index);

#define RTW_EXEMPT_MAX          8
#define RTW_EXEMPT_NONE         0       /* = WDI_EXEMPT_NO_EXEMPTION */
#define RTW_EXEMPT_ALWAYS       1       /* = WDI_EXEMPT_ALWAYS */
#define RTW_EXEMPT_NO_KEY       2       /* = WDI_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE */
#define RTW_EXEMPT_UNICAST      1       /* = WDI_EXEMPT_PACKET_TYPE_* */
#define RTW_EXEMPT_MULTICAST    2
#define RTW_EXEMPT_BOTH         3
struct rtw_exemption {
	uint16_t ethertype;             /* host order */
	uint8_t action;                 /* RTW_EXEMPT_NONE/ALWAYS/NO_KEY */
	uint8_t packet_type;            /* RTW_EXEMPT_UNICAST/MULTICAST/BOTH */
};                                      /* 4 bytes */
/* PASSIVE. Replaces the list (at most RTW_EXEMPT_MAX entries are kept); n = 0 or list = NULL empties it. 802.1X
   (0x888E) without an entry counts as RTW_EXEMPT_NO_KEY. */
void rtw_linux_set_exemptions (const struct rtw_exemption *list, uint32_t n);

/*
 * Management frame protection and SAE. The 8852B does not protect management frames itself, so
 * the mac layer does CCMP and BIP for them in software (mac\pmf.c, AES of the kernel's CNG: driver\crypto.c).
 */
#define RTW_PMF_OFF             0       /* Parameters\Pmf = 0 (the default): no MFP capability, no SAE */
#define RTW_PMF_ON              1       /* MFP, no SAE */
#define RTW_PMF_SAE             2       /* MFP and SAE */
/* PASSIVE, after a probe (the caller serializes it with probe/remove). Returns the mode in effect: the one asked for,
   or RTW_PMF_OFF when the crypto self-tests failed. Kept for the driver's lifetime. */
uint32_t rtw_linux_set_pmf (uint32_t mode);
uint32_t rtw_linux_get_pmf (void);      /* any IRQL: the mode in effect */

/* SAE (WPA3-Personal) under WiFiCx: the OS does the SAE computations, the mac layer (mac\sae.c) sends and receives
   the authentication frames. The front end turns each indication into WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED and each
   WDI_SET_SAE_AUTH_PARAMS into rtw_linux_sae_set(). */
#define RTW_SAE_IND_PARAMS      0       /* = WDI_SAE_INDICATION_TYPE_COMMIT_REQUEST_PARAMS_NEEDED */
#define RTW_SAE_IND_COMMIT      1       /* = ..._COMMIT_FRAME: the AP's commit */
#define RTW_SAE_IND_CONFIRM     2       /* = ..._CONFIRM_FRAME: the AP's confirm */
#define RTW_SAE_IND_ERROR       3       /* = ..._ERROR */
#define RTW_SAE_IND_RESEND      4       /* = ..._CONFIRM_REQUEST_RESEND_REQUEST: a new confirm, please */
struct rtw_sae_ind {
	uint8_t bssid[6];
	uint8_t type;                   /* RTW_SAE_IND_* */
	uint8_t has_status;             /* status is valid (ERROR, RESEND) */
	uint32_t status;                /* WDI_SAE_STATUS_* */
	uint16_t frame_len;             /* COMMIT, CONFIRM: the AP's authentication frame body, from the algorithm
					   number on, follows in the caller's buffer */
	uint16_t reserved;
};                                      /* 16 bytes */
/* PASSIVE. Takes the oldest indication not taken yet: 1 (the frame body in buf), 0 if none, -ENOSPC if buf is too
   small (it stays queued), -ENODEV. */
int rtw_linux_sae_get (struct rtw_sae_ind *ind, void *buf, uint32_t len);

#define RTW_SAE_REQ_COMMIT      0       /* = WDI_SAE_REQUEST_TYPE_COMMIT_PARAMS */
#define RTW_SAE_REQ_CONFIRM     1       /* = ..._CONFIRM_PARAMS */
#define RTW_SAE_REQ_FAILURE     2       /* = ..._FAILURE */
#define RTW_SAE_REQ_SUCCESS     3       /* = ..._SUCCESS */
#define RTW_SAE_REQ_COMMIT_H2E  4       /* = ..._COMMIT_H2E_PARAMS: hash-to-element */
struct rtw_sae_params {
	uint8_t bssid[6];
	uint8_t type;                   /* RTW_SAE_REQ_* */
	uint8_t akm;                    /* commit: suite type n of the AKM 00-0F-AC:n the OS gave, 0 = none given */
	uint16_t group;                 /* commit: finite cyclic group */
	uint16_t status_code;           /* commit: the status code field the OS gave (0 = ours: 0, or 126 with H2E) */
	uint16_t send_confirm;          /* confirm */
	uint16_t scalar_len, element_len, token_len, rejected_len, confirm_len;
	uint32_t sae_status;            /* failure, success: WDI_SAE_STATUS_* */
	uint32_t reserved;
	const uint8_t *scalar;          /* commit */
	const uint8_t *element;
	const uint8_t *token;           /* anti-clogging token (H2E: or its Anti-Clogging Token Container element) */
	const uint8_t *rejected;        /* H2E: rejected groups (or the Rejected Groups element) */
	const uint8_t *confirm;         /* confirm */
};                                      /* 72 bytes */
/* PASSIVE. The OS's next SAE step for the attempt running with bssid. 0; -ENOTCONN (no SAE authentication with that
   BSS now); -EINVAL (malformed, or not expected in this state); -EBUSY (hardware restart); -ENODEV. The mac layer
   copies what it needs before returning. */
int rtw_linux_sae_set (const struct rtw_sae_params *p);

/*
 * Regulatory domain, 802.11 power save, and the radio/coexistence diagnostics. Shared by clang and
 * MSVC like the ones above (mac\ps.c and driver\diag.c check the sizes).
 */

/* Regulatory (mac\reg.c): the rules of a country from wireless-regdb decide which channels may transmit. Sources,
   strongest first: the user's country (Parameters\Country), the Country element of the connected BSS, the efuse,
   the world rules. PASSIVE, probed, with the caller serializing it with probe/remove; returns after the rules are
   applied. alpha2 NULL (or "00") clears it. 0, -ENODEV, -EINVAL (not two letters), -ENOENT (no rules for it: the
   other sources apply). */
int rtw_linux_reg_set_user (const char *alpha2);

/* 802.11 power save (mac\ps.c). The policy comes from three sources; the connection then uses rtw89's leisure power
   save (LPS) while it is authorized and not probing the AP. PASSIVE; the caller serializes it with probe/remove. The
   values are kept across probe/remove (the driver's lifetime). */
#define RTW_PS_SRC_REG          0       /* Parameters\PowerSave: RTW_PS_REG_* */
#define RTW_PS_SRC_PLAN         1       /* power plan, wireless adapter power saving mode: 0 maximum performance,
					   1 low, 2 medium, 3 maximum power saving */
#define RTW_PS_SRC_HINT         2       /* WDI_CONNECTION_QUALITY_HINT of WDI_SET_CONNECTION_QUALITY: 1 auto power save,
					   2 low latency, 3 high throughput, 4 no power save */
#define RTW_PS_REG_NEVER        0
#define RTW_PS_REG_FOLLOW_OS    1       /* the default */
#define RTW_PS_REG_ALWAYS       2       /* tests */
void rtw_linux_set_ps_policy (uint32_t src, uint32_t value);

/* IOCTL_RTW_RADIO: regulatory domain, power save and Bluetooth coexistence state. PASSIVE (takes the mac layer's
   lock); with nothing probed only the policy fields are filled. */
#define RTW_REG_SRC_WORLD       0
#define RTW_REG_SRC_USER        1
#define RTW_REG_SRC_IE          2       /* Country element of the connected BSS */
#define RTW_REG_SRC_EFUSE       3
#define RTW_RF_POWERON          0x01    /* rtw89 flags (RTW89_FLAG_*) */
#define RTW_RF_LEISURE_PS       0x02
#define RTW_RF_LOW_POWER_MODE   0x04
#define RTW_RF_INACTIVE_PS      0x08
#define RTW_RF_RUNNING          0x10
#define RTW_BT_PROFILE_HFP      0x01
#define RTW_BT_PROFILE_HID      0x02
#define RTW_BT_PROFILE_A2DP     0x04
#define RTW_BT_PROFILE_PAN      0x08
#define RTW_BTC_NOTIFY_MAX      24      /* BTC_NCNT_* counters kept (rtw89info.c asserts BTC_NCNT_NUM fits) */
struct rtw_radio_info {
	uint8_t probed;                 /* the rest is valid */
	uint8_t reg_src;                /* RTW_REG_SRC_* applied */
	char reg_cur[2];                /* "00" = world */
	char reg_user[2];               /* 0 = none */
	char reg_ie[2];                 /* Country element of the connected BSS, 0 = none */
	char reg_efuse[2];              /* 0 = not programmed */
	uint8_t reg_dfs_region;         /* NL80211_DFS_* of the rules applied */
	uint8_t reg_ie_ignored;         /* rtw89 set REGULATORY_COUNTRY_IE_IGNORE */
	char rtw_alpha2[2];             /* the country of rtw89's TX power tables */
	uint8_t rtw_txpwr_regd[2];      /* rtw89's TX power regulation per band (2.4, 5 GHz), RTW89_FCC ... */
	uint8_t n_active[2];            /* channels per band (2.4, 5 GHz): may transmit */
	uint8_t n_passive[2];           /* listen only (NO_IR or radar) */
	uint8_t n_disabled[2];
	uint16_t beacon_int;            /* TU, of the connected BSS; 0 = not connected */
	uint32_t reg_changes;           /* rule sets applied since the driver was loaded */
	uint8_t ps_reg;                 /* RTW_PS_REG_* */
	int8_t ps_plan;                 /* -1 = not known */
	uint8_t ps_hint;                /* 0 = none received */
	uint8_t ps_policy;              /* 1: power save wanted */
	uint8_t ps_cfg;                 /* vif->cfg.ps now */
	uint8_t lps_enabled;            /* rtw89 */
	uint8_t ps_mode;                /* rtw89 deep power save mode: 0 none, 1 RF off, 2 clock gated, 3 power gated */
	uint8_t rtw_flags;              /* RTW_RF_* */
	uint32_t ps_changes;            /* times cfg.ps changed */
	uint32_t ps_ms;                 /* milliseconds with cfg.ps on since the driver was loaded */
	/* Bluetooth coexistence (rtw89 btc) */
	uint8_t btc_lps;                /* btc keeps LPS off (rtw89 btc.lps) */
	uint8_t bt_enable;
	uint8_t bt_profile_cnt;
	uint8_t bt_profiles;            /* RTW_BT_PROFILE_* */
	uint32_t bt_fw_ver;
	uint32_t bt_scbd;               /* scoreboard as last read */
	uint16_t btc_policy_type;
	uint8_t btc_run_reason;
	uint8_t btc_run_action;
	uint8_t btc_tdma[8];            /* the TDMA in use (struct rtw89_btc_fbtc_tdma, raw) */
	uint8_t ant_type, ant_num, ant_isolation, ant_single_pos;
	uint8_t ant_btg_pos, wl_only, bt_only, freerun;
	uint32_t btc_c2h;               /* firmware C2H events of the btc class */
	uint32_t btc_h2c, btc_h2c_fail;
	uint32_t btc_fw_reports;        /* firmware reports (BTF_EVNT_RPT) */
	uint32_t bt_info_updates;       /* BT info from the BT side (BTC_BCNT_INFOUPDATE) */
	uint32_t bt_scbd_updates;       /* scoreboard changes (BTC_BCNT_SCBDUPDATE) */
	uint32_t btc_runs;              /* coex decisions (BTC_DCNT_RUN) */
	uint32_t btc_notify[RTW_BTC_NOTIFY_MAX];   /* rtw89 dm.cnt_notify[BTC_NCNT_*]: which hooks were called */
};                                      /* 200 bytes */
void rtw_linux_radio_info (struct rtw_radio_info *info);
/* IOCTL_RTW_BTC: rtw89's coexistence report (rtw89_btc_dump_info, the text of Linux's debugfs btc_info), only while
   the chip is powered (it sends H2C and reads registers). PASSIVE. Returns the bytes written (no NUL), -ENODEV, or
   -ENETDOWN while the chip is powered off. */
int32_t rtw_linux_btc_dump (char *buf, uint32_t len);

/*
 * The Windows mobile hotspot, which WiFiCx runs as a Wi-Fi Direct group owner (GO) on a role port. The
 * mac layer (mac\ap.c) runs a second rtw89 interface of type AP while the GO is started: the beacon template (the
 * firmware sends the beacons), probe and open system authentication responses, the (re)association requests the OS
 * decides on, the clients' stations and keys, power save buffering and the GO's data path. The OS's authenticator runs
 * the 4-way handshake over that data path, like the station's. WPA2-Personal (PSK, CCMP) or open only. Shared by
 * clang and MSVC like the ones above (mac\ap.c and wificx\wifi.h check the sizes).
 */
#define RTW_WFD_OFF             0       /* Parameters\WifiDirect = 0: no Wi-Fi Direct capabilities, no GO */
#define RTW_WFD_ON              1
/* PASSIVE, after a probe (driver\probe.c); kept for the driver's lifetime like rtw_linux_set_pmf */
void rtw_linux_set_wfd (uint32_t mode);
uint32_t rtw_linux_get_wfd (void);      /* any IRQL */

/* The channels of a band (0 = 2.4 GHz, 1 = 5 GHz) a GO may beacon on now: not DISABLED, NO_IR or RADAR under the
   regulatory domain in force, no channel 14. PASSIVE. Returns how many were stored. */
uint32_t rtw_linux_ap_channels (uint8_t band, uint8_t *out, uint32_t max);
/* The station's channel while it is connecting or connected: 1 with band/channel, else 0. PASSIVE. */
int rtw_linux_sta_channel (uint8_t *band, uint8_t *channel);

#define RTW_AP_MAX_CLIENTS      8
#define RTW_AP_IES_MAX          512     /* the OS's additional beacon or probe response elements, each */
#define RTW_AP_F_RSN            0x1     /* WPA2-Personal: privacy, RSN element (PSK, CCMP) */
#define RTW_AP_F_EXCLUDE_UNENCRYPTED 0x2        /* recorded only: an RSN GO passes frames in clear only when exempt,
                                                   whatever START_AP says (the OS starts a WPA2 hotspot with it
                                                   FALSE yet opens the client's port at association); as mac80211's
                                                   ieee80211_drop_unencrypted() */
#define RTW_AP_F_11B            0x4     /* 2.4 GHz: the CCK rates are supported and basic */
struct rtw_ap_start {
	uint8_t addr[6];                /* the GO's interface address, its BSSID */
	uint8_t band;                   /* NL80211_BAND_* */
	uint8_t channel;
	uint8_t ssid_len;               /* 1..32 */
	uint8_t ssid[32];
	uint8_t dtim_period;            /* 1..255 */
	uint16_t beacon_int;            /* TU, 20..1000 */
	uint8_t flags;                  /* RTW_AP_F_* */
	uint8_t reserved[3];
};                                      /* 48 bytes */
/* Called with the mac layer's lock held at PASSIVE_LEVEL; may only record and signal (like rtw_conn_event_fn). */
#define RTW_AP_EV_IND           0x1     /* rtw_linux_ap_get_ind() has something */
typedef void (*rtw_ap_event_fn) (void *ctx, uint32_t events);
/* PASSIVE. Starts the GO on the channel: 0 (events follow), -ENODEV, -ENOTCONN (not started), -EBUSY (running,
   restarting, listening), -EINVAL, -ERANGE (the channel may not beacon), -EXDEV (the station uses another channel),
   -ENOMEM, or the driver's error. */
int rtw_linux_ap_start (const struct rtw_ap_start *req, rtw_ap_event_fn fn, void *ctx);
/* PASSIVE. Stops the GO if it runs: a broadcast deauthentication first when tx, then every client, the keys and the
   interface. No events; the indications still queued are dropped. */
void rtw_linux_ap_stop (int tx);
/* PASSIVE. The OS's additional elements for beacons and probe responses (whole elements, RTW_AP_IES_MAX bytes each;
   len 0 clears); kept across GO starts; a running GO's beacon is updated. 0 or -EINVAL. */
int rtw_linux_ap_set_ies (const uint8_t *beacon, uint32_t beacon_len, const uint8_t *presp, uint32_t presp_len);

#define RTW_AP_IND_ASSOC_REQ    1       /* a client asks to (re)associate; its request body (from the capability field
					   on) follows */
#define RTW_AP_IND_ASSOC_DONE   2       /* the association response we sent for rtw_linux_ap_assoc_resp(): status 0 =
					   acknowledged (and the client associated, if accepted), else -errno; the
					   response body, then the beacon's body (from the Timestamp field on) follow */
#define RTW_AP_IND_LEFT         3       /* an associated client is gone: status = RTW_LOST_* (WDI_ASSOC_STATUS_*), code =
					   the reason; the deauthentication/disassociation body it sent may follow */
#define RTW_AP_IND_STOPPED      4       /* the mac layer stopped the GO itself: status = RTW_AP_STOP_* */
#define RTW_AP_STOP_RESTART     1       /* hardware restart (SER), done: the GO may start again */
#define RTW_AP_STOP_CHANNEL     2       /* the regulatory domain no longer lets the channel beacon */
#define RTW_LOST_BY_RESET       60      /* WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET */
struct rtw_ap_ind {
	uint8_t type;                   /* RTW_AP_IND_* */
	uint8_t peer[6];
	uint8_t reassoc;                /* ASSOC_REQ, ASSOC_DONE: a reassociation request */
	int32_t status;                 /* see the types */
	uint16_t code;                  /* 802.11 status code (ASSOC_DONE) or reason code (LEFT) */
	uint16_t frame_len;             /* body that follows in the caller's buffer */
	uint16_t ies_len;               /* ASSOC_DONE: the beacon's body (fixed fields and elements) after the response */
	uint8_t accepted;               /* ASSOC_DONE: the response accepted the client */
	uint8_t ht, wmm, rsn;           /* ASSOC_DONE: what the association uses */
	uint8_t band;                   /* NL80211_BAND_* of the GO */
	uint8_t reserved[5];
};                                      /* 28 bytes */
/* PASSIVE. Takes the oldest indication: 1 (its frames in buf), 0 if none, -ENOSPC (buf too small: dropped). */
int rtw_linux_ap_get_ind (struct rtw_ap_ind *ind, void *buf, uint32_t len);
/* PASSIVE. The OS's answer to an association request: sends the (re)association response (accept: status 0 and an AID;
   else status, which must not be 0); ies = the OS's vendor elements for it (whole elements, at most RTW_AP_IES_MAX).
   RTW_AP_IND_ASSOC_DONE follows after 0; -ENOENT (no request of that client waits), -ENOTCONN, -EINVAL, -ENOMEM. */
int rtw_linux_ap_assoc_resp (const uint8_t *peer, int accept, uint16_t status, const uint8_t *ies, uint32_t ie_len);
/* PASSIVE. Deauthenticates a client (broadcast address: every client) and forgets it; no indication. 0, -ENOENT. */
int rtw_linux_ap_disconnect (const uint8_t *peer, uint16_t reason);
/* PASSIVE. Keys of the GO: pairwise with key->peer = an associated client, group (the GO sends group frames with the
   last one installed, or the one rtw_linux_ap_set_tx_key names). Like rtw_linux_add_key/del_key. */
int rtw_linux_ap_add_key (const struct rtw_key *key);
int rtw_linux_ap_del_key (uint8_t type, uint8_t index, const uint8_t *peer);
int rtw_linux_ap_set_tx_key (uint8_t index);
/* PASSIVE. The GO port's privacy exemption list (the station's stays apart). */
void rtw_linux_ap_set_exemptions (const struct rtw_exemption *list, uint32_t n);
/* The GO port's data path, like the station's above (rtw_linux_dp_attach ... rtw_linux_rx_flush). TX frames come from
   the OS with From DS (A1 = DA, A2 = BSSID, A3 = SA; also accepted without DS bits: A2 = SA, A3 = BSSID); RX frames go
   up with To DS (A1 = BSSID, A2 = SA, A3 = DA). rtw_linux_ap_tx_data also returns -ENOENT: no such client (dropped and
   counted). */
void rtw_linux_ap_dp_attach (const struct rtw_dp_ops *ops);
int rtw_linux_ap_tx_ready (void);
void rtw_linux_ap_tx_drop (uint32_t n);
int rtw_linux_ap_tx_data (const struct rtw_frag *frags, uint32_t n, uint8_t prio, uint32_t flags);
uint32_t rtw_linux_ap_rx_data (void *buf, uint32_t cap, uint32_t *info);
uint32_t rtw_linux_ap_rx_pending (void);
void rtw_linux_ap_rx_flush (void);
uint32_t rtw_linux_ap_clients (void);   /* associated clients now; any IRQL */

/* IOCTL_RTW_AP: the GO, its clients and counters; any IRQL <= DISPATCH_LEVEL (a snapshot) */
#define RTW_APC_F_AUTHORIZED    0x01
#define RTW_APC_F_PS            0x02
#define RTW_APC_F_HT            0x04
#define RTW_APC_F_WMM           0x08
struct rtw_ap_client {
	uint8_t addr[6];
	uint8_t state;                  /* mac\ap.c MAC_APC_*: 1 authenticated, 2 waits for the OS, 3 response sent,
					   4 associated */
	uint8_t flags;                  /* RTW_APC_F_* */
	uint16_t aid;
	uint16_t ps_queued;             /* frames buffered while it sleeps */
	uint32_t idle_ms;               /* since its last frame */
	uint32_t rx_data;
	uint32_t tx_data;
};                                      /* 24 bytes */
struct rtw_ap_stats {
	uint8_t running;
	uint8_t band;
	uint8_t channel;
	uint8_t n_clients;              /* entries in clients[] */
	uint8_t bssid[6];
	uint16_t beacon_int;
	uint32_t starts, stops;
	uint32_t beacons;               /* beacon templates built for the firmware */
	uint32_t probe_req, probe_resp, auth_rx, assoc_req, assoc_ok, assoc_refused, assoc_tx_failed;
	uint32_t deauth_rx, deauth_tx, class3, inactive, unauthorized;
	uint32_t tx_data, tx_dropped, tx_nokey, ps_buffered, ps_dropped, ps_released, pspoll;
	uint32_t rx_data, rx_dropped, rx_dup, rx_decrypted, rx_dropped_unencrypted, rx_dropped_replay;
	uint32_t key_adds, key_dels, mgmt_tx, mgmt_dropped, inds_dropped, reserved;
	struct rtw_ap_client clients[RTW_AP_MAX_CLIENTS];
};                                      /* 340 bytes */
void rtw_linux_ap_stats (struct rtw_ap_stats *st);

#ifdef __cplusplus
}
#endif

#endif
