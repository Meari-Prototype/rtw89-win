// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * mac\wide.c as a user-mode program (tools-dev\wide-test.ps1): the channel width choice (HT/VHT Operation ->
 * channel, the W52 primaries, the 160 and 80+80 MHz encodings, downgrades), the capabilities of our association request
 * (at 20 MHz, HT without any 40 MHz or long A-MSDU bits) and the AP station's VHT capabilities, checked on the PC
 * before the driver goes to the board.
 */
#include <stdio.h>
#include "../mac/wide.h"

static int fails;

#define CHECK(c)                                                                \
  do {                                                                          \
    if (!(c)) {                                                                 \
      printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
      fails++;                                                                  \
    }                                                                           \
  } while (0)

// rtw89 v7.2 core.c: the 8852B's HT and VHT capabilities (2 streams, no 160 MHz)
#define OWN_HT_CAP   0x19e3     // LDPC, 20/40, SGI20, SGI40, TX STBC, RX STBC 1, 7935-octet A-MSDU, DSSS/CCK in 40
#define OWN_VHT_CAP  (0x00000002 | 0x00000010 | 0x00000020 | 0x00000080 | 0x00000100 | 0x00001000 | (3u << 13) | \
                      0x00100000 | 0x00400000 | 0x03800000)
#define MAP_2SS_0_9  0xfffa

// the 5 GHz channels and their flags (JP-like: everything usable; tests change single entries)
static u32  Flags[200];

static u32
ChanFlags (
  void  *Ctx,
  u8    Channel
  )
{
  (void)Ctx;
  return Flags[Channel];
}

static void
ResetFlags (
  void
  )
{
  static const u8  Chans[] = {
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165
  };
  size_t  i;

  for (i = 0; i < sizeof (Flags) / sizeof (Flags[0]); i++) {
    Flags[i] = WIDE_CH_DISABLED;
  }

  for (i = 0; i < sizeof (Chans); i++) {
    Flags[Chans[i]] = 0;
  }
}

// a beacon's elements: HT Capabilities, HT Operation (primary, offset), optionally VHT Capabilities and Operation
static void
Bss (
  struct wide_bss  *w,
  u8               Primary,
  u8               SecOffset,
  bool             Vht,
  u8               Chw,
  u8               S0,
  u8               S1
  )
{
  u8  HtCap[WIDE_HT_CAP_LEN];
  u8  HtOper[WIDE_HT_OPER_LEN];
  u8  VhtCap[WIDE_VHT_CAP_LEN] = { 0x32, 0x78, 0x91, 0x0f, 0xfa, 0xff, 0, 0, 0xfa, 0xff, 0, 0 };
  u8  VhtOper[WIDE_VHT_OPER_LEN] = { 0, 0, 0, 0xfc, 0xff };

  memset (w, 0, sizeof (*w));
  memset (HtCap, 0, sizeof (HtCap));
  memset (HtOper, 0, sizeof (HtOper));
  HtCap[0]  = 0xef;
  HtCap[1]  = 0x09;
  HtCap[3]  = 0xff;
  HtCap[4]  = 0xff;
  HtOper[0] = Primary;
  HtOper[1] = (u8)(SecOffset | ((SecOffset != 0) ? 0x04 : 0));
  wide_parse_ie (w, WIDE_EID_HT_CAP, HtCap, sizeof (HtCap), false);
  wide_parse_ie (w, WIDE_EID_HT_OPER, HtOper, sizeof (HtOper), false);
  if (Vht) {
    VhtOper[0] = Chw;
    VhtOper[1] = S0;
    VhtOper[2] = S1;
    wide_parse_ie (w, WIDE_EID_VHT_CAP, VhtCap, sizeof (VhtCap), false);
    wide_parse_ie (w, WIDE_EID_VHT_OPER, VhtOper, sizeof (VhtOper), false);
  }
}

