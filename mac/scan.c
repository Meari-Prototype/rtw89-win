// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: hardware (firmware offload) scan the way mac80211 scan.c runs one with hw_scan: a single request
 * over the enabled channels of all bands (rtw89 sets SINGLE_SCAN_ON_ALL_BANDS) with a wildcard SSID and the probe
 * request elements per band. The firmware probes on channels without NO_IR/RADAR and listens on the others.
 * The driver reports the end with ieee80211_scan_completed() (from its C2H work, or synchronously when the scan
 * is cancelled); scan_done_work then frees the request under wiphy->mtx, after the driver has dropped it, and
 * wakes the waiter. Results land in the BSS table (bss.c).
 * rtw_linux_scan_start() is the asynchronous form for the WiFiCx front end (requested SSIDs and channels,
 * a completion callback, the table is kept); the IOCTL's rtw_linux_scan() starts the same way and waits.
 */
#include "mac.h"

#define SCAN_IE_BUF                     256
#define SCAN_TIMEOUT_MS                 15000
/* the caller holds its control lock while waiting, which also holds up device removal: keep it short */
#define SCAN_TIMEOUT_MAX_MS             30000

/* ---- probe request frame and elements ---- */
struct sk_buff *linuxkpi_ieee80211_probereq_get (struct ieee80211_hw *hw, const uint8_t *addr, const uint8_t *ssid,
	size_t ssid_len, size_t tailroom)
{
	struct ieee80211_hdr_3addr *hdr;
	struct sk_buff *skb;
	u8 *pos;

	if (ssid_len > IEEE80211_MAX_SSID_LEN)
		return NULL;
	skb = dev_alloc_skb(hw->extra_tx_headroom + sizeof(*hdr) + 2 + ssid_len + tailroom);
	if (!skb)
		return NULL;
	skb_reserve(skb, hw->extra_tx_headroom);
	hdr = skb_put_zero(skb, sizeof(*hdr));
	hdr->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_PROBE_REQ);
	eth_broadcast_addr(hdr->addr1);
	ether_addr_copy(hdr->addr2, addr);
	eth_broadcast_addr(hdr->addr3);
	pos = skb_put(skb, 2 + ssid_len);
	pos[0] = WLAN_EID_SSID;
	pos[1] = (u8)ssid_len;
	if (ssid_len)
		memcpy(pos + 2, ssid, ssid_len);
	return skb;
}

/* Supported Rates (first 8) and Extended Supported Rates, in 500 kbit/s units */
u8 *mac_put_rates (u8 *pos, const struct ieee80211_supported_band *sb)
{
	int n = min(sb->n_bitrates, 8 + 255);
	int i;

	*pos++ = WLAN_EID_SUPP_RATES;
	*pos++ = (u8)min(n, 8);
	for (i = 0; i < n && i < 8; i++)
		*pos++ = (u8)(sb->bitrates[i].bitrate / 5);
	if (n > 8) {
		*pos++ = WLAN_EID_EXT_SUPP_RATES;
		*pos++ = (u8)(n - 8);
		for (; i < n; i++)
			*pos++ = (u8)(sb->bitrates[i].bitrate / 5);
	}
	return pos;
}

/* HT Capabilities element (802.11-2020 9.4.2.55) */
u8 *mac_put_ht_cap (u8 *pos, const struct ieee80211_sta_ht_cap *ht)
{
	BUILD_BUG_ON(sizeof(ht->mcs) != 16);
	*pos++ = WLAN_EID_HT_CAPABILITY;
	*pos++ = 26;
	put_unaligned_le16(ht->cap, pos);
	pos += 2;
	*pos++ = (u8)(ht->ampdu_factor | (ht->ampdu_density << IEEE80211_HT_AMPDU_PARM_DENSITY_SHIFT));
	memcpy(pos, &ht->mcs, 16);
	pos += 16;
	memset(pos, 0, 7);              /* extended capabilities, TX beamforming, ASEL */
	return pos + 7;
}

/* VHT Capabilities element (802.11-2020 9.4.2.157) */
static u8 *put_vht_cap (u8 *pos, const struct ieee80211_sta_vht_cap *vht)
{
	BUILD_BUG_ON(sizeof(vht->vht_mcs) != 8);
	*pos++ = WLAN_EID_VHT_CAPABILITY;
	*pos++ = 12;
	put_unaligned_le32(vht->cap, pos);
	pos += 4;
	memcpy(pos, &vht->vht_mcs, 8);
	return pos + 8;
}

