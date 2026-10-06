// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: the regulatory domain, what cfg80211's reg.c does for a driver that does not manage
 * it itself (rtw89): the rules of a country from wireless-regdb (regdb.h) set the flags of every 2.4/5 GHz channel
 * (DISABLED, NO_IR, RADAR, NO_OFDM, bandwidth limits; handle_channel), then the driver's reg_notifier picks its TX
 * power tables. The country comes from, strongest first:
 *   the user (Parameters\Country, rtw_linux_reg_set_user)                          NL80211_REGDOM_SET_BY_USER
 *   the Country element of the connected BSS (sme.c regulatory_hint_country_ie;
 *     ignored while the wiphy has REGULATORY_COUNTRY_IE_IGNORE)                     NL80211_REGDOM_SET_BY_COUNTRY_IE
 *   the efuse (rtw89_regd_init_hint's regulatory_hint)                             NL80211_REGDOM_SET_BY_DRIVER
 *   the world rules "00"                                                           NL80211_REGDOM_SET_BY_CORE
 * A Country element counts from the connection's success until it ends (regulatory_hint_disconnect). No beacon hints.
 * Changes are applied by reg_work on the hw work queue: rtw89's notifier takes wiphy->mtx itself (regd.c), so it is
 * called after the channel flags were set under the lock, outside it, and never from a wiphy work. When the channel of
 * the connection becomes DISABLED, the connection is dropped (cfg80211 reg_leave_invalid_chans: only DISABLED counts
 * for a station, NO_IR does not). A scan in progress keeps the channel list it started with.
 */
#include "mac.h"

#define MAC_REG_NO_OFDM                 0x01    /* the fwdb_flags of net/wireless/reg.c */
#define MAC_REG_NO_OUTDOOR              0x02
#define MAC_REG_DFS                     0x04
#define MAC_REG_NO_IR                   0x08
#define MAC_REG_AUTO_BW                 0x10
#define REG_BAND_LIMIT_KHZ              2000000 /* freq_in_rule_band() below 45 GHz */
#define REG_HALF_20_KHZ                 10000

struct mac_reg_rule {
	u32 start_khz, end_khz, max_bw_khz;
	u8 flags;                       /* MAC_REG_* */
};

struct mac_reg_country {
	char alpha2[3];
	u16 list;                       /* in mac_regdb_lists: the number of rules, then their indexes */
	u8 dfs_region;                  /* NL80211_DFS_* */
};

#include "regdb.h"

/* cfg80211's built-in world_regdom (Linux net/wireless/reg.c, ISC licence; see THIRD_PARTY_NOTICES), its 2.4/5 GHz
 * rules: used only if regdb.h fails its check */
static const struct mac_reg_rule reg_world_builtin[] = {
	{ 2402000, 2472000, 40000, 0 },
	{ 2457000, 2482000, 20000, MAC_REG_NO_IR | MAC_REG_AUTO_BW },
	{ 2474000, 2494000, 20000, MAC_REG_NO_IR | MAC_REG_NO_OFDM },
	{ 5170000, 5250000, 80000, MAC_REG_NO_IR | MAC_REG_AUTO_BW },
	{ 5250000, 5330000, 80000, MAC_REG_NO_IR | MAC_REG_AUTO_BW | MAC_REG_DFS },
	{ 5490000, 5730000, 160000, MAC_REG_NO_IR | MAC_REG_DFS },
	{ 5735000, 5835000, 80000, MAC_REG_NO_IR },
};
static const u16 reg_world_builtin_list[] = { 7, 0, 1, 2, 3, 4, 5, 6 };

/* the rules of one domain, in frequency order */
struct reg_set {
	const struct mac_reg_rule *base;
	const u16 *idx;                 /* idx[0] = the number, then the rule indexes into base */
	char alpha2[2];
	u8 dfs_region;
};

static bool regdb_ok;                   /* regdb.h passed reg_check(); set once at the first registration */

static inline u8 reg_n (const struct reg_set *s)
{
	return (u8)s->idx[0];
}

static inline const struct mac_reg_rule *reg_rule (const struct reg_set *s, unsigned int i)
{
	return &s->base[s->idx[1 + i]];
}

/* regdb.h as generated: every list inside the arrays, every rule valid (is_valid_reg_rule) and the rules of a list
   in frequency order */
static bool reg_check (void)
{
	unsigned int c, i;

	for (c = 0; c < ARRAY_SIZE(mac_regdb_countries); c++) {
		u16 at = mac_regdb_countries[c].list;
		u16 n;

		if (at >= ARRAY_SIZE(mac_regdb_lists))
			return false;
		n = mac_regdb_lists[at];
		if (!n || at + 1 + n > ARRAY_SIZE(mac_regdb_lists))
			return false;
		for (i = 0; i < n; i++) {
			u16 r = mac_regdb_lists[at + 1 + i];
			const struct mac_reg_rule *rr;

			if (r >= ARRAY_SIZE(mac_regdb_rules))
				return false;
			rr = &mac_regdb_rules[r];
			if (!rr->start_khz || rr->start_khz >= rr->end_khz || rr->max_bw_khz > rr->end_khz - rr->start_khz)
				return false;
			if (i && mac_regdb_rules[mac_regdb_lists[at + i]].start_khz > rr->start_khz)
				return false;
		}
	}
	return mac_regdb_countries[0].alpha2[0] == '0' && mac_regdb_countries[0].alpha2[1] == '0';
}

static bool reg_alpha2_ok (const char *a)
{
	return ((a[0] >= 'A' && a[0] <= 'Z') || (a[0] >= 'a' && a[0] <= 'z')) &&
	       ((a[1] >= 'A' && a[1] <= 'Z') || (a[1] >= 'a' && a[1] <= 'z'));
}

static char reg_upper (char ch)
{
	return ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : ch;
}

/* query_regdb(): the domain of alpha2 ("00" = world); false if there is none */
static bool reg_find (const char *alpha2, struct reg_set *s)
{
	unsigned int c;

	if (!regdb_ok) {
		if (alpha2[0] != '0' || alpha2[1] != '0')
			return false;
		s->base = reg_world_builtin;
		s->idx = reg_world_builtin_list;
		s->alpha2[0] = s->alpha2[1] = '0';
		s->dfs_region = NL80211_DFS_UNSET;
		return true;
	}
	for (c = 0; c < ARRAY_SIZE(mac_regdb_countries); c++) {
		const struct mac_reg_country *rc = &mac_regdb_countries[c];

		if (rc->alpha2[0] != alpha2[0] || rc->alpha2[1] != alpha2[1])
			continue;
		s->base = mac_regdb_rules;
		s->idx = &mac_regdb_lists[rc->list];
		s->alpha2[0] = rc->alpha2[0];
		s->alpha2[1] = rc->alpha2[1];
		s->dfs_region = rc->dfs_region;
		return true;
	}
	return false;
}

/* cfg80211_does_bw_fit_range() */
static bool reg_fits (u32 start_khz, u32 end_khz, u32 center_khz, u32 half_khz)
{
	return center_khz - half_khz >= start_khz && center_khz + half_khz <= end_khz;
}

/* freq_reg_info() for a 20 MHz channel: the index of the first rule it fits into, or -ERANGE when no rule is in its
   band (freq_in_rule_band), -EINVAL otherwise */
static int reg_freq_rule (const struct reg_set *s, u32 khz)
{
	bool band = false;
	unsigned int i;

	for (i = 0; i < reg_n(s); i++) {
		const struct mac_reg_rule *r = reg_rule(s, i);

		if (!band)
			band = abs((s64)khz - r->start_khz) <= REG_BAND_LIMIT_KHZ ||
			       abs((s64)khz - r->end_khz) <= REG_BAND_LIMIT_KHZ;
		if (band && reg_fits(r->start_khz, r->end_khz, khz, REG_HALF_20_KHZ))
			return (int)i;
	}
	return band ? -EINVAL : -ERANGE;
}

/* reg_rule_to_chan_bw_flags() for 20 MHz channels: with AUTO_BW the bandwidth is the width of the run of contiguous
   rules around it (reg_get_max_bandwidth_from_range) */
static u32 reg_bw_flags (const struct reg_set *s, unsigned int i)
{
	const struct mac_reg_rule *r = reg_rule(s, i);
	u32 bw = r->max_bw_khz, start = r->start_khz, end = r->end_khz;
	unsigned int j;
	u32 flags = 0;

	if (r->flags & MAC_REG_AUTO_BW) {
		for (j = i; j > 0 && reg_rule(s, j - 1)->end_khz >= start; j--)
			start = reg_rule(s, j - 1)->start_khz;
		for (j = i; j + 1 < reg_n(s) && reg_rule(s, j + 1)->start_khz <= end; j++)
			end = reg_rule(s, j + 1)->end_khz;
		bw = end - start;
	}
	if (bw < 40000)
		flags |= IEEE80211_CHAN_NO_HT40;
	if (bw < 80000)
		flags |= IEEE80211_CHAN_NO_80MHZ;
	if (bw < 160000)
		flags |= IEEE80211_CHAN_NO_160MHZ;
	return flags;
}

/* map_regdom_flags() */
static u32 reg_chan_flags (const struct mac_reg_rule *r)
{
	return ((r->flags & MAC_REG_NO_IR) ? IEEE80211_CHAN_NO_IR : 0) |
	       ((r->flags & MAC_REG_DFS) ? IEEE80211_CHAN_RADAR : 0) |
	       ((r->flags & MAC_REG_NO_OFDM) ? IEEE80211_CHAN_NO_OFDM : 0) |
	       ((r->flags & MAC_REG_NO_OUTDOOR) ? IEEE80211_CHAN_INDOOR_ONLY : 0);
}

/* handle_channel(): the channel's flags under the domain; strict: an efuse country with REGULATORY_STRICT_REG, which
   becomes the base (orig_flags) of all later domains */
static void reg_handle_channel (const struct reg_set *s, struct ieee80211_channel *c, int initiator, bool strict)
{
	u32 khz = c->center_freq * 1000;
	u32 flags = strict ? 0 : c->orig_flags;
	int i, i1, i2;

	i = reg_freq_rule(s, khz);
	if (i >= 0) {
		flags |= reg_bw_flags(s, (unsigned int)i) | reg_chan_flags(reg_rule(s, (unsigned int)i));
	} else {
		/* two adjacent rules the channel straddles */
		i1 = reg_freq_rule(s, khz - 20000);
		i2 = reg_freq_rule(s, khz + 20000);
		if (i1 >= 0 && i2 >= 0 && reg_rule(s, (unsigned int)i1)->end_khz == reg_rule(s, (unsigned int)i2)->start_khz &&
		    reg_fits(reg_rule(s, (unsigned int)i1)->start_khz, reg_rule(s, (unsigned int)i2)->end_khz, khz,
			     REG_HALF_20_KHZ)) {
			flags |= reg_bw_flags(s, (unsigned int)i1) | reg_bw_flags(s, (unsigned int)i2) |
				 reg_chan_flags(reg_rule(s, (unsigned int)i1)) | reg_chan_flags(reg_rule(s, (unsigned int)i2));
		} else if (initiator == NL80211_REGDOM_SET_BY_COUNTRY_IE && i == -ERANGE) {
			return;         /* the element's country says nothing about this band: as it was */
		} else {
			flags = c->orig_flags | IEEE80211_CHAN_DISABLED;    /* strict: disabled for good */
		}
	}
	if (strict)
		c->orig_flags = flags;
	c->flags = flags;
}

/* wiphy_update_regulatory(): every 2.4/5 GHz channel; 6 GHz stays disabled (no rules here, and the 8852B has none) */
static void reg_apply (struct mac_hw *mh, const struct reg_set *s, int initiator)
{
	struct wiphy *wiphy = mh->hw.wiphy;
	bool strict = initiator == NL80211_REGDOM_SET_BY_DRIVER && (wiphy->regulatory_flags & REGULATORY_STRICT_REG);
	struct ieee80211_supported_band *sb;
	int b, i;

	for (b = 0; b < NUM_NL80211_BANDS; b++) {
		sb = wiphy->bands[b];
		for (i = 0; sb && i < sb->n_channels; i++) {
			if (b == NL80211_BAND_2GHZ || b == NL80211_BAND_5GHZ)
				reg_handle_channel(s, &sb->channels[i], initiator, strict);
			else
				sb->channels[i].flags = sb->channels[i].orig_flags | IEEE80211_CHAN_DISABLED;
		}
	}
}

/* "1-11, 36-48": the channel numbers of the band whose flags pass test, in ranges of consecutive entries */
static void reg_ranges (const struct ieee80211_supported_band *sb, int kind, char *buf, size_t len)
{
	int i, start = -1, n = 0;

	buf[0] = '\0';
	for (i = 0; sb && i <= sb->n_channels; i++) {
		bool in = false;

		if (i < sb->n_channels) {
			u32 f = sb->channels[i].flags;

			in = kind == 0 ? !(f & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR)) :
			     kind == 1 ? !(f & IEEE80211_CHAN_DISABLED) && (f & (IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR)) :
					 (f & IEEE80211_CHAN_DISABLED);
		}
		if (in && start < 0)
			start = i;
		if (!in && start >= 0) {
			if (i - 1 == start)
				n += scnprintf(buf + n, len - n, "%s%u", n ? " " : "", sb->channels[start].hw_value);
			else
				n += scnprintf(buf + n, len - n, "%s%u-%u", n ? " " : "", sb->channels[start].hw_value,
					       sb->channels[i - 1].hw_value);
			start = -1;
		}
	}
	if (!n)
		scnprintf(buf, len, "-");
}

