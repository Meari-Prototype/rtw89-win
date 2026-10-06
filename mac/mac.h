/*
 * rtw89win mac: the stand-in for mac80211/cfg80211 under rtw89 (the linuxkpi_* backends of the FreeBSD headers
 * in compat\include\net). Internal declarations.
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_MAC_H
#define RTW_MAC_H

#include <rtw/linux.h>
#include <rtw/glue.h>
#include <net/mac80211.h>
#include "wide.h"

/* Allocation header in front of struct wiphy (which must stay last: wiphy->priv[] follows it). */
struct mac_wiphy {
	spinlock_t work_lock;
	struct list_head work_list;     /* queued wiphy_work items */
	struct work_struct work;        /* runs them on mac_wiphy_wq, holding wiphy->mtx */
	bool registered;
	struct wiphy wiphy;
};

/* An interface (mac80211's ieee80211_sub_if_data). */
struct mac_vif {
	struct list_head list;          /* mac_hw.vif_list */
	struct mac_hw *mh;
	u8 bssid[ETH_ALEN];             /* bss_conf.bssid points here; zero while not associated */
	struct ieee80211_vif vif;       /* must stay last: vif.drv_priv[] follows */
};

/* A channel context (mac80211's ieee80211_chanctx). */
struct mac_chanctx {
	struct list_head list;          /* mac_hw.chanctx_list */
	int users;                      /* interfaces assigned to it (the station and the GO share one, SCC) */
	struct ieee80211_chanctx_conf conf;     /* must stay last: conf.drv_priv[] follows */
};

/* Block ack agreements (agg.c), for the HT TIDs 0-7 (TIDs 8-15 of the 4-bit fields are refused or ignored). */
#define MAC_BA_TIDS                     8
#define MAC_BA_BUF                      64      /* reorder slots: the HT window limit, and 64 divides 4096 */
struct mac_ba_rx {                      /* a receive session (the AP aggregates); under mac_hw.rx_lock */
	u16 head;                       /* sequence number the window starts at (WinStartB): next to go up */
	u16 buf_size;                   /* window, 1..MAC_BA_BUF */
	u16 timeout;                    /* TUs, as agreed (not enforced) */
	u8 token;                       /* dialog token of the request */
	u8 stored;                      /* frames held */
	bool ap;                        /* a client of our GO sends: frames go up the GO's data path (ap.c) */
	struct sk_buff *frames[MAC_BA_BUF];     /* held at sequence number % MAC_BA_BUF */
	u64 held_ns[MAC_BA_BUF];
};

enum mac_ba_tx_state {                  /* a transmit session (we aggregate) */
	MAC_BA_TX_IDLE = 0,
	MAC_BA_TX_PENDING,              /* ADDBA request on its way: the TID's frames still go one by one */
	MAC_BA_TX_ON,                   /* operational: the TID's frames go with IEEE80211_TX_CTL_AMPDU */
};
struct mac_ba_tx {                      /* with wiphy->mtx held, except next_ns (read by the data path) */
	u64 next_ns;                    /* no new request before this (backoff); U64_MAX = never on this connection */
	u64 deadline_ns;                /* of the ADDBA response, 0 = none */
	u64 last_req_ns;
	u16 ssn;                        /* starting sequence number of the request */
	u16 buf_size;
	u8 token;
	u8 reqs;                        /* requests without an agreement since the last one */
};

/* A client of our group owner (ap.c), in its struct mac_sta. Changed with wiphy->mtx held unless noted. */
enum mac_apc_state {
	MAC_APC_NONE = 0,
	MAC_APC_AUTH,                   /* open system authentication answered */
	MAC_APC_WAIT_OS,                /* (re)association request indicated: the OS decides */
	MAC_APC_RESP,                   /* the OS accepted: our association response is on its way */
	MAC_APC_ASSOC,                  /* associated (the OS knows it) */
};
#define MAC_APC_F_PSPOLL                0       /* a PS-Poll came in (atomic bit) */
#define MAC_APC_F_TIM                   1       /* its TIM bit may have to change */
struct mac_key;
struct mac_ap_sta {
	u8 state;                       /* MAC_APC_*; read by the data path */
	bool reassoc;                   /* the request was a reassociation request */
	bool resp_accept;
	u8 resp_txs;                    /* the association response's tx status: 0 none yet, 1 acked, 2 failed (set
					   from tx.c in any context, WRITE_ONCE) */
	u16 resp_cookie;                /* in the response's mac_tag */
	u16 resp_status;
	bool wmm;                       /* the client asked for WMM (QoS data) */
	bool ps;                        /* in power save (PM bit), under mac_ap.ps_lock */
	bool ps_flush;                  /* awake again: the AP work sends what was buffered first */
	bool tim;                       /* its TIM bit as last told to the driver (set_tim) */
	unsigned long flags;            /* MAC_APC_F_* */
	u64 since_ns;                   /* when it entered the state */
	u64 last_rx_ns;                 /* last frame from it (data path), or our Null frame acknowledged */
	u64 probe_ns;                   /* inactive: when we sent it a Null frame to see whether it is there, 0 = none */
	u16 probe_cookie;               /* ... in that frame's mac_tag */
	struct mac_key *ptk;            /* its pairwise key (RCU, like mac_conn.ptk) */
	struct sk_buff_head ps_q;       /* frames buffered while it sleeps (lock: mac_ap.ps_lock) */
	u64 ps_ns;                      /* the buffer was last served (became non-empty, or frames left it) */
	u8 *req;                        /* its (re)association request body, kmalloc'ed */
	u16 req_len;
	u16 resp_len;
	u8 *resp;                       /* our association response body, kmalloc'ed */
	u32 rx_data, tx_data;
};