/* ---- the request ---- */
/* enabled, and listed in the request if it lists channels */
static bool scan_channel_ok (const struct rtw_scan_req *rq, int band, const struct ieee80211_channel *c)
{
	u32 i;

	if (c->flags & IEEE80211_CHAN_DISABLED)
		return false;
	if (!rq->n_channels)
		return true;
	for (i = 0; i < rq->n_channels && i < RTW_SCAN_MAX_CHANNELS; i++)
		if (rq->channels[i].band == band && rq->channels[i].number == c->hw_value)
			return true;
	return false;
}

static struct ieee80211_scan_request *scan_req_alloc (struct mac_hw *mh, const struct rtw_scan_req *rq)
{
	struct wiphy *wiphy = mh->hw.wiphy;
	struct ieee80211_scan_request *req;
	struct cfg80211_scan_request *r;
	struct cfg80211_ssid *ssid;
	int n = 0, n_ssids, band, i;
	u8 *ie, *pos;

	for (band = 0; band < NUM_NL80211_BANDS; band++)
		for (i = 0; wiphy->bands[band] && i < wiphy->bands[band]->n_channels; i++)
			n += scan_channel_ok(rq, band, &wiphy->bands[band]->channels[i]);
	if (!n)
		return NULL;
	/* the listed SSIDs (an empty one is the wildcard), else the wildcard; none for a passive scan */
	n_ssids = (int)min_t(u32, rq->n_ssids, RTW_SCAN_MAX_SSIDS);
	if (!n_ssids)
		n_ssids = 1;
	if (wiphy->max_scan_ssids > 0)
		n_ssids = min(n_ssids, wiphy->max_scan_ssids);
	if (rq->flags & RTW_SCAN_PASSIVE)
		n_ssids = 0;
	req = kzalloc(sizeof(*req) + n * sizeof(req->req.channels[0]) + max(n_ssids, 1) * sizeof(*ssid) + SCAN_IE_BUF,
		      GFP_KERNEL);
	if (!req)
		return NULL;
	r = &req->req;
	r->wiphy = wiphy;
	for (band = 0; band < NUM_NL80211_BANDS; band++)
		for (i = 0; wiphy->bands[band] && i < wiphy->bands[band]->n_channels; i++)
			if (scan_channel_ok(rq, band, &wiphy->bands[band]->channels[i]))
				r->channels[r->n_channels++] = &wiphy->bands[band]->channels[i];

	ssid = (struct cfg80211_ssid *)&r->channels[n];         /* zeroed: wildcard SSIDs */
	for (i = 0; i < n_ssids && (u32)i < rq->n_ssids; i++) {
		ssid[i].ssid_len = min_t(int, rq->ssids[i].len, IEEE80211_MAX_SSID_LEN);
		memcpy(ssid[i].ssid, rq->ssids[i].ssid, ssid[i].ssid_len);
	}
	r->ssids = ssid;
	r->n_ssids = n_ssids;

	/* ieee80211_build_preq_ies(): per-band elements, then the common ones (none) */
	ie = (u8 *)(ssid + max(n_ssids, 1));
	pos = ie;
	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		const struct ieee80211_supported_band *sb = wiphy->bands[band];
		u8 *start = pos;

		if (!sb)
			continue;
		pos = mac_put_rates(pos, sb);
		if (sb->ht_cap.ht_supported)
			pos = mac_put_ht_cap(pos, &sb->ht_cap);
		if (band == NL80211_BAND_5GHZ && sb->vht_cap.vht_supported)
			pos = put_vht_cap(pos, &sb->vht_cap);
		req->ies.ies[band] = start;
		req->ies.len[band] = (int)(pos - start);
	}
	if (WARN_ON(pos - ie > SCAN_IE_BUF)) {
		kfree(req);
		return NULL;
	}
	req->ies.common_ies = pos;
	req->ies.common_ie_len = 0;
	r->ie = ie;
	r->ie_len = (int)(pos - ie);    /* the driver asks probereq_get() for this much tailroom */
	eth_broadcast_addr(r->bssid);
	r->tsf_report_link_id = -1;
	if (rq->dwell_ms) {
		r->duration = (u16)clamp_t(u32, rq->dwell_ms, 10, 250);     /* the AX firmware takes a u8 period */
		r->duration_mandatory = true;
	}
	return req;
}

