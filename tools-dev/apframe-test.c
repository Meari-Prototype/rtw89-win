// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * mac\apframe.c as a user-mode program (tools-dev\apframe-test.ps1): the group owner's frames (TIM element, beacon
 * and probe response bodies, association response, association request parsing and checks) checked on the PC
 * before the driver goes to the board.
 */
#include <stdio.h>
#include "../mac/apframe.h"

static int fails;

#define CHECK(c)                                                                \
  do {                                                                          \
    if (!(c)) {                                                                 \
      printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
      fails++;                                                                  \
    }                                                                           \
  } while (0)

static void
Bss (
  struct apf_bss  *b,
  bool            band5
  )
{
  static const u8  Rates5[] = { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };
  static const u8  Rates24[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };

  memset (b, 0, sizeof (*b));
  memcpy (b->bssid, "\x02\x11\x22\x33\x44\x55", 6);
  memcpy (b->ssid, "rtw-test", 8);
  b->ssid_len    = 8;
  b->band5       = band5;
  b->channel     = band5 ? 36 : 6;
  b->rsn         = true;
  b->beacon_int  = 100;
  b->dtim_period = 2;
  if (band5) {
    memcpy (b->rates, Rates5, sizeof (Rates5));
    b->n_rates = (u8)sizeof (Rates5);
  } else {
    memcpy (b->rates, Rates24, sizeof (Rates24));
    b->n_rates = (u8)sizeof (Rates24);
  }

  b->ht        = true;
  b->ht_cap[0] = 0x6c;
  memcpy (b->country, "JP ", 3);
  b->n_triplets    = 1;
  b->triplets[0][0] = 36;
  b->triplets[0][1] = 4;
  b->triplets[0][2] = 20;
}

static int
CountIe (
  const u8  *p,
  size_t    len,
  u8        id
  )
{
  int  n;

  n = 0;
  while (len >= 2 && (size_t)p[1] + 2 <= len) {
    n += (p[0] == id);
    len -= (size_t)p[1] + 2;
    p += p[1] + 2;
  }

  return n;
}

static void
TestTim (
  void
  )
{
  u8      t[APF_TIM_MAX];
  size_t  n;

  n = apf_put_tim (t, 0, 2, false, 0);
  CHECK (n == 6 && t[0] == APF_EID_TIM && t[1] == 4 && t[3] == 2 && t[4] == 0 && t[5] == 0);
  n = apf_put_tim (t, 0, 1, true, 1u << 1);
  CHECK (n == 6 && t[4] == 1 && t[5] == 0x02);
  n = apf_put_tim (t, 0, 1, false, 1u << 9);           // octet 1: N1 = 0, two octets
  CHECK (n == 7 && t[1] == 5 && t[4] == 0 && t[5] == 0 && t[6] == 0x02);
  n = apf_put_tim (t, 0, 1, false, 1u << 17);          // octet 2: N1 = 2
  CHECK (n == 6 && t[4] == 2 && t[5] == 0x02);
  n = apf_put_tim (t, 0, 1, false, (1u << 3) | (1u << 20));
  CHECK (n == 8 && t[4] == 0 && t[5] == 0x08 && t[6] == 0 && t[7] == 0x10);
}

