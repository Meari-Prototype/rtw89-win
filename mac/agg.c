// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: block ack agreements with the AP, what mac80211's agg-rx.c, agg-tx.c and the reordering in rx.c do
 * under Linux (read for reference only). HT immediate block ack on TIDs 0-7.
 *  - Receive (the AP aggregates): an ADDBA request is accepted when the AP's station is HT, with the window
 *    min(AP's, hw->max_rx_aggregation_subframes, 64): ampdu_action(RX_START) (rtw89 sets up its BA CAM, the
 *    hardware answers with block acks), then the ADDBA response. The TID's unicast QoS data frames then go through
 *    a reorder buffer: a frame in order goes up at once, one after a gap is held until the gap fills, the window is
 *    pushed forward (a frame beyond it, a block ack request) or the oldest held frame has waited
 *    REORDER_TIMEOUT_MS (one timer per station). Only frames that leave the buffer, in sequence order, get the key
 *    and PN check and become MSDUs (rx.c mac_rx_deliver). DELBA from the AP or a new request for the TID end a
 *    session with the held frames going up; leaving, a hardware restart or a new pairwise key drop them.
 *  - Transmit (we aggregate): the data path (tx.c) asks for a session on a TID it sends QoS data on once the AP's
 *    station is authorized; rtw89's txq path also asks (ieee80211_start_tx_ba_session, routed here). ba_work takes
 *    the starting sequence number from the data path's next one, calls ampdu_action(TX_START) (rtw89 answers
 *    IMMEDIATE), sends the ADDBA request and waits ADDBA_RESP_TIMEOUT_MS; the TID's frames keep going one by one
 *    meanwhile (their sequence numbers are at or past the starting one, so the AP's window accepts them). Accepted:
 *    ampdu_action(TX_OPERATIONAL) with the window min(AP's, ours); the TID's frames then carry
 *    IEEE80211_TX_CTL_AMPDU and rtw89 has the hardware aggregate them. Declined, no answer, DELBA from the AP or
 *    rtw89's stop_tx_ba_session: ampdu_action(TX_STOP_CONT) (and a DELBA to the AP when we end it ourselves), and a
 *    new request only after a backoff (mac80211: a few in a row, then one every 15 s, 15 at most; 15 s after a
 *    session that ran). EAPOL stops a running session (like rtw89's txq path), so a group key update is never
 *    aggregated. Only on an HT connection, both ways.
 *  - A hardware restart (rtw89 reset, mac_hw.restarting) leaves the sessions alone until mac_restart_work drops the
 *    connection; ba_work does nothing meanwhile.
 * A client of our GO (ap.c) may get receive sessions the same way (its frames then leave the reorder buffer up the
 *    GO's data path, and the action frames go out from the GO's interface); no transmit sessions with clients.
 * Locks: sessions start and end with wiphy->mtx held (the MLME work for action frames, ba_work, ba_timer, leaving);
 * the reorder buffers, their timer and everything after them under mac_hw.rx_lock (rx.c). The data path reads
 * ba_tx_state in its read-side section.
 */
#include "mac.h"

#define REORDER_TIMEOUT_MS              100     /* mac80211 HT_RX_REORDER_BUF_TIMEOUT */
#define ADDBA_RESP_TIMEOUT_MS           1000    /* ADDBA_RESP_INTERVAL */
#define BA_BURST_REQS                   3       /* HT_AGG_BURST_RETRIES: tries back to back */
#define BA_MAX_REQS                     15      /* HT_AGG_MAX_RETRIES */
#define BA_RETRY_MS                     15000   /* HT_AGG_RETRIES_PERIOD */
#define BA_AGAIN_MS                     1000    /* between the first bursts, and after a session ended */

#define ACTION_ADDBA_REQ                0
#define ACTION_ADDBA_RESP               1
#define ACTION_DELBA                    2
#define BA_PARAM_AMSDU                  0x0001  /* Block Ack Parameter Set (802.11-2020 9.4.1.13) */
#define BA_PARAM_POLICY_IMMEDIATE       0x0002
#define BA_PARAM_TID_SHIFT              2
#define BA_PARAM_BUF_SHIFT              6
#define BA_PARAM_BUF_MASK               0x3ff
#define DELBA_INITIATOR                 0x0800  /* DELBA Parameter Set (9.4.1.16) */
#define DELBA_TID_SHIFT                 12
#define STATUS_SUCCESS                  0
#define STATUS_REQUEST_DECLINED         37
#define STATUS_INVALID_PARAMETERS       38      /* WLAN_STATUS_INVALID_QOS_PARAM */
#define REASON_QSTA_NOT_USE             37      /* the originator ends the agreement */
#define BAR_CTRL_MULTI_TID              0x0002  /* BlockAckReq frame (9.3.1.7) */
#define BAR_CTRL_TID_SHIFT              12
#define BAR_LEN                         20      /* up to the Starting Sequence Control, without the FCS */
#define ADDBA_REQ_LEN                   9       /* category..start sequence control */
#define ADDBA_RESP_LEN                  9       /* category..timeout */
#define DELBA_LEN                       6

#define SN_MASK                         0xfff
#define SN_HALF                         ((SN_MASK + 1) / 2)
#define SLOT(sn)                        ((sn) & (MAC_BA_BUF - 1))

_Static_assert(MAC_BA_BUF == 64 && (SN_MASK + 1) % MAC_BA_BUF == 0, "reorder slots");
_Static_assert(MAC_BA_TIDS <= BITS_PER_LONG, "TID bits");

/* sequence numbers are modulo 4096; a is before b when b is less than half the space ahead (802.11-2020 10.3.2.14) */
static bool sn_less (u16 a, u16 b)
{
	return ((b - a) & SN_MASK) != 0 && ((b - a) & SN_MASK) < SN_HALF;
}

static u16 sn_add (u16 a, u16 b)
{
	return (a + b) & SN_MASK;
}

static u16 sn_sub (u16 a, u16 b)
{
	return (a - b) & SN_MASK;
}

/* ---- action frames (wiphy->mtx held) ---- */
/* to ms: the AP's station, or a client of our GO (ap.c sends those from the GO's interface) */
static void ba_send (struct mac_hw *mh, struct mac_sta *ms, const u8 *body, unsigned int len)
{
	struct sk_buff *skb;
	u8 *p;

	if (ms->ap_client) {
		mac_ap_send_action(mh, ms, body, len);
		return;
	}
	skb = mac_mlme_frame(mh, IEEE80211_STYPE_ACTION, len, &p);
	if (!skb)
		return;
	memcpy(p, body, len);
	mac_tx_mgmt(mh, skb, READ_ONCE(mh->conn.ap), 0, MAC_TXT_ACTION);
}

/* frames left a reorder buffer: tell the front end's port */
static void agg_rx_ready (const struct mac_sta *ms)
{
	if (ms->ap_client)
		mac_ap_dp_rx_ready();
	else
		mac_dp_rx_ready();
}

static u16 ba_params (bool amsdu, unsigned int tid, unsigned int buf)
{
	return (u16)((amsdu ? BA_PARAM_AMSDU : 0) | BA_PARAM_POLICY_IMMEDIATE | tid << BA_PARAM_TID_SHIFT |
		     buf << BA_PARAM_BUF_SHIFT);
}

/* ADDBA request (9.6.4.2): category, action, dialog token, parameters, timeout, starting sequence control */
static void ba_send_addba_req (struct mac_hw *mh, struct mac_sta *ms, u8 token, u16 params, u16 timeout, u16 ssn)
{
	u8 b[11] = { WLAN_CATEGORY_BACK, ACTION_ADDBA_REQ, token };

	put_unaligned_le16(params, b + 3);
	put_unaligned_le16(timeout, b + 5);
	put_unaligned_le16((u16)(ssn << 4), b + 7);
	ba_send(mh, ms, b, sizeof(b));
}

/* ADDBA response (9.6.4.3): category, action, dialog token, status, parameters, timeout */
static void ba_send_resp (struct mac_hw *mh, struct mac_sta *ms, u8 token, u16 status, u16 params, u16 timeout)
{
	u8 b[ADDBA_RESP_LEN] = { WLAN_CATEGORY_BACK, ACTION_ADDBA_RESP, token };

	put_unaligned_le16(status, b + 3);
	put_unaligned_le16(params, b + 5);
	put_unaligned_le16(timeout, b + 7);
	ba_send(mh, ms, b, sizeof(b));
}

/* DELBA (9.6.4.4): category, action, parameters (initiator, TID), reason */
static void ba_send_delba (struct mac_hw *mh, struct mac_sta *ms, unsigned int tid, bool initiator, u16 reason)
{
	u8 b[DELBA_LEN] = { WLAN_CATEGORY_BACK, ACTION_DELBA };

	put_unaligned_le16((u16)((initiator ? DELBA_INITIATOR : 0) | tid << DELBA_TID_SHIFT), b + 2);
	put_unaligned_le16(reason, b + 4);
	ba_send(mh, ms, b, sizeof(b));
}

/* ---- receive: the reorder buffer (mac_hw.rx_lock held; the caller also holds rcu_read_lock when frames go up) ---- */
/* The window moves on by one; the frame at its start, if held, goes up (deliver) or is dropped. Returns delivered. */
static bool rb_pop (struct mac_hw *mh, struct mac_ba_rx *r, bool deliver)
{
	unsigned int i = SLOT(r->head);
	struct sk_buff *skb = r->frames[i];

	r->head = sn_add(r->head, 1);
	if (!skb)
		return false;
	r->frames[i] = NULL;
	r->held_ns[i] = 0;
	r->stored--;
	if (deliver) {
		if (r->ap)
			mac_ap_rx_deliver(mh, skb);     /* a client of our GO */
		else
			mac_rx_deliver(mh, skb);
		return true;
	}
	kfree_skb(skb);
	return false;
}

/* Releases held frames from the window start while they are present (in sequence order). Returns delivered count. */
static int rb_release_ready (struct mac_hw *mh, struct mac_ba_rx *r)
{
	int n = 0;

	while (r->stored && r->frames[SLOT(r->head)])
		n += rb_pop(mh, r, true);
	return n;
}

/* The window starts at sn (ahead of it) from now on: what is held before sn goes up in order, gaps are skipped,
   then the contiguous frames at the new start go up. Returns delivered count. */
static int rb_move (struct mac_hw *mh, struct mac_ba_rx *r, u16 sn)
{
	int n = 0;

	while (r->stored && sn_less(r->head, sn))
		n += rb_pop(mh, r, true);
	if (sn_less(r->head, sn))
		r->head = sn;
	return n + rb_release_ready(mh, r);
}

/* The oldest held frame's timestamp, 0 if none held. */
static u64 rb_oldest (const struct mac_ba_rx *r)
{
	u64 t = 0;

	for (int i = 0; i < MAC_BA_BUF; i++)
		if (r->frames[i] && (!t || r->held_ns[i] < t))
			t = r->held_ns[i];
	return t;
}

/* Releases frames the reorder timeout has made due: present frames at the window start go up; a gap whose oldest
   waiting frame is overdue is skipped. Returns delivered count. */
static int rb_release_timeout (struct mac_hw *mh, struct mac_ba_rx *r, u64 now)
{
	int n = 0;

	while (r->stored) {
		u64 oldest;

		if (r->frames[SLOT(r->head)]) {
			n += rb_pop(mh, r, true);
			continue;
		}
		oldest = rb_oldest(r);
		if (oldest && now - oldest >= (u64)REORDER_TIMEOUT_MS * NSEC_PER_MSEC) {
			r->head = sn_add(r->head, 1);   /* give up on the missing sequence number */
			MAC_COUNT(rx_reorder_timeouts);
		} else {
			break;
		}
	}
	return n;
}

/* Re-arms (or leaves un-armed) the station's reorder timer for the earliest held frame; an armed timer that already
   fits is left alone (this runs for every frame of a session). mac_hw.rx_lock held. */
static void rb_arm_timer (struct mac_hw *mh, struct mac_sta *ms, u64 now)
{
	u64 earliest = 0, when;

	for (int tid = 0; tid < MAC_BA_TIDS; tid++) {
		struct mac_ba_rx *r = ms->ba_rx[tid];
		u64 o;

		if (!r || !r->stored)
			continue;
		o = rb_oldest(r);
		if (o && (!earliest || o < earliest))
			earliest = o;
	}
	if (!earliest || ms->ba_rx_dead) {
		ms->ba_rx_timer_ns = 0;
		return;
	}
	when = earliest + (u64)REORDER_TIMEOUT_MS * NSEC_PER_MSEC;
	if (when == ms->ba_rx_timer_ns)
		return;
	ms->ba_rx_timer_ns = when;
	mod_timer(&ms->ba_rx_timer, jiffies + msecs_to_jiffies(when > now ?
		  (unsigned long)DIV_ROUND_UP(when - now, NSEC_PER_MSEC) : 1));
}

static void mac_agg_rx_timer (struct timer_list *t)
{
	struct mac_sta *ms = timer_container_of(ms, t, ba_rx_timer);
	struct mac_hw *mh = ms->mv->mh;
	u64 now;
	int n = 0;

	rcu_read_lock();
	spin_lock_bh(&mh->rx_lock);
	if (!ms->ba_rx_dead) {
		now = ktime_get_ns();
		ms->ba_rx_timer_ns = 0;         /* it fired: arm it again whatever is left */
		for (int tid = 0; tid < MAC_BA_TIDS; tid++)
			if (ms->ba_rx[tid])
				n += rb_release_timeout(mh, ms->ba_rx[tid], now);
		rb_arm_timer(mh, ms, now);
	}
	spin_unlock_bh(&mh->rx_lock);
	rcu_read_unlock();
	if (n)
		agg_rx_ready(ms);
}

/* A received unicast QoS data frame on a TID (rx.c, NAPI thread, rcu_read_lock held). Returns true if it took the
   skb (a block ack session exists: held or delivered), false if there is no session (the caller delivers it). */
bool mac_agg_rx_frame (struct mac_hw *mh, struct mac_sta *ms, unsigned int tid, struct sk_buff *skb)
{
	const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)skb->data;
	u16 sn = ieee80211_get_sn((struct ieee80211_hdr *)hdr);
	struct mac_ba_rx *r;
	unsigned int dist, i;
	u64 now;
	int n = 0;

	if (tid >= MAC_BA_TIDS)
		return false;
	spin_lock_bh(&mh->rx_lock);
	r = ms->ba_rx[tid];
	if (!r) {
		spin_unlock_bh(&mh->rx_lock);
		return false;
	}
	now = ktime_get_ns();
	dist = sn_sub(sn, r->head);
	if (dist >= SN_HALF) {
		MAC_COUNT(rx_dropped_old);      /* before the window: a retransmission */
		kfree_skb(skb);
	} else {
		if (dist >= r->buf_size)
			n += rb_move(mh, r, sn_sub(sn_add(sn, 1), r->buf_size));
		i = SLOT(sn);
		if (r->frames[i]) {
			MAC_COUNT(rx_dropped_old);      /* already held */
			kfree_skb(skb);
		} else {
			r->frames[i] = skb;
			r->held_ns[i] = now;
			r->stored++;
			if (sn != r->head)
				MAC_COUNT(rx_reordered);
			n += rb_release_ready(mh, r);
		}
		rb_arm_timer(mh, ms, now);
	}
	spin_unlock_bh(&mh->rx_lock);
	if (n)
		agg_rx_ready(ms);
	return true;
}

/* A BlockAckReq from the AP (rx.c, NAPI thread): push the TID's window forward to its starting sequence number. */
void mac_agg_rx_bar (struct mac_hw *mh, const u8 *frame, unsigned int len)
{
	const u8 *own = mh->hw.wiphy->perm_addr;
	struct mac_sta *ms;
	struct mac_ba_rx *r;
	u16 ctrl, ssn;
	unsigned int tid;
	int n = 0;

	if (len < BAR_LEN)
		return;
	/* A1 = RA (us), A2 = TA (our AP) */
	if (!ether_addr_equal(frame + 4, own) || !ether_addr_equal(frame + 10, mh->conn.bssid))
		return;
	ctrl = get_unaligned_le16(frame + 16);
	if (ctrl & BAR_CTRL_MULTI_TID)
		return;
	tid = (ctrl >> BAR_CTRL_TID_SHIFT) & 0xf;
	if (tid >= MAC_BA_TIDS)
		return;
	ssn = get_unaligned_le16(frame + 18) >> 4;
	MAC_COUNT(rx_bar);
	rcu_read_lock();
	spin_lock_bh(&mh->rx_lock);
	/* the connection may be leaving: like rx_data(), only while connected (the station stays until the read side
	   ends) */
	ms = READ_ONCE(mh->conn.state) == RTW_CONN_CONNECTED ? rcu_dereference(mh->conn.ap) : NULL;
	r = ms ? ms->ba_rx[tid] : NULL;
	if (r && !sn_less(ssn, r->head)) {      /* only forward */
		n = rb_move(mh, r, ssn);
		rb_arm_timer(mh, ms, ktime_get_ns());
	}
	spin_unlock_bh(&mh->rx_lock);
	rcu_read_unlock();
	if (n)
		mac_dp_rx_ready();
}

/* A BlockAckReq from a client of our GO (ap.c, NAPI thread, rcu_read_lock held; ms found by its address, the
   frame's RA being the GO's). */
void mac_agg_rx_bar_sta (struct mac_hw *mh, struct mac_sta *ms, const u8 *frame, unsigned int len)
{
	struct mac_ba_rx *r;
	u16 ctrl, ssn;
	unsigned int tid;
	int n = 0;

	if (len < BAR_LEN)
		return;
	ctrl = get_unaligned_le16(frame + 16);
	if (ctrl & BAR_CTRL_MULTI_TID)
		return;
	tid = (ctrl >> BAR_CTRL_TID_SHIFT) & 0xf;
	if (tid >= MAC_BA_TIDS)
		return;
	ssn = get_unaligned_le16(frame + 18) >> 4;
	spin_lock_bh(&mh->rx_lock);
	r = ms->ba_rx[tid];
	if (r && !sn_less(ssn, r->head)) {
		n = rb_move(mh, r, ssn);
		rb_arm_timer(mh, ms, ktime_get_ns());
	}
	spin_unlock_bh(&mh->rx_lock);
	if (n)
		agg_rx_ready(ms);
}

/* Ends the receive session on a TID: the held frames go up (deliver) or are dropped, the buffer is freed, and
   rtw89 is told to tear down its BA CAM (RX_STOP) when call_driver. mac_hw.rx_lock is taken here (and a read-side
   section for the keys, as mac_rx_deliver() expects); wiphy->mtx held. */
static void ba_rx_stop (struct mac_hw *mh, struct mac_sta *ms, unsigned int tid, bool deliver, bool call_driver)
{
	struct mac_ba_rx *r;
	int n = 0;

	rcu_read_lock();
	spin_lock_bh(&mh->rx_lock);
	r = ms->ba_rx[tid];
	ms->ba_rx[tid] = NULL;
	if (r) {
		while (r->stored)
			n += rb_pop(mh, r, deliver);
	}
	spin_unlock_bh(&mh->rx_lock);
	rcu_read_unlock();
	if (r) {
		kfree(r);
		if (call_driver && mh->ops->ampdu_action) {
			struct ieee80211_ampdu_params p = { 0 };

			p.sta = &ms->sta;
			p.action = IEEE80211_AMPDU_RX_STOP;
			p.tid = (u8)tid;
			mh->ops->ampdu_action(&mh->hw, &ms->mv->vif, &p);
		}
	}
	if (n)
		agg_rx_ready(ms);
}

/* A new pairwise key replaces the old one (key.c): the frames held for reordering were decrypted with the old key,
   and checked against the new key's PN counters they would push them past the new key's own frames (dropped as
   replays until the new PNs caught up). They are dropped and each window moves past them; the sessions stay
   (mac80211 ends them on such a rekey). mac_hw.rx_lock held: the caller publishes the new key under it, so no held
   frame goes up in between. */
void mac_agg_rx_drop_held (struct mac_hw *mh, struct mac_sta *ms)
{
	for (int tid = 0; tid < MAC_BA_TIDS; tid++) {
		struct mac_ba_rx *r = ms->ba_rx[tid];

		while (r && r->stored)
			rb_pop(mh, r, false);
	}
	ms->ba_rx_timer_ns = 0;                 /* a pending timer finds nothing held */
}

/* ADDBA request from the AP: set up the receive reorder buffer and answer. wiphy->mtx held. */
static void ba_rx_addba (struct mac_hw *mh, struct mac_sta *ms, const u8 *body, unsigned int len)
{
	u16 params, timeout, buf, status = STATUS_SUCCESS;
	struct mac_ba_rx *r;
	unsigned int tid;
	u8 token;
	u16 ssn;

	if (len < 9)
		return;
	token = body[2];
	params = get_unaligned_le16(body + 3);
	timeout = get_unaligned_le16(body + 5);
	ssn = get_unaligned_le16(body + 7) >> 4;
	tid = (params >> BA_PARAM_TID_SHIFT) & 0xf;
	buf = (params >> BA_PARAM_BUF_SHIFT) & BA_PARAM_BUF_MASK;

	if (tid >= MAC_BA_TIDS || !(params & BA_PARAM_POLICY_IMMEDIATE)) {
		/* answer on the requested TID when it has a parameter field; TID >= 8 has no reorder buffer */
		ba_send_resp(mh, ms, token, STATUS_INVALID_PARAMETERS, ba_params(false, tid & 0xf, 0), timeout);
		return;
	}
	if (!ms->sta.deflink.ht_cap.ht_supported) {
		/* not an HT connection (mac80211 __ieee80211_start_rx_ba_session declines too) */
		ba_send_resp(mh, ms, token, STATUS_REQUEST_DECLINED, ba_params(false, tid, 0), timeout);
		return;
	}
	if (!buf)
		buf = MAC_BA_BUF;                               /* 0 means "the receiver decides" (mac80211) */
	buf = (u16)min(buf, mh->hw.max_rx_aggregation_subframes);
	buf = (u16)min_t(u16, buf, MAC_BA_BUF);

	r = ms->ba_rx[tid];
	if (r && r->token == token) {                           /* a repeat of the same request: resend the response */
		ba_send_resp(mh, ms, token, STATUS_SUCCESS, ba_params(false, tid, r->buf_size), r->timeout);
		return;
	}
	if (r)
		ba_rx_stop(mh, ms, tid, true, true);            /* a new agreement replaces the old one */

	if (mh->ops->ampdu_action) {
		struct ieee80211_ampdu_params p = { 0 };
		int ret;

		p.sta = &ms->sta;
		p.action = IEEE80211_AMPDU_RX_START;
		p.buf_size = buf;
		p.timeout = timeout;
		p.ssn = ssn;
		p.tid = (u8)tid;
		ret = mh->ops->ampdu_action(&mh->hw, &ms->mv->vif, &p);
		if (ret) {
			pr_warn("mac: drv ampdu_action(RX_START) tid %u failed: %d", tid, ret);
			status = STATUS_REQUEST_DECLINED;
		}
	}
	if (status == STATUS_SUCCESS) {
		r = kzalloc(sizeof(*r), GFP_KERNEL);
		if (!r) {
			status = STATUS_REQUEST_DECLINED;
			if (mh->ops->ampdu_action) {
				struct ieee80211_ampdu_params p = { 0 };

				p.sta = &ms->sta;
				p.action = IEEE80211_AMPDU_RX_STOP;
				p.tid = (u8)tid;
				mh->ops->ampdu_action(&mh->hw, &ms->mv->vif, &p);
			}
		}
	}
	if (status == STATUS_SUCCESS) {
		r->head = ssn;
		r->buf_size = buf;
		r->timeout = timeout;
		r->token = token;
		r->ap = ms->ap_client;
		spin_lock_bh(&mh->rx_lock);
		ms->ba_rx[tid] = r;
		spin_unlock_bh(&mh->rx_lock);
		if (!ms->ap_client)
			MAC_COUNT(ba_rx_sessions);
		pr_info("mac: block ack receive session for TID %u (window %u, SSN %u)%s", tid, buf, ssn,
			ms->ap_client ? " with a client of the GO" : "");
	}
	ba_send_resp(mh, ms, token, status, ba_params(false, tid, status == STATUS_SUCCESS ? buf : 0), timeout);
}

/* ---- transmit sessions (wiphy->mtx held, except what the data path reads) ---- */
static void ba_tx_action (struct mac_hw *mh, struct mac_sta *ms, unsigned int tid, enum ieee80211_ampdu_mlme_action act,
	u16 buf, u16 ssn, bool amsdu)
{
	struct ieee80211_ampdu_params p = { 0 };

	if (!mh->ops->ampdu_action)
		return;
	p.sta = &ms->sta;
	p.action = act;
	p.buf_size = buf;
	p.ssn = ssn;
	p.tid = (u8)tid;
	p.amsdu = amsdu;
	mh->ops->ampdu_action(&mh->hw, &ms->mv->vif, &p);
}

/* Ends a transmit session on a TID (TX_STOP_CONT), with a backoff before the next request on it. delba: we end it
   ourselves (EAPOL, rtw89's stop_tx_ba_session), so the AP is told, as mac80211 does for AGG_STOP_LOCAL_REQUEST; a
   declined or unanswered request and the AP's own DELBA need none. */
static void ba_tx_stop (struct mac_hw *mh, struct mac_sta *ms, unsigned int tid, u64 now, bool delba)
{
	struct mac_ba_tx *t = &ms->ba_tx[tid];
	bool was_on = ms->ba_tx_state[tid] == MAC_BA_TX_ON;

	if (ms->ba_tx_state[tid] == MAC_BA_TX_IDLE)
		return;
	WRITE_ONCE(ms->ba_tx_state[tid], MAC_BA_TX_IDLE);
	t->deadline_ns = 0;
	ba_tx_action(mh, ms, tid, IEEE80211_AMPDU_TX_STOP_CONT, 0, 0, false);
	if (delba)
		ba_send_delba(mh, ms, tid, true, REASON_QSTA_NOT_USE);
	if (t->reqs >= BA_MAX_REQS)
		WRITE_ONCE(t->next_ns, U64_MAX);        /* give up for this connection */
	else
		/* after a session that ran, or after a burst of failed requests, the long period: an AP that keeps
		   accepting and then ending sessions costs at most one ADDBA request per TID every 15 s */
		WRITE_ONCE(t->next_ns, now + (u64)(was_on || t->reqs > BA_BURST_REQS ? BA_RETRY_MS : BA_AGAIN_MS) *
			   NSEC_PER_MSEC);
}

static void ba_tx_try_start (struct mac_hw *mh, struct mac_sta *ms, unsigned int tid, u64 now)
{
	struct mac_ba_tx *t = &ms->ba_tx[tid];
	u16 buf, ssn;

	if (ms->ba_tx_state[tid] != MAC_BA_TX_IDLE)
		return;
	if (!ms->sta.deflink.ht_cap.ht_supported) {
		WRITE_ONCE(t->next_ns, U64_MAX);        /* not an HT connection: no block ack */
		return;
	}
	if (ms->state < IEEE80211_STA_AUTHORIZED) {
		WRITE_ONCE(t->next_ns, now + (u64)BA_AGAIN_MS * NSEC_PER_MSEC);
		return;
	}
	if (t->next_ns == U64_MAX || now < t->next_ns || t->reqs >= BA_MAX_REQS)
		return;
	buf = (u16)min_t(u16, mh->hw.max_tx_aggregation_subframes ? mh->hw.max_tx_aggregation_subframes : MAC_BA_BUF,
			 MAC_BA_BUF);
	ssn = READ_ONCE(ms->tx_seq[tid]);       /* the data path's next one (it keeps sending meanwhile) */
	ba_tx_action(mh, ms, tid, IEEE80211_AMPDU_TX_START, 0, ssn, false);
	t->ssn = ssn;
	t->buf_size = buf;
	t->token = ++mh->conn.ba_token ? mh->conn.ba_token : ++mh->conn.ba_token;
	t->last_req_ns = now;
	t->deadline_ns = now + (u64)ADDBA_RESP_TIMEOUT_MS * NSEC_PER_MSEC;
	t->reqs++;
	WRITE_ONCE(ms->ba_tx_state[tid], MAC_BA_TX_PENDING);
	ba_send_addba_req(mh, ms, t->token, ba_params(false, tid, buf), 0, t->ssn);
	pr_info("mac: block ack transmit request for TID %u (window %u, SSN %u, try %u)", tid, buf, t->ssn, t->reqs);
}

/* ADDBA response to our request. wiphy->mtx held. */
static void ba_tx_resp (struct mac_hw *mh, struct mac_sta *ms, const u8 *body, unsigned int len, u64 now)
{
	struct mac_ba_tx *t;
	u16 status, params, buf;
	unsigned int tid;
	u8 token;

	if (len < 9)
		return;
	token = body[2];
	status = get_unaligned_le16(body + 3);
	params = get_unaligned_le16(body + 5);
	tid = (params >> BA_PARAM_TID_SHIFT) & 0xf;
	if (tid >= MAC_BA_TIDS)
		return;
	t = &ms->ba_tx[tid];
	if (ms->ba_tx_state[tid] != MAC_BA_TX_PENDING || t->token != token)
		return;
	if (status != STATUS_SUCCESS) {
		pr_info("mac: block ack transmit request for TID %u declined (status %u)", tid, status);
		ba_tx_stop(mh, ms, tid, now, false);
		return;
	}
	buf = (params >> BA_PARAM_BUF_SHIFT) & BA_PARAM_BUF_MASK;
	if (!buf || buf > t->buf_size)
		buf = t->buf_size;
	t->buf_size = buf;
	t->deadline_ns = 0;
	t->reqs = 0;
	WRITE_ONCE(t->next_ns, 0);
	WRITE_ONCE(ms->ba_tx_state[tid], MAC_BA_TX_ON);
	ba_tx_action(mh, ms, tid, IEEE80211_AMPDU_TX_OPERATIONAL, buf, t->ssn, params & BA_PARAM_AMSDU);
	MAC_COUNT(ba_tx_sessions);
	pr_info("mac: block ack transmit session for TID %u (window %u)", tid, buf);
}

/* DELBA from the AP: ends a session (initiator set: the AP's receive session, i.e. our transmit side). */
static void ba_rx_delba (struct mac_hw *mh, struct mac_sta *ms, const u8 *body, unsigned int len)
{
	u16 params;
	unsigned int tid;
	bool ap_initiator;

	if (len < 5)
		return;
	params = get_unaligned_le16(body + 2);
	tid = (params >> DELBA_TID_SHIFT) & 0xf;
	ap_initiator = params & DELBA_INITIATOR;
	if (tid >= MAC_BA_TIDS)
		return;
	pr_info("mac: DELBA from the AP for TID %u (%s)", tid, ap_initiator ? "its transmit" : "its receive");
	if (ap_initiator)
		ba_rx_stop(mh, ms, tid, true, true);            /* the AP stops aggregating to us */
	else
		ba_tx_stop(mh, ms, tid, ktime_get_ns(), false); /* the AP will no longer accept our aggregates */
}

/* A block ack action frame from the AP (mlme.c, wiphy->mtx held). body starts at the category octet. */
void mac_agg_rx_action (struct mac_hw *mh, const u8 *body, unsigned int len)
{
	struct mac_sta *ms = mh->conn.ap;

	if (ms)
		mac_agg_rx_action_sta(mh, ms, body, len);
}

/* ... from the station ms (the AP's, or a client of our GO, ap.c, which never gets a transmit session, so its
   ADDBA responses and DELBAs for our side find nothing to do). wiphy->mtx held. */
void mac_agg_rx_action_sta (struct mac_hw *mh, struct mac_sta *ms, const u8 *body, unsigned int len)
{
	if (len < 2 || body[0] != WLAN_CATEGORY_BACK)
		return;
	switch (body[1]) {
	case ACTION_ADDBA_REQ:
		ba_rx_addba(mh, ms, body, len);
		break;
	case ACTION_ADDBA_RESP:
		ba_tx_resp(mh, ms, body, len, ktime_get_ns());
		break;
	case ACTION_DELBA:
		ba_rx_delba(mh, ms, body, len);
		break;
	default:
		break;
	}
}

/* The data path (tx.c, rcu_read_lock held) asks whether a QoS data frame of a TID should be aggregated, and asks
   for a session when there is none. true = carry IEEE80211_TX_CTL_AMPDU. */
bool mac_agg_tx_check (struct mac_hw *mh, struct mac_sta *ms, unsigned int tid, u16 ethertype)
{
	u64 next;
	u8 state;

	if (tid >= MAC_BA_TIDS)
		return false;
	if (ethertype == ETH_P_PAE) {
		/* a group key update must not be aggregated: stop a running session, like rtw89's txq path */
		if (READ_ONCE(ms->ba_tx_state[tid]) != MAC_BA_TX_IDLE && !test_and_set_bit(tid, &ms->ba_tx_stop))
			wiphy_work_queue(mh->hw.wiphy, &mh->conn.ba_work);
		return false;
	}
	state = READ_ONCE(ms->ba_tx_state[tid]);
	if (state == MAC_BA_TX_ON)
		return true;
	if (state == MAC_BA_TX_PENDING || ms->state < IEEE80211_STA_AUTHORIZED)
		return false;
	/* only once the backoff is over: otherwise every frame would queue ba_work (and take wiphy->mtx) for nothing */
	next = READ_ONCE(ms->ba_tx[tid].next_ns);
	if (next != U64_MAX && ktime_get_ns() >= next && !test_and_set_bit(tid, &ms->ba_tx_want))
		wiphy_work_queue(mh->hw.wiphy, &mh->conn.ba_work);
	return false;
}

/* ---- the TX block ack session calls rtw89 makes (mac80211.h routes them here) ---- */
/* rtw89's txq path asks for a session (core.c rtw89_core_ba_work). Any context: set the bit and let ba_work act. */
int linuxkpi_ieee80211_start_tx_ba_session (struct ieee80211_sta *sta, uint8_t tid, int timeout)
{
	struct mac_sta *ms = container_of(sta, struct mac_sta, sta);
	struct mac_hw *mh = ms->mv ? ms->mv->mh : NULL;

	(void)timeout;
	if (!mh || tid >= MAC_BA_TIDS)
		return -EINVAL;
	if (!test_and_set_bit(tid, &ms->ba_tx_want))
		wiphy_work_queue(mh->hw.wiphy, &mh->conn.ba_work);
	return 0;
}

/* rtw89 asks to stop a session (EAPOL, tid config). Any context: set the bit and let ba_work act. */
int linuxkpi_ieee80211_stop_tx_ba_session (struct ieee80211_sta *sta, uint8_t tid)
{
	struct mac_sta *ms = container_of(sta, struct mac_sta, sta);
	struct mac_hw *mh = ms->mv ? ms->mv->mh : NULL;

	if (!mh || tid >= MAC_BA_TIDS)
		return -EINVAL;
	if (!test_and_set_bit(tid, &ms->ba_tx_stop))
		wiphy_work_queue(mh->hw.wiphy, &mh->conn.ba_work);
	return 0;
}

/*
 * The session callbacks rtw89 calls from inside ampdu_action (mac80211.c: stop_tx_ba_cb_irqsafe during TX_STOP).
 * They may run in any context, including re-entrant inside ampdu_action, so they only note the request and let
 * ba_work do the work: they never take the wiphy lock, call ampdu_action, or send a frame.
 */
void linuxkpi_ieee80211_start_tx_ba_cb_irqsafe (struct ieee80211_vif *vif, const uint8_t *addr, uint8_t tid)
{
	struct mac_hw *mh = mac_vif(vif)->mh;

	(void)addr;
	if (tid < MAC_BA_TIDS)
		wiphy_work_queue(mh->hw.wiphy, &mh->conn.ba_work);
}

void linuxkpi_ieee80211_stop_tx_ba_cb_irqsafe (struct ieee80211_vif *vif, const uint8_t *addr, uint8_t tid)
{
	struct mac_hw *mh = mac_vif(vif)->mh;
	struct mac_sta *ms = rcu_access_pointer(mh->conn.ap);

	(void)addr;
	if (ms && tid < MAC_BA_TIDS && !test_and_set_bit(tid, &ms->ba_tx_stop))
		wiphy_work_queue(mh->hw.wiphy, &mh->conn.ba_work);
}

/* ba_work / ba_timer: process the requests and stops the data path and rtw89 asked for, and the ADDBA timeouts. */
static void ba_run (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	struct mac_sta *ms;
	u64 now, next = 0;

	lockdep_assert_wiphy(mh->hw.wiphy);
	if (READ_ONCE(mh->restarting))
		return;         /* rtw89 is being reset: mac_restart_work ends the sessions without it */
	ms = c->ap;
	if (!mh->started || !ms || READ_ONCE(c->state) != RTW_CONN_CONNECTED) {
		if (ms) {
			ms->ba_tx_want = 0;
			ms->ba_tx_stop = 0;
		}
		return;
	}
	now = ktime_get_ns();
	for (int tid = 0; tid < MAC_BA_TIDS; tid++) {
		struct mac_ba_tx *t = &ms->ba_tx[tid];

		if (test_and_clear_bit(tid, &ms->ba_tx_stop))
			ba_tx_stop(mh, ms, tid, now, true);
		if (ms->ba_tx_state[tid] == MAC_BA_TX_PENDING && t->deadline_ns && now >= t->deadline_ns) {
			pr_info("mac: block ack transmit request for TID %u timed out", tid);
			ba_tx_stop(mh, ms, tid, now, false);
		}
		if (test_and_clear_bit(tid, &ms->ba_tx_want))
			ba_tx_try_start(mh, ms, tid, now);
		if (ms->ba_tx_state[tid] == MAC_BA_TX_PENDING && t->deadline_ns &&
		    (!next || t->deadline_ns < next))
			next = t->deadline_ns;
	}
	if (next)
		wiphy_delayed_work_queue(mh->hw.wiphy, &c->ba_timer,
					 msecs_to_jiffies(next > now ? (unsigned long)DIV_ROUND_UP(next - now, NSEC_PER_MSEC) : 1));
}

static void ba_work_fn (struct wiphy *wiphy, struct wiphy_work *w)
{
	(void)wiphy;
	ba_run(container_of(w, struct mac_hw, conn.ba_work));
}

static void ba_timer_fn (struct wiphy *wiphy, struct wiphy_work *w)
{
	(void)wiphy;
	ba_run(container_of(w, struct mac_hw, conn.ba_timer.work));
}

void mac_agg_init (struct mac_hw *mh)
{
	wiphy_work_init(&mh->conn.ba_work, ba_work_fn);
	wiphy_delayed_work_init(&mh->conn.ba_timer, ba_timer_fn);
}

/* The station is allocated (mlme.c sta_alloc): set up its reorder timer. The rest is zeroed already. */
void mac_agg_sta_init (struct mac_sta *ms)
{
	timer_setup(&ms->ba_rx_timer, mac_agg_rx_timer, 0);
	for (int tid = 0; tid < MAC_BA_TIDS; tid++)
		ms->ba_tx[tid].next_ns = 0;
}

/* Ends every session of a station while it is still in the driver (leaving, rekey, restart): drops the held
   receive frames and resets the transmit state; tells rtw89 only when call_driver. wiphy->mtx held. */
void mac_agg_stop_sta (struct mac_hw *mh, struct mac_sta *ms, bool call_driver)
{
	for (int tid = 0; tid < MAC_BA_TIDS; tid++) {
		if (ms->ba_rx[tid])
			ba_rx_stop(mh, ms, tid, false, call_driver);
		if (ms->ba_tx_state[tid] != MAC_BA_TX_IDLE) {
			WRITE_ONCE(ms->ba_tx_state[tid], MAC_BA_TX_IDLE);
			ms->ba_tx[tid].deadline_ns = 0;
			if (call_driver)
				ba_tx_action(mh, ms, tid, IEEE80211_AMPDU_TX_STOP_CONT, 0, 0, false);
		}
	}
	ms->ba_tx_want = 0;
	ms->ba_tx_stop = 0;
	wiphy_work_cancel(mh->hw.wiphy, &mh->conn.ba_work);
	wiphy_delayed_work_cancel(mh->hw.wiphy, &mh->conn.ba_timer);
}

/* A client of our GO leaves (ap.c): its receive sessions end with the held frames dropped. Unlike
   mac_agg_stop_sta() it leaves the station's block ack work alone. wiphy->mtx held. */
void mac_agg_stop_client (struct mac_hw *mh, struct mac_sta *ms, bool call_driver)
{
	for (int tid = 0; tid < MAC_BA_TIDS; tid++)
		if (ms->ba_rx[tid])
			ba_rx_stop(mh, ms, tid, false, call_driver);
	ms->ba_tx_want = 0;
	ms->ba_tx_stop = 0;
}

/* The station is being freed (mlme.c mac_sta_free): drop any held frames and stop the reorder timer for good.
   The timer callback takes mac_hw.rx_lock, so set the dead flag and drop the buffers under the lock, then wait for
   the timer outside it. PASSIVE_LEVEL. */
void mac_agg_sta_free (struct mac_hw *mh, struct mac_sta *ms)
{
	spin_lock_bh(&mh->rx_lock);
	ms->ba_rx_dead = true;
	for (int tid = 0; tid < MAC_BA_TIDS; tid++) {
		struct mac_ba_rx *r = ms->ba_rx[tid];

		ms->ba_rx[tid] = NULL;
		if (!r)
			continue;
		for (int i = 0; i < MAC_BA_BUF; i++)
			if (r->frames[i])
				kfree_skb(r->frames[i]);
		kfree(r);
	}
	ms->ba_rx_timer_ns = 0;
	spin_unlock_bh(&mh->rx_lock);
	timer_delete_sync(&ms->ba_rx_timer);
}

/* The connection is going away: stop the transmit state machine (wiphy->mtx held). */
void mac_agg_cancel (struct mac_hw *mh)
{
	wiphy_work_cancel(mh->hw.wiphy, &mh->conn.ba_work);
	wiphy_delayed_work_cancel(mh->hw.wiphy, &mh->conn.ba_timer);
}

/* Fills the current per-TID session bitmaps and the restart counter for IOCTL_RTW_LINK. Without wiphy->mtx (a
   snapshot, like the rest of rtw_linux_link_stats()): the station is read in a read-side section, as the data path
   does, so a connection leaving meanwhile cannot free it under us. */
void mac_agg_stats (struct mac_hw *mh, struct rtw_link_stats *st)
{
	struct mac_sta *ms;

	st->restarts = READ_ONCE(mh->restarts);
	st->ba_rx_tids = 0;
	st->ba_tx_tids = 0;
	rcu_read_lock();
	ms = rcu_dereference(mh->conn.ap);
	for (int tid = 0; ms && tid < MAC_BA_TIDS; tid++) {
		if (READ_ONCE(ms->ba_rx[tid]))
			st->ba_rx_tids |= (u8)(1u << tid);
		if (READ_ONCE(ms->ba_tx_state[tid]) == MAC_BA_TX_ON)
			st->ba_tx_tids |= (u8)(1u << tid);
	}
	rcu_read_unlock();
}