static void reg_log (struct mac_hw *mh, const char *why)
{
	static const char * const names[2] = { "2.4 GHz", "5 GHz" };
	struct mac_reg *rg = &mh->reg;
	char act[128], pas[128], dis[128];
	int b;

	pr_info("mac: regulatory: %c%c (%s; wireless-regdb %s%s, DFS region %u)", rg->cur[0], rg->cur[1], why,
		MAC_REGDB_VERSION, regdb_ok ? "" : " NOT USED, built-in world", rg->dfs_region);
	for (b = 0; b < 2; b++) {
		const struct ieee80211_supported_band *sb = mh->hw.wiphy->bands[b ? NL80211_BAND_5GHZ : NL80211_BAND_2GHZ];

		if (!sb)
			continue;
		reg_ranges(sb, 0, act, sizeof(act));
		reg_ranges(sb, 1, pas, sizeof(pas));
		reg_ranges(sb, 2, dis, sizeof(dis));
		pr_info("mac: regulatory: %s active %s; passive %s; disabled %s", names[b], act, pas, dis);
	}
}

static const char *reg_src_name (u8 src)
{
	switch (src) {
	case RTW_REG_SRC_USER:  return "user, Parameters\\Country";
	case RTW_REG_SRC_IE:    return "Country element of the connected BSS";
	case RTW_REG_SRC_EFUSE: return "efuse";
	default:                return "world";
	}
}

