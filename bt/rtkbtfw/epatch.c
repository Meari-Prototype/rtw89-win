// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * Realtek Bluetooth patch file handling for rtkbtfw and the rtkbtflt filter driver; see epatch.h for the format
 * and the rules. Kernel mode (_KERNEL_MODE) gets everything but the malloc-based rtkbt_epatch_build.
 */
#if !defined (_KERNEL_MODE)
#include <stdlib.h>
#endif
#include <string.h>
#include "epatch.h"

#define EPATCH_HDR_LEN   14     /* signature[8], fw_version le32, num_patches le16 */

static const uint8_t  mSigV1[8] = { 'R', 'e', 'a', 'l', 't', 'e', 'c', 'h' };
static const uint8_t  mSigV2[8] = { 'R', 'T', 'B', 'T', 'C', 'o', 'r', 'e' };
static const uint8_t  mExtSig[4] = { 0x51, 0x04, 0xfd, 0x77 };

/* project id -> ROM LMP subversion, as in btrtl's project_id_to_lmp_subver[] */
static const struct {
  uint8_t   Id;
  uint16_t  LmpSubver;
} mProjects[] = {
  { 0, 0x1200 }, { 1, 0x8723 }, { 2, 0x8821 }, { 3, 0x8761 }, { 7, 0x8703 }, { 8, 0x8822 }, { 9, 0x8723 },
  { 10, 0x8821 }, { 13, 0x8822 }, { 14, 0x8761 }, { 18, 0x8852 }, { 20, 0x8852 }, { 25, 0x8852 },
  { 36, 0x8851 }, { 44, 0x8922 }, { 47, 0x8852 }, { 51, 0x8761 },
};

static uint16_t
Le16 (
  const uint8_t  *P
  )
{
  return (uint16_t)(P[0] | (P[1] << 8));
}

static uint32_t
Le32 (
  const uint8_t  *P
  )
{
  return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
}

static int
Fail (
  char        *Err,
  size_t      ErrLen,
  const char  *Msg
  )
{
  size_t  i;

  if ((Err != NULL) && (ErrLen > 0)) {
    for (i = 0; (i + 1 < ErrLen) && (Msg[i] != 0); i++) {
      Err[i] = Msg[i];
    }

    Err[i] = 0;
  }

  return -1;
}

int
rtkbt_is_rom_8852b (
  uint16_t  LmpSubver,
  uint16_t  HciRev,
  uint8_t   HciVer
  )
{
  return (LmpSubver == RTKBT_LMP_SUBVER_8852) && (HciRev == RTKBT_HCI_REV_8852B) && (HciVer == RTKBT_HCI_VER_8852B);
}

uint16_t
rtkbt_project_lmp_subver (
  int  ProjectId
  )
{
  size_t  i;

  for (i = 0; i < sizeof (mProjects) / sizeof (mProjects[0]); i++) {
    if (mProjects[i].Id == ProjectId) {
      return mProjects[i].LmpSubver;
    }
  }

  return 0;
}

