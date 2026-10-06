// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: struct wiphy / struct ieee80211_hw lifetime, wiphy work and the hw work queue.
 * Memory layout of one hw: [struct mac_wiphy | wiphy.priv = struct mac_hw | hw->priv = driver data].
 * Semantics follow cfg80211/mac80211: wiphy work items run one at a time with wiphy->mtx held; the hw work queue
 * exists between ieee80211_register_hw() and ieee80211_unregister_hw(). Registration also applies the world
 * regulatory domain to the channels and calls the driver's reg_notifier with it (reg.c).
 */
#include "mac.h"

int linuxkpi_debug_80211;
const uint8_t rfc1042_header[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

_Static_assert(sizeof(struct rtw_caps) == 594, "rtw_caps layout");
_Static_assert(sizeof(struct rtw_scan_req) == 276, "rtw_scan_req layout");

struct workqueue_struct *mac_wiphy_wq;
struct mac_hw *mac_the_hw;

/* cfg80211 requests come from WiFiCx (driver\), not from nl80211, so the wiphy has no cfg80211 ops. */
static const struct cfg80211_ops mac_cfg_ops;

int rtw_mac_init (void)
{
	mac_wiphy_wq = alloc_ordered_workqueue("wiphy", 0);
	return mac_wiphy_wq ? 0 : -ENOMEM;
}

void rtw_mac_exit (void)
{
	destroy_workqueue(mac_wiphy_wq);
	mac_wiphy_wq = NULL;
	mac_bss_clear();                /* frees the kept frame bodies */
}

/* ---- wiphy work ---- */
static void wiphy_work_run (struct work_struct *w)
{
	struct mac_wiphy *mw = container_of(w, struct mac_wiphy, work);
	struct wiphy_work *wk;

	wiphy_lock(&mw->wiphy);
	spin_lock_bh(&mw->work_lock);
	wk = list_first_entry_or_null(&mw->work_list, struct wiphy_work, entry);
	if (wk) {
		list_del_init(&wk->entry);
		if (!list_empty(&mw->work_list))
			queue_work(mac_wiphy_wq, &mw->work);
	}
	spin_unlock_bh(&mw->work_lock);
	if (wk)
		wk->fn(&mw->wiphy, wk);
	wiphy_unlock(&mw->wiphy);
}

void linuxkpi_wiphy_work_queue (struct wiphy *wiphy, struct wiphy_work *wwk)
{
	struct mac_wiphy *mw = mac_wiphy(wiphy);

	spin_lock_bh(&mw->work_lock);
	if (list_empty(&wwk->entry))
		list_add_tail(&wwk->entry, &mw->work_list);
	spin_unlock_bh(&mw->work_lock);
	queue_work(mac_wiphy_wq, &mw->work);
}

/* Callers hold wiphy->mtx, so the item is not running. */
void linuxkpi_wiphy_work_cancel (struct wiphy *wiphy, struct wiphy_work *wwk)
{
	struct mac_wiphy *mw = mac_wiphy(wiphy);

	spin_lock_bh(&mw->work_lock);
	if (!list_empty(&wwk->entry))
		list_del_init(&wwk->entry);
	spin_unlock_bh(&mw->work_lock);
}

/* Runs queued items in the caller (which holds wiphy->mtx) up to and including end; all of them if end is NULL. */
void linuxkpi_wiphy_work_flush (struct wiphy *wiphy, struct wiphy_work *end)
{
	struct mac_wiphy *mw = mac_wiphy(wiphy);
	int limit = 100;

	spin_lock_bh(&mw->work_lock);
	while ((!end || !list_empty(&end->entry)) && !list_empty(&mw->work_list) && limit--) {
		struct wiphy_work *wk = list_first_entry(&mw->work_list, struct wiphy_work, entry);

		list_del_init(&wk->entry);
		spin_unlock_bh(&mw->work_lock);
		wk->fn(wiphy, wk);
		spin_lock_bh(&mw->work_lock);
	}
	spin_unlock_bh(&mw->work_lock);
}

void lkpi_wiphy_delayed_work_timer (struct timer_list *t)
{
	struct wiphy_delayed_work *dw = timer_container_of(dw, t, timer);

	wiphy_work_queue(dw->wiphy, &dw->work);
}

void linuxkpi_wiphy_delayed_work_queue (struct wiphy *wiphy, struct wiphy_delayed_work *dw, unsigned long delay)
{
	if (!delay) {
		del_timer(&dw->timer);
		wiphy_work_queue(wiphy, &dw->work);
		return;
	}
	dw->wiphy = wiphy;
	mod_timer(&dw->timer, jiffies + delay);
}

void linuxkpi_wiphy_delayed_work_cancel (struct wiphy *wiphy, struct wiphy_delayed_work *dw)
{
	del_timer_sync(&dw->timer);
	wiphy_work_cancel(wiphy, &dw->work);
}

void linuxkpi_wiphy_delayed_work_flush (struct wiphy *wiphy, struct wiphy_delayed_work *dw)
{
	del_timer_sync(&dw->timer);
	wiphy_work_flush(wiphy, &dw->work);
}

/* ---- wiphy ---- */
struct wiphy *linuxkpi_wiphy_new (const struct cfg80211_ops *ops, size_t priv_len)
{
	struct mac_wiphy *mw = kzalloc(sizeof(*mw) + priv_len, GFP_KERNEL);

	(void)ops;
	if (!mw)
		return NULL;
	spin_lock_init(&mw->work_lock);
	INIT_LIST_HEAD(&mw->work_list);
	INIT_WORK(&mw->work, wiphy_work_run);
	mutex_init(&mw->wiphy.mtx);
	INIT_LIST_HEAD(&mw->wiphy.wdev_list);
	/* cfg80211 defaults (wiphy_new_nm) */
	mw->wiphy.retry_short = 7;
	mw->wiphy.retry_long = 4;
	mw->wiphy.frag_threshold = (u32)-1;
	mw->wiphy.rts_threshold = (u32)-1;
	return &mw->wiphy;
}

int linuxkpi_80211_wiphy_register (struct wiphy *wiphy)
{
	mac_wiphy(wiphy)->registered = true;
	return 0;
}

void linuxkpi_wiphy_free (struct wiphy *wiphy)
{
	struct mac_wiphy *mw;

	if (!wiphy)
		return;
	mw = mac_wiphy(wiphy);
	cancel_work_sync(&mw->work);
	if (!list_empty(&mw->work_list))
		pr_warn("mac: wiphy freed with queued work");
	/* only the regulatory_hint() inline sets it, with lkpi_get_linuxkpi_ieee80211_regdomain() memory */
	kfree(wiphy->regd);
	kfree(mw);
}

struct linuxkpi_ieee80211_regdomain *lkpi_get_linuxkpi_ieee80211_regdomain (size_t n)
{
	struct linuxkpi_ieee80211_regdomain *regd;

	return kzalloc(struct_size(regd, reg_rules, n), GFP_KERNEL);
}

/* ---- ieee80211_hw ---- */
struct ieee80211_hw *linuxkpi_ieee80211_alloc_hw (size_t priv_len, const struct ieee80211_ops *ops)
{
	struct wiphy *wiphy = wiphy_new(&mac_cfg_ops, MAC_HW_SIZE + priv_len);
	struct mac_hw *mh;
	struct ieee80211_hw *hw;

	if (!wiphy)
		return NULL;
	mh = wiphy_priv(wiphy);
	mh->ops = ops;
	INIT_LIST_HEAD(&mh->vif_list);
	INIT_LIST_HEAD(&mh->chanctx_list);
	INIT_LIST_HEAD(&mh->sta_list);
	spin_lock_init(&mh->rx_lock);
	mac_restart_init(mh);
	mac_scan_init(mh);
	mac_tx_init(mh);
	mac_rx_init(mh);
	mac_mlme_init(mh);
	mac_ps_init(mh);
	mac_ap_init(mh);
	hw = &mh->hw;
	hw->wiphy = wiphy;
	hw->priv = (u8 *)mh + MAC_HW_SIZE;
	/* mac80211 defaults (ieee80211_alloc_hw_nm) */
	hw->queues = 1;
	hw->max_rates = 1;
	hw->max_rx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF_HT;
	hw->max_tx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF_HT;
	hw->conf.flags |= IEEE80211_CONF_IDLE;
	return hw;
}

void linuxkpi_set_ieee80211_dev (struct ieee80211_hw *hw)
{
	(void)hw;                       /* set_wiphy_dev() already stored the device */
}

struct ieee80211_hw *linuxkpi_wiphy_to_ieee80211_hw (struct wiphy *wiphy)
{
	return &((struct mac_hw *)wiphy_priv(wiphy))->hw;
}

static void log_band (const struct ieee80211_supported_band *sb, const char *name)
{
	if (!sb)
		return;
	pr_info("mac: %s: %d channels (%u-%u MHz), %d rates, ht %d vht %d", name, sb->n_channels,
		sb->n_channels ? sb->channels[0].center_freq : 0,
		sb->n_channels ? sb->channels[sb->n_channels - 1].center_freq : 0,
		sb->n_bitrates, sb->ht_cap.ht_supported, sb->vht_cap.vht_supported);
}

/* ieee80211_register_hw(), after wiphy_register() */
int linuxkpi_ieee80211_ifattach (struct ieee80211_hw *hw)
{
	struct mac_hw *mh = mac_hw(hw);
	struct wiphy *wiphy = hw->wiphy;

	mh->wq = alloc_ordered_workqueue("%s", 0, wiphy_name(wiphy));
	if (!mh->wq)
		return -ENOMEM;
	mh->restart_wq = alloc_ordered_workqueue("rtw-restart", 0);
	if (!mh->restart_wq) {
		destroy_workqueue(mh->wq);
		mh->wq = NULL;
		return -ENOMEM;
	}
	mh->registered = true;
	pr_info("mac: registered %pM, fw %s, queues %u, modes %#x, antennas tx %#x rx %#x",
		wiphy->perm_addr, wiphy->fw_version, hw->queues, wiphy->interface_modes,
		wiphy->available_antennas_tx, wiphy->available_antennas_rx);
	log_band(wiphy->bands[NL80211_BAND_2GHZ], "2 GHz");
	log_band(wiphy->bands[NL80211_BAND_5GHZ], "5 GHz");
	log_band(wiphy->bands[NL80211_BAND_6GHZ], "6 GHz");

	/* like wiphy_regulatory_register(): the world rules (from wireless-regdb, reg.c), then the driver's notifier
	   sees them (rtw89 picks its world TX power tables there) */
	mac_reg_register(mh);

	mac_the_hw = mh;
	mac_state_set(RTW_STATE_PROBED, true);
	return 0;
}

/*
 * After the driver's probe (rtw89_regd_init_hint() runs after register_hw): a country code programmed into the
 * efuse reaches us as regulatory_hint(), which only records the alpha2 in wiphy->regd. Its rules then apply
 * (reg.c, NL80211_REGDOM_SET_BY_DRIVER) unless the user's country (Parameters\Country) is set. WiFiCx/WDI never
 * hands a country to the driver.
 */
void rtw_mac_log_regulatory (void)
{
	struct mac_hw *mh = mac_the_hw;
	const struct linuxkpi_ieee80211_regdomain *regd;

	if (!mh)
		return;
	regd = mh->hw.wiphy->regd;
	if (regd) {
		pr_info("mac: efuse country code %c%c", regd->alpha2[0], regd->alpha2[1]);
		mac_reg_efuse(mh, (const char *)regd->alpha2);
	} else {
		pr_info("mac: efuse country code not programmed (worldwide)");
	}
}

void linuxkpi_ieee80211_ifdetach (struct ieee80211_hw *hw)
{
	(void)hw;
}

void linuxkpi_ieee80211_unregister_hw (struct ieee80211_hw *hw)
{
	struct mac_hw *mh = mac_hw(hw);
	struct mac_wiphy *mw = mac_wiphy(hw->wiphy);

	if (!mh->registered)
		return;
	/* like mac80211 closing the interfaces first; glue.c normally stopped it already */
	if (mh->started) {
		pr_warn("mac: unregistering a started hw; stopping it");
		mac_hw_stop(mh);
	}
	mac_the_hw = NULL;
	mac_state_set(RTW_STATE_PROBED, false);
	/* like wiphy_unregister(): run what is still queued, then stop the runner */
	wiphy_lock(hw->wiphy);
	wiphy_work_flush(hw->wiphy, NULL);
	wiphy_unlock(hw->wiphy);
	cancel_work_sync(&mw->work);
	mw->registered = false;
	if (mh->restart_wq) {
		cancel_work_sync(&mh->restart_work);
		destroy_workqueue(mh->restart_wq);
		mh->restart_wq = NULL;
	}
	mac_parked_free(mh);            /* the driver is gone: free what the restarts kept */
	mac_reg_unregister(mh);         /* a regulatory change still queued is dropped */
	destroy_workqueue(mh->wq);
	mh->wq = NULL;
	mh->registered = false;
	pr_info("mac: unregistered");
}

void linuxkpi_ieee80211_iffree (struct ieee80211_hw *hw)
{
	struct mac_hw *mh = mac_hw(hw);

	/* the driver is gone now, so a scan request it never completed can go too; the rest of the memory goes
	   with wiphy_free() */
	kfree(mh->scan_orphan);
	mh->scan_orphan = NULL;
	mac_mlme_free(mh);
	mac_ap_free(mh);
	mac_rx_flush(mh);
}

void linuxkpi_ieee80211_queue_work (struct ieee80211_hw *hw, struct work_struct *w)
{
	struct mac_hw *mh = mac_hw(hw);

	if (WARN_ONCE(!mh->wq, "mac: queue_work before register_hw"))
		return;
	queue_work(mh->wq, w);
}

void linuxkpi_ieee80211_queue_delayed_work (struct ieee80211_hw *hw, struct delayed_work *dw, int delay)
{
	struct mac_hw *mh = mac_hw(hw);

	if (WARN_ONCE(!mh->wq, "mac: queue_delayed_work before register_hw"))
		return;
	queue_delayed_work(mh->wq, dw, delay);
}

/* What the WiFiCx front end reports as device, band and PHY capabilities (rtw/glue.h) */
int rtw_linux_get_caps (struct rtw_caps *caps)
{
	static const enum nl80211_band bands[2] = { NL80211_BAND_2GHZ, NL80211_BAND_5GHZ };
	struct mac_hw *mh = mac_the_hw;
	struct wiphy *wiphy;
	int b, i;

	memset(caps, 0, sizeof(*caps));
	if (!mh)
		return -ENODEV;
	wiphy = mh->hw.wiphy;
	wiphy_lock(wiphy);
	ether_addr_copy(caps->addr, wiphy->perm_addr);
	caps->tx_streams = (u8)hweight32(wiphy->available_antennas_tx);
	caps->rx_streams = (u8)hweight32(wiphy->available_antennas_rx);
	strscpy(caps->fw_version, wiphy->fw_version, sizeof(caps->fw_version));
	caps->max_scan_ssids = (u8)clamp(wiphy->max_scan_ssids, 0, 255);
	for (b = 0; b < 2; b++) {
		const struct ieee80211_supported_band *sb = wiphy->bands[bands[b]];

		if (!sb)
			continue;
		caps->ht[b] = sb->ht_cap.ht_supported;
		caps->vht[b] = sb->vht_cap.vht_supported;
		for (i = 0; i < sb->n_bitrates && caps->n_rates[b] < ARRAY_SIZE(caps->rates[b]); i++)
			caps->rates[b][caps->n_rates[b]++] = (u8)(sb->bitrates[i].bitrate / 5);
		for (i = 0; i < sb->n_channels && caps->n_chans[b] < ARRAY_SIZE(caps->chans[b]); i++) {
			const struct ieee80211_channel *c = &sb->channels[i];
			struct rtw_caps_chan *o;

			/* reported once, so the world's channels as well as the ones the domain now enables */
			if (!mac_reg_chan_possible(c))
				continue;
			o = &caps->chans[b][caps->n_chans[b]++];
			o->freq = (u16)c->center_freq;
			o->number = (u8)c->hw_value;
			o->flags = ((c->flags & IEEE80211_CHAN_NO_IR) ? RTW_CHAN_PASSIVE : 0) |
				   ((c->flags & IEEE80211_CHAN_RADAR) ? RTW_CHAN_RADAR : 0);
		}
	}
	wiphy_unlock(wiphy);
	return 0;
}

/* SER (rtw89 ser.c, level 2) asks for a full restart after a firmware/MAC error. rtw89 has already stopped and
   reset itself under wiphy->mtx; we queue the work that drops the connection and brings the mac layer back to a
   started, idle, scannable state (iface.c mac_restart_work). mac_hw.restarting is set now so the entry points and
   the deferred MLME work leave the reset driver alone until the restart is done. */
void linuxkpi_ieee80211_restart_hw (struct ieee80211_hw *hw)
{
	struct mac_hw *mh = mac_hw(hw);

	if (!mh->restart_wq) {
		/* not registered (no work queue to recover on): nothing would ever clear the flag */
		pr_err("mac: hardware restart requested before registration; ignored");
		return;
	}
	pr_err("mac: hardware restart requested (SER); recovering");
	WRITE_ONCE(mh->restarting, true);
	queue_work(mh->restart_wq, &mh->restart_work);
}
