// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win mac: SAE authentication (WPA3-Personal), the driver's part under WiFiCx. The OS
 * does the SAE computations; the mac layer sends our commit and confirm in authentication frames (algorithm 3) and
 * hands the AP's to the OS: an indication queued here becomes WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED in the front end
 * (rtw_linux_sae_get), and the OS's WDI_SET_SAE_AUTH_PARAMS comes back as rtw_linux_sae_set(). The flow (Microsoft's
 * WiFiCx sample wifihal.cpp, IEEE 802.11-2020 12.4.8):
 *   the candidate's turn: "commit parameters needed" -> OS: commit parameters -> our commit -> the AP's commit -> to
 *   the OS -> OS: confirm parameters -> our confirm -> the AP's confirm -> to the OS -> OS: success (then the
 *   association, and the OS's 4-way handshake with the PMK of SAE) or failure (the candidate failed).
 * The AP's commit with status 76 (anti-clogging token needed) or 77 (group not supported) goes to the OS as well,
 * which then gives new commit parameters. The AP may send its confirm right after its commit: one that comes before
 * ours went out is held and handed on after it. Our commit goes again unchanged when it is not acknowledged or not
 * answered (three times in all); an unanswered confirm makes us ask the OS for a new one (its send-confirm counts
 * up), three in all. The OS has SAE_OS_WAIT_MS for each of its steps; when it misses one, it gets an ERROR
 * indication with the WDI status of the step (51-53) and the candidate fails.
 * The frame bodies (scalars, elements, confirms) never go to the log. wiphy->mtx held throughout (MLME work, the
 * entry points).
 */
#include "mac.h"

#define MAC_HDR_LEN                     24
#define AUTH_ALG_SAE                    3
#define SAE_SEQ_COMMIT                  1
#define SAE_SEQ_CONFIRM                 2
#define STATUS_ANTI_CLOGGING            76      /* WLAN_STATUS_ANTI_CLOG_REQUIRED */
#define STATUS_SAE_H2E                  126     /* WLAN_STATUS_SAE_HASH_TO_ELEMENT */
#define EID_EXTENSION                   255
#define EID_EXT_REJECTED_GROUPS         92
#define EID_EXT_TOKEN_CONTAINER         93
#define AKM_SAE                         8
#define SAE_TRIES                       3       /* sends of one commit */
#define SAE_CONFIRMS                    3       /* confirms per candidate */
#define SAE_TX_WAIT_MS                  500     /* the tx status of our frame */
#define SAE_RESP_WAIT_MS                2000    /* the AP's answer to an acknowledged frame (it computes too) */
#define SAE_OS_WAIT_MS                  3000    /* the OS's next step */
#define WDI_SAE_RESEND_REQUEST          50      /* WDI_SAE_STATUS_CONFIRM_MESSAGE_RESEND_REQUEST */
#define WDI_SAE_TIMEOUT_COMMIT          51      /* WDI_SAE_STATUS_TIMEOUT_WAITING_FOR_COMMIT_REQUEST */
#define WDI_SAE_TIMEOUT_CONFIRM         52      /* ..._CONFIRM_REQUEST */
#define WDI_SAE_TIMEOUT_STATUS          53      /* ..._STATUS */

_Static_assert(sizeof(struct rtw_sae_ind) == 16, "rtw_sae_ind layout");
_Static_assert(sizeof(struct rtw_sae_params) == 72, "rtw_sae_params layout");
_Static_assert(offsetof(struct rtw_sae_params, sae_status) == 24, "rtw_sae_params layout");
_Static_assert(offsetof(struct rtw_sae_params, scalar) == 32, "rtw_sae_params layout");

static const char *const sae_state_name[] = {
	"idle", "waiting for commit parameters", "committed", "waiting for confirm parameters", "confirmed",
	"waiting for the result",
};

static void sae_state (struct mac_hw *mh, u8 state)
{
	WRITE_ONCE(mh->conn.sae.state, state);
}