/* Picks the domain, strongest source first (see the top). wiphy->mtx held. */
static void reg_pick (struct mac_hw *mh, struct reg_set *s, u8 *src, int *initiator)
{
	struct mac_reg *rg = &mh->reg;

	if (rg->user[0] && reg_find(rg->user, s)) {
		*src = RTW_REG_SRC_USER;
		*initiator = NL80211_REGDOM_SET_BY_USER;
	} else if (rg->ie[0] && !(mh->hw.wiphy->regulatory_flags & REGULATORY_COUNTRY_IE_IGNORE) && reg_find(rg->ie, s)) {
		*src = RTW_REG_SRC_IE;
		*initiator = NL80211_REGDOM_SET_BY_COUNTRY_IE;
	} else if (rg->efuse[0] && reg_find(rg->efuse, s)) {
		*src = RTW_REG_SRC_EFUSE;
		*initiator = NL80211_REGDOM_SET_BY_DRIVER;
	} else {
		reg_find("00", s);
		*src = RTW_REG_SRC_WORLD;
		*initiator = NL80211_REGDOM_SET_BY_CORE;
	}
}

static void reg_call_notifier (struct wiphy *wiphy, const char *alpha2, int initiator)
{
	struct regulatory_request req;

	if (!wiphy->reg_notifier)
		return;
	memset(&req, 0, sizeof(req));
	req.alpha2[0] = alpha2[0];
	req.alpha2[1] = alpha2[1];
	req.initiator = initiator;
	wiphy->reg_notifier(wiphy, &req);
}