int
rtkbt_epatch_parse (
  const uint8_t        *Fw,
  size_t               FwLen,
  struct rtkbt_epatch  *Info,
  char                 *Err,
  size_t               ErrLen
  )
{
  size_t    Pos;
  uint8_t   Op;
  uint8_t   Len;
  uint8_t   Data;
  uint16_t  i;
  size_t    Need;

  memset (Info, 0, sizeof (*Info));
  Info->project_id = -1;
  if (FwLen <= 8) {
    return Fail (Err, ErrLen, "file too short");
  }

  if (memcmp (Fw, mSigV2, 8) == 0) {
    return Fail (Err, ErrLen, "\"RTBTCore\" (v2) patch files are not supported (this is for rtl8852bu_fw.bin, v1)");
  }

  if (memcmp (Fw, mSigV1, 8) != 0) {
    return Fail (Err, ErrLen, "no \"Realtech\" epatch signature");
  }

  if (FwLen < EPATCH_HDR_LEN + sizeof (mExtSig) + 3) {
    return Fail (Err, ErrLen, "file too short for an epatch");
  }

  if (memcmp (Fw + FwLen - sizeof (mExtSig), mExtSig, sizeof (mExtSig)) != 0) {
    return Fail (Err, ErrLen, "extension section signature (51 04 fd 77) missing at the end");
  }

  //
  // Walk the extension instructions backwards from the signature until the project id. Pos is the position
  // just after the next byte to read; the stepping (3 bytes read, then Len more skipped) is btrtl's.
  //
  Pos = FwLen - sizeof (mExtSig);
  while (Pos >= EPATCH_HDR_LEN + 3) {
    Op = Fw[--Pos];
    Len = Fw[--Pos];
    Data = Fw[--Pos];
    if (Op == 0xff) {
      break;
    }

    if (Len == 0) {
      return Fail (Err, ErrLen, "extension instruction with length 0");
    }

    if ((Op == 0) && (Len == 1)) {
      Info->project_id = Data;
      break;
    }

    if (Len > Pos) {
      break;
    }

    Pos -= Len;
  }

  if (Info->project_id < 0) {
    return Fail (Err, ErrLen, "no project id instruction in the extension section");
  }

  if (rtkbt_project_lmp_subver (Info->project_id) == 0) {
    return Fail (Err, ErrLen, "unknown project id");
  }

  Info->fw_version = Le32 (Fw + 8);
  Info->num_patches = Le16 (Fw + 12);
  if ((Info->num_patches == 0) || (Info->num_patches > RTKBT_MAX_PATCHES)) {
    return Fail (Err, ErrLen, "unexpected number of patches");
  }

  Need = EPATCH_HDR_LEN + sizeof (mExtSig) + 3 + 8 * (size_t)Info->num_patches;
  if (FwLen < Need) {
    return Fail (Err, ErrLen, "file too short for its patch table");
  }

  for (i = 0; i < Info->num_patches; i++) {
    Info->chip_id[i] = Le16 (Fw + EPATCH_HDR_LEN + 2 * i);
    Info->length[i] = Le16 (Fw + EPATCH_HDR_LEN + 2 * Info->num_patches + 2 * i);
    Info->offset[i] = Le32 (Fw + EPATCH_HDR_LEN + 4 * Info->num_patches + 4 * i);
  }

  return 0;
}

/* The patch for RomVersion: its index, or -1 with a message in Err. */
static int
SelectPatch (
  size_t                     FwLen,
  const struct rtkbt_epatch  *Info,
  uint8_t                    RomVersion,
  char                       *Err,
  size_t                     ErrLen
  )
{
  int       Sel;
  uint16_t  i;
  size_t    PLen;
  size_t    POff;

  Sel = -1;
  for (i = 0; i < Info->num_patches; i++) {
    if (Info->chip_id[i] == (uint16_t)(RomVersion + 1)) {
      Sel = i;
      break;
    }
  }

  // btrtl also treats offset 0 as "not found"
  if ((Sel < 0) || (Info->offset[Sel] == 0)) {
    return Fail (Err, ErrLen, "the file has no patch for this ROM version (chip id = ROM version + 1)");
  }

  PLen = Info->length[Sel];
  POff = Info->offset[Sel];
  if ((PLen < 4) || (POff > FwLen) || (PLen > FwLen - POff)) {
    return Fail (Err, ErrLen, "patch length/offset outside the file");
  }

  return Sel;
}

size_t
rtkbt_epatch_image_len (
  const uint8_t              *Fw,
  size_t                     FwLen,
  const struct rtkbt_epatch  *Info,
  uint8_t                    RomVersion,
  size_t                     CfgLen,
  int                        *PatchIndex,
  char                       *Err,
  size_t                     ErrLen
  )
{
  int  Sel;

  (void)Fw;
  Sel = SelectPatch (FwLen, Info, RomVersion, Err, ErrLen);
  if (Sel < 0) {
    return 0;
  }

  if (PatchIndex != NULL) {
    *PatchIndex = Sel;
  }

  return (size_t)Info->length[Sel] + CfgLen;
}