/* An indication for the OS; the caller raises RTW_CONN_EV_SAE (mac_mlme_events) when it is done. */
static void sae_indicate (struct mac_hw *mh, u8 type, bool has_status, u32 status, const u8 *frame, unsigned int len)
{
	struct mac_sae *s = &mh->conn.sae;
	struct mac_sae_ind *e;

	if (s->n_ind == MAC_SAE_IND_MAX) {
		pr_warn("mac: SAE: the OS did not take the earlier indications; the oldest is dropped");
		memmove(&s->ind[0], &s->ind[1], sizeof(s->ind[0]) * (MAC_SAE_IND_MAX - 1));
		s->n_ind--;
	}
	e = &s->ind[s->n_ind++];
	memset(&e->ind, 0, sizeof(e->ind));
	ether_addr_copy(e->ind.bssid, mh->conn.bssid);
	e->ind.type = type;
	e->ind.has_status = has_status;
	e->ind.status = status;
	len = min_t(unsigned int, len, sizeof(e->frame));
	if (frame && len)
		memcpy(e->frame, frame, len);
	e->ind.frame_len = (u16)(frame ? len : 0);
	MAC_COUNT(sae_ind);
}

/* the OS is to make the next step (state), within SAE_OS_WAIT_MS */
static void sae_wait_os (struct mac_hw *mh, u8 state)
{
	sae_state(mh, state);
	mh->conn.wait_txt = 0;          /* no tx status matters now */
	mac_mlme_arm(mh, SAE_OS_WAIT_MS);
	mac_mlme_events(mh, RTW_CONN_EV_SAE);
}

/* sends s->frame (our commit or confirm) */
static int sae_send (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	struct sk_buff *skb;
	u8 *body;

	skb = mac_mlme_frame(mh, IEEE80211_STYPE_AUTH, c->sae.frame_len, &body);
	if (!skb)
		return -ENOMEM;
	memcpy(body, c->sae.frame, c->sae.frame_len);
	c->tries++;
	c->wait_txt = MAC_TXT_AUTH;
	c->resp_wait_ms = SAE_RESP_WAIT_MS;
	MAC_COUNT(sae_tx);
	pr_info("mac: connect: SAE %s to %pM (try %u)", c->sae.state == MAC_SAE_COMMITTED ? "commit" : "confirm",
		c->bssid, c->tries);
	mac_mlme_arm(mh, SAE_TX_WAIT_MS);
	return mac_tx_mgmt(mh, skb, c->ap, 0, MAC_TXT_AUTH);
}

static void sae_fail (struct mac_hw *mh, int status, u16 code, const char *why)
{
	pr_info("mac: connect: SAE with %pM ended: %s", mh->conn.bssid, why);
	mac_mlme_attempt_failed(mh, status, code);
}

/* the candidate's turn begins (mlme.c, state AUTH): the OS gives the commit */
void mac_sae_start (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;

	mac_sae_reset(mh);
	c->tries = 0;
	c->acked = false;
	sae_indicate(mh, RTW_SAE_IND_PARAMS, false, 0, NULL, 0);
	pr_info("mac: connect: SAE with %pM: asking the OS for the commit", c->bssid);
	sae_wait_os(mh, MAC_SAE_WAIT_COMMIT);
}

/* the attempt ends or the next candidate comes (memory only; the queued indications stay for the OS) */
void mac_sae_reset (struct mac_hw *mh)
{
	struct mac_sae *s = &mh->conn.sae;

	kfree(s->frame);
	s->frame = NULL;
	s->frame_len = 0;
	kfree(s->peer_confirm);
	s->peer_confirm = NULL;
	s->peer_confirm_len = 0;
	s->h2e = false;
	s->confirms = 0;
	sae_state(mh, MAC_SAE_IDLE);
}

/* a new connect: nothing of an earlier one goes to the OS any more */
void mac_sae_clear_ind (struct mac_hw *mh)
{
	mh->conn.sae.n_ind = 0;
}

/* The AP's confirm (the frame body from the algorithm number on) after ours went out: to the OS for the verdict. */
static void sae_peer_confirm (struct mac_hw *mh, const u8 *body, unsigned int len)
{
	mac_mlme_disarm(mh);
	sae_indicate(mh, RTW_SAE_IND_CONFIRM, false, 0, body, len);
	pr_info("mac: connect: SAE confirm from %pM; waiting for the OS", mh->conn.bssid);
	sae_wait_os(mh, MAC_SAE_WAIT_STATUS);
}