/* ---- start, completion, cancel ---- */
static void scan_finish (struct mac_hw *mh, bool aborted, bool keep_req)
{
	rtw_scan_done_fn done = mh->scan_done_fn;
	void *ctx = mh->scan_done_ctx;

	lockdep_assert_wiphy(mh->hw.wiphy);
	mh->scan_done_fn = NULL;
	mh->scan_done_ctx = NULL;
	if (!keep_req)
		kfree(mh->scan_req);
	else if (!mh->scan_orphan)
		mh->scan_orphan = mh->scan_req;
	else
		pr_err("mac: a second scan request was never completed; leaking it");
	mh->scan_req = NULL;
	mh->scan_vif = NULL;
	mh->scanning = false;
	mh->scan_completed = false;
	mh->last_scan_aborted = aborted;
	mh->last_scan_ms = (u32)((ktime_get_ns() - mh->scan_start_ns) / NSEC_PER_MSEC);
	/* __ieee80211_scan_completed() */
	mac_recalc_idle(mh);
	mac_state_set(RTW_STATE_SCANNING, false);
	mac_bss_scan_done(mh->last_scan_ms, aborted);
	complete(&mh->scan_done);
	if (done)
		done(ctx, 0, aborted, mh->scan_start_ns);
}

/* scan.c ieee80211_scan_work() for a completed hw scan: runs under wiphy->mtx after the driver's work */
static void scan_done_work (struct wiphy *wiphy, struct wiphy_work *w)
{
	struct mac_hw *mh = container_of(w, struct mac_hw, scan_done_work);

	(void)wiphy;
	if (!mh->scanning || !mh->scan_completed)
		return;
	scan_finish(mh, mh->scan_aborted, false);
}

/* From the driver, with wiphy->mtx held (rtw89: C2H work, or its abort path). Like mac80211, only records the
   end and queues the work; the driver still uses the request until it returns. */
void linuxkpi_ieee80211_scan_completed (struct ieee80211_hw *hw, struct cfg80211_scan_info *info)
{
	struct mac_hw *mh = mac_hw(hw);

	if (WARN_ON(!mh->scanning))
		return;
	mh->scan_aborted = info->aborted;
	mh->scan_completed = true;
	wiphy_work_queue(hw->wiphy, &mh->scan_done_work);
}

void mac_scan_init (struct mac_hw *mh)
{
	wiphy_work_init(&mh->scan_done_work, scan_done_work);
	init_completion(&mh->scan_done);
}

/* scan.c ieee80211_scan_cancel() for a hw scan; with wiphy->mtx held. Returns with no scan running. */
void mac_scan_cancel (struct mac_hw *mh)
{
	lockdep_assert_wiphy(mh->hw.wiphy);
	if (!mh->scanning)
		return;
	if (!mh->scan_completed)
		mh->ops->cancel_hw_scan(&mh->hw, &mh->scan_vif->vif);
	/* rtw89 reports the abort before cancel_hw_scan() returns; run scan_done_work here (it cannot be
	   running: the runner holds wiphy->mtx while it takes an item) */
	wiphy_work_flush(mh->hw.wiphy, &mh->scan_done_work);
	if (mh->scanning) {
		/* the driver may still point at the request: keep it until the hw is freed */
		pr_err("mac: the driver did not complete the cancelled scan");
		scan_finish(mh, true, true);
	}
}

/* A hardware restart (iface.c): the driver has been reset, so a running scan is finished as aborted without
   calling cancel_hw_scan. Its request is kept until the hw is freed (the driver may still point at it). */
void mac_scan_abandon (struct mac_hw *mh)
{
	lockdep_assert_wiphy(mh->hw.wiphy);
	if (!mh->scanning)
		return;
	pr_warn("mac: abandoning a scan for the hardware restart");
	scan_finish(mh, true, true);
}