/* A station (mac80211's sta_info): the AP while connecting or connected (mlme.c). The data path reads it under
   rcu_read_lock() (mac_hw.conn.ap); it is freed only after list_del_rcu() and synchronize_rcu(). Also a client of
   our group owner (ap.c), with ap_client set and mv = the GO's interface. */
#define MAC_SEQ_NONQOS                  IEEE80211_NUM_TIDS      /* tx_seq/rx_seq slot of non-QoS data */
struct mac_sta {
	struct list_head list;          /* mac_hw.sta_list (or mac_hw.parked_sta after a hardware restart) */
	struct mac_vif *mv;
	enum ieee80211_sta_state state; /* as last told to the driver */
	void *txq_mem;                  /* sta.txq[] point into it */
	u16 tx_seq[IEEE80211_NUM_TIDS + 1];     /* next sequence number (data path; one TX queue, so one writer) */
	u16 rx_seq[IEEE80211_NUM_TIDS + 1];     /* last sequence control received, 0xffff = none (NAPI thread) */
	/* block ack (agg.c) */
	struct mac_ba_rx *ba_rx[MAC_BA_TIDS];   /* under mac_hw.rx_lock */
	struct timer_list ba_rx_timer;  /* releases frames held too long */
	u64 ba_rx_timer_ns;             /* under mac_hw.rx_lock: when the timer fires, 0 = not armed */
	bool ba_rx_dead;                /* under mac_hw.rx_lock: going away, the timer must not be armed again */
	u8 ba_tx_state[MAC_BA_TIDS];    /* MAC_BA_TX_*: changed with wiphy->mtx held, read by the data path */
	unsigned long ba_tx_want;       /* TIDs the data path asks a session for (atomic bits) */
	unsigned long ba_tx_stop;       /* TIDs rtw89 asked to stop (stop_tx_ba_session) */
	struct mac_ba_tx ba_tx[MAC_BA_TIDS];
	/* group owner */
	bool ap_client;                 /* a client of our GO (ap.c) */
	struct mac_ap_sta ap;
	struct ieee80211_sta sta;       /* must stay last: sta.drv_priv[] follows */
};

/* What a frame the mac layer handed to the driver is (sk_buff.mac_tag; tx.c). MGMT frames also sit in the
   pending table (mac_hw.txp) until their tx status, with their slot and MAC_TXT_* in the tag. */