static void
TestBeacon (
  void
  )
{
  struct apf_bss  b;
  u8              Tim[APF_TIM_MAX];
  u8              Body[1024];
  u8              Extra[] = { 0xdd, 0x05, 0x00, 0x50, 0xf2, 0x04, 0x10 };   // a WPS element
  u8              ExtraRsn[] = { 0x30, 0x02, 0x01, 0x00 };
  size_t          TimLen;
  size_t          Len;
  size_t          Off;
  const u8        *Ie;

  Bss (&b, true);
  TimLen = apf_put_tim (Tim, 0, 2, false, 1u << 1);
  Len    = apf_build_bss_body (&b, APF_BEACON, Tim, TimLen, Extra, sizeof (Extra), Body, sizeof (Body), &Off);
  CHECK (Len > APF_FIXED_LEN);
  CHECK (Body[8] == 100 && Body[9] == 0);
  CHECK ((Body[10] | Body[11] << 8) == (APF_CAP_ESS | APF_CAP_PRIVACY));
  CHECK (apf_ies_ok (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN));
  CHECK (Off > APF_FIXED_LEN && Body[Off] == APF_EID_TIM && memcmp (Body + Off, Tim, TimLen) == 0);
  Ie = apf_find_ie (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_SSID);
  CHECK (Ie && Ie[1] == 8 && memcmp (Ie + 2, "rtw-test", 8) == 0);
  Ie = apf_find_ie (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_RSN);
  CHECK (Ie && Ie[1] == 20 && Ie[20] == 0x0c && Ie[21] == 0x00);   // RSN capabilities: 16 PTKSA replay counters
  Ie = apf_find_ie (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_HT_OPER);
  CHECK (Ie && Ie[1] == 22 && Ie[2] == 36);
  Ie = apf_find_ie (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_COUNTRY);
  CHECK (Ie && Ie[1] == 6 && Ie[2] == 'J' && Ie[5] == 36);
  CHECK (CountIe (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_ERP) == 0);   // 5 GHz
  CHECK (Body[Len - sizeof (Extra)] == 0xdd && memcmp (Body + Len - sizeof (Extra), Extra, sizeof (Extra)) == 0);

  // probe response: no TIM; the OS's RSN element replaces ours
  Len = apf_build_bss_body (&b, APF_PROBE_RESP, NULL, 0, ExtraRsn, sizeof (ExtraRsn), Body, sizeof (Body), &Off);
  CHECK (Len > 0 && Off == 0);
  CHECK (CountIe (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_TIM) == 0);
  CHECK (CountIe (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_RSN) == 1);

  // 2.4 GHz: rates split over two elements, ERP present, short slot/preamble in the capability
  Bss (&b, false);
  b.country[0] = 0;
  Len = apf_build_bss_body (&b, APF_BEACON, Tim, TimLen, NULL, 0, Body, sizeof (Body), &Off);
  CHECK (Len > 0);
  CHECK ((Body[10] | Body[11] << 8) == (APF_CAP_ESS | APF_CAP_PRIVACY | APF_CAP_SHORT_PREAMBLE | APF_CAP_SHORT_SLOT));
  Ie = apf_find_ie (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_RATES);
  CHECK (Ie && Ie[1] == 8);
  Ie = apf_find_ie (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_EXT_RATES);
  CHECK (Ie && Ie[1] == 4);
  CHECK (CountIe (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_ERP) == 1);
  CHECK (CountIe (Body + APF_FIXED_LEN, Len - APF_FIXED_LEN, APF_EID_COUNTRY) == 0);

  // too small a buffer
  CHECK (apf_build_bss_body (&b, APF_BEACON, Tim, TimLen, NULL, 0, Body, 40, &Off) == 0);
}

/* an association request body: capability, listen interval, [current AP], SSID, rates, RSN, HT, WMM */
static size_t
Req (
  u8          *p,
  bool        Reassoc,
  const char  *Ssid,
  bool        Rate24,
  const u8    *Rsn,
  size_t      RsnLen
  )
{
  static const u8  Wmm[] = { 0xdd, 0x07, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x0f };
  size_t           n;
  size_t           l;

  n      = 0;
  p[n++] = 0x31;
  p[n++] = 0x04;
  p[n++] = 0x0a;
  p[n++] = 0x00;
  if (Reassoc) {
    memcpy (p + n, "\x02\x11\x22\x33\x44\x55", 6);
    n += 6;
  }

  l      = strlen (Ssid);
  p[n++] = APF_EID_SSID;
  p[n++] = (u8)l;
  memcpy (p + n, Ssid, l);
  n     += l;
  p[n++] = APF_EID_RATES;
  p[n++] = Rate24 ? 8 : 7;
  memcpy (p + n, "\x0c\x12\x18\x24\x30\x48\x60\x6c", 8);
  if (!Rate24) {
    memcpy (p + n, "\x0c\x12\x18\x24\x48\x60\x6c", 7);    // no 24 Mbit/s (0x30)
  }

  n += Rate24 ? 8 : 7;
  if (Rsn != NULL) {
    memcpy (p + n, Rsn, RsnLen);
    n += RsnLen;
  }

  p[n++] = APF_EID_HT_CAP;
  p[n++] = APF_HT_CAP_LEN;
  memset (p + n, 0, APF_HT_CAP_LEN);
  p[n] = 0x6c;
  p[n + 3] = 0xff;
  n += APF_HT_CAP_LEN;
  memcpy (p + n, Wmm, sizeof (Wmm));
  return n + sizeof (Wmm);
}