/* An authentication frame from the candidate while SAE runs (mlme.c, state AUTH). */
void mac_sae_rx (struct mac_hw *mh, const u8 *frame, unsigned int len)
{
	struct mac_conn *c = &mh->conn;
	struct mac_sae *s = &c->sae;
	const u8 *body = frame + MAC_HDR_LEN;
	unsigned int blen = len - MAC_HDR_LEN;
	u16 seq, status;

	if (len < MAC_HDR_LEN + 6 || get_unaligned_le16(body) != AUTH_ALG_SAE)
		return;
	seq = get_unaligned_le16(body + 2);
	status = get_unaligned_le16(body + 4);
	MAC_COUNT(sae_rx);
	if (blen > MAC_SAE_FRAME_MAX) {
		pr_warn("mac: connect: SAE frame of %u bytes from %pM ignored", blen, c->bssid);
		return;
	}
	/* the last authentication frame goes into ASSOCIATION_RESULT like the open system response */
	c->info.auth_resp_len = (u16)min_t(unsigned int, blen, sizeof(c->auth_resp));
	memcpy(c->auth_resp, body, c->info.auth_resp_len);

	if (seq == SAE_SEQ_COMMIT) {
		if (s->state != MAC_SAE_COMMITTED)
			return;         /* a repeat of the AP's commit, or not asked for */
		mac_mlme_disarm(mh);
		sae_indicate(mh, RTW_SAE_IND_COMMIT, false, 0, body, blen);
		pr_info("mac: connect: SAE commit from %pM, status %u; waiting for the OS", c->bssid, status);
		/* 0 and 126 (hash-to-element): the OS confirms next; 76, 77 and others: it commits again or gives up */
		sae_wait_os(mh, status == 0 || status == STATUS_SAE_H2E ? MAC_SAE_WAIT_CONFIRM : MAC_SAE_WAIT_COMMIT);
		return;
	}
	if (seq != SAE_SEQ_CONFIRM)
		return;
	if (status) {
		if (s->state == MAC_SAE_COMMITTED || s->state == MAC_SAE_WAIT_CONFIRM || s->state == MAC_SAE_CONFIRMED)
			sae_fail(mh, RTW_ASSOC_AUTH_REFUSED, status, "the AP refused the confirm");
		return;
	}
	if (s->state == MAC_SAE_CONFIRMED) {
		sae_peer_confirm(mh, body, blen);
	} else if (s->state == MAC_SAE_WAIT_CONFIRM && !s->peer_confirm) {
		/* the AP confirmed before our confirm went out: hold it until then */
		s->peer_confirm = kmemdup(body, blen, GFP_KERNEL);
		if (s->peer_confirm)
			s->peer_confirm_len = (u16)blen;
	}
}

/* The step ran out (mlme.c mlme_retry, state AUTH): no tx status or no acknowledgement, the AP did not answer, or the
   OS did not make its step in time. */
void mac_sae_retry (struct mac_hw *mh)
{
	struct mac_conn *c = &mh->conn;
	struct mac_sae *s = &c->sae;
	u32 wdi;

	switch (s->state) {
	case MAC_SAE_COMMITTED:
		if (c->tries < SAE_TRIES && !sae_send(mh))
			return;
		sae_fail(mh, c->acked ? RTW_ASSOC_AUTH_NO_RESP : RTW_ASSOC_AUTH_NO_ACK, 0, "no commit from the AP");
		return;
	case MAC_SAE_CONFIRMED:
		if (s->confirms >= SAE_CONFIRMS) {
			sae_fail(mh, c->acked ? RTW_ASSOC_AUTH_NO_RESP : RTW_ASSOC_AUTH_NO_ACK, 0,
				 "no confirm from the AP");
			return;
		}
		/* a confirm may only go again with a new send-confirm: the OS makes it */
		sae_indicate(mh, RTW_SAE_IND_RESEND, true, WDI_SAE_RESEND_REQUEST, NULL, 0);
		pr_info("mac: connect: no SAE confirm from %pM yet; asking the OS for a new one", c->bssid);
		sae_wait_os(mh, MAC_SAE_WAIT_CONFIRM);
		return;
	case MAC_SAE_WAIT_COMMIT:
	case MAC_SAE_WAIT_CONFIRM:
	case MAC_SAE_WAIT_STATUS:
		wdi = s->state == MAC_SAE_WAIT_COMMIT ? WDI_SAE_TIMEOUT_COMMIT :
		      s->state == MAC_SAE_WAIT_CONFIRM ? WDI_SAE_TIMEOUT_CONFIRM : WDI_SAE_TIMEOUT_STATUS;
		sae_indicate(mh, RTW_SAE_IND_ERROR, true, wdi, NULL, 0);
		mac_mlme_events(mh, RTW_CONN_EV_SAE);
		sae_fail(mh, RTW_ASSOC_FAILURE, 0, "the OS did not answer in time");
		return;
	default:
		sae_fail(mh, RTW_ASSOC_FAILURE, 0, "unexpected state");
		return;
	}
}