/* Applies what the sources say now, if that changed. On the hw work queue, without wiphy->mtx. */
static void reg_work (struct work_struct *w)
{
	struct mac_hw *mh = container_of(w, struct mac_hw, reg.work);
	struct wiphy *wiphy = mh->hw.wiphy;
	struct mac_reg *rg = &mh->reg;
	struct mac_conn *c = &mh->conn;
	struct reg_set s;
	u32 lost = 0, lost_mhz = 0;
	int initiator;
	char alpha2[2];
	u8 src;

	if (!mac_lock_driver(mh)) {
		/* a hardware restart still runs: mac_restart_work queues this again when it is done */
		rg->retry = true;
		wiphy_unlock(wiphy);
		return;
	}
	rg->retry = false;
	reg_pick(mh, &s, &src, &initiator);
	if (s.alpha2[0] == rg->cur[0] && s.alpha2[1] == rg->cur[1] && src == rg->src) {
		wiphy_unlock(wiphy);
		return;
	}
	reg_apply(mh, &s, initiator);
	rg->cur[0] = s.alpha2[0];
	rg->cur[1] = s.alpha2[1];
	rg->src = src;
	rg->dfs_region = s.dfs_region;
	rg->changes++;
	/* reg_leave_invalid_chans(): a station leaves only a channel that became DISABLED (any 20 MHz channel of its
	   40/80 MHz one, or the flags no longer allow that width) */
	if (READ_ONCE(c->state) == RTW_CONN_CONNECTED && c->chan && !mac_mlme_chan_ok(mh)) {
		lost = c->chan->hw_value;
		lost_mhz = c->wide.width;
		mac_mlme_conn_loss(mh);
	}
	alpha2[0] = s.alpha2[0];
	alpha2[1] = s.alpha2[1];
	reg_log(mh, reg_src_name(src));
	mac_ap_reg_changed(mh);         /* a GO on a channel that may no longer beacon stops */
	wiphy_unlock(wiphy);

	if (lost)
		pr_warn("mac: regulatory: channel %u (%u MHz) is not usable under %c%c; leaving the BSS", lost, lost_mhz,
			alpha2[0], alpha2[1]);
	/* rtw89 picks its TX power tables (and policies) for the country; it takes wiphy->mtx itself */
	reg_call_notifier(wiphy, alpha2, initiator);
}

