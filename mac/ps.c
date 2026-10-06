// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: 802.11 power save of the station, mac80211's part of it: vif->cfg.ps and
 * BSS_CHANGED_PS (mlme.c ieee80211_recalc_ps, ieee80211_powersave_allowed). rtw89 (SUPPORTS_PS, SUPPORTS_DYNAMIC_PS)
 * does the rest: while cfg.ps is on, its track works put the chip into leisure power save (LPS) when the traffic is
 * low and take it out when the traffic grows, for a scan, and around its own operations (rtw89_leave_ps_mode).
 * Power save is allowed when the policy wants it, the connection is up with the AP's station AUTHORIZED (after the
 * 4-way handshake: none during EAPOL), the DTIM period is known (the BSS body was a beacon, or a beacon of the BSS
 * came; mac80211 have_beacon), and the AP is not being probed after a beacon loss (CONNECTION_POLL). It goes off
 * before the connection is taken apart (mac80211 ieee80211_set_disassoc: the deauthentication must not go out from
 * power save) and, for rtw89's lps_enabled, once on the new interface after a hardware restart.
 * The policy (rtw_linux_set_ps_policy), kept for the driver's lifetime:
 *   Parameters\PowerSave   0 never, 1 follow the OS (default), 2 always
 *   following the OS       off for the WDI connection quality hints low latency, high throughput and no power save;
 *                          otherwise the power plan's wireless adapter power saving mode: on for low, medium and
 *                          maximum power saving, off for maximum performance or while it is not known
 * Also here: the radio diagnostics (IOCTL_RTW_RADIO, IOCTL_RTW_BTC; rtw89's fields are read in compat\rtw89info.c).
 */
#include "mac.h"

_Static_assert(sizeof(struct rtw_radio_info) == 200, "rtw_radio_info layout");
_Static_assert(offsetof(struct rtw_radio_info, reg_changes) == 24, "rtw_radio_info layout");
_Static_assert(offsetof(struct rtw_radio_info, ps_changes) == 36, "rtw_radio_info layout");
_Static_assert(offsetof(struct rtw_radio_info, bt_fw_ver) == 48, "rtw_radio_info layout");
_Static_assert(offsetof(struct rtw_radio_info, btc_tdma) == 60, "rtw_radio_info layout");
_Static_assert(offsetof(struct rtw_radio_info, btc_c2h) == 76, "rtw_radio_info layout");
_Static_assert(offsetof(struct rtw_radio_info, btc_notify) == 104, "rtw_radio_info layout");

#define PS_HINT_AUTO                    1       /* WDI_CONNECTION_QUALITY_HINT */
#define PS_HINT_NO_POWER_SAVE           4

/* the policy's sources (written by rtw_linux_set_ps_policy, read anywhere) */
static u8 ps_reg = RTW_PS_REG_FOLLOW_OS;
static s8 ps_plan = -1;
static u8 ps_hint;
/* with wiphy->mtx held */
static u32 ps_changes;
static u64 ps_total_ns;                 /* cfg.ps on, before the stretch that began at mac_hw.ps_on_ns */

static bool ps_policy (void)
{
	u8 reg = READ_ONCE(ps_reg), hint = READ_ONCE(ps_hint);

	if (reg == RTW_PS_REG_NEVER)
		return false;
	if (reg == RTW_PS_REG_ALWAYS)
		return true;
	if (hint > PS_HINT_AUTO && hint <= PS_HINT_NO_POWER_SAVE)
		return false;
	return READ_ONCE(ps_plan) >= 1;
}

/* vif->cfg.ps and BSS_CHANGED_PS (rtw89_recalc_lps) */
static void ps_set (struct mac_hw *mh, bool on, const char *why)
{
	struct ieee80211_vif *vif = &mh->sta_vif->vif;
	u64 now = ktime_get_ns();

	vif->cfg.ps = on;
	if (on) {
		mh->ps_on_ns = now ? now : 1;
	} else if (mh->ps_on_ns) {
		ps_total_ns += now - mh->ps_on_ns;
		mh->ps_on_ns = 0;
	}
	ps_changes++;
	mh->ops->vif_cfg_changed(&mh->hw, vif, BSS_CHANGED_PS);
	pr_info("mac: power save %s (%s)", on ? "on" : "off", why);
}

/* ieee80211_recalc_ps(); wiphy->mtx held */
void mac_ps_recalc (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	const char *why = "connected, authorized";
	bool allowed = false;

	lockdep_assert_wiphy(mh->hw.wiphy);
	if (READ_ONCE(mh->restarting) || !mh->started || !mh->sta_vif)
		return;
	if (!ps_policy())
		why = "policy";
	else if (READ_ONCE(c->state) != RTW_CONN_CONNECTED || !c->ap)
		why = "not connected";
	else if (c->ap->state != IEEE80211_STA_AUTHORIZED)
		why = "not authorized";
	else if (!c->bss.tim && !test_bit(MAC_CONN_F_HAVE_BEACON, &c->flags))
		why = "no beacon yet";
	else if (c->probe_tries)
		why = "probing the AP";
	else
		allowed = true;
	if (allowed != mh->sta_vif->vif.cfg.ps)
		ps_set(mh, allowed, why);
}

/* Off before the connection is taken apart or the AP probed; wiphy->mtx held. Not while rtw89 is being reset. */
void mac_ps_leave (struct mac_hw *mh, const char *why)
{
	lockdep_assert_wiphy(mh->hw.wiphy);
	if (!READ_ONCE(mh->restarting) && mh->started && mh->sta_vif && mh->sta_vif->vif.cfg.ps)
		ps_set(mh, false, why);
}

/* A hardware restart brought up a new interface (iface.c, wiphy->mtx held): rtw89 keeps lps_enabled over its reset
   and changes it only in rtw89_recalc_lps, so it is told once that this interface does not save power. */
void mac_ps_restart (struct mac_hw *mh)
{
	lockdep_assert_wiphy(mh->hw.wiphy);
	if (mh->ps_on_ns) {
		ps_total_ns += ktime_get_ns() - mh->ps_on_ns;
		mh->ps_on_ns = 0;
	}
	mh->sta_vif->vif.cfg.ps = false;
	mh->ops->vif_cfg_changed(&mh->hw, &mh->sta_vif->vif, BSS_CHANGED_PS);
}

/* A beacon of the BSS (rx.c, NAPI thread): the first one may allow power save (mac80211 have_beacon) */
void mac_ps_beacon (struct mac_hw *mh)
{
	if (!test_bit(MAC_CONN_F_HAVE_BEACON, &mh->conn.flags) &&
	    !test_and_set_bit(MAC_CONN_F_HAVE_BEACON, &mh->conn.flags))
		wiphy_work_queue(mh->hw.wiphy, &mh->ps_work);
}

static void ps_work (struct wiphy *wiphy, struct wiphy_work *work)
{
	(void)wiphy;
	mac_ps_recalc(container_of(work, struct mac_hw, ps_work));
}

void mac_ps_init (struct mac_hw *mh)
{
	wiphy_work_init(&mh->ps_work, ps_work);
}

/* ---- entry points (rtw/glue.h) ---- */
void rtw_linux_set_ps_policy (uint32_t src, uint32_t value)
{
	static const char * const names[3] = { "Parameters\\PowerSave", "power plan", "connection quality hint" };
	struct mac_hw *mh = mac_the_hw;
	bool old = ps_policy();
	bool changed;

	switch (src) {
	case RTW_PS_SRC_REG:
		if (value > RTW_PS_REG_ALWAYS) {
			pr_warn("mac: Parameters\\PowerSave %u is not 0, 1 or 2; following the OS", value);
			value = RTW_PS_REG_FOLLOW_OS;
		}
		changed = READ_ONCE(ps_reg) != value;
		WRITE_ONCE(ps_reg, (u8)value);
		break;
	case RTW_PS_SRC_PLAN:
		value = min_t(u32, value, 3);
		changed = READ_ONCE(ps_plan) != (s8)value;
		WRITE_ONCE(ps_plan, (s8)value);
		break;
	case RTW_PS_SRC_HINT:
		if (value < PS_HINT_AUTO || value > PS_HINT_NO_POWER_SAVE)
			return;
		changed = READ_ONCE(ps_hint) != value;
		WRITE_ONCE(ps_hint, (u8)value);
		break;
	default:
		return;
	}
	if (changed)
		pr_info("mac: power save policy: %s %u; power save %s%s", names[src], value,
			ps_policy() ? "wanted" : "not wanted", ps_policy() != old ? " now" : "");
	if (mh && ps_policy() != old)
		wiphy_work_queue(mh->hw.wiphy, &mh->ps_work);
}

void rtw_linux_radio_info (struct rtw_radio_info *info)
{
	struct mac_hw *mh = mac_the_hw;
	u64 ns;

	memset(info, 0, sizeof(*info));
	info->ps_reg = READ_ONCE(ps_reg);
	info->ps_plan = READ_ONCE(ps_plan);
	info->ps_hint = READ_ONCE(ps_hint);
	info->ps_policy = ps_policy();
	if (!mh)
		return;
	wiphy_lock(mh->hw.wiphy);
	info->probed = 1;
	info->ps_changes = ps_changes;
	ns = ps_total_ns + (mh->ps_on_ns ? ktime_get_ns() - mh->ps_on_ns : 0);
	info->ps_ms = (u32)min_t(u64, ns / NSEC_PER_MSEC, U32_MAX);
	info->ps_cfg = mh->sta_vif && mh->sta_vif->vif.cfg.ps;
	if (mh->sta_vif && READ_ONCE(mh->conn.state) == RTW_CONN_CONNECTED)
		info->beacon_int = mh->sta_vif->vif.bss_conf.beacon_int;
	mac_reg_info(mh, info);
	rtw89info_radio(mh->hw.priv, info);
	wiphy_unlock(mh->hw.wiphy);
}

int32_t rtw_linux_btc_dump (char *buf, uint32_t len)
{
	struct mac_hw *mh = mac_the_hw;
	long ret;

	if (!mh)
		return -ENODEV;
	if (!mac_lock_driver(mh))
		ret = -EBUSY;           /* rtw89 is being reset */
	else
		ret = rtw89info_btc_dump(mh->hw.priv, buf, len);
	wiphy_unlock(mh->hw.wiphy);
	return (int32_t)ret;
}