/* An element the OS may give whole or as its contents: the extension element ext with data, as given if it is one
   already. Returns the end. */
static u8 *sae_put_ext (u8 *p, u8 ext, const u8 *data, u16 len)
{
	if (len >= 3 && data[0] == EID_EXTENSION && data[1] == len - 2 && data[2] == ext) {
		memcpy(p, data, len);
		return p + len;
	}
	*p++ = EID_EXTENSION;
	*p++ = (u8)(len + 1);
	*p++ = ext;
	memcpy(p, data, len);
	return p + len;
}

/* Our commit (802.11-2020 9.3.3.12, Table 9-41): algorithm 3, sequence 1, status (0, or 126 for hash-to-element),
   group, [anti-clogging token], scalar, element, [H2E: Rejected Groups element, Anti-Clogging Token Container
   element]. */
static int sae_build_commit (struct mac_hw *mh, const struct rtw_sae_params *p)
{
	struct mac_sae *s = &mh->conn.sae;
	bool h2e = p->type == RTW_SAE_REQ_COMMIT_H2E || p->status_code == STATUS_SAE_H2E;
	unsigned int n;
	u16 status;
	u8 *f, *q;

	n = 8 + p->scalar_len + p->element_len + (h2e ? (p->rejected_len ? 3 + p->rejected_len : 0) +
	    (p->token_len ? 3 + p->token_len : 0) : p->token_len);
	if (!p->scalar_len || !p->element_len || n > MAC_SAE_FRAME_MAX || p->rejected_len > 253 ||
	    (h2e && p->token_len > 253))
		return -EINVAL;
	f = kmalloc(n, GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	status = p->status_code ? p->status_code : h2e ? STATUS_SAE_H2E : 0;
	put_unaligned_le16(AUTH_ALG_SAE, f);
	put_unaligned_le16(SAE_SEQ_COMMIT, f + 2);
	put_unaligned_le16(status, f + 4);
	put_unaligned_le16(p->group, f + 6);
	q = f + 8;
	if (!h2e && p->token_len) {
		memcpy(q, p->token, p->token_len);
		q += p->token_len;
	}
	memcpy(q, p->scalar, p->scalar_len);
	q += p->scalar_len;
	memcpy(q, p->element, p->element_len);
	q += p->element_len;
	if (h2e && p->rejected_len)
		q = sae_put_ext(q, EID_EXT_REJECTED_GROUPS, p->rejected, p->rejected_len);
	if (h2e && p->token_len)
		q = sae_put_ext(q, EID_EXT_TOKEN_CONTAINER, p->token, p->token_len);
	kfree(s->frame);
	s->frame = f;
	s->frame_len = (u16)(q - f);
	s->h2e = h2e;
	return 0;
}

/* Our confirm: algorithm 3, sequence 2, status 0, send-confirm, confirm. */
static int sae_build_confirm (struct mac_hw *mh, const struct rtw_sae_params *p)
{
	struct mac_sae *s = &mh->conn.sae;
	unsigned int n = 8 + p->confirm_len;
	u8 *f;

	if (!p->confirm_len || n > MAC_SAE_FRAME_MAX)
		return -EINVAL;
	f = kmalloc(n, GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	put_unaligned_le16(AUTH_ALG_SAE, f);
	put_unaligned_le16(SAE_SEQ_CONFIRM, f + 2);
	put_unaligned_le16(0, f + 4);
	put_unaligned_le16(p->send_confirm, f + 6);
	memcpy(f + 8, p->confirm, p->confirm_len);
	kfree(s->frame);
	s->frame = f;
	s->frame_len = (u16)n;
	return 0;
}

static int sae_set_locked (struct mac_hw *mh, const struct rtw_sae_params *p)
{
	struct mac_conn *c = &mh->conn;
	struct mac_sae *s = &c->sae;
	int ret;

	if (c->state != RTW_CONN_AUTH || c->auth != RTW_AUTH_SAE || !ether_addr_equal(p->bssid, c->bssid))
		return -ENOTCONN;
	MAC_COUNT(sae_params);
	switch (p->type) {
	case RTW_SAE_REQ_COMMIT:
	case RTW_SAE_REQ_COMMIT_H2E:
		if (s->state != MAC_SAE_WAIT_COMMIT || (p->akm && p->akm != AKM_SAE))
			return -EINVAL;
		ret = sae_build_commit(mh, p);
		if (ret)
			return ret;
		pr_info("mac: connect: SAE commit parameters: group %u%s, scalar %u, element %u, token %u, rejected "
			"groups %u bytes", p->group, s->h2e ? " (hash-to-element)" : "", p->scalar_len, p->element_len,
			p->token_len, p->rejected_len);
		mac_mlme_disarm(mh);
		sae_state(mh, MAC_SAE_COMMITTED);
		c->tries = 0;
		c->acked = false;
		if (sae_send(mh))
			sae_fail(mh, RTW_ASSOC_FAILURE, 0, "our commit could not be sent");
		return 0;
	case RTW_SAE_REQ_CONFIRM:
		if (s->state != MAC_SAE_WAIT_CONFIRM)
			return -EINVAL;
		ret = sae_build_confirm(mh, p);
		if (ret)
			return ret;
		mac_mlme_disarm(mh);
		sae_state(mh, MAC_SAE_CONFIRMED);
		s->confirms++;
		c->tries = 0;
		c->acked = false;
		if (sae_send(mh)) {
			sae_fail(mh, RTW_ASSOC_FAILURE, 0, "our confirm could not be sent");
			return 0;
		}
		if (s->peer_confirm) {
			/* the AP's confirm came early; the tx status of ours no longer matters */
			u8 *pc = s->peer_confirm;

			s->peer_confirm = NULL;
			sae_peer_confirm(mh, pc, s->peer_confirm_len);
			s->peer_confirm_len = 0;
			kfree(pc);
		}
		return 0;
	case RTW_SAE_REQ_SUCCESS:
		if (s->state != MAC_SAE_WAIT_STATUS)
			return -EINVAL;
		pr_info("mac: connect: SAE with %pM succeeded", c->bssid);
		mac_mlme_disarm(mh);
		sae_state(mh, MAC_SAE_IDLE);
		mac_mlme_authenticated(mh);     /* on to the association */
		return 0;
	case RTW_SAE_REQ_FAILURE:
		if (s->state == MAC_SAE_IDLE)
			return -EINVAL;
		pr_info("mac: connect: the OS ended SAE with %pM (WDI SAE status %u, while %s)", c->bssid, p->sae_status,
			sae_state_name[s->state]);
		sae_fail(mh, RTW_ASSOC_AUTH_REFUSED, 0, "failure");
		return 0;
	default:
		return -EINVAL;
	}
}

/* ---- entry points for the front end (rtw/glue.h) ---- */
int rtw_linux_sae_set (const struct rtw_sae_params *p)
{
	struct mac_hw *mh = mac_the_hw;
	int ret;

	if (!mh)
		return -ENODEV;
	if (p->scalar_len > MAC_SAE_FRAME_MAX || p->element_len > MAC_SAE_FRAME_MAX || p->token_len > MAC_SAE_FRAME_MAX ||
	    p->rejected_len > MAC_SAE_FRAME_MAX || p->confirm_len > MAC_SAE_FRAME_MAX ||
	    (p->scalar_len && !p->scalar) || (p->element_len && !p->element) || (p->token_len && !p->token) ||
	    (p->rejected_len && !p->rejected) || (p->confirm_len && !p->confirm))
		return -EINVAL;
	if (!mac_lock_driver(mh))
		ret = -EBUSY;           /* a hardware restart ends the attempt */
	else
		ret = sae_set_locked(mh, p);
	wiphy_unlock(mh->hw.wiphy);
	if (ret)
		pr_warn("mac: SAE parameters (type %u) not taken: %d", p->type, ret);
	return ret;
}

int rtw_linux_sae_get (struct rtw_sae_ind *ind, void *buf, uint32_t len)
{
	struct mac_hw *mh = mac_the_hw;
	struct mac_sae *s;
	int ret = 0;

	memset(ind, 0, sizeof(*ind));
	if (!mh)
		return -ENODEV;
	s = &mh->conn.sae;
	wiphy_lock(mh->hw.wiphy);
	if (s->n_ind) {
		if (s->ind[0].ind.frame_len > len || (s->ind[0].ind.frame_len && !buf)) {
			ret = -ENOSPC;
		} else {
			*ind = s->ind[0].ind;
			if (ind->frame_len)
				memcpy(buf, s->ind[0].frame, ind->frame_len);
			memmove(&s->ind[0], &s->ind[1], sizeof(s->ind[0]) * (MAC_SAE_IND_MAX - 1));
			s->n_ind--;
			ret = 1;
		}
	}
	wiphy_unlock(mh->hw.wiphy);
	return ret;
}
