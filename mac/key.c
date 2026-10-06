// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: keys and the privacy rules of the data path. mac80211's key.c and the CCMP parts
 * of wpa.c/tx.c/rx.c do this under Linux (read for reference only).
 *  - The OS runs the 4-way and group key handshakes over the data path and hands the keys over
 *    (WDI_SET_ADD_CIPHER_KEYS -> rtw_linux_add_key). Each becomes an ieee80211_key_conf given to rtw89's set_key,
 *    which puts it into its security CAM; the pairwise key with the AP's station, group keys without a station
 *    (both land in the interface's address CAM entry in station mode). Only CCMP-128: the 8852B has no TKIP.
 *  - TX: the 8852B does not make the security header (hw_sec_hdr = false, rtw89 sets GENERATE_IV), so tx.c puts the
 *    8-byte CCMP header (PN of the key, ExtIV, key ID) after the MAC header and sets the Protected bit; the hardware
 *    encrypts and appends the MIC. The PN comes from the key's atomic tx_pn.
 *  - RX: rtw89 only reports RX_FLAG_DECRYPTED (CCMP header and MIC stay in the frame), so rx.c takes both off after
 *    the key and the PN have been checked here (per key, per TID, non-QoS in a slot of its own).
 *  - Unprotected frames on an RSN connection, both ways, follow the OS's privacy exemption list
 *    (WDI_SET_PRIVACY_EXEMPTION_LIST, Native 802.11 semantics like the sample's N6MatchPrivacyExemptList): ALWAYS
 *    exempts the EtherType, ON_KEY_MAPPING_KEY_UNAVAILABLE only while there is no pairwise key, the rest must be
 *    protected (dot11ExcludeUnencrypted). A protected frame that decrypted and passed the PN check goes up whatever
 *    its EtherType (also an encrypted EAPOL frame of a group key update).
 * Keys change with wiphy->mtx held; the data path reads mac_conn.ptk/gtk[] in read-side sections, and a key is
 * freed only after it was unpublished, synchronize_rcu() and DISABLE_KEY. Key bytes are never logged.
 * The same for WPA3-SAE connections; with management frame protection also the integrity group keys (IGTK,
 * BIP-CMAC-128, IDs 4/5), which pmf.c checks group-addressed management frames with. They go to rtw89 as well (like
 * mac80211 does; rtw89 puts them in its BIP CAM slots), but nothing depends on it: one rtw89 refuses stays ours only.
 * The GO's keys live with ap.c (a pairwise key per client, the group keys); the exemption list is per port, and
 * SER's key iteration covers the GO's interface as well.
 */
#include "mac.h"

#define CCMP_HDR_LEN                    IEEE80211_CCMP_HDR_LEN
#define CCMP_KEY_LEN                    16
#define CCMP_EXT_IV                     0x20
#define PN_MAX                          0xffffffffffffULL
#define BIP_KEY_LEN                     16
#define MMIE_LEN                        18      /* element ID, length, key ID, IPN, MIC (BIP-CMAC-128) */

_Static_assert(sizeof(struct rtw_key) == 52, "rtw_key layout");
_Static_assert(sizeof(struct rtw_exemption) == 4, "rtw_exemption layout");

/* The privacy exemption list: a port setting that outlives connections and stop/start, so it lives with the driver
   (zeroed spin lock = unlocked, valid before rtw_linux_init). One per port: the station's (MAC_PORT_STA) and the
   GO's (MAC_PORT_AP, ap.c). */
struct mac_exempt {
	spinlock_t lock;
	struct rtw_exemption e[RTW_EXEMPT_MAX];
	u32 n;
};
static struct mac_exempt exempt_ports[2];

/* volatile stores: not optimised away before the memory is freed or goes out of scope */
static void key_wipe (void *p, size_t n)
{
	volatile u8 *v = p;

	while (n--)
		*v++ = 0;
}

/* ---- the exemption list ---- */
void mac_exempt_set (int port, const struct rtw_exemption *list, u32 n)
{
	struct mac_exempt *ex = &exempt_ports[port];
	const char *name = port == MAC_PORT_AP ? "GO " : "";
	u32 i;

	if (n > RTW_EXEMPT_MAX) {
		pr_warn("mac: %sprivacy exemption list: %u entries, keeping %u", name, n, RTW_EXEMPT_MAX);
		n = RTW_EXEMPT_MAX;
	}
	if (!list)
		n = 0;
	spin_lock_bh(&ex->lock);
	for (i = 0; i < n; i++)
		ex->e[i] = list[i];
	ex->n = n;
	spin_unlock_bh(&ex->lock);
	for (i = 0; i < n; i++)
		pr_info("mac: %sprivacy exemption %u: EtherType %#06x, action %u, packets %u", name, i, list[i].ethertype,
			list[i].action, list[i].packet_type);
	if (!n)
		pr_info("mac: %sprivacy exemption list empty (802.1X still passes while there is no pairwise key)", name);
}

void rtw_linux_set_exemptions (const struct rtw_exemption *list, uint32_t n)
{
	mac_exempt_set(MAC_PORT_STA, list, n);
}

/* The action of the first entry for this EtherType and destination kind, -1 if none. 802.1X without an entry
   counts as RTW_EXEMPT_NO_KEY (what the OS normally sets; without it no 4-way handshake could start, and after the
   pairwise key it changes nothing). Any IRQL <= DISPATCH_LEVEL. */
int mac_exempt_lookup (int port, u16 ethertype, bool group)
{
	struct mac_exempt *ex = &exempt_ports[port];
	int action = -1;
	u32 i;

	spin_lock_bh(&ex->lock);
	for (i = 0; i < ex->n; i++) {
		const struct rtw_exemption *e = &ex->e[i];

		if (e->ethertype != ethertype)
			continue;
		if ((e->packet_type == RTW_EXEMPT_UNICAST && group) ||
		    (e->packet_type == RTW_EXEMPT_MULTICAST && !group))
			continue;
		if (e->packet_type < RTW_EXEMPT_UNICAST || e->packet_type > RTW_EXEMPT_BOTH)
			continue;
		action = e->action;
		break;
	}
	spin_unlock_bh(&ex->lock);
	if (action < 0 && ethertype == ETH_P_PAE)
		action = RTW_EXEMPT_NO_KEY;
	return action;
}

static int exempt_lookup (u16 ethertype, bool group)
{
	return mac_exempt_lookup(MAC_PORT_STA, ethertype, group);
}

u8 mac_exempt_count (void)
{
	return (u8)READ_ONCE(exempt_ports[MAC_PORT_STA].n);
}

/* ---- the data path (read-side section held) ---- */
/* How a data frame for the AP goes out: *key = the key to protect it with, NULL = in clear. 0, or -EACCES when it
   has to be protected and there is no pairwise key (yet). */
int mac_key_tx (struct mac_hw *mh, u16 ethertype, bool group_da, u32 flags, struct mac_key **key)
{
	struct mac_conn *c = &mh->conn;
	struct mac_key *k;
	int action;

	*key = NULL;
	if (!mac_conn_rsn(c))
		return 0;
	/* the frame's own action (ms_packet_wifiexemptionaction); NO_EXEMPTION there (also what an extension nobody
	   filled in reads) still lets the list exempt the EtherType: the OS registered it, and with a pairwise key both
	   ways end in "protected" */
	action = (flags & RTW_TX_EXEMPT_VALID) ? (int)RTW_TX_EXEMPT_GET(flags) : -1;
	if (action <= RTW_EXEMPT_NONE || action > RTW_EXEMPT_NO_KEY)
		action = exempt_lookup(ethertype, group_da);
	k = rcu_dereference(c->ptk);
	if (k && READ_ONCE(k->lost))
		k = NULL;               /* rtw89 forgot it: nothing may go out "protected" by it */
	switch (action) {
	case RTW_EXEMPT_ALWAYS:
		return 0;
	case RTW_EXEMPT_NO_KEY:
		*key = k;
		return 0;
	default:
		*key = k;
		if (!k && (c->conn_flags & RTW_CONN_F_EXCLUDE_UNENCRYPTED))
			return -EACCES;
		return 0;
	}
}

/* The CCMP header (802.11-2020 12.5.3.2) of the next frame protected with k: PN0, PN1, reserved, ExtIV | key ID,
   PN2..PN5. Each call takes a new PN (atomic: frames of different queues may be built at the same time). */
void mac_key_ccmp_hdr (struct mac_key *k, u8 *p)
{
	u64 pn = (u64)atomic64_inc_return(&k->conf.tx_pn);

	p[0] = (u8)pn;
	p[1] = (u8)(pn >> 8);
	p[2] = 0;
	p[3] = CCMP_EXT_IV | (k->conf.keyidx << 6);
	p[4] = (u8)(pn >> 16);
	p[5] = (u8)(pn >> 24);
	p[6] = (u8)(pn >> 32);
	p[7] = (u8)(pn >> 40);
}

_Static_assert(MAC_SEQ_NONQOS + 1 == MAC_PN_MGMT && MAC_PN_MGMT + 1 == MAC_PN_SLOTS, "PN slots");

/* A protected data frame the hardware decrypted (ccmp = its CCMP header, slot = TID or MAC_SEQ_NONQOS; never
   MAC_PN_MGMT, which belongs to pmf.c under wiphy->mtx): the key
   of its key ID and the PN check (mac80211 ieee80211_crypto_ccmp_decrypt). 0 (the PN is recorded) or -1 (dropped
   and counted). Called from rx.c mac_rx_deliver(): a TID with a block ack session runs under mac_hw.rx_lock (the
   NAPI thread or the reorder timer), everything else on the NAPI thread; either way one writer per PN slot. */
int mac_key_rx (struct mac_hw *mh, const u8 *ccmp, bool group, unsigned int slot)
{
	struct mac_conn *c = &mh->conn;
	struct mac_key *k;
	u8 keyid = ccmp[3] >> 6;
	u64 pn;

	if (!(ccmp[3] & CCMP_EXT_IV) || slot > MAC_SEQ_NONQOS) {
		MAC_COUNT(rx_dropped_protected);
		return -1;
	}
	k = group ? rcu_dereference(c->gtk[keyid]) : rcu_dereference(c->ptk);
	if (!k || (!group && k->conf.keyidx != keyid)) {
		MAC_COUNT(rx_dropped_nokey);
		return -1;
	}
	pn = (u64)ccmp[0] | (u64)ccmp[1] << 8 | (u64)ccmp[4] << 16 | (u64)ccmp[5] << 24 | (u64)ccmp[6] << 32 |
	     (u64)ccmp[7] << 40;
	if (pn <= k->rx_pn[slot]) {
		MAC_COUNT(rx_dropped_replay);
		return -1;
	}
	k->rx_pn[slot] = pn;
	MAC_COUNT(rx_decrypted);
	return 0;
}

/* An unprotected data frame on this connection: the RTW_EXEMPT_* action that lets it up (an open network: NONE),
   or -1 to drop it (counted). */
int mac_key_rx_clear (struct mac_hw *mh, u16 ethertype, bool group)
{
	struct mac_conn *c = &mh->conn;
	int action;

	if (!mac_conn_rsn(c))
		return RTW_EXEMPT_NONE;
	/* ALWAYS: in clear is fine; ON_KEY_MAPPING_KEY_UNAVAILABLE: only while there is no pairwise key (with one, a
	   clear frame of this EtherType is dropped, like the sample); otherwise dot11ExcludeUnencrypted decides */
	action = exempt_lookup(ethertype, group);
	if (action == RTW_EXEMPT_ALWAYS ||
	    (action == RTW_EXEMPT_NO_KEY && !rcu_dereference(c->ptk)) ||
	    (action != RTW_EXEMPT_ALWAYS && action != RTW_EXEMPT_NO_KEY &&
	     !(c->conn_flags & RTW_CONN_F_EXCLUDE_UNENCRYPTED))) {
		MAC_COUNT(rx_exempt);
		return action < 0 ? RTW_EXEMPT_NONE : action;
	}
	MAC_COUNT(rx_dropped_unencrypted);
	return -1;
}

/* RTW_LINK_KEY_* of the keys installed now (a snapshot for IOCTL_RTW_LINK) */
u8 mac_link_keys (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	u8 keys = 0;

	if (READ_ONCE(c->ptk))
		keys |= RTW_LINK_KEY_PAIRWISE;
	for (int i = 0; i < (int)ARRAY_SIZE(c->gtk); i++)
		if (READ_ONCE(c->gtk[i]))
			keys |= RTW_LINK_KEY_GROUP(i);
	for (int i = 0; i < (int)ARRAY_SIZE(c->igtk); i++)
		if (READ_ONCE(c->igtk[i]))
			keys |= RTW_LINK_KEY_IGTK(4 + i);
	return keys;
}

/* ---- installing and removing (wiphy->mtx held) ---- */
static struct mac_key **key_slot (struct mac_conn *c, u8 type, u8 index)
{
	if (type == RTW_KEY_PAIRWISE)
		return index == 0 ? &c->ptk : NULL;
	if (type == RTW_KEY_GROUP && index < ARRAY_SIZE(c->gtk))
		return &c->gtk[index];
	if (type == RTW_KEY_IGTK && index >= 4 && index < 4 + ARRAY_SIZE(c->igtk))
		return &c->igtk[index - 4];
	return NULL;
}

static const char *key_kind (const struct mac_key *k)
{
	if (k->conf.cipher == WLAN_CIPHER_SUITE_AES_CMAC)
		return "integrity group";
	return (k->conf.flags & IEEE80211_KEY_FLAG_PAIRWISE) ? "pairwise" : "group";
}

static void key_free (struct mac_key *k)
{
	key_wipe(k, sizeof(*k) + MAC_KEY_MAX_LEN);
	kfree(k);
}

void mac_key_free (struct mac_key *k)
{
	if (k)
		key_free(k);
}

/* Takes the keys in the n slots out of use: unpublished, readers gone, out of rtw89 (DISABLE_KEY; ms = the AP's
   station for the pairwise key), freed. The channel context must still be there (rtw89 talks to the firmware). */
#define KEYS_MAX                        (1 + 4 + 2)     /* pairwise, group, integrity group */

static void keys_remove (struct mac_hw *mh, struct mac_key **slots[], int n, struct mac_sta *ms)
{
	struct ieee80211_vif *vif = &mh->sta_vif->vif;
	struct mac_key *k[KEYS_MAX];
	int i, ret;

	lockdep_assert_wiphy(mh->hw.wiphy);
	for (i = 0; i < n; i++) {
		k[i] = *slots[i];
		rcu_assign_pointer(*slots[i], NULL);
	}
	synchronize_rcu();              /* no frame is being built or checked with them any more */
	for (i = 0; i < n; i++) {
		bool pairwise;

		if (!k[i])
			continue;
		pairwise = k[i]->conf.flags & IEEE80211_KEY_FLAG_PAIRWISE;
		if (READ_ONCE(k[i]->lost) || !k[i]->hw || !vif->bss_conf.chanctx_conf || (pairwise && !ms)) {
			ret = 0;        /* rtw89 has it no more (SER) or never took it, or cannot be told: only ours */
		} else {
			ret = mh->ops->set_key(&mh->hw, DISABLE_KEY, vif, pairwise ? &ms->sta : NULL, &k[i]->conf);
		}
		if (ret)
			pr_warn("mac: drv set_key(DISABLE) of the %s key %u failed: %d", key_kind(k[i]), k[i]->conf.keyidx,
				ret);
		else
			pr_info("mac: %s key %u removed", key_kind(k[i]), k[i]->conf.keyidx);
		MAC_COUNT(key_dels);
		key_free(k[i]);
	}
}

/* every key slot of the connection that holds a key (pairwise first) */
static int keys_slots (struct mac_conn *c, struct mac_key **slots[KEYS_MAX])
{
	int n = 0;

	if (c->ptk)
		slots[n++] = &c->ptk;
	for (int i = 0; i < (int)ARRAY_SIZE(c->gtk); i++)
		if (c->gtk[i])
			slots[n++] = &c->gtk[i];
	for (int i = 0; i < (int)ARRAY_SIZE(c->igtk); i++)
		if (c->igtk[i])
			slots[n++] = &c->igtk[i];
	return n;
}

/* Every key of the connection, while the AP's station ms (may be NULL) is still in the driver and the channel
   context is assigned: mac80211 ieee80211_free_sta_keys() before the station leaves ASSOC, then the group keys. */
void mac_keys_clear (struct mac_hw *mh, struct mac_sta *ms)
{
	struct mac_key **slots[KEYS_MAX];
	int n = keys_slots(&mh->conn, slots);

	if (n)
		keys_remove(mh, slots, n, ms);
}

/* A hardware restart (mlme.c mac_mlme_restart): rtw89 reset its security CAM, so the keys are unpublished and freed
   without telling the driver. wiphy->mtx held; the connection is already leaving, so the data path is gone. */
void mac_keys_drop (struct mac_hw *mh)
{
	struct mac_key **slots[KEYS_MAX];
	struct mac_key *k[KEYS_MAX];
	int n = keys_slots(&mh->conn, slots);

	if (!n)
		return;
	for (int i = 0; i < n; i++) {
		k[i] = *slots[i];
		rcu_assign_pointer(*slots[i], NULL);
	}
	synchronize_rcu();
	for (int i = 0; i < n; i++) {
		key_free(k[i]);
		MAC_COUNT(key_dels);
	}
}

/* the hw goes away: nothing of the driver is left to tell */
void mac_keys_free (struct mac_hw *mh)
{
	struct mac_key **slots[KEYS_MAX];
	int n = keys_slots(&mh->conn, slots);

	for (int i = 0; i < n; i++) {
		key_free(*slots[i]);
		*slots[i] = NULL;
	}
}

/* The RSC (receive sequence counter) as the lowest PN of the group key's frames to drop. WDI gives 6 bytes without
   saying their order; EAPOL-Key carries the RSC PN0 first. Taking the smaller of both readings never drops a
   frame the AP may still send, and a real counter is far below 2^40, where both readings meet only for zero. */
static u64 key_rsc (const u8 *rsc)
{
	u64 le = 0, be = 0;

	for (int i = 0; i < 6; i++) {
		le |= (u64)rsc[i] << (8 * i);
		be = (be << 8) | rsc[i];
	}
	return min(le, be);
}

/* mac80211 ieee80211_key_identical(): same cipher and key value (compared in constant time), still in rtw89 */
static bool key_identical (const struct mac_key *old, const struct mac_key *k)
{
	u8 diff = 0;

	if (READ_ONCE(old->lost) || old->conf.cipher != k->conf.cipher || old->conf.keylen != k->conf.keylen)
		return false;
	for (int i = 0; i < k->conf.keylen; i++)
		diff |= old->conf.key[i] ^ k->conf.key[i];
	return diff == 0;
}

/* the same for the GO's keys (ap.c) */
bool mac_key_identical (const struct mac_key *old, const struct mac_key *k)
{
	return key_identical(old, k);
}

/* For ap.c: a CCMP-128 key (pairwise or group) as rtw_linux_add_key() makes one; NULL if not CCMP-128 or no memory */
struct mac_key *mac_key_alloc (const struct rtw_key *in)
{
	bool pairwise = in->type == RTW_KEY_PAIRWISE;
	struct mac_key *k;
	u64 rsc = 0;

	if (in->cipher != RTW_CIPHER_CCMP || in->len != CCMP_KEY_LEN || in->index > 3)
		return NULL;
	k = kzalloc(sizeof(*k) + MAC_KEY_MAX_LEN, GFP_KERNEL);
	if (!k)
		return NULL;
	if (!pairwise && (in->flags & RTW_KEY_F_RSC))
		rsc = key_rsc(in->rsc);
	for (int i = 0; i < MAC_PN_SLOTS; i++)
		k->rx_pn[i] = rsc;
	atomic64_set(&k->conf.tx_pn, 0);
	k->conf.cipher = WLAN_CIPHER_SUITE_CCMP;
	k->conf.keyidx = in->index;
	k->conf.keylen = in->len;
	k->conf.iv_len = IEEE80211_CCMP_HDR_LEN;
	k->conf.icv_len = IEEE80211_CCMP_MIC_LEN;
	k->conf.link_id = -1;
	k->conf.flags = pairwise ? IEEE80211_KEY_FLAG_PAIRWISE : 0;
	memcpy(k->conf.key, in->key, in->len);
	return k;
}

int rtw_linux_add_key (const struct rtw_key *in)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_key **slot, *k;
	struct mac_conn *c;
	struct mac_sta *ap;
	bool pairwise = in->type == RTW_KEY_PAIRWISE, igtk = in->type == RTW_KEY_IGTK, again = false, ready;
	bool rekey = false;
	u64 rsc = 0;
	int ret;

	if (!mh)
		return -ENODEV;
	if (igtk ? in->cipher != RTW_CIPHER_BIP || in->len != BIP_KEY_LEN :
	    in->cipher != RTW_CIPHER_CCMP || in->len != CCMP_KEY_LEN)
		return -EOPNOTSUPP;
	if (!key_slot(&mh->conn, in->type, in->index))
		return -EINVAL;
	k = kzalloc(sizeof(*k) + MAC_KEY_MAX_LEN, GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	if (!pairwise && (in->flags & RTW_KEY_F_RSC))
		rsc = key_rsc(in->rsc);
	for (int i = 0; i < MAC_PN_SLOTS; i++)
		k->rx_pn[i] = rsc;
	atomic64_set(&k->conf.tx_pn, 0);
	/* the IGTK like mac80211's AES_CMAC key: no IV, the MMIE as its "ICV"; rx_pn[0] is its IPN */
	k->conf.cipher = igtk ? WLAN_CIPHER_SUITE_AES_CMAC : WLAN_CIPHER_SUITE_CCMP;
	k->conf.keyidx = in->index;
	k->conf.keylen = in->len;
	k->conf.iv_len = igtk ? 0 : IEEE80211_CCMP_HDR_LEN;
	k->conf.icv_len = igtk ? MMIE_LEN : IEEE80211_CCMP_MIC_LEN;
	k->conf.link_id = -1;
	k->conf.flags = pairwise ? IEEE80211_KEY_FLAG_PAIRWISE : 0;
	memcpy(k->conf.key, in->key, in->len);

	c = &mh->conn;
	slot = key_slot(c, in->type, in->index);
	ready = mac_lock_driver(mh);
	ap = c->ap;
	if (!ready) {
		ret = -EBUSY;           /* rtw89 is being reset (SER); the connection is about to be dropped */
	} else if (!mh->sta_vif || READ_ONCE(c->state) != RTW_CONN_CONNECTED || !ap ||
		   ap->state < IEEE80211_STA_ASSOC || !mac_conn_rsn(c)) {
		ret = -ENOTCONN;
	} else if (pairwise && !is_zero_ether_addr(in->peer) && !ether_addr_equal(in->peer, c->bssid)) {
		ret = -EINVAL;          /* a key for another station than our AP */
	} else if (igtk && !c->mfp) {
		ret = -EOPNOTSUPP;      /* no management frame protection on this connection */
	} else if (*slot && key_identical(*slot, k)) {
		/* the same key again (a replayed message 3 or group message 1): keep it and its PN state, as
		   mac80211 ieee80211_key_link() does against key reinstallation */
		ret = 0;
		again = true;
	} else {
		if (*slot) {
			/* rekey: rtw89 matches received frames by key ID, so the old key with this ID goes first */
			struct mac_key **old[1] = { slot };

			keys_remove(mh, old, 1, ap);
			rekey = pairwise;
		}
		ret = mh->ops->set_key(&mh->hw, SET_KEY, &mh->sta_vif->vif, pairwise ? &ap->sta : NULL, &k->conf);
		k->hw = !ret;
		if (ret && igtk) {
			/* pmf.c checks BIP itself; rtw89 having the key too only follows mac80211 */
			pr_warn("mac: drv set_key of the integrity group key %u failed: %d; it is checked here only",
				in->index, ret);
			ret = 0;
		}
		if (ret) {
			pr_err("mac: drv set_key of the %s key %u failed: %d", pairwise ? "pairwise" : "group", in->index,
			       ret);
			ret = -EIO;
		} else {
			/* a pairwise rekey: frames held for reordering were decrypted with the old key; they go as the
			   new key is published, under the lock the reorder buffers deliver under (agg.c), so none of them
			   is checked against (and raises) the new key's PN counters */
			if (rekey)
				spin_lock_bh(&mh->rx_lock);
			rcu_assign_pointer(*slot, k);
			if (rekey) {
				mac_agg_rx_drop_held(mh, ap);
				spin_unlock_bh(&mh->rx_lock);
			}
			k = NULL;
			MAC_COUNT(key_adds);
			/* the 4-way handshake is done: like the supplicant setting the AP's station AUTHORIZED */
			if (pairwise && ap->state == IEEE80211_STA_ASSOC && !mac_sta_move(mh, ap, IEEE80211_STA_AUTHORIZED)) {
				pr_info("mac: %pM authorized", ap->sta.addr);
				mac_ps_recalc(mh);      /* power save may begin now */
			}
		}
	}
	wiphy_unlock(mh->hw.wiphy);
	if (again) {
		pr_info("mac: %s key %u installed again with the same value; kept (PN state unchanged)",
			pairwise ? "pairwise" : igtk ? "integrity group" : "group", in->index);
	} else if (!ret) {
		if (pairwise)
			pr_info("mac: pairwise key %u installed (CCMP, %u bytes)", in->index, CCMP_KEY_LEN);
		else if (igtk)
			pr_info("mac: integrity group key %u installed (BIP-CMAC-128, %u bytes), IPN above %llu",
				in->index, BIP_KEY_LEN, rsc);
		else
			pr_info("mac: group key %u installed (CCMP, %u bytes), PN above %llu", in->index, CCMP_KEY_LEN,
				rsc);
	}
	if (k)
		key_free(k);
	return ret;
}

int rtw_linux_del_key (uint8_t type, uint8_t index)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_key **slot;
	bool ready;
	int ret = 0;

	if (!mh)
		return -ENODEV;
	ready = mac_lock_driver(mh);
	slot = key_slot(&mh->conn, type, index);
	if (!ready) {
		ret = -EBUSY;           /* rtw89 is being reset (SER): the restart drops the keys */
	} else if (!slot || !*slot || !mh->sta_vif) {
		ret = -ENOENT;
	} else {
		struct mac_key **one[1] = { slot };

		keys_remove(mh, one, 1, mh->conn.ap);
	}
	wiphy_unlock(mh->hw.wiphy);
	return ret;
}

/*
 * ieee80211_iter_keys(_rcu)(): rtw89 iterates the keys only to drop them from its CAM without telling the firmware
 * (SER's ser_reset_mac_binding(); WoWLAN is not built). Afterwards the keys must not be used again (rtw89 would
 * send a frame with a CCMP header but without encryption), and the connection cannot go on without them: the
 * hardware restart that follows drops it. Called with wiphy->mtx held or in a read-side section.
 */
void linuxkpi_ieee80211_iterate_keys (struct ieee80211_hw *hw, struct ieee80211_vif *vif,
	void (*fn)(struct ieee80211_hw *, struct ieee80211_vif *, struct ieee80211_sta *, struct ieee80211_key_conf *,
		   void *), void *arg, bool rcu)
{
	struct mac_hw *mh = mac_hw(hw);
	struct mac_conn *c = &mh->conn;
	struct mac_sta *ap;
	struct mac_key *k;
	bool any = false;

	(void)rcu;
	/* the GO's keys (its clients' pairwise keys and its group keys), the same way */
	if (mac_ap_iterate_keys(mh, vif, fn, arg)) {
		WRITE_ONCE(mh->restarting, true);
		pr_warn("mac: the driver dropped the GO's keys (SER); the GO stops with the hardware restart");
	}
	if (!mh->sta_vif || (vif && vif != &mh->sta_vif->vif))
		return;
	rcu_read_lock();
	ap = rcu_dereference(c->ap);
	k = rcu_dereference(c->ptk);
	if (k && ap && !READ_ONCE(k->lost)) {
		fn(hw, &mh->sta_vif->vif, &ap->sta, &k->conf, arg);
		WRITE_ONCE(k->lost, true);
		any = true;
	}
	for (int i = 0; i < (int)ARRAY_SIZE(c->gtk); i++) {
		k = rcu_dereference(c->gtk[i]);
		if (!k || READ_ONCE(k->lost))
			continue;
		fn(hw, &mh->sta_vif->vif, NULL, &k->conf, arg);
		WRITE_ONCE(k->lost, true);
		any = true;
	}
	for (int i = 0; i < (int)ARRAY_SIZE(c->igtk); i++) {
		k = rcu_dereference(c->igtk[i]);
		if (!k || !k->hw || READ_ONCE(k->lost))
			continue;               /* only the integrity group keys rtw89 took */
		fn(hw, &mh->sta_vif->vif, NULL, &k->conf, arg);
		WRITE_ONCE(k->lost, true);
		any = true;
	}
	rcu_read_unlock();
	if (any) {
		/* rtw89 iterates the keys only in SER level 2 (ser_reset_mac_binding), with wiphy->mtx held, before
		   it stops itself and calls ieee80211_restart_hw(). Mark the restart now, so nothing that runs once SER
		   lets go of the lock (MLME and block ack work, entry points, the data path) reaches the reset driver;
		   mac_restart_work then drops the connection without driver calls and reports it lost. */
		WRITE_ONCE(mh->restarting, true);
		pr_warn("mac: the driver dropped the keys (SER); the connection ends with the hardware restart");
	}
}
