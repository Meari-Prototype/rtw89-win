// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: interfaces, channel contexts, the RX filter and idle handling, in the order mac80211 uses them
 * (iface.c ieee80211_do_open/do_stop, chan.c ieee80211_add_chanctx/assign_link_chanctx/free_chanctx, main.c
 * ieee80211_configure_filter/hw_conf_init). There is one station interface and at most one channel context, used
 * for listening on a fixed channel (the diagnostic IOCTLs, through rtw_linux_start/stop/monitor in rtw/glue.h) or by
 * a connection (mac_chanctx_use/release, mlme.c); stopping leaves the BSS first.
 * Driver ops are called with wiphy->mtx held, like mac80211. The interface/station iterators for the driver are
 * here as well.
 * The GO's interface (ap.c) shares that channel context with the station on the same channel (users counted; the
 * last user removes it); stopping and a hardware restart take the GO first.
 */
#include "mac.h"

#ifndef IEEE80211_INVAL_HW_QUEUE
#define IEEE80211_INVAL_HW_QUEUE        0xff
#endif

u32 mac_state;
u32 mac_listen_channel;

void mac_state_set (u32 bits, bool on)
{
	if (on)
		__atomic_fetch_or(&mac_state, bits, __ATOMIC_SEQ_CST);
	else
		__atomic_fetch_and(&mac_state, ~bits, __ATOMIC_SEQ_CST);
}

/* Waits until no lock-free reader can still see an entry unlinked before the call. PASSIVE_LEVEL, not inside a
   read-side section. */
void mac_sync_readers (struct mac_hw *mh)
{
	(void)mh;
	synchronize_rcu();
}

static int drv_config (struct mac_hw *mh, u32 changed)
{
	lockdep_assert_wiphy(mh->hw.wiphy);
	return mh->ops->config(&mh->hw, -1, changed);
}

/*
 * mac80211 __ieee80211_recalc_idle(): the device is busy while a channel context exists. A hw scan does not
 * count (mac80211 only counts software scans); rtw89 leaves inactive power save for it by itself and queues its
 * ips_work when the scan ends. Returns IEEE80211_CONF_CHANGE_IDLE when the state flips.
 */
static u32 recalc_idle (struct mac_hw *mh, bool force_active)
{
	struct ieee80211_hw *hw = &mh->hw;

	if (force_active || !list_empty(&mh->chanctx_list)) {
		if (!(hw->conf.flags & IEEE80211_CONF_IDLE))
			return 0;
		hw->conf.flags &= ~IEEE80211_CONF_IDLE;
		return IEEE80211_CONF_CHANGE_IDLE;
	}
	if (hw->conf.flags & IEEE80211_CONF_IDLE)
		return 0;
	/* __ieee80211_idle_on(): flush the TX queues first */
	if (mh->ops->flush)
		mh->ops->flush(hw, NULL, (u32)(BIT(hw->queues) - 1), false);
	hw->conf.flags |= IEEE80211_CONF_IDLE;
	return IEEE80211_CONF_CHANGE_IDLE;
}

void mac_recalc_idle (struct mac_hw *mh)
{
	u32 changed = recalc_idle(mh, false);

	if (changed)
		drv_config(mh, changed);
}

/* main.c ieee80211_configure_filter(): listening needs beacons and probe responses of every BSS */
void mac_configure_filter (struct mac_hw *mh)
{
	unsigned int new_flags = 0;
	unsigned int changed;

	lockdep_assert_wiphy(mh->hw.wiphy);
	if (mh->listening)
		new_flags |= FIF_BCN_PRBRESP_PROMISC | FIF_OTHER_BSS;
	/* an AP interface takes probe requests (iface.c ieee80211_do_open: fif_probe_req) */
	if (mh->ap.vif)
		new_flags |= FIF_PROBE_REQ;
	changed = mh->filter_flags ^ new_flags;
	new_flags |= 1U << 31;          /* like mac80211: the driver must clear flags it does not know */
	mh->ops->configure_filter(&mh->hw, changed, &new_flags, 0);
	WARN_ON(new_flags & (1U << 31));
	mh->filter_flags = new_flags & ~(1U << 31);
}

/* ---- the station interface (and the GO's, ap.c) ---- */
struct mac_vif *mac_vif_alloc (struct mac_hw *mh, enum nl80211_iftype type, const u8 *addr)
{
	struct mac_vif *mv = kzalloc(sizeof(*mv) + mh->hw.vif_data_size, GFP_KERNEL);
	struct ieee80211_vif *vif;
	int ac;