#define MAC_SKB_DATA                    0x10000
#define MAC_SKB_MGMT                    0x20000
#define MAC_SKB_AP                      0x40000 /* a frame of our GO (ap.c): with DATA or MGMT, and the low 16 bits a
						   cookie (the association response's); not in the pending table */
#define MAC_SKB_SLOT(tag)               (((tag) >> 8) & 0xff)
#define MAC_SKB_TXT(tag)                ((tag) & 0xff)
enum mac_txt {
	MAC_TXT_PROBE = 1,              /* IOCTL_RTW_TXPROBE */
	MAC_TXT_AUTH,
	MAC_TXT_ASSOC,
	MAC_TXT_DEAUTH,
	MAC_TXT_NULL,                   /* probing the AP after a beacon loss */
	MAC_TXT_ACTION,
};
#define MAC_TXP_MAX                     16
struct mac_txp {
	struct sk_buff *skb;            /* NULL = free */
	u8 txt;
};

/* The station MLME (mlme.c). */
#define MAC_FRAME_MAX                   2304    /* frame body limit (802.11-2020 9.2.4.7) */
#define MAC_ASSOC_REQ_MAX               512
#define MAC_ASSOC_RESP_MAX              1024
#define MAC_AUTH_RESP_MAX               256
#define MAC_LOST_FRAME_MAX              256

struct mac_cand {                       /* a BSS the OS asked us to try */
	u8 bssid[ETH_ALEN];
	u8 band, channel;
	u16 body_len;
	u8 *body;                       /* beacon/probe response body (from the Timestamp on), kmalloc'ed */
};

struct mac_bss_params {                 /* what the MLME parsed from the BSS's beacon/probe response and assoc resp */
	u16 capability;
	u16 beacon_int;
	u8 dtim_period;
	u8 ds_channel;
	u32 rates;                      /* bit i: sband->bitrates[i] is supported */
	u32 basic_rates;                /* ... and basic */
	bool erp, erp_protection, erp_barker;
	bool ht;                        /* HT Capabilities and Operation present */
	u8 ht_cap[26];
	u8 ht_oper[22];
	bool wmm;                       /* WMM Information/Parameter element */
	bool wmm_params;                /* WMM Parameter element (with the AC parameters) */
	u8 wmm_ac[4][4];                /* AC parameter records as received: ACI/AIFSN, ECW, TXOP (le16) */
	/* the SSID and the security the BSS offers (beacon/probe response only) */
	const u8 *ssid;                 /* points into the candidate body */
	u8 ssid_len;
	bool wpa;                       /* WPA (vendor) element */
	bool rsn;                       /* RSN element, parsed below */
	bool rsn_bad;                   /* ... but malformed */
	u32 rsn_group;                  /* group cipher suite selector, OUI and type, big endian (00-0F-AC:4 CCMP) */
	bool rsn_ccmp;                  /* CCMP among the pairwise ciphers */
	bool rsn_psk;                   /* PSK (00-0F-AC:2) among the AKMs */
	u16 rsn_caps;                   /* RSN capabilities, 0 if absent */
	/* for the regulatory hint, SAE and management frame protection (beacon/probe response only) */
	bool tim;                       /* TIM element: the body is a beacon, dtim_period is the AP's */
	u8 country_len;                 /* Country element, 0 = none */
	u8 country[6];                  /* its first bytes: alpha2, environment, first triplet */
	bool rsn_sae;                   /* SAE (00-0F-AC:8) among the AKMs */
	u32 rsn_mgmt_group;             /* group management cipher suite (BIP-CMAC-128, 00-0F-AC:6, when not given) */
	/* the elements that decide the channel width and VHT (wide.c; the association response updates the VHT
	   Capabilities and may add an Operating Mode Notification) */
	struct wide_bss w;
};

/* A key of the connection (key.c), mac80211's ieee80211_key. Published in mac_conn.ptk/gtk[] for the data
   path (RCU); freed only after it was unpublished, synchronize_rcu() and DISABLE_KEY. */
#define MAC_PN_MGMT                     (IEEE80211_NUM_TIDS + 1)        /* protected management frames */
#define MAC_PN_SLOTS                    (IEEE80211_NUM_TIDS + 2)        /* per TID, then non-QoS, then management */
struct mac_key {
	u64 rx_pn[MAC_PN_SLOTS];        /* highest PN accepted; under mac_hw.rx_lock (NAPI thread or the reorder timer),
					   except MAC_PN_MGMT and an IGTK's IPN ([0]): wiphy->mtx (MLME work) */
	bool lost;                      /* rtw89 dropped it from its CAM (SER): never use it for TX again */
	bool hw;                        /* rtw89 took it (set_key), so it gets DISABLE_KEY */
	struct ieee80211_key_conf conf; /* must stay last: conf.key[] follows */
};
#define MAC_KEY_MAX_LEN                 32

/* SAE authentication of the current candidate (sae.c), with wiphy->mtx held */
enum mac_sae_state {
	MAC_SAE_IDLE = 0,
	MAC_SAE_WAIT_COMMIT,            /* the OS has to give commit parameters */
	MAC_SAE_COMMITTED,              /* our commit sent, the AP's to come */
	MAC_SAE_WAIT_CONFIRM,           /* the OS has to give confirm parameters */
	MAC_SAE_CONFIRMED,              /* our confirm sent, the AP's to come */
	MAC_SAE_WAIT_STATUS,            /* the OS has to say success or failure */
};
#define MAC_SAE_FRAME_MAX               1024    /* authentication frame body, from the algorithm number on */
#define MAC_SAE_IND_MAX                 2
struct mac_sae_ind {                    /* for the OS (rtw_linux_sae_get) */
	struct rtw_sae_ind ind;
	u8 frame[MAC_SAE_FRAME_MAX];
};
struct mac_sae {
	u8 state;                       /* MAC_SAE_* */
	bool h2e;                       /* the OS chose hash-to-element: RSNXE in the association request */
	u8 confirms;                    /* confirm frames sent for this candidate */
	u16 frame_len;
	u8 *frame;                      /* our last commit or confirm (kmalloc'ed), to send again */
	u16 peer_confirm_len;
	u8 *peer_confirm;               /* the AP's confirm, come before ours went out (kmalloc'ed) */
	u8 n_ind;                       /* indications queued */
	struct mac_sae_ind ind[MAC_SAE_IND_MAX];
};

/* An SA Query we started after an unprotected deauthentication/disassociation (pmf.c), wiphy->mtx held */
#define MAC_SAQ_IDS                     6
struct mac_saq {
	u64 deadline_ns;                /* the AP must have answered by then; 0 = none running */
	u64 next_ns;                    /* next request */
	u8 ids[MAC_SAQ_IDS][2];         /* transaction IDs of the requests sent */
	u8 n_ids;
	u16 stype;                      /* of the frame that started it */
	u16 reason;
};

struct mac_conn {
	u32 state;                      /* RTW_CONN_*; changed with wiphy->mtx held, read anywhere */
	struct mac_sta *ap;             /* the AP's station (RCU) */
	u8 bssid[ETH_ALEN];             /* of the attempt or connection; stable while state != IDLE */
	rtw_conn_event_fn event_fn;
	void *event_ctx;
	/* the request */
	struct mac_cand cand[RTW_CONN_MAX_CANDS];
	u8 n_cands, cur;
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u8 ssid_len;
	u8 auth;                        /* RTW_AUTH_* asked for; stable while state != IDLE */
	u8 conn_flags;                  /* RTW_CONN_F_* */
	u16 ie_len;
	u8 *ies;                        /* the OS's association request elements, kmalloc'ed */
	u8 auths;                       /* RTW_AUTH_BIT()s of the RSN authentications allowed */
	/* keys (key.c); changed with wiphy->mtx held, read by the data path in read-side sections */
	struct mac_key *ptk;
	struct mac_key *gtk[4];
	struct mac_key *igtk[2];        /* integrity group keys, IDs 4 and 5 (pmf.c reads them, wiphy->mtx held) */
	/* the attempt */
	struct ieee80211_channel *chan;
	struct mac_chanctx *ctx;        /* the channel context of the attempt/connection */
	struct mac_bss_params bss;
	struct wide_chan wide;          /* the channel and mode of the attempt/connection (mlme_prep) */
	u32 vht_ours;                   /* VHT Capabilities Information of our association request (VHT mode) */
	u8 sta_bw;                      /* the AP's station by its capabilities: IEEE80211_STA_RX_BW_*, streams */
	u8 sta_nss;                     /* (an Operating Mode Notification narrows from these) */
	u8 tries;                       /* frames sent in this step (auth or assoc) */
	bool acked;                     /* one of them was acknowledged */
	bool authed;                    /* the AP authenticated us: leaving takes a deauthentication */
	u8 wait_txt;                    /* MAC_TXT_* whose tx status the current step waits for */
	u16 resp_wait_ms;               /* the response to an acknowledged request may take this long */
	bool mfp;                       /* management frame protection with this candidate (pmf.c); stable while
					   state != IDLE */
	u8 comebacks;                   /* associations the AP refused temporarily (status 30) */
	struct mac_sae sae;             /* SAE (sae.c) */
	struct mac_saq saq;             /* our SA Query (pmf.c) */
	struct wiphy_delayed_work saq_timer;
	u64 saq_last_ns;                /* when the last SA Query started, 0 = never; kept across connections */
	u64 unprot_log_ns;              /* last log line about a dropped unprotected disconnection, 0 = none */
	struct wiphy_work work;         /* the state machine (rx frames, tx status, events) */
	struct wiphy_delayed_work timer;        /* step timeout, beacon-loss probing, link reports */
	u64 deadline_ns;                /* of the armed timer, 0 = not armed */
	/* from tx_status/rx (any context), under txs_lock / rxq.lock */
	spinlock_t txs_lock;
	u8 txs_txt;
	bool txs_new, txs_acked;
	struct sk_buff_head rxq;        /* MLME frames from rx.c */
	u16 bcn_opmode;                 /* 0x100 | the last Operating Mode Notification in a beacon of the BSS, 0 =
					   none yet (rx.c writes, READ_ONCE/WRITE_ONCE; mlme_prep clears) */
	unsigned long flags;            /* MAC_CONN_F_* (atomic bit ops); also set by key.c from any context */
	/* block ack sessions with the AP (agg.c) */
	struct wiphy_work ba_work;      /* sessions the data path or rtw89 asked for or ended */
	struct wiphy_delayed_work ba_timer;     /* ADDBA response timeouts */
	u8 ba_token;                    /* last dialog token we used */
	/* link monitoring (connected) */
	u8 probe_tries;                 /* Null frames sent after a beacon loss */
	u64 connected_ns;
	u64 last_rx_ns;                 /* last frame from the AP */
	s32 rssi_x16;                   /* EWMA of the AP's signal, dBm * 16; 0 = none yet */
	u64 link_report_ns;
	s8 link_rssi;                   /* last reported */
	u32 link_kbps;
	/* results for the front end (rtw_linux_conn_get) */
	struct rtw_conn_info attempts[RTW_CONN_MAX_CANDS];      /* failed candidates not fetched yet */
	u8 n_attempts;
	struct rtw_conn_info info;      /* the connection, and how it ended (status != 0) */
	u8 assoc_req[MAC_ASSOC_REQ_MAX];
	u8 assoc_resp[MAC_ASSOC_RESP_MAX];
	u8 auth_resp[MAC_AUTH_RESP_MAX];
	u8 bss_body[MAC_FRAME_MAX];
	u8 lost_frame[MAC_LOST_FRAME_MAX];
};
#define MAC_CONN_F_BEACON_LOSS          0       /* ieee80211_beacon_loss() */
#define MAC_CONN_F_CONN_LOSS            1       /* ieee80211_connection_loss() */
#define MAC_CONN_F_TIMEOUT              2       /* the step timer fired */
#define MAC_CONN_F_HAVE_BEACON          3       /* a beacon of the BSS came in since the attempt began (rx.c) */
#define MAC_CONN_F_OPMODE               4       /* a beacon brought another Operating Mode Notification */

/* The regulatory domain (reg.c); under wiphy->mtx */
struct mac_reg {
	char user[2];                   /* Parameters\Country; 0 = none */
	char ie[2];                     /* Country element of the connected BSS; 0 = none */
	char efuse[2];                  /* programmed into the efuse; 0 = none */
	char cur[2];                    /* applied; "00" = world */
	u8 src;                         /* RTW_REG_SRC_* applied */
	u8 dfs_region;
	bool retry;                     /* reg_work met a hardware restart: run it again when that is done */
	u32 changes;
	struct work_struct work;        /* applies a change, on mac_hw.wq without wiphy->mtx (the notifier takes it) */
};

/* Our group owner (ap.c), the second interface (type AP) while it runs. Changed with wiphy->mtx held unless
   noted; the data path reads running, vif, the keys and the clients in read-side sections. */
#include "apframe.h"
#define MAC_AP_GTKS                     4
struct mac_ap {
	bool running;                   /* started: the data path and rx.c use it (READ_ONCE) */
	struct mac_vif *vif;            /* the GO's interface while it exists */
	struct mac_chanctx *ctx;        /* its channel context (maybe the station's too) */
	struct ieee80211_channel *chan;
	struct apf_bss bss;             /* what the beacons say */
	u8 flags;                       /* RTW_AP_F_* */
	rtw_ap_event_fn event_fn;
	void *event_ctx;
	u8 *beacon_ies;                 /* the OS's additional elements (kmalloc'ed), kept across starts */
	u16 beacon_ies_len;
	u16 presp_ies_len;
	u8 *presp_ies;
	struct mac_key *gtk[MAC_AP_GTKS];       /* group keys (RCU) */
	u8 gtk_tx;                      /* the one group frames go out with */
	u32 aid_map;                    /* AIDs in use (bit n = AID n) */
	u16 mc_seq;                     /* sequence number of group frames (data path, one TX queue) */
	u16 cookie;                     /* last association response cookie */
	struct sk_buff_head rxq;        /* management and PS-Poll frames for the AP work, from rx.c */
	struct wiphy_work work;         /* MLME: frames, tx status, power save, indications */
	struct wiphy_delayed_work timer;        /* once a second while running: timeouts, inactivity, old buffered frames */
	spinlock_t ps_lock;             /* the clients' ps, ps_flush and ps_q; the class 3 list */
	atomic_t n_ps;                  /* clients in power save (group frames then wait for the DTIM) */
	atomic_t tx_inflight;           /* data frames given to the driver without a tx status yet */
	atomic_t mgmt_inflight;         /* management frames likewise */
	struct sk_buff_head rx_fifo;    /* data frames for the front end (rtw_linux_ap_rx_data) */
	struct list_head inds;          /* struct mac_ap_ind for the front end */
	u32 n_inds;
	u8 class3[4][ETH_ALEN];         /* senders of data without association, to deauthenticate (under ps_lock) */
	u8 n_class3;
	u8 class3_last[ETH_ALEN];       /* rate limit: the last one and when (AP work) */
	u64 class3_ns;
	u64 presp_window_ns;            /* probe responses: at most AP_PRESP_PER_S a second (ap.c) */
	u32 presp_count;
	bool restart_ind;               /* a hardware restart stopped the GO: STOPPED goes out once it is done */
	bool reg_check;                 /* the regulatory domain changed: is the channel still allowed? */
};

/*
 * wiphy_priv() of the wiphy that ieee80211_alloc_hw() creates; the driver's hw->priv follows it.
 * State and lists change only with wiphy->mtx held (driver ops are called with it, like mac80211). The lists
 * are also read without it, by the *_atomic iterators (mac_read_lock); an entry is freed only after it has been
 * unlinked and mac_sync_readers() has returned.
 */
struct mac_hw {
	struct ieee80211_hw hw;
	const struct ieee80211_ops *ops;
	struct workqueue_struct *wq;    /* ieee80211_queue_work(): ordered, like mac80211's local->workqueue */
	bool registered;
	struct list_head vif_list;      /* interfaces added to the driver */
	struct list_head chanctx_list;  /* channel contexts added to the driver */
	struct list_head sta_list;      /* stations added to the driver */

	bool started;                   /* drv_start() done */
	struct mac_vif *sta_vif;        /* the station interface while started */
	struct mac_chanctx *listen_ctx; /* fixed-channel listening (rtw_linux_monitor) */
	bool listening;                 /* RX filter passes beacons of all BSSs */
	unsigned int filter_flags;      /* FIF_* last accepted by the driver */

	/* hw scan (scan.c) */
	bool scanning;                  /* hw_scan() accepted, scan_done_work not yet run */
	bool scan_completed;            /* ieee80211_scan_completed() called */
	bool scan_aborted;
	bool last_scan_aborted;
	struct ieee80211_scan_request *scan_req;
	struct ieee80211_scan_request *scan_orphan;     /* a cancelled request the driver never completed */
	struct mac_vif *scan_vif;
	u64 scan_start_ns;
	u32 last_scan_ms;
	struct wiphy_work scan_done_work;
	struct completion scan_done;
	rtw_scan_done_fn scan_done_fn;  /* rtw_linux_scan_start(): called once from scan_finish() */
	void *scan_done_ctx;

	/* transmit (tx.c) */
	spinlock_t txp_lock;            /* txp[], the TXPROBE counters */
	struct mac_txp txp[MAC_TXP_MAX];
	atomic_t tx_inflight;           /* data frames given to the driver without a tx status yet */
	bool queues_stopped;            /* rtw89 stopped its queues (scan off channel, SER) */

	/* receive (rx.c) and the station MLME (mlme.c) */
	struct sk_buff_head rx_fifo;    /* data frames for the front end (rtw_linux_rx_data) */
	struct mac_conn conn;
	/* data frames after the duplicate check go through the reorder buffers and the key/PN check under this
	   lock (NAPI thread, reorder timer, block ack setup and teardown): one writer for the PN counters */
	spinlock_t rx_lock;

	/* hardware restart (rtw89 SER level 2: ieee80211_restart_hw; iface.c) */
	bool restarting;                /* from the SER (rtw89 dropping the keys, or restart_hw) until the restart is
					   done: the mac layer leaves the driver alone (entry points wait in
					   mac_lock_driver, the MLME and block ack work and the data path stand by) */
	u32 restarts;
	struct workqueue_struct *restart_wq;
	struct work_struct restart_work;
	struct list_head parked_sta;    /* stations, channel contexts and interfaces the driver forgot in a restart, */
	struct list_head parked_ctx;    /* kept until the hw is freed: rtw89 may still hold stale pointers to them */
	struct list_head parked_vif;

	/* regulatory domain (reg.c) and power save (ps.c) */
	struct mac_reg reg;
	struct wiphy_work ps_work;      /* the power save policy changed, or the first beacon came */
	u64 ps_on_ns;                   /* when vif->cfg.ps went on, 0 = off */

	/* the group owner (ap.c) */
	struct mac_ap ap;
};

#define MAC_HW_SIZE                     ALIGN(sizeof(struct mac_hw), CACHE_LINE_SIZE)

static inline struct mac_wiphy *mac_wiphy (struct wiphy *wiphy)
{
	return container_of(wiphy, struct mac_wiphy, wiphy);
}

static inline struct mac_hw *mac_hw (struct ieee80211_hw *hw)
{
	return container_of(hw, struct mac_hw, hw);
}

static inline struct mac_vif *mac_vif (struct ieee80211_vif *vif)
{
	return container_of(vif, struct mac_vif, vif);
}

/* Lock-free list readers: RCU read-side sections (one counter with rtw89's readers, compat/rcu.c). Writers unlink
   with list_del_rcu() and wait in mac_sync_readers() (= synchronize_rcu(), PASSIVE_LEVEL) before freeing. */
static inline void mac_read_lock (struct mac_hw *mh)
{
	(void)mh;
	rcu_read_lock();
}

static inline void mac_read_unlock (struct mac_hw *mh)
{
	(void)mh;
	rcu_read_unlock();
}

extern struct workqueue_struct *mac_wiphy_wq;
extern struct mac_hw *mac_the_hw;       /* the registered hw (one device), NULL if none */
extern u32 mac_state;                   /* RTW_STATE_* for rtw_linux_bss_get() */
extern u32 mac_listen_channel;

/* iface.c */
struct ieee80211_channel *mac_find_channel (struct wiphy *wiphy, u32 band, u32 number);
int mac_chanctx_use (struct mac_hw *mh, struct mac_vif *mv, struct ieee80211_channel *chan,
	enum nl80211_channel_type type, struct mac_chanctx **out);
int mac_chanctx_use_def (struct mac_hw *mh, struct mac_vif *mv, const struct cfg80211_chan_def *def,
	struct mac_chanctx **out);      /* any width */
bool mac_chanctx_release (struct mac_hw *mh, struct mac_vif *mv, struct mac_chanctx *ctx);
struct mac_vif *mac_vif_alloc (struct mac_hw *mh, enum nl80211_iftype type, const u8 *addr);
void mac_state_set (u32 bits, bool on);
void mac_sync_readers (struct mac_hw *mh);
void mac_recalc_idle (struct mac_hw *mh);
void mac_configure_filter (struct mac_hw *mh);
void mac_hw_stop (struct mac_hw *mh);
void mac_restart_init (struct mac_hw *mh);
bool mac_restart_wait (struct mac_hw *mh);
bool mac_lock_driver (struct mac_hw *mh);
void mac_parked_free (struct mac_hw *mh);

/* scan.c */
void mac_scan_init (struct mac_hw *mh);
void mac_scan_cancel (struct mac_hw *mh);
void mac_scan_abandon (struct mac_hw *mh);      /* finish a scan without touching the (reset) driver */

/* scan.c: probe request elements */
u8 *mac_put_rates (u8 *pos, const struct ieee80211_supported_band *sb);
u8 *mac_put_ht_cap (u8 *pos, const struct ieee80211_sta_ht_cap *ht);

/* tx.c */
extern struct rtw_link_stats mac_st;    /* counters (the state fields are filled in on read) */
#define MAC_COUNT(field)                __atomic_fetch_add(&mac_st.field, 1, __ATOMIC_RELAXED)
void mac_tx_init (struct mac_hw *mh);
void mac_tx_reset (struct mac_hw *mh);
int mac_tx_mgmt (struct mac_hw *mh, struct sk_buff *skb, struct mac_sta *ms, u32 flags, enum mac_txt txt);
int mac_txt_pending (struct mac_hw *mh, enum mac_txt txt);
void mac_txprobe_rx (struct mac_hw *mh, const struct ieee80211_mgmt *mgmt);
void mac_dp_tx_ready (void);
void mac_dp_rx_ready (void);
void mac_tx_set_protocol (struct sk_buff *skb, unsigned int body);

/* rx.c */
void mac_rx_init (struct mac_hw *mh);
void mac_rx_flush (struct mac_hw *mh);
void mac_rx_deliver (struct mac_hw *mh, struct sk_buff *skb);

/* mlme.c */
void mac_mlme_init (struct mac_hw *mh);
void mac_mlme_stop (struct mac_hw *mh);
void mac_mlme_free (struct mac_hw *mh);
void mac_mlme_tx_status (struct mac_hw *mh, enum mac_txt txt, bool acked);
void mac_mlme_rx (struct mac_hw *mh, struct sk_buff *skb);
void mac_mlme_rx_from_ap (struct mac_hw *mh, const struct ieee80211_rx_status *rs);
bool mac_mlme_wants (struct mac_hw *mh, const u8 *hdr, unsigned int len);
void mac_mlme_conn_loss (struct mac_hw *mh);
bool mac_mlme_chan_ok (struct mac_hw *mh);
void mac_mlme_beacon (struct mac_hw *mh, const u8 *f, unsigned int len);
void mac_mlme_restart (struct mac_hw *mh);
int mac_sta_move (struct mac_hw *mh, struct mac_sta *ms, enum ieee80211_sta_state to);
void mac_sta_free (struct mac_sta *ms);
struct mac_sta *mac_sta_alloc (struct mac_hw *mh, struct mac_vif *mv, const u8 *addr);
struct sk_buff *mac_mlme_frame (struct mac_hw *mh, u16 stype, unsigned int body_len, u8 **body);
/* for sae.c and pmf.c (wiphy->mtx held) */
void mac_mlme_arm (struct mac_hw *mh, u32 ms);
void mac_mlme_disarm (struct mac_hw *mh);
void mac_mlme_events (struct mac_hw *mh, u32 events);
void mac_mlme_attempt_failed (struct mac_hw *mh, int status, u16 code);
void mac_mlme_authenticated (struct mac_hw *mh);
void mac_mlme_peer_left (struct mac_hw *mh, u16 stype, u16 reason);
static inline bool mac_conn_rsn (const struct mac_conn *c)
{
	return c->auth == RTW_AUTH_RSN_PSK || c->auth == RTW_AUTH_SAE;
}

/* pmf.c: management frame protection */
bool mac_pmf_usable (void);
struct sk_buff *mac_pmf_tx (struct mac_hw *mh, struct sk_buff *skb);
bool mac_pmf_rx (struct mac_hw *mh, struct sk_buff *skb);
void mac_pmf_rx_sa_query (struct mac_hw *mh, const u8 *body, unsigned int len);
void mac_pmf_init (struct mac_hw *mh);
void mac_pmf_conn_end (struct mac_hw *mh);
void mac_pmf_stats (struct mac_hw *mh, struct rtw_link_stats *st);

/* sae.c: SAE authentication */
void mac_sae_start (struct mac_hw *mh);
void mac_sae_rx (struct mac_hw *mh, const u8 *frame, unsigned int len);
void mac_sae_retry (struct mac_hw *mh);
void mac_sae_reset (struct mac_hw *mh);
void mac_sae_clear_ind (struct mac_hw *mh);

/* agg.c: block ack */
void mac_agg_init (struct mac_hw *mh);
void mac_agg_sta_init (struct mac_sta *ms);
void mac_agg_sta_free (struct mac_hw *mh, struct mac_sta *ms);
void mac_agg_rx_action (struct mac_hw *mh, const u8 *body, unsigned int len);
bool mac_agg_rx_frame (struct mac_hw *mh, struct mac_sta *ms, unsigned int tid, struct sk_buff *skb);
void mac_agg_rx_bar (struct mac_hw *mh, const u8 *frame, unsigned int len);
void mac_agg_rx_drop_held (struct mac_hw *mh, struct mac_sta *ms);
bool mac_agg_tx_check (struct mac_hw *mh, struct mac_sta *ms, unsigned int tid, u16 ethertype);
void mac_agg_stop_sta (struct mac_hw *mh, struct mac_sta *ms, bool driver);
void mac_agg_cancel (struct mac_hw *mh);
void mac_agg_stats (struct mac_hw *mh, struct rtw_link_stats *st);
/* block ack with a client of our GO (receive sessions only) */
void mac_agg_rx_action_sta (struct mac_hw *mh, struct mac_sta *ms, const u8 *body, unsigned int len);
void mac_agg_rx_bar_sta (struct mac_hw *mh, struct mac_sta *ms, const u8 *frame, unsigned int len);
void mac_agg_stop_client (struct mac_hw *mh, struct mac_sta *ms, bool call_driver);

/* key.c: keys and privacy exemptions */
int mac_key_tx (struct mac_hw *mh, u16 ethertype, bool group_da, u32 flags, struct mac_key **key);
void mac_key_ccmp_hdr (struct mac_key *k, u8 *p);
int mac_key_rx (struct mac_hw *mh, const u8 *ccmp, bool group, unsigned int slot);
int mac_key_rx_clear (struct mac_hw *mh, u16 ethertype, bool group);
u8 mac_link_keys (struct mac_hw *mh);
u8 mac_exempt_count (void);
void mac_keys_clear (struct mac_hw *mh, struct mac_sta *ms);
void mac_keys_drop (struct mac_hw *mh);
void mac_keys_free (struct mac_hw *mh);
/* for ap.c: keys outside the station's connection, and the exemption list of each port */
#define MAC_PORT_STA                    0
#define MAC_PORT_AP                     1
struct mac_key *mac_key_alloc (const struct rtw_key *in);
void mac_key_free (struct mac_key *k);
bool mac_key_identical (const struct mac_key *old, const struct mac_key *k);
int mac_exempt_lookup (int port, u16 ethertype, bool group);
void mac_exempt_set (int port, const struct rtw_exemption *list, u32 n);

/* reg.c: regulatory domain */
void mac_reg_register (struct mac_hw *mh);
void mac_reg_unregister (struct mac_hw *mh);
void mac_reg_hint_country_ie (struct mac_hw *mh, const u8 *country, u8 len);
void mac_reg_disconnected (struct mac_hw *mh);
void mac_reg_restart_done (struct mac_hw *mh);
void mac_reg_efuse (struct mac_hw *mh, const char *alpha2);
bool mac_reg_chan_possible (const struct ieee80211_channel *c);
void mac_reg_info (struct mac_hw *mh, struct rtw_radio_info *info);

/* ps.c: power save */
void mac_ps_init (struct mac_hw *mh);
void mac_ps_recalc (struct mac_hw *mh);
void mac_ps_leave (struct mac_hw *mh, const char *why);
void mac_ps_restart (struct mac_hw *mh);
void mac_ps_beacon (struct mac_hw *mh);

/* compat\rtw89info.c: rtw89's internal state for the diagnostics; drv is hw->priv (struct rtw89_dev) */
void rtw89info_radio (void *drv, struct rtw_radio_info *info);
long rtw89info_btc_dump (void *drv, char *buf, size_t len);

/* bss.c */
void mac_bss_rx (struct ieee80211_hw *hw, struct sk_buff *skb);
bool mac_bss_heard (const u8 *bssid, u64 max_age_ns);
void mac_bss_clear (void);
void mac_bss_new_session (void);
u32 mac_bss_count (void);
void mac_bss_scan_done (u32 ms, bool aborted);
void mac_bss_listen_done (u32 channel);

/* ap.c: the group owner */
void mac_ap_init (struct mac_hw *mh);
void mac_ap_stop_locked (struct mac_hw *mh, bool tx);
void mac_ap_restart (struct mac_hw *mh);
void mac_ap_restart_done (struct mac_hw *mh);
void mac_ap_reg_changed (struct mac_hw *mh);
void mac_ap_free (struct mac_hw *mh);
bool mac_ap_rx (struct mac_hw *mh, struct sk_buff *skb);
void mac_ap_rx_deliver (struct mac_hw *mh, struct sk_buff *skb);
void mac_ap_tx_done (struct mac_hw *mh, struct sk_buff *skb, bool acked);
void mac_ap_dp_rx_ready (void);
void mac_ap_dp_tx_ready (void);
void mac_ap_send_action (struct mac_hw *mh, struct mac_sta *ms, const u8 *body, unsigned int len);
bool mac_ap_iterate_keys (struct mac_hw *mh, struct ieee80211_vif *vif,
	void (*fn)(struct ieee80211_hw *, struct ieee80211_vif *, struct ieee80211_sta *, struct ieee80211_key_conf *,
		   void *), void *arg);
static inline bool mac_ap_vif (struct mac_hw *mh, const struct ieee80211_vif *vif)
{
	return mh->ap.vif && vif == &mh->ap.vif->vif;
}

/* A backend that is not implemented yet: log the first call. */
#define MAC_TODO()                      pr_warn_once("mac: %s is not implemented", __func__)

#endif