static bool
Is (
  const struct wide_chan  *o,
  u8                      Mode,
  u8                      Width,
  u8                      Center
  )
{
  if ((o->mode == Mode) && (o->width == Width) && (o->center == Center)) {
    return true;
  }

  printf ("  got %s%u center %u (why %u, BSS %u MHz)\n", wide_mode_str (o->mode), o->width, o->center, o->why,
          o->bss_width);
  return false;
}

static void
TestChannels (
  void
  )
{
  static const u8   W52[4]   = { 36, 40, 44, 48 };
  static const u8   Above[4] = { WIDE_HT_SEC_ABOVE, WIDE_HT_SEC_BELOW, WIDE_HT_SEC_ABOVE, WIDE_HT_SEC_BELOW };
  struct wide_bss   w;
  struct wide_chan  o;
  int               i;

  ResetFlags ();
  // the channel blocks
  CHECK (wide_center40 (36, true) == 38 && wide_center40 (40, false) == 38 && wide_center40 (40, true) == 0);
  CHECK (wide_center40 (36, false) == 0 && wide_center40 (64, false) == 62 && wide_center40 (144, false) == 142);
  CHECK (wide_center40 (149, true) == 151 && wide_center40 (165, true) == 167 && wide_center40 (165, false) == 0);
  CHECK (wide_center80 (36) == 42 && wide_center80 (48) == 42 && wide_center80 (52) == 58 && wide_center80 (64) == 58);
  CHECK (wide_center80 (100) == 106 && wide_center80 (144) == 138 && wide_center80 (161) == 155);
  CHECK (wide_center80 (165) == 171 && wide_center80 (177) == 171 && wide_center80 (6) == 0 && wide_center80 (34) == 0);
  CHECK (wide_center40 (32, true) == 0 && wide_center40 (181, false) == 0);

  // HT only: 20 MHz without a secondary channel, 40 MHz with one
  Bss (&w, 36, 0, false, 0, 0, 0);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 20, 36) && o.why == WIDE_WHY_NONE);
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, false, 0, 0, 0);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 40, 38) && o.bss_width == 40);
  // the wrong way: 40 MHz above 40 is no pair
  Bss (&w, 40, WIDE_HT_SEC_ABOVE, false, 0, 0, 0);
  wide_choose (&w, 40, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 20, 40) && o.why == WIDE_WHY_PAIR);
  // HT Operation for another primary channel: HT20 (mac80211 disables HT here instead)
  Bss (&w, 40, WIDE_HT_SEC_BELOW, true, 1, 42, 0);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 20, 36) && o.why == WIDE_WHY_PRIMARY);

  // VHT80 with each W52 primary
  for (i = 0; i < 4; i++) {
    Bss (&w, W52[i], Above[i], true, 1, 42, 0);
    wide_choose (&w, W52[i], 80, true, ChanFlags, NULL, &o);
    CHECK (Is (&o, WIDE_MODE_VHT, 80, 42) && o.why == WIDE_WHY_NONE && o.bss_width == 80);
    // Parameters\Bandwidth 40: HT40 (VHT below the BSS's 80 MHz would let the AP send us 80 MHz)
    wide_choose (&w, W52[i], 40, true, ChanFlags, NULL, &o);
    CHECK (Is (&o, WIDE_MODE_HT, 40, (u8)((W52[i] < 44) ? 38 : 46)) && o.why == WIDE_WHY_SETTING);
  }

  // W53/W56 (DFS: the flags allow it; passive channels are mlme.c's business)
  Bss (&w, 60, WIDE_HT_SEC_ABOVE, true, 1, 58, 0);
  wide_choose (&w, 60, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 80, 58));
  Bss (&w, 140, WIDE_HT_SEC_ABOVE, true, 1, 138, 0);
  wide_choose (&w, 140, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 80, 138));

  // 160 MHz, new encoding (CCFS0 = the primary's 80 MHz, CCFS1 = the 160 MHz center): our 80 MHz
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, true, 1, 42, 50);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 80, 42) && o.bss_width == 160);
  Bss (&w, 60, WIDE_HT_SEC_ABOVE, true, 1, 58, 50);
  wide_choose (&w, 60, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 80, 58));
  // 160 MHz, old encoding (width 2, CCFS0 = the 160 MHz center)
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, true, 2, 50, 0);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 80, 42) && o.bss_width == 160);
  Bss (&w, 64, WIDE_HT_SEC_BELOW, true, 2, 50, 0);
  wide_choose (&w, 64, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 80, 58));
  // 80+80 MHz, old encoding: the segment with the primary
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, true, 3, 42, 106);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 80, 42));
  Bss (&w, 100, WIDE_HT_SEC_ABOVE, true, 3, 42, 106);
  wide_choose (&w, 100, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 80, 106));

  // VHT20/VHT40 (channel width 0): the HT width
  Bss (&w, 36, 0, true, 0, 36, 0);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 20, 36));
  Bss (&w, 44, WIDE_HT_SEC_ABOVE, true, 0, 46, 0);
  wide_choose (&w, 44, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 40, 46));
  wide_choose (&w, 44, 40, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_VHT, 40, 46));
  // a VHT40 BSS we use at 20 MHz only: HT20; a VHT40 BSS without a 40 MHz pair that way: HT20
  Flags[48] = WIDE_CH_DISABLED;
  wide_choose (&w, 44, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 20, 44) && o.why == WIDE_WHY_FLAGS);
  ResetFlags ();
  Bss (&w, 48, WIDE_HT_SEC_ABOVE, true, 0, 50, 0);
  wide_choose (&w, 48, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 20, 48) && o.why == WIDE_WHY_PAIR);

  // a VHT Operation that does not match: HT only (40 MHz)
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, true, 1, 58, 0);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 40, 38) && o.why == WIDE_WHY_VHT_OPER);
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, true, 7, 42, 0);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 40, 38) && o.why == WIDE_WHY_VHT_OPER);
  // the OS (or the band) has no VHT for it: HT40
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, true, 1, 42, 0);
  wide_choose (&w, 36, 80, false, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 40, 38) && o.why == WIDE_WHY_NO_VHT);
  // an AP whose VHT RX MCS map has no stream: no VHT
  w.vht_cap_ie[4] = 0xff;
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 40, 38) && o.why == WIDE_WHY_VHT_MCS);

  // regulatory: no 80 MHz on one channel -> HT40; a disabled channel in the pair -> HT20 (no VHT below the BSS's 80 MHz)
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, true, 1, 42, 0);
  Flags[48] = WIDE_CH_NO_80MHZ;
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 40, 38) && o.why == WIDE_WHY_FLAGS);
  Flags[48] = WIDE_CH_DISABLED;
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 40, 38));
  Flags[40] = WIDE_CH_DISABLED;
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 20, 36) && o.why == WIDE_WHY_FLAGS);
  // the 80 MHz of a 160 MHz BSS not usable: HT40
  ResetFlags ();
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, true, 1, 42, 50);
  Flags[44] = WIDE_CH_NO_80MHZ;
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 40, 38) && o.bss_width == 160);
  ResetFlags ();
  Flags[36] = WIDE_CH_NO_HT40PLUS;
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, false, 0, 0, 0);
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 20, 36) && o.why == WIDE_WHY_FLAGS);
  ResetFlags ();
  // a world without W53 (52-64 not channels): no 80 MHz there
  for (i = 52; i <= 64; i += 4) {
    Flags[i] = WIDE_CH_DISABLED;
  }

  Flags[60] = 0;
  Bss (&w, 60, WIDE_HT_SEC_ABOVE, true, 1, 58, 0);
  wide_choose (&w, 60, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_HT, 20, 60) && o.why == WIDE_WHY_FLAGS);
  ResetFlags ();

  // no HT: legacy
  memset (&w, 0, sizeof (w));
  wide_choose (&w, 36, 80, true, ChanFlags, NULL, &o);
  CHECK (Is (&o, WIDE_MODE_LEGACY, 20, 36));

  // every primary, both offsets: only the right pair and block are used, and only where all its channels exist (165:
  // its pair 169 and block 165-177 are not channels here)
  for (i = 36; i <= 165; i++) {
    u8  c80 = wide_center80 ((u8)i);
    int s;

    if (Flags[i] != 0) {
      continue;
    }

    for (s = 0; s < 2; s++) {
      u8  c40 = wide_center40 ((u8)i, s == 0);

      Bss (&w, (u8)i, (s == 0) ? WIDE_HT_SEC_ABOVE : WIDE_HT_SEC_BELOW, c80 != 0, 1, c80, 0);
      wide_choose (&w, (u8)i, 80, true, ChanFlags, NULL, &o);
      if ((c80 != 0) && wide_usable ((u8)i, 80, c80, ChanFlags, NULL)) {
        CHECK (Is (&o, WIDE_MODE_VHT, 80, c80));
      } else if ((c40 != 0) && wide_usable ((u8)i, 40, c40, ChanFlags, NULL)) {
        CHECK (Is (&o, WIDE_MODE_HT, 40, c40));
      } else {
        CHECK (Is (&o, WIDE_MODE_HT, 20, (u8)i));
        CHECK (i == 165);
      }

      CHECK (wide_usable ((u8)i, o.width, o.center, ChanFlags, NULL));
    }
  }

  CHECK (!wide_usable (36, 40, 46, ChanFlags, NULL));     // the primary is not in it
  CHECK (!wide_usable (36, 60, 38, ChanFlags, NULL));
}