	if (!mv)
		return NULL;
	INIT_LIST_HEAD(&mv->list);
	mv->mh = mh;
	vif = &mv->vif;
	vif->type = type;
	ether_addr_copy(vif->addr, addr);
	vif->cfg.idle = true;
	/* non-MLD: link 0 is bss_conf itself */
	vif->bss_conf.vif = vif;
	vif->bss_conf.bssid = mv->bssid;
	ether_addr_copy(vif->bss_conf.addr, addr);
	vif->bss_conf.link_id = 0;
	vif->bss_conf.beacon_int = 100;
	vif->link_conf[0] = &vif->bss_conf;
	/* ieee80211_set_default_queues() */
	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
		vif->hw_queue[ac] = mh->hw.queues >= IEEE80211_NUM_ACS ? ac : 0;
	vif->cab_queue = IEEE80211_INVAL_HW_QUEUE;
	return mv;
}

/* iface.c ieee80211_do_open() for the first interface */
static int start_locked (struct mac_hw *mh)
{
	struct ieee80211_hw *hw = &mh->hw;
	struct mac_vif *mv;
	int ret;

	mv = mac_vif_alloc(mh, NL80211_IFTYPE_STATION, hw->wiphy->perm_addr);
	if (!mv)
		return -ENOMEM;
	ret = mh->ops->start(hw);
	if (ret) {
		pr_err("mac: drv start failed: %d", ret);
		kfree(mv);
		return ret;
	}
	mh->started = true;
	ret = mh->ops->add_interface(hw, &mv->vif);
	if (ret) {
		pr_err("mac: drv add_interface failed: %d", ret);
		mh->ops->stop(hw, false);
		mh->started = false;
		kfree(mv);
		return ret;
	}
	smp_wmb();
	list_add_tail_rcu(&mv->list, &mh->vif_list);
	mh->sta_vif = mv;
	mac_configure_filter(mh);
	/* ieee80211_hw_conf_init(); the device is idle, so rtw89 enters inactive power save (powers off) */
	drv_config(mh, ~(IEEE80211_CONF_CHANGE_CHANNEL | IEEE80211_CONF_CHANGE_POWER));
	return 0;
}

/* ---- channel contexts: chan.c, one context at a time, assigned to the station interface (listening or a
   connection) and to the GO's ---- */
/* an enabled channel of the band, NULL if none */
struct ieee80211_channel *mac_find_channel (struct wiphy *wiphy, u32 band, u32 number)
{
	struct ieee80211_supported_band *sb;
	int i;

	if (band >= NUM_NL80211_BANDS)
		return NULL;
	sb = wiphy->bands[band];
	for (i = 0; sb && i < sb->n_channels; i++)
		if (sb->channels[i].hw_value == number && !(sb->channels[i].flags & IEEE80211_CHAN_DISABLED))
			return &sb->channels[i];
	return NULL;
}

/* The one context there is, assigned to another interface as well (same channel only: rtw89 allows a station and
   an AP on one channel, no MCC). Only while the GO's interface exists; without it there is never a second user.
   The context keeps its width; the interface's chanreq is what it asked for (def), like mac80211, where the
   context combines the interfaces' requests (the station decides itself whether it can use the context's width,
   mlme.c; the GO stays at 20 MHz in it). rtw89 reads only chanreq.oper.chan. */
static int chanctx_share (struct mac_hw *mh, struct mac_vif *mv, const struct cfg80211_chan_def *def,
	struct mac_chanctx *ctx)
{
	struct ieee80211_hw *hw = &mh->hw;
	struct ieee80211_vif *vif = &mv->vif;
	struct ieee80211_channel *chan = def->chan;
	int ret;