static void
TestAssoc (
  void
  )
{
  static const u8  RsnPsk[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
  static const u8  RsnSae[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                0x01, 0x00, 0x00, 0x0f, 0xac, 0x08, 0xc0, 0x00 };
  static const u8  RsnMfpr[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0xc0, 0x00 };
  static const u8  RsnTkip[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                                 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
  struct apf_bss        b;
  struct apf_assoc_req  r;
  u8                    p[256];
  u8                    Resp[512];
  size_t                n;
  const u8              *Ie;

  Bss (&b, true);
  n = Req (p, false, "rtw-test", true, RsnPsk, sizeof (RsnPsk));
  CHECK (apf_parse_assoc_req (p, n, false, &r) == 0);
  CHECK (r.capability == 0x0431 && r.listen_int == 10 && r.has_ssid && r.ssid_len == 8);
  CHECK (r.rsn && !r.rsn_bad && r.rsn_psk && r.rsn_ccmp && r.rsn_group == 0x000fac04u);
  CHECK (r.ht && r.ht_cap[0] == 0x6c && r.wmm && r.wmm_qos_info == 0x0f && r.n_rates == 8);
  CHECK (apf_check_assoc_req (&b, &r) == APF_STATUS_SUCCESS);

  n = Req (p, true, "rtw-test", true, RsnPsk, sizeof (RsnPsk));
  CHECK (apf_parse_assoc_req (p, n, true, &r) == 0 && apf_check_assoc_req (&b, &r) == APF_STATUS_SUCCESS);

  n = Req (p, false, "other", true, RsnPsk, sizeof (RsnPsk));
  CHECK (apf_parse_assoc_req (p, n, false, &r) == 0 && apf_check_assoc_req (&b, &r) == APF_STATUS_UNSPECIFIED);
  n = Req (p, false, "rtw-test", false, RsnPsk, sizeof (RsnPsk));
  CHECK (apf_parse_assoc_req (p, n, false, &r) == 0 && apf_check_assoc_req (&b, &r) == APF_STATUS_RATES);
  n = Req (p, false, "rtw-test", true, NULL, 0);
  CHECK (apf_parse_assoc_req (p, n, false, &r) == 0 && apf_check_assoc_req (&b, &r) == APF_STATUS_INVALID_IE);
  n = Req (p, false, "rtw-test", true, RsnSae, sizeof (RsnSae));
  CHECK (apf_parse_assoc_req (p, n, false, &r) == 0 && apf_check_assoc_req (&b, &r) == APF_STATUS_AKMP);
  n = Req (p, false, "rtw-test", true, RsnMfpr, sizeof (RsnMfpr));
  CHECK (apf_parse_assoc_req (p, n, false, &r) == 0 && apf_check_assoc_req (&b, &r) == APF_STATUS_MFP_POLICY);
  n = Req (p, false, "rtw-test", true, RsnTkip, sizeof (RsnTkip));
  CHECK (apf_parse_assoc_req (p, n, false, &r) == 0 && apf_check_assoc_req (&b, &r) == APF_STATUS_GROUP_CIPHER);
  // an open GO takes a request without RSN
  b.rsn = false;
  n = Req (p, false, "rtw-test", true, NULL, 0);
  CHECK (apf_parse_assoc_req (p, n, false, &r) == 0 && apf_check_assoc_req (&b, &r) == APF_STATUS_SUCCESS);
  b.rsn = true;
  // malformed: the last element runs past the end, or no room for the fixed fields
  n = Req (p, false, "rtw-test", true, RsnPsk, sizeof (RsnPsk));
  CHECK (apf_parse_assoc_req (p, n - 1, false, &r) == -1);
  CHECK (apf_parse_assoc_req (p, 3, false, &r) == -1);

  // the response: capability, status, AID with the top bits, rates, HT, WMM, the OS's elements
  n = apf_build_assoc_resp (&b, APF_STATUS_SUCCESS, 1, true, (const u8 *)"\xdd\x01\x00", 3, Resp, sizeof (Resp));
  CHECK (n > 6 && Resp[2] == 0 && Resp[3] == 0 && Resp[4] == 0x01 && Resp[5] == 0xc0);
  CHECK (apf_ies_ok (Resp + 6, n - 6));
  Ie = apf_find_ie (Resp + 6, n - 6, APF_EID_HT_OPER);
  CHECK (Ie != NULL);
  CHECK (CountIe (Resp + 6, n - 6, APF_EID_VENDOR) == 2);
  CHECK (CountIe (Resp + 6, n - 6, APF_EID_RSN) == 0);
  n = apf_build_assoc_resp (&b, APF_STATUS_RATES, 3, false, NULL, 0, Resp, sizeof (Resp));
  CHECK (n > 6 && Resp[2] == APF_STATUS_RATES && Resp[4] == 0 && Resp[5] == 0);
  CHECK (CountIe (Resp + 6, n - 6, APF_EID_HT_CAP) == 0);
}

int
main (
  void
  )
{
  TestTim ();
  TestBeacon ();
  TestAssoc ();
  printf ("%s\n", (fails == 0) ? "PASS" : "FAIL");
  return (fails == 0) ? 0 : 1;
}