int
rtkbt_epatch_build_into (
  const uint8_t              *Fw,
  size_t                     FwLen,
  const struct rtkbt_epatch  *Info,
  uint8_t                    RomVersion,
  const uint8_t              *Cfg,
  size_t                     CfgLen,
  uint8_t                    *Out,
  size_t                     OutCap,
  size_t                     *OutLen,
  int                        *PatchIndex,
  char                       *Err,
  size_t                     ErrLen
  )
{
  int     Sel;
  size_t  PLen;
  size_t  POff;

  *OutLen = 0;
  Sel = SelectPatch (FwLen, Info, RomVersion, Err, ErrLen);
  if (Sel < 0) {
    return -1;
  }

  PLen = Info->length[Sel];
  POff = Info->offset[Sel];
  if ((Out == NULL) || (OutCap < PLen) || (CfgLen > OutCap - PLen)) {
    return Fail (Err, ErrLen, "output buffer too small for the image");
  }

  memcpy (Out, Fw + POff, PLen - 4);
  // the last 4 bytes of the patch become the file's fw_version (little endian, as stored in the header)
  memcpy (Out + PLen - 4, Fw + 8, 4);
  if (CfgLen > 0) {
    memcpy (Out + PLen, Cfg, CfgLen);
  }

  *OutLen = PLen + CfgLen;
  if (PatchIndex != NULL) {
    *PatchIndex = Sel;
  }

  return 0;
}

#if !defined (_KERNEL_MODE)

int
rtkbt_epatch_build (
  const uint8_t              *Fw,
  size_t                     FwLen,
  const struct rtkbt_epatch  *Info,
  uint8_t                    RomVersion,
  const uint8_t              *Cfg,
  size_t                     CfgLen,
  uint8_t                    **Out,
  size_t                     *OutLen,
  int                        *PatchIndex,
  char                       *Err,
  size_t                     ErrLen
  )
{
  size_t   Len;
  uint8_t  *Buf;

  *Out = NULL;
  *OutLen = 0;
  Len = rtkbt_epatch_image_len (Fw, FwLen, Info, RomVersion, CfgLen, NULL, Err, ErrLen);
  if (Len == 0) {
    return -1;
  }

  Buf = malloc (Len);
  if (Buf == NULL) {
    return Fail (Err, ErrLen, "out of memory");
  }

  if (rtkbt_epatch_build_into (Fw, FwLen, Info, RomVersion, Cfg, CfgLen, Buf, Len, OutLen, PatchIndex, Err,
        ErrLen) != 0) {
    free (Buf);
    return -1;
  }

  *Out = Buf;
  return 0;
}

#endif

int
rtkbt_config_check (
  const uint8_t  *Cfg,
  size_t         CfgLen,
  char           *Err,
  size_t         ErrLen
  )
{
  size_t  Total;
  size_t  Pos;
  int     Entries;

  if (CfgLen < 6) {
    return Fail (Err, ErrLen, "config file shorter than its 6-byte header");
  }

  if (Le32 (Cfg) != RTKBT_CONFIG_MAGIC) {
    return Fail (Err, ErrLen, "config signature is not 0x8723ab55");
  }

  Total = Le16 (Cfg + 4);
  if (Total != CfgLen - 6) {
    return Fail (Err, ErrLen, "config total length does not match the file size");
  }

  // entries: offset le16, len u8, data[len]
  Entries = 0;
  Pos = 6;
  while (Pos < CfgLen) {
    if ((CfgLen - Pos < 3) || ((size_t)Cfg[Pos + 2] > CfgLen - Pos - 3)) {
      return Fail (Err, ErrLen, "config entry runs past the end");
    }

    Pos += 3 + Cfg[Pos + 2];
    Entries++;
  }

  return Entries;
}

size_t
rtkbt_frag_count (
  size_t  Len
  )
{
  return Len / RTKBT_FRAG_LEN + 1;
}