	if (!mh->ap.vif || ctx->users <= 0 || ctx->conf.def.chan != chan || vif->bss_conf.chanctx_conf)
		return -EBUSY;
	vif->bss_conf.chanreq.oper = *def;
	ret = mh->ops->assign_vif_chanctx(hw, vif, &vif->bss_conf, &ctx->conf);
	if (ret) {
		pr_err("mac: drv assign_vif_chanctx (shared) failed: %d", ret);
		memset(&vif->bss_conf.chanreq, 0, sizeof(vif->bss_conf.chanreq));
		return ret;
	}
	rcu_assign_pointer(vif->bss_conf.chanctx_conf, &ctx->conf);
	ctx->users++;
	vif->cfg.idle = false;
	if (mh->ops->vif_cfg_changed)
		mh->ops->vif_cfg_changed(hw, vif, BSS_CHANGED_IDLE);
	pr_info("mac: channel %u shared by %u interfaces (context %u MHz, this interface %u MHz)", chan->hw_value,
		ctx->users, cfg80211_chandef_get_width(&ctx->conf.def), cfg80211_chandef_get_width(def));
	return 0;
}

/* ieee80211_link_use_channel(): a new context on chan (20 MHz; type NO_HT or HT20) assigned to the interface (the
   station's or the GO's), with the device out of idle. If a context exists it is shared on the same channel
   (chanctx_share). With wiphy->mtx held. */
int mac_chanctx_use (struct mac_hw *mh, struct mac_vif *mv, struct ieee80211_channel *chan,
	enum nl80211_channel_type type, struct mac_chanctx **out)
{
	struct cfg80211_chan_def def;

	cfg80211_chandef_create(&def, chan, type);
	return mac_chanctx_use_def(mh, mv, &def, out);
}

/* The same with any chandef (the station's 40/80 MHz, mlme.c); rtw89 sets the radio up from def.width and
   def.center_freq1 (add_chanctx). */
int mac_chanctx_use_def (struct mac_hw *mh, struct mac_vif *mv, const struct cfg80211_chan_def *def,
	struct mac_chanctx **out)
{
	struct ieee80211_hw *hw = &mh->hw;
	struct ieee80211_vif *vif = &mv->vif;
	struct mac_chanctx *ctx;
	u32 changed;
	int ret;

	lockdep_assert_wiphy(hw->wiphy);
	*out = NULL;
	if (mh->ops->add_chanctx == ieee80211_emulate_add_chanctx || !mh->ops->assign_vif_chanctx)
		return -EOPNOTSUPP;
	if (!list_empty(&mh->chanctx_list)) {
		ctx = list_first_entry(&mh->chanctx_list, struct mac_chanctx, list);
		ret = chanctx_share(mh, mv, def, ctx);
		if (!ret)
			*out = ctx;
		return ret;
	}
	ctx = kzalloc(sizeof(*ctx) + hw->chanctx_data_size, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	INIT_LIST_HEAD(&ctx->list);
	ctx->users = 1;
	ctx->conf.def = *def;
	ctx->conf.min_def = ctx->conf.def;
	ctx->conf.rx_chains_static = 1;
	ctx->conf.rx_chains_dynamic = 1;
	vif->bss_conf.chanreq.oper = ctx->conf.def;

	/* ieee80211_add_chanctx(): idle off before the driver sees the context (rtw89 leaves IPS here) */
	changed = recalc_idle(mh, true);
	if (changed)
		drv_config(mh, changed);
	ret = mh->ops->add_chanctx(hw, &ctx->conf);
	if (ret) {
		pr_err("mac: drv add_chanctx failed: %d", ret);
		goto err_idle;
	}
	smp_wmb();
	list_add_tail_rcu(&ctx->list, &mh->chanctx_list);

	/* ieee80211_assign_link_chanctx() */
	ret = mh->ops->assign_vif_chanctx(hw, vif, &vif->bss_conf, &ctx->conf);
	if (ret) {
		pr_err("mac: drv assign_vif_chanctx failed: %d", ret);
		list_del_rcu(&ctx->list);
		mh->ops->remove_chanctx(hw, &ctx->conf);
		goto err_idle;
	}
	rcu_assign_pointer(vif->bss_conf.chanctx_conf, &ctx->conf);
	vif->cfg.idle = false;
	if (mh->ops->vif_cfg_changed)
		mh->ops->vif_cfg_changed(hw, vif, BSS_CHANGED_IDLE);
	*out = ctx;
	return 0;

err_idle:
	memset(&vif->bss_conf.chanreq, 0, sizeof(vif->bss_conf.chanreq));
	mac_recalc_idle(mh);
	mac_sync_readers(mh);
	kfree(ctx);
	return ret;
}

/* ieee80211_link_release_channel(): undoes mac_chanctx_use() for the interface. Returns true when that was the
   context's last user (false while the other interface still uses it): it is gone from the driver then, and the
   caller frees ctx after mac_sync_readers(). */
bool mac_chanctx_release (struct mac_hw *mh, struct mac_vif *mv, struct mac_chanctx *ctx)
{
	struct ieee80211_hw *hw = &mh->hw;
	struct ieee80211_vif *vif = &mv->vif;

	lockdep_assert_wiphy(hw->wiphy);
	mh->ops->unassign_vif_chanctx(hw, vif, &vif->bss_conf, &ctx->conf);
	rcu_assign_pointer(vif->bss_conf.chanctx_conf, NULL);
	memset(&vif->bss_conf.chanreq, 0, sizeof(vif->bss_conf.chanreq));
	vif->cfg.idle = true;
	if (mh->ops->vif_cfg_changed)
		mh->ops->vif_cfg_changed(hw, vif, BSS_CHANGED_IDLE);
	if (--ctx->users > 0)
		return false;

	/* ieee80211_free_chanctx(): unlink, remove from the driver, then idle again (rtw89 enters IPS) */
	list_del_rcu(&ctx->list);
	mh->ops->remove_chanctx(hw, &ctx->conf);
	mac_recalc_idle(mh);
	return true;
}

/* ---- listening on one channel (rtw_linux_monitor): a context like a monitor interface's ---- */
static int listen_locked (struct mac_hw *mh, struct ieee80211_channel *chan)
{
	struct mac_chanctx *ctx;
	int ret;

	ret = mac_chanctx_use(mh, mh->sta_vif, chan, NL80211_CHAN_HT20, &ctx);
	if (ret)
		return ret;
	mh->listen_ctx = ctx;
	mh->listening = true;
	mac_configure_filter(mh);
	return 0;
}

/* Undoes listen_locked(); returns the context for the caller to free after mac_sync_readers(). */
static struct mac_chanctx *unlisten_locked (struct mac_hw *mh)
{
	struct mac_chanctx *ctx = mh->listen_ctx;