/* At registration (linuxkpi_ieee80211_ifattach, before anything runs): checks regdb.h, keeps the driver's channel
   flags as orig_flags, applies the world rules and calls the notifier like wiphy_regulatory_register(). */
void mac_reg_register (struct mac_hw *mh)
{
	struct wiphy *wiphy = mh->hw.wiphy;
	struct ieee80211_supported_band *sb;
	struct reg_set s;
	int b, i;

	INIT_WORK(&mh->reg.work, reg_work);
	regdb_ok = reg_check();
	if (!regdb_ok)
		pr_err("mac: regulatory: the wireless-regdb table failed its check; using cfg80211's built-in world rules only");
	for (b = 0; b < NUM_NL80211_BANDS; b++) {
		sb = wiphy->bands[b];
		for (i = 0; sb && i < sb->n_channels; i++)
			sb->channels[i].orig_flags = sb->channels[i].flags;
	}
	wiphy_lock(wiphy);
	reg_find("00", &s);
	reg_apply(mh, &s, NL80211_REGDOM_SET_BY_CORE);
	mh->reg.cur[0] = mh->reg.cur[1] = '0';
	mh->reg.src = RTW_REG_SRC_WORLD;
	mh->reg.dfs_region = s.dfs_region;
	reg_log(mh, "world");
	wiphy_unlock(wiphy);
	/* rtw89 picks its world TX power tables */
	reg_call_notifier(wiphy, "00", NL80211_REGDOM_SET_BY_CORE);
}

