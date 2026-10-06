// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win compat: reads rtw89's internal state for the diagnostics (IOCTL_RTW_RADIO, IOCTL_RTW_BTC). Our own file
 * that includes rtw89's headers (core.h, coex.h, ps.h) to reach the fields, so the rtw89 sources stay as they are; the
 * mac layer calls it with wiphy->mtx held and rtw89's device (hw->priv), like mac\iface.c calls rtw89 directly.
 */
#include "core.h"
#include "coex.h"
#include "ps.h"
#include <rtw/glue.h>

void rtw89info_radio (void *drv, struct rtw_radio_info *info);
long rtw89info_btc_dump (void *drv, char *buf, size_t len);

_Static_assert(BTC_NCNT_NUM <= RTW_BTC_NOTIFY_MAX, "rtw_radio_info.btc_notify too small");

/* power save, rtw89's TX power regulation and the btc summary; wiphy->mtx held */
void rtw89info_radio (void *drv, struct rtw_radio_info *info)
{
	struct rtw89_dev *rtwdev = drv;
	const struct rtw89_regd *regd = rtwdev->regulatory.regd;
	struct rtw89_btc *btc = &rtwdev->btc;
	struct rtw89_btc_bt_info *bt = &btc->cx.bt;
	struct rtw89_btc_bt_link_info *bl = &bt->link_info;
	struct rtw89_btc_dm *dm = &btc->dm;
	int i;

	lockdep_assert_wiphy(rtwdev->hw->wiphy);
	if (regd) {
		info->rtw_alpha2[0] = regd->alpha2[0];
		info->rtw_alpha2[1] = regd->alpha2[1];
		info->rtw_txpwr_regd[0] = regd->txpwr_regd[RTW89_BAND_2G];
		info->rtw_txpwr_regd[1] = regd->txpwr_regd[RTW89_BAND_5G];
	}
	info->lps_enabled = rtwdev->lps_enabled;
	info->ps_mode = (u8)rtwdev->ps_mode;
	info->rtw_flags = (test_bit(RTW89_FLAG_POWERON, rtwdev->flags) ? RTW_RF_POWERON : 0) |
			  (test_bit(RTW89_FLAG_LEISURE_PS, rtwdev->flags) ? RTW_RF_LEISURE_PS : 0) |
			  (test_bit(RTW89_FLAG_LOW_POWER_MODE, rtwdev->flags) ? RTW_RF_LOW_POWER_MODE : 0) |
			  (test_bit(RTW89_FLAG_INACTIVE_PS, rtwdev->flags) ? RTW_RF_INACTIVE_PS : 0) |
			  (test_bit(RTW89_FLAG_RUNNING, rtwdev->flags) ? RTW_RF_RUNNING : 0);

	info->btc_lps = btc->lps;
	info->bt_enable = bt->enable.now;
	info->bt_profile_cnt = bl->profile_cnt.now;
	info->bt_profiles = (bl->hfp_desc.exist ? RTW_BT_PROFILE_HFP : 0) | (bl->hid_desc.exist ? RTW_BT_PROFILE_HID : 0) |
			    (bl->a2dp_desc.exist ? RTW_BT_PROFILE_A2DP : 0) | (bl->pan_desc.exist ? RTW_BT_PROFILE_PAN : 0);
	info->bt_fw_ver = bt->ver_info.fw;
	info->bt_scbd = bt->scbd;
	info->btc_policy_type = btc->policy_type;
	info->btc_run_reason = dm->run_reason;
	info->btc_run_action = dm->run_action;
	memcpy(info->btc_tdma, &dm->tdma_now, min(sizeof(info->btc_tdma), sizeof(dm->tdma_now)));
	if (btc->ver && btc->ver->fcxinit == 7) {
		info->ant_type = btc->mdinfo.md_v7.ant.type;
		info->ant_num = btc->mdinfo.md_v7.ant.num;
		info->ant_isolation = btc->mdinfo.md_v7.ant.isolation;
		info->ant_single_pos = btc->mdinfo.md_v7.ant.single_pos;
		info->ant_btg_pos = btc->mdinfo.md_v7.ant.btg_pos;
	} else {
		info->ant_type = btc->mdinfo.md.ant.type;
		info->ant_num = btc->mdinfo.md.ant.num;
		info->ant_isolation = btc->mdinfo.md.ant.isolation;
		info->ant_single_pos = btc->mdinfo.md.ant.single_pos;
		info->ant_btg_pos = btc->mdinfo.md.ant.btg_pos;
	}
	info->wl_only = dm->wl_only;
	info->bt_only = dm->bt_only;
	info->freerun = dm->freerun;
	info->btc_c2h = btc->fwinfo.cnt_c2h;
	info->btc_h2c = btc->fwinfo.cnt_h2c;
	info->btc_h2c_fail = btc->fwinfo.cnt_h2c_fail;
	info->btc_fw_reports = btc->fwinfo.event[BTF_EVNT_RPT];
	info->bt_info_updates = btc->cx.cnt_bt[BTC_BCNT_INFOUPDATE];
	info->bt_scbd_updates = btc->cx.cnt_bt[BTC_BCNT_SCBDUPDATE];
	info->btc_runs = dm->cnt_dm[BTC_DCNT_RUN];
	for (i = 0; i < BTC_NCNT_NUM; i++)
		info->btc_notify[i] = dm->cnt_notify[i];
}

/* rtw89_btc_dump_info() (Linux debugfs btc_info): it enables firmware reports (H2C) and reads MAC registers, so the
   chip must be powered and out of its deep power save; wiphy->mtx held. Bytes written, or -ENETDOWN. */
long rtw89info_btc_dump (void *drv, char *buf, size_t len)
{
	struct rtw89_dev *rtwdev = drv;

	lockdep_assert_wiphy(rtwdev->hw->wiphy);
	if (!len)
		return 0;
	if (!test_bit(RTW89_FLAG_POWERON, rtwdev->flags) || !test_bit(RTW89_FLAG_RUNNING, rtwdev->flags))
		return -ENETDOWN;
	rtw89_leave_ps_mode(rtwdev);
	return (long)rtw89_btc_dump_info(rtwdev, buf, len);
}