	if (!ctx)
		return NULL;
	mh->listening = false;
	mac_configure_filter(mh);
	mh->listen_ctx = NULL;
	return mac_chanctx_release(mh, mh->sta_vif, ctx) ? ctx : NULL;
}

/* iface.c ieee80211_do_stop() for the last interface; returns the interface to free after mac_sync_readers() */
static struct mac_vif *stop_locked (struct mac_hw *mh, struct mac_chanctx **ctx)
{
	struct ieee80211_hw *hw = &mh->hw;
	struct mac_vif *mv = mh->sta_vif;

	/* the GO first (its clients, keys and interface), then like cfg80211 before the interface goes down: leave
	   the BSS (deauthentication, no event) */
	mac_ap_stop_locked(mh, true);
	mac_mlme_stop(mh);
	mac_scan_cancel(mh);
	*ctx = unlisten_locked(mh);
	if (*ctx)
		mac_bss_listen_done(READ_ONCE(mac_listen_channel));
	if (mv) {
		/* drv_remove_interface(): out of the iterators' sight before the driver takes it apart */
		list_del_rcu(&mv->list);
		mac_sync_readers(mh);
		mh->ops->remove_interface(hw, &mv->vif);
		mh->sta_vif = NULL;
	}
	mh->ops->stop(hw, false);
	mh->started = false;
	mh->filter_flags = 0;
	hw->conf.flags |= IEEE80211_CONF_IDLE;
	return mv;
}

/* Stops the hw if it is started. PASSIVE_LEVEL, without wiphy->mtx. */
void mac_hw_stop (struct mac_hw *mh)
{
	struct mac_chanctx *ctx = NULL;
	struct mac_vif *mv;

	(void)mac_lock_driver(mh);      /* after a restart in progress if possible; stopping goes on regardless */
	if (!mh->started) {
		wiphy_unlock(mh->hw.wiphy);
		return;
	}
	mv = stop_locked(mh, &ctx);
	wiphy_unlock(mh->hw.wiphy);
	/* like ieee80211_stop_device(): nothing of the stopped device may still run on the hw work queue */
	flush_workqueue(mh->wq);
	mac_sync_readers(mh);
	kfree(ctx);
	kfree(mv);
	mac_tx_reset(mh);               /* the driver gave back or dropped every frame when it stopped */
	mac_rx_flush(mh);
	mac_parked_free(mh);            /* free what any hardware restart kept (the driver is stopped now) */
	mac_state_set(RTW_STATE_STARTED | RTW_STATE_LISTENING | RTW_STATE_SCANNING, false);
	WRITE_ONCE(mac_listen_channel, 0);
	pr_info("mac: stopped");
}

/* ---- entry points for the driver (rtw/glue.h); the caller serializes them ---- */
int rtw_linux_start (void)
{
	struct mac_hw *mh = mac_the_hw;
	int ret;

	if (!mh)
		return -ENODEV;
	ret = !mac_lock_driver(mh) || mh->started ? -EBUSY : start_locked(mh);
	wiphy_unlock(mh->hw.wiphy);
	if (ret)
		return ret;
	mac_state_set(RTW_STATE_STARTED, true);
	pr_info("mac: started, station interface %pM, idle", mh->hw.wiphy->perm_addr);
	return 0;
}

void rtw_linux_stop (void)
{
	if (mac_the_hw)
		mac_hw_stop(mac_the_hw);
}

int rtw_linux_monitor (uint32_t channel)
{
	struct mac_hw *mh = mac_the_hw;
	struct ieee80211_channel *chan = NULL;
	struct mac_chanctx *old = NULL;
	u32 old_channel;
	int ret = 0;

	if (!mh)
		return -ENODEV;
	if (!mac_lock_driver(mh))
		ret = -EBUSY;
	if (!ret && channel) {
		chan = mac_find_channel(mh->hw.wiphy, channel <= 14 ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ, channel);
		if (!chan)
			ret = -EINVAL;
	}
	if (!ret && !mh->started)
		ret = channel ? -ENOTCONN : 0;
	else if (!ret && (mh->scanning || READ_ONCE(mh->conn.state) != RTW_CONN_IDLE || mh->ap.vif))
		ret = -EBUSY;   /* not while the GO runs either */
	if (ret || !mh->started) {
		wiphy_unlock(mh->hw.wiphy);
		return ret;
	}
	old_channel = READ_ONCE(mac_listen_channel);
	old = unlisten_locked(mh);
	WRITE_ONCE(mac_listen_channel, 0);
	mac_state_set(RTW_STATE_LISTENING, false);
	if (old)
		mac_bss_listen_done(old_channel);       /* before a new session clears the counters */
	if (chan) {
		mac_bss_clear();
		ret = listen_locked(mh, chan);
		if (!ret) {
			WRITE_ONCE(mac_listen_channel, channel);
			mac_state_set(RTW_STATE_LISTENING, true);
		}
	}
	wiphy_unlock(mh->hw.wiphy);
	if (old) {
		mac_sync_readers(mh);
		kfree(old);
	}
	if (chan && !ret)
		pr_info("mac: listening on channel %u (%u MHz, flags %#x)", channel, chan->center_freq, chan->flags);
	return ret;
}

/* ---- hardware restart (rtw89 SER level 2) ---- */
/* rtw89's firmware CPU-exception trigger, for the diagnostic IOCTL_RTW_SER (fw.c; linked into rtw89hal.lib). The
   mac layer passes rtw89's device, which is hw->priv. */
struct rtw89_dev;
extern int rtw89_fw_h2c_trigger_cpu_exception (struct rtw89_dev *rtwdev);

void mac_restart_init (struct mac_hw *mh);      /* forward (defined after mac_restart_work) */

/* Frees the stations, channel contexts and interfaces that a restart forgot but kept, because rtw89 might still
   have held a stale pointer to them while it was being reset. Safe once the driver is stopped. */
void mac_parked_free (struct mac_hw *mh)
{
	struct mac_sta *ms, *ms2;
	struct mac_chanctx *ctx, *ctx2;
	struct mac_vif *mv, *mv2;

	list_for_each_entry_safe(ms, ms2, &mh->parked_sta, list) {
		list_del(&ms->list);
		mac_sta_free(ms);
	}
	list_for_each_entry_safe(ctx, ctx2, &mh->parked_ctx, list) {
		list_del(&ctx->list);
		kfree(ctx);
	}
	list_for_each_entry_safe(mv, mv2, &mh->parked_vif, list) {
		list_del(&mv->list);
		kfree(mv);
	}
}

/* Waits (bounded) for a running restart to finish. PASSIVE_LEVEL, no wiphy->mtx. Returns false on timeout. */
bool mac_restart_wait (struct mac_hw *mh)
{
	int i;

	for (i = 0; i < 300 && READ_ONCE(mh->restarting); i++)
		msleep(10);
	return !READ_ONCE(mh->restarting);
}

/* An entry point that calls the driver: waits (bounded, without wiphy->mtx) for a hardware restart to finish, then
   takes wiphy->mtx. Returns false - with the lock held all the same - while a restart is still pending (it began
   meanwhile or outlasted the wait): the caller then leaves the reset driver alone (-EBUSY, or nothing to do).
   PASSIVE_LEVEL. */
bool mac_lock_driver (struct mac_hw *mh)
{
	mac_restart_wait(mh);
	wiphy_lock(mh->hw.wiphy);
	return !READ_ONCE(mh->restarting);
}

static void mac_restart_work (struct work_struct *w)
{
	struct mac_hw *mh = container_of(w, struct mac_hw, restart_work);
	struct mac_vif *old_vif;
	struct mac_chanctx *listen;
	int ret;

	wiphy_lock(mh->hw.wiphy);
	if (!mh->started) {
		/* stopped meanwhile (removal): nothing to bring back */
		WRITE_ONCE(mh->restarting, false);
		mac_reg_restart_done(mh);
		wiphy_unlock(mh->hw.wiphy);
		return;
	}
	mh->restarts++;
	pr_info("mac: hardware restart %u: recovering the driver", mh->restarts);

	/* like ieee80211_restart_work: finish anything the reset left in the air without touching the driver */
	mac_scan_abandon(mh);
	mac_ap_restart(mh);             /* the GO's clients, keys and interface parked (before the station's context) */
	mac_mlme_restart(mh);           /* drop the connection, park its station and channel context, report it lost */

	/* forget the old interface and any listening context (rtw89 reset their bindings), keeping the memory */
	listen = mh->listen_ctx;
	if (listen) {
		list_del_rcu(&listen->list);
		mh->listen_ctx = NULL;
		mh->listening = false;
		/* listening ends here (rtw_linux_monitor would say so): the state bit and channel for the readers */
		mac_bss_listen_done(READ_ONCE(mac_listen_channel));
		WRITE_ONCE(mac_listen_channel, 0);
		mac_state_set(RTW_STATE_LISTENING, false);
	}
	old_vif = mh->sta_vif;
	if (old_vif)
		list_del_rcu(&old_vif->list);
	mh->sta_vif = NULL;
	mh->started = false;            /* rtw89 stopped itself in the SER reset */
	mh->filter_flags = 0;
	mh->hw.conf.flags |= IEEE80211_CONF_IDLE;
	mac_tx_reset(mh);
	synchronize_rcu();
	if (listen)
		list_add_tail(&listen->list, &mh->parked_ctx);
	if (old_vif)
		list_add_tail(&old_vif->list, &mh->parked_vif);

	/* bring the driver back up to a started, idle, scannable state with a fresh interface (drv start +
	   add_interface, like the start of mac80211's ieee80211_reconfig) */
	ret = start_locked(mh);
	if (ret) {
		pr_err("mac: hardware restart %u: could not start the driver again: %d; staying down", mh->restarts, ret);
	} else {
		pr_info("mac: hardware restart %u: driver started, idle", mh->restarts);
		/* like ieee80211_reconfig's BSS_CHANGED_PS: rtw89's lps_enabled outlived its reset */
		mac_ps_restart(mh);
	}

	/* tell rtw89 the reconfiguration is done (rtw89_ser_recfg_done ends its SER) */
	if (mh->ops->reconfig_complete)
		mh->ops->reconfig_complete(&mh->hw, IEEE80211_RECONFIG_TYPE_RESTART);

	WRITE_ONCE(mh->restarting, false);
	if (ret)
		mac_state_set(RTW_STATE_STARTED, false);
	mac_reg_restart_done(mh);       /* a regulatory change that waited for the restart */
	mac_ap_restart_done(mh);        /* tell the front end the GO stopped (it may start again) */
	wiphy_unlock(mh->hw.wiphy);
}

void mac_restart_init (struct mac_hw *mh)
{
	INIT_LIST_HEAD(&mh->parked_sta);
	INIT_LIST_HEAD(&mh->parked_ctx);
	INIT_LIST_HEAD(&mh->parked_vif);
	INIT_WORK(&mh->restart_work, mac_restart_work);
}

/* IOCTL_RTW_SER (diagnostic): have the firmware raise a CPU exception so rtw89's SER runs the restart above. */
int rtw_linux_ser_trigger (void)
{
	struct mac_hw *mh = mac_the_hw;
	int ret;

	if (!mh)
		return -ENODEV;
	if (!mac_lock_driver(mh))
		ret = -EBUSY;
	else if (!mh->started || !mh->sta_vif ||
		 (!mh->sta_vif->vif.bss_conf.chanctx_conf && !(mh->ap.vif && mh->ap.vif->vif.bss_conf.chanctx_conf)))
		ret = -ENOTCONN;        /* the chip is powered off without a channel context (the GO's counts) */
	else if (mh->scanning)
		ret = -EBUSY;
	else
		ret = rtw89_fw_h2c_trigger_cpu_exception((struct rtw89_dev *)mh->hw.priv);
	wiphy_unlock(mh->hw.wiphy);
	if (!ret)
		pr_info("mac: SER trigger: firmware CPU exception requested");
	else
		pr_warn("mac: SER trigger: %d", ret);
	return ret;
}

/* ---- iteration for the driver ---- */
void linuxkpi_ieee80211_iterate_interfaces (struct ieee80211_hw *hw, enum ieee80211_iface_iter flags,
	void (*fn)(void *, uint8_t *, struct ieee80211_vif *), void *arg)
{
	struct mac_hw *mh = mac_hw(hw);
	struct mac_vif *mv;

	/* every interface on the list is in the driver and up, so ITER_ACTIVE selects them all */
	(void)flags;
	mac_read_lock(mh);
	list_for_each_entry_rcu(mv, &mh->vif_list, list)
		fn(arg, mv->vif.addr, &mv->vif);
	mac_read_unlock(mh);
}

/* A station is on the list before the driver knows it (mlme.c, like sta_info_insert()); mac80211's iterators and
   ieee80211_find_sta() skip such ones (sta->uploaded), so the driver never sees its zeroed private part. */
static bool sta_uploaded (const struct mac_sta *ms)
{
	return READ_ONCE(ms->state) != IEEE80211_STA_NOTEXIST;
}

void linuxkpi_ieee80211_iterate_stations_atomic (struct ieee80211_hw *hw,
	void (*fn)(void *, struct ieee80211_sta *), void *arg)
{
	struct mac_hw *mh = mac_hw(hw);
	struct mac_sta *ms;

	mac_read_lock(mh);
	list_for_each_entry_rcu(ms, &mh->sta_list, list)
		if (sta_uploaded(ms))
			fn(arg, &ms->sta);
	mac_read_unlock(mh);
}

/* for_each_station(), with wiphy->mtx held */
struct ieee80211_sta *__ieee80211_iterate_stations (struct ieee80211_hw *hw, struct ieee80211_sta *prev)
{
	struct mac_hw *mh = mac_hw(hw);
	struct mac_sta *ms;