static void
TestCaps (
  void
  )
{
  static const u8  ApVht[WIDE_VHT_CAP_LEN] = { 0x32, 0x78, 0x91, 0x0f, 0xfa, 0xff, 0, 0, 0xfa, 0xff, 0, 0 };
  u8               ApNoBf[WIDE_VHT_CAP_LEN];
  u8               Elem[2 + WIDE_VHT_CAP_LEN];
  struct wide_vht  v;
  struct wide_bss  w;
  u32              Ours;
  u16              Old;
  u16              Own;
  u8               Bw;
  u8               Nss;

  // HT at 20 MHz, for any own capabilities: no 20/40 channel width (0x0002), SGI40 (0x0040), DSSS/CCK in 40 MHz
  // (0x1000) or 7935-octet A-MSDU (0x0800); SM power save disabled (0x000c)
  for (Own = 0; ; Own++) {
    Old = (u16)((Own & ~(0x0002 | 0x0040 | 0x1000 | 0x0800 | 0x000c)) | 0x000c);
    if (wide_ht_cap_ours (Own, 20) != Old) {
      printf ("  own 0x%04x: 0x%04x, expected 0x%04x\n", Own, wide_ht_cap_ours (Own, 20), Old);
      fails++;
      break;
    }

    if (Own == 0xffff) {
      break;
    }
  }

  CHECK (wide_ht_cap_ours (OWN_HT_CAP, 20) == 0x01ad);
  CHECK (wide_ht_cap_ours (OWN_HT_CAP, 40) == 0x01ef);
  CHECK (wide_ht_cap_ours (OWN_HT_CAP, 80) == 0x01ef);

  // VHT we advertise: no 11454-octet MPDU, +HTC, MU beamformee; SU beamformee with an SU beamformer AP (STS: AP's 3)
  Ours = wide_vht_cap_ours (OWN_VHT_CAP, ApVht);
  CHECK ((Ours & WIDE_VHT_MAX_MPDU_MASK) == 0);
  CHECK ((Ours & (WIDE_VHT_HTC_VHT | WIDE_VHT_MU_BEAMFORMEE | WIDE_VHT_SUPP_WIDTH_MASK | WIDE_VHT_SHORT_GI_160)) == 0);
  CHECK ((Ours & WIDE_VHT_SU_BEAMFORMEE) != 0 && (Ours & WIDE_VHT_BEAMFORMEE_STS_MASK) == (3u << 13));
  CHECK ((Ours & (WIDE_VHT_SHORT_GI_80 | WIDE_VHT_RXLDPC | WIDE_VHT_TXSTBC | WIDE_VHT_RXSTBC_MASK)) ==
         (WIDE_VHT_SHORT_GI_80 | WIDE_VHT_RXLDPC | WIDE_VHT_TXSTBC | 0x100));
  CHECK ((Ours & WIDE_VHT_MAX_AMPDU_EXP_MASK) == WIDE_VHT_MAX_AMPDU_EXP_MASK);
  memcpy (ApNoBf, ApVht, sizeof (ApNoBf));
  ApNoBf[1] &= (u8)~0x08;                   // no SU beamformer
  ApNoBf[1] &= (u8)~0xe0;                   // beamformee STS 1 (field 0)
  Ours = wide_vht_cap_ours (OWN_VHT_CAP, ApNoBf);
  CHECK ((Ours & WIDE_VHT_SU_BEAMFORMEE) == 0 && (Ours & WIDE_VHT_BEAMFORMEE_STS_MASK) == 0);

  // the element
  CHECK (wide_put_vht_cap (Elem, 0x03801032, MAP_2SS_0_9, 867, MAP_2SS_0_9, 867) == sizeof (Elem));
  CHECK (Elem[0] == 191 && Elem[1] == 12 && Elem[2] == 0x32 && Elem[3] == 0x10 && Elem[4] == 0x80 && Elem[5] == 0x03);
  CHECK (Elem[6] == 0xfa && Elem[7] == 0xff && Elem[8] == 0x63 && Elem[9] == 0x03 && Elem[12] == 0x63);

  // the AP's station: SGI80, LDPC, its SU beamformer and sounding dimensions, 3895 octets, 2 streams
  Ours = wide_vht_cap_ours (OWN_VHT_CAP, ApVht);
  wide_vht_sta (Ours, MAP_2SS_0_9, MAP_2SS_0_9, ApVht, &v);
  CHECK (v.ok && v.rx_mcs_map == MAP_2SS_0_9 && v.tx_mcs_map == MAP_2SS_0_9 && wide_vht_nss (v.rx_mcs_map) == 2);
  CHECK ((v.cap & (WIDE_VHT_SHORT_GI_80 | WIDE_VHT_RXLDPC | WIDE_VHT_SU_BEAMFORMER)) ==
         (WIDE_VHT_SHORT_GI_80 | WIDE_VHT_RXLDPC | WIDE_VHT_SU_BEAMFORMER));
  CHECK ((v.cap & WIDE_VHT_MU_BEAMFORMER) == 0 && (v.cap & WIDE_VHT_MAX_MPDU_MASK) == 0);
  CHECK (wide_vht_amsdu_len (v.cap) == 3895 && wide_vht_amsdu_len (2) == 11454 && wide_vht_amsdu_len (1) == 7991);
  // its RX STBC with our TX STBC; its TX STBC with our RX STBC
  CHECK ((v.cap & WIDE_VHT_RXSTBC_MASK) == (0x0f917832u & WIDE_VHT_RXSTBC_MASK) && (v.cap & WIDE_VHT_TXSTBC) == 0);
  // one stream, and MCS 0-7 only: limited both ways
  memcpy (ApNoBf, ApVht, sizeof (ApNoBf));
  ApNoBf[4] = 0xfc;                         // RX: 1 stream, MCS 0-7
  ApNoBf[8] = 0xfe;                         // TX: 1 stream, MCS 0-9
  wide_vht_sta (Ours, MAP_2SS_0_9, MAP_2SS_0_9, ApNoBf, &v);
  CHECK (v.ok && v.rx_mcs_map == 0xfffc && v.tx_mcs_map == 0xfffe && wide_vht_nss (v.rx_mcs_map) == 1);
  // our TX map limits its RX map (we send only 1 stream)
  wide_vht_sta (Ours, MAP_2SS_0_9, 0xfffe, ApVht, &v);
  CHECK (v.ok && v.rx_mcs_map == 0xfffe);
  ApNoBf[4] = 0xff;
  ApNoBf[5] = 0xff;
  wide_vht_sta (Ours, MAP_2SS_0_9, MAP_2SS_0_9, ApNoBf, &v);
  CHECK (!v.ok);
  CHECK (wide_vht_nss (0xffff) == 0 && wide_vht_nss (0xfff0) == 2);

  // Operating Mode Notification: 80 MHz 2 streams, 20 MHz 1 stream; never above the capabilities; type 1 ignored
  Bw  = 2;
  Nss = 2;
  CHECK (wide_opmode (0x12, &Bw, &Nss) && Bw == 2 && Nss == 2);
  CHECK (wide_opmode (0x00, &Bw, &Nss) && Bw == 0 && Nss == 1);
  Bw  = 1;
  Nss = 1;
  CHECK (wide_opmode (0x16, &Bw, &Nss) && Bw == 1 && Nss == 1);
  // the 160/80+80 bit counts with 80 MHz in the width field only (vht.c): 20 MHz + the bit is 20 MHz
  Bw  = 2;
  Nss = 2;
  CHECK (wide_opmode (0x14, &Bw, &Nss) && Bw == 0 && Nss == 2);
  Bw  = 2;
  CHECK (wide_opmode (0x15, &Bw, &Nss) && Bw == 1);
  Bw  = 2;
  CHECK (wide_opmode (0x16, &Bw, &Nss) && Bw == 2);
  Bw  = 2;
  Nss = 2;
  CHECK (!wide_opmode (0x80, &Bw, &Nss) && Bw == 2 && Nss == 2);

  // the association response: VHT Capabilities replace the beacon's, HT/VHT Operation do not, OMN only there
  Bss (&w, 36, WIDE_HT_SEC_ABOVE, true, 1, 42, 0);
  wide_parse_ie (&w, WIDE_EID_HT_OPER, (const u8 *)"\x28\x07\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                 "\x00\x00\x00\x00\x00\x00", 22, true);
  wide_parse_ie (&w, WIDE_EID_VHT_OPER, (const u8 *)"\x00\x00\x00\xfc\xff", 5, true);
  wide_parse_ie (&w, WIDE_EID_VHT_CAP, ApNoBf, sizeof (ApNoBf), true);
  CHECK (w.ht_primary == 36 && w.vht_oper_ie[0] == 1 && w.vht_cap_ie[4] == 0xff);
  CHECK (!w.opmode);
  wide_parse_ie (&w, WIDE_EID_OPMODE_NOTIF, (const u8 *)"\x01", 1, false);
  CHECK (!w.opmode);
  wide_parse_ie (&w, WIDE_EID_OPMODE_NOTIF, (const u8 *)"\x01", 1, true);
  CHECK (w.opmode && w.opmode_val == 1);
  wide_parse_ie (&w, WIDE_EID_EXT, (const u8 *)"\x23\x00", 2, false);
  wide_parse_ie (&w, WIDE_EID_EXT, (const u8 *)"\x24\x00", 2, false);
  CHECK (w.he_cap && w.he_oper);
  // short elements are ignored
  memset (&w, 0, sizeof (w));
  wide_parse_ie (&w, WIDE_EID_HT_OPER, ApVht, 12, false);
  wide_parse_ie (&w, WIDE_EID_VHT_CAP, ApVht, 11, false);
  wide_parse_ie (&w, WIDE_EID_VHT_OPER, ApVht, 4, false);
  CHECK (!w.ht_oper && !w.vht_cap && !w.vht_oper);
}

int
main (
  void
  )
{
  TestChannels ();
  TestCaps ();
  printf ("%s\n", (fails == 0) ? "PASS" : "FAIL");
  return (fails == 0) ? 0 : 1;
}