void
rtkbt_frag_get (
  size_t   Len,
  size_t   I,
  uint8_t  *Index,
  size_t   *Offset,
  size_t   *FragLen
  )
{
  size_t  Count;

  Count = rtkbt_frag_count (Len);
  // 0, 1, ..., 0x7f, then 1, 2, ...: after the first fragment the index cycles through 1..0x7f
  *Index = (uint8_t)((I == 0) ? 0 : ((I - 1) % 0x7f) + 1);
  *Offset = I * RTKBT_FRAG_LEN;
  *FragLen = RTKBT_FRAG_LEN;
  if (I == Count - 1) {
    *Index |= 0x80;
    *FragLen = Len % RTKBT_FRAG_LEN;
  }
}

/* A Command Complete / Command Status event for Opcode at Buf[Off]? Same return values as rtkbt_evt_find. */
static int
EvtAt (
  const uint8_t  *Buf,
  size_t         Len,
  size_t         Off,
  uint16_t       Opcode,
  size_t         *RetOff,
  size_t         *RetLen,
  uint8_t        *CmdStatus
  )
{
  size_t  PLen;

  if (Len - Off < 2) {
    return 0;
  }

  PLen = Buf[Off + 1];
  if (PLen > Len - Off - 2) {
    return 0;
  }

  if ((Buf[Off] == 0x0e) && (PLen >= 3) && (Le16 (Buf + Off + 3) == Opcode)) {
    *RetOff = Off + 5;
    *RetLen = PLen - 3;
    return 1;
  }

  if ((Buf[Off] == 0x0f) && (PLen == 4) && (Le16 (Buf + Off + 4) == Opcode)) {
    *CmdStatus = Buf[Off + 2];
    return 2;
  }

  return 0;
}

int
rtkbt_evt_find (
  const uint8_t          *Buf,
  size_t                 Len,
  uint16_t               Opcode,
  size_t                 *RetOff,
  size_t                 *RetLen,
  uint8_t                *CmdStatus,
  enum rtkbt_evt_layout  *Layout
  )
{
  int     R;
  size_t  Off;

  *Layout = RTKBT_EVT_NONE;
  *RetOff = 0;
  *RetLen = 0;
  *CmdStatus = 0;
  if (Len == 0) {
    return 0;
  }

  // the documented form: the event including its header
  R = EvtAt (Buf, Len, 0, Opcode, RetOff, RetLen, CmdStatus);
  if (R != 0) {
    *Layout = RTKBT_EVT_RAW;
    return R;
  }

  // BTH_VENDOR_EVENT_INFO (BTH_ADDR, ULONG EventSize, EventInfo[]), with a size that fits
  if ((Len >= 14) && (Le32 (Buf + 8) <= Len - 12)) {
    R = EvtAt (Buf, 12 + Le32 (Buf + 8), 12, Opcode, RetOff, RetLen, CmdStatus);
    if (R != 0) {
      *Layout = RTKBT_EVT_INFO;
      return R;
    }
  }

  // anything else that is a consistent event for this opcode
  for (Off = 1; Off + 2 <= Len; Off++) {
    R = EvtAt (Buf, Len, Off, Opcode, RetOff, RetLen, CmdStatus);
    if (R != 0) {
      *Layout = RTKBT_EVT_SCAN;
      return R;
    }
  }

  // no event header at all: Num_HCI_Command_Packets, opcode, return parameters
  if ((Len >= 4) && (Le16 (Buf + 1) == Opcode)) {
    *RetOff = 3;
    *RetLen = Len - 3;
    *Layout = RTKBT_EVT_NOHDR;
    return 1;
  }

  return 0;
}

int
rtkbt_evt_match (
  const uint8_t  *Buf,
  size_t         Len,
  uint16_t       Opcode,
  size_t         *RetOff,
  size_t         *RetLen,
  uint8_t        *CmdStatus
  )
{
  *RetOff = 0;
  *RetLen = 0;
  *CmdStatus = 0;
  if (Len == 0) {
    return 0;
  }

  return EvtAt (Buf, Len, 0, Opcode, RetOff, RetLen, CmdStatus);
}