	if (!prev)
		ms = list_first_entry_or_null(&mh->sta_list, struct mac_sta, list);
	else {
		ms = container_of(prev, struct mac_sta, sta);
		ms = list_entry_is_head(list_next_entry(ms, list), &mh->sta_list, list) ? NULL :
		     list_next_entry(ms, list);
	}
	while (ms && !sta_uploaded(ms))
		ms = list_entry_is_head(list_next_entry(ms, list), &mh->sta_list, list) ? NULL :
		     list_next_entry(ms, list);
	return ms ? &ms->sta : NULL;
}

/* The pointer stays valid while the caller holds wiphy->mtx (stations come and go only under it) or stays inside
   a read-side section (rcu_read_lock: they are freed only after list_del_rcu() and synchronize_rcu()). */
struct ieee80211_sta *linuxkpi_ieee80211_find_sta (struct ieee80211_vif *vif, const u8 *addr)
{
	struct mac_vif *mv = mac_vif(vif);
	struct mac_hw *mh = mv->mh;
	struct ieee80211_sta *found = NULL;
	struct mac_sta *ms;

	mac_read_lock(mh);
	list_for_each_entry_rcu(ms, &mh->sta_list, list) {
		if (ms->mv == mv && sta_uploaded(ms) && ether_addr_equal(ms->sta.addr, addr)) {
			found = &ms->sta;
			break;
		}
	}
	mac_read_unlock(mh);
	return found;
}
