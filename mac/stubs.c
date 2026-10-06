// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: backends that are not implemented. Each does the safe thing: the txq scheduler stays empty (tx.c
 * sends without it), and the chanctx emulation, which rtw89 does not use here, logs its first call. (Key iteration
 * is in key.c, the TX block ack session calls in agg.c, the real channel contexts in iface.c.)
 */
#include "mac.h"

/* ---- txq scheduling: not used (tx.c hands frames to ops->tx directly), so the txqs stay empty ---- */
void linuxkpi_ieee80211_txq_schedule_start (struct ieee80211_hw *hw, uint8_t ac)
{
}

struct ieee80211_txq *linuxkpi_ieee80211_next_txq (struct ieee80211_hw *hw, uint8_t ac)
{
	return NULL;
}

void linuxkpi_ieee80211_schedule_txq (struct ieee80211_hw *hw, struct ieee80211_txq *txq, bool withoutpkts)
{
}

struct sk_buff *linuxkpi_ieee80211_tx_dequeue (struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
	return NULL;
}

void linuxkpi_ieee80211_txq_get_depth (struct ieee80211_txq *txq, unsigned long *frame_cnt, unsigned long *byte_cnt)
{
	if (frame_cnt)
		*frame_cnt = 0;
	if (byte_cnt)
		*byte_cnt = 0;
}

/* The TX block ack session calls (start/stop_tx_ba_session, the cb_irqsafe variants) are in agg.c. */

/* ---- chanctx emulation: rtw89 only uses it with firmware lacking SCAN_OFFLOAD/BEACON_FILTER (not ours) ---- */
int ieee80211_emulate_add_chanctx (struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *conf)
{
	MAC_TODO();
	return -EOPNOTSUPP;
}

void ieee80211_emulate_remove_chanctx (struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *conf)
{
	MAC_TODO();
}

void ieee80211_emulate_change_chanctx (struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *conf, uint32_t changed)
{
	MAC_TODO();
}

int ieee80211_emulate_switch_vif_chanctx (struct ieee80211_hw *hw, struct ieee80211_vif_chanctx_switch *vifs, int n_vifs,
	enum ieee80211_chanctx_switch_mode mode)
{
	MAC_TODO();
	return -EOPNOTSUPP;
}