/* Before the hw work queue goes (unregister): no reg_work runs or is queued afterwards. */
void mac_reg_unregister (struct mac_hw *mh)
{
	cancel_work_sync(&mh->reg.work);
}

/* A change of a source, with wiphy->mtx held: reg_work applies it. */
static void reg_queue (struct mac_hw *mh)
{
	if (mh->wq)
		queue_work(mh->wq, &mh->reg.work);
}

/* sme.c: the connection succeeded; the Country element of its BSS (regulatory_hint_country_ie: an even length of at
   least IEEE80211_COUNTRY_IE_MIN_LEN, two letters first). wiphy->mtx held. */
void mac_reg_hint_country_ie (struct mac_hw *mh, const u8 *country, u8 len)
{
	struct mac_reg *rg = &mh->reg;
	struct reg_set s;
	char a[2];

	lockdep_assert_wiphy(mh->hw.wiphy);
	if (!country || len < 6 || (len & 1) || !reg_alpha2_ok((const char *)country))
		return;
	a[0] = reg_upper((char)country[0]);
	a[1] = reg_upper((char)country[1]);
	if (!reg_find(a, &s)) {
		pr_info("mac: regulatory: Country element %c%c: no rules for it in wireless-regdb %s; ignored", a[0], a[1],
			MAC_REGDB_VERSION);
		return;
	}
	if (rg->ie[0] == a[0] && rg->ie[1] == a[1])
		return;
	rg->ie[0] = a[0];
	rg->ie[1] = a[1];
	if (rg->user[0] || (mh->hw.wiphy->regulatory_flags & REGULATORY_COUNTRY_IE_IGNORE))
		pr_info("mac: regulatory: Country element %c%c of the BSS ignored (%s)", a[0], a[1],
			rg->user[0] ? "Parameters\\Country set" : "the driver ignores Country elements");
	reg_queue(mh);
}

/* sme.c regulatory_hint_disconnect(): the connection ended; back to what holds without it. wiphy->mtx held. */
void mac_reg_disconnected (struct mac_hw *mh)
{
	lockdep_assert_wiphy(mh->hw.wiphy);
	if (!mh->reg.ie[0])
		return;
	mh->reg.ie[0] = mh->reg.ie[1] = 0;
	reg_queue(mh);
}

/* A hardware restart is done (iface.c, wiphy->mtx held): run a change that waited for it. */
void mac_reg_restart_done (struct mac_hw *mh)
{
	if (mh->reg.retry)
		reg_queue(mh);
}

/* rtw_mac_log_regulatory() after the probe: a country programmed into the efuse reached us as regulatory_hint(),
   which only recorded it in wiphy->regd */