static int scan_start_locked (struct mac_hw *mh, const struct rtw_scan_req *rq, rtw_scan_done_fn done, void *ctx)
{
	struct ieee80211_scan_request *req;
	int ret;

	lockdep_assert_wiphy(mh->hw.wiphy);
	if (!mh->started)
		return -ENOTCONN;
	/* while connected rtw89 scans off the operating channel and comes back; not while a connection is being
	   made or taken apart, nor before the OS's 4-way handshake gave us the pairwise key: its EAPOL frames
	   would wait for the scan */
	if (mh->scanning || mh->listen_ctx ||
	    (mh->conn.state != RTW_CONN_IDLE && mh->conn.state != RTW_CONN_CONNECTED) ||
	    (mh->conn.state == RTW_CONN_CONNECTED && mac_conn_rsn(&mh->conn) && !mh->conn.ptk))
		return -EBUSY;
	req = scan_req_alloc(mh, rq);
	if (!req)
		return -ENOMEM;
	if (!(rq->flags & RTW_SCAN_KEEP))
		mac_bss_clear();
	else
		mac_bss_new_session();
	mh->scan_req = req;
	mh->scan_vif = mh->sta_vif;
	mh->scanning = true;
	mh->scan_completed = false;
	mh->scan_aborted = false;
	mh->scan_done_fn = done;
	mh->scan_done_ctx = ctx;
	reinit_completion(&mh->scan_done);
	mh->scan_start_ns = ktime_get_ns();
	mac_recalc_idle(mh);
	ret = mh->ops->hw_scan(&mh->hw, &mh->scan_vif->vif, req);
	if (ret) {
		/* 1 asks for a software scan, which is not implemented */
		if (ret == 1)
			ret = -EOPNOTSUPP;
		pr_err("mac: drv hw_scan failed: %d", ret);
		/* a completion the driver reported on its way out is dropped by scan_done_work (!scanning) */
		kfree(req);
		mh->scan_req = NULL;
		mh->scan_vif = NULL;
		mh->scanning = false;
		mh->scan_completed = false;
		mh->scan_done_fn = NULL;
		mh->scan_done_ctx = NULL;
		mac_recalc_idle(mh);
		return ret;
	}
	mac_state_set(RTW_STATE_SCANNING, true);
	pr_info("mac: scan started: %d channels, %d SSIDs%s, dwell %u ms", req->req.n_channels, req->req.n_ssids,
		req->req.n_ssids ? " (passive where NO_IR)" : " (passive)", req->req.duration);
	return 0;
}

int rtw_linux_scan_start (const struct rtw_scan_req *req, rtw_scan_done_fn done, void *ctx)
{
	struct mac_hw *mh = mac_the_hw;
	struct rtw_scan_req rq;
	int ret;

	if (!mh)
		return -ENODEV;
	rq = *req;
	rq.flags |= RTW_SCAN_KEEP;
	ret = mac_lock_driver(mh) ? scan_start_locked(mh, &rq, done, ctx) : -EBUSY;
	wiphy_unlock(mh->hw.wiphy);
	return ret;
}

void rtw_linux_scan_cancel (void)
{
	struct mac_hw *mh = mac_the_hw;

	if (!mh)
		return;
	/* while a hardware restart is still under way, the scan ends without the (reset) driver */
	if (mac_lock_driver(mh))
		mac_scan_cancel(mh);
	else
		mac_scan_abandon(mh);
	wiphy_unlock(mh->hw.wiphy);
}

int rtw_linux_scan (const struct rtw_scan_param *param, struct rtw_scan_result *res)
{
	struct mac_hw *mh = mac_the_hw;
	u32 timeout = param->timeout_ms ? clamp_t(u32, param->timeout_ms, 1000, SCAN_TIMEOUT_MAX_MS) : SCAN_TIMEOUT_MS;
	struct rtw_scan_req rq = { 0 };
	bool timed_out = false, ready;
	int ret;

	memset(res, 0, sizeof(*res));
	if (!mh) {
		res->result = -ENODEV;
		return res->result;
	}
	rq.flags = param->flags & (RTW_SCAN_KEEP | RTW_SCAN_PASSIVE);
	rq.dwell_ms = param->dwell_ms;
	ret = mac_lock_driver(mh) ? scan_start_locked(mh, &rq, NULL, NULL) : -EBUSY;
	wiphy_unlock(mh->hw.wiphy);
	if (ret) {
		res->result = ret;
		return ret;
	}

	/* without wiphy->mtx: the driver's C2H work needs it to complete the scan */
	wait_for_completion_timeout(&mh->scan_done, msecs_to_jiffies(timeout));

	ready = mac_lock_driver(mh);
	if (mh->scanning && !mh->scan_completed) {
		pr_warn("mac: scan not done after %u ms; cancelling", timeout);
		timed_out = true;
	}
	/* also when the driver completed it after the wait ended: scan_done_work has not run yet (a hardware restart
	   still under way: without the driver) */
	if (ready)
		mac_scan_cancel(mh);
	else
		mac_scan_abandon(mh);
	res->aborted = mh->last_scan_aborted;
	res->duration_ms = mh->last_scan_ms;
	wiphy_unlock(mh->hw.wiphy);
	res->bss_count = mac_bss_count();
	res->result = timed_out ? -ETIMEDOUT : 0;
	return res->result;
}