void mac_reg_efuse (struct mac_hw *mh, const char *alpha2)
{
	struct reg_set s;

	wiphy_lock(mh->hw.wiphy);
	mh->reg.efuse[0] = reg_upper(alpha2[0]);
	mh->reg.efuse[1] = reg_upper(alpha2[1]);
	if (!reg_find(mh->reg.efuse, &s))
		pr_warn("mac: regulatory: efuse country %c%c has no rules in wireless-regdb %s; world rules", alpha2[0],
			alpha2[1], MAC_REGDB_VERSION);
	reg_queue(mh);
	wiphy_unlock(mh->hw.wiphy);
	flush_work(&mh->reg.work);
}

/* rtw_linux_get_caps(): a channel the OS may hear of. Every channel the world rules leave enabled, and those the
   domain now in force enables: the capabilities are reported once, and a Country element only makes some of the
   world's channels active. wiphy->mtx held. */
bool mac_reg_chan_possible (const struct ieee80211_channel *c)
{
	struct reg_set s;
	u32 khz = c->center_freq * 1000;
	int i1, i2;

	if (!(c->flags & IEEE80211_CHAN_DISABLED))
		return true;
	if ((c->orig_flags & IEEE80211_CHAN_DISABLED) || !reg_find("00", &s))
		return false;
	if (reg_freq_rule(&s, khz) >= 0)
		return true;
	i1 = reg_freq_rule(&s, khz - 20000);
	i2 = reg_freq_rule(&s, khz + 20000);
	return i1 >= 0 && i2 >= 0 && reg_rule(&s, (unsigned int)i1)->end_khz == reg_rule(&s, (unsigned int)i2)->start_khz;
}

/* IOCTL_RTW_RADIO, wiphy->mtx held */
void mac_reg_info (struct mac_hw *mh, struct rtw_radio_info *info)
{
	struct mac_reg *rg = &mh->reg;
	int b, i;

	info->reg_src = rg->src;
	memcpy(info->reg_cur, rg->cur, 2);
	memcpy(info->reg_user, rg->user, 2);
	memcpy(info->reg_ie, rg->ie, 2);
	memcpy(info->reg_efuse, rg->efuse, 2);
	info->reg_dfs_region = rg->dfs_region;
	info->reg_ie_ignored = !!(mh->hw.wiphy->regulatory_flags & REGULATORY_COUNTRY_IE_IGNORE);
	info->reg_changes = rg->changes;
	for (b = 0; b < 2; b++) {
		const struct ieee80211_supported_band *sb = mh->hw.wiphy->bands[b ? NL80211_BAND_5GHZ : NL80211_BAND_2GHZ];

		for (i = 0; sb && i < sb->n_channels; i++) {
			u32 f = sb->channels[i].flags;

			if (f & IEEE80211_CHAN_DISABLED)
				info->n_disabled[b]++;
			else if (f & (IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR))
				info->n_passive[b]++;
			else
				info->n_active[b]++;
		}
	}
}

/* ---- entry point (rtw/glue.h) ---- */
int rtw_linux_reg_set_user (const char *alpha2)
{
	struct mac_hw *mh = mac_the_hw;
	struct reg_set s;
	char a[2] = { 0, 0 };
	int ret = 0;

	if (!mh || !mh->wq)
		return -ENODEV;
	if (alpha2 && alpha2[0] && !(alpha2[0] == '0' && alpha2[1] == '0')) {
		if (!reg_alpha2_ok(alpha2) || alpha2[2])
			return -EINVAL;
		a[0] = reg_upper(alpha2[0]);
		a[1] = reg_upper(alpha2[1]);
		if (!reg_find(a, &s)) {
			pr_warn("mac: regulatory: Parameters\\Country %c%c has no rules in wireless-regdb %s; not used", a[0],
				a[1], MAC_REGDB_VERSION);
			a[0] = a[1] = 0;
			ret = -ENOENT;
		}
	}
	wiphy_lock(mh->hw.wiphy);
	mh->reg.user[0] = a[0];
	mh->reg.user[1] = a[1];
	reg_queue(mh);
	wiphy_unlock(mh->hw.wiphy);
	flush_work(&mh->reg.work);
	return ret;
}
