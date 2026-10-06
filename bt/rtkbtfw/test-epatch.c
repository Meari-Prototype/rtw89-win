// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * PC-side unit test of epatch.c (bt\rtkbtfw\build.ps1 builds it for x64 and runs it):
 *   test-epatch <rtl8852bu_fw.bin> <rtl8852bu_config.bin>
 * Checks the real patch file (the linux-firmware rtl_bt/rtl8852bu_fw.bin the packages ship) against its known size,
 * fw_version and patch table, the image built for each ROM version, the
 * fragment indices against a separate model of btrtl's download loop, error paths on synthetic files, and the
 * search for the Command Complete event in the possible IOCTL output layouts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "epatch.h"

static int  mFails;
static int  mChecks;

#define CHECK(c)  Check ((c), #c, __LINE__)

static void
Check (
  int         Ok,
  const char  *What,
  int         Line
  )
{
  mChecks++;
  if (!Ok) {
    mFails++;
    printf ("FAIL line %d: %s\n", Line, What);
  }
}

static uint8_t *
ReadAll (
  const char  *Path,
  size_t      *Len
  )
{
  FILE     *f;
  long     n;
  uint8_t  *P;

  if (fopen_s (&f, Path, "rb") != 0) {
    return NULL;
  }

  fseek (f, 0, SEEK_END);
  n = ftell (f);
  fseek (f, 0, SEEK_SET);
  P = malloc ((size_t)n);
  if ((P != NULL) && (fread (P, 1, (size_t)n, f) != (size_t)n)) {
    free (P);
    P = NULL;
  }

  fclose (f);
  *Len = (size_t)n;
  return P;
}

/*
 * A model of btrtl's loop written the other way round: a running counter that starts at 0, is sent, and wraps
 * from 0x7f to 1 for the next fragment. Compares index, offset and length of every fragment, and that the
 * fragments cover the image exactly once.
 */
static void
CheckFrags (
  size_t  Len
  )
{
  size_t   Count;
  size_t   i;
  uint8_t  Next;
  uint8_t  Want;
  uint8_t  Idx;
  size_t   Off;
  size_t   FLen;
  size_t   Sum;
  int      Bad;

  Count = rtkbt_frag_count (Len);
  Next = 0;
  Sum = 0;
  Bad = 0;
  for (i = 0; i < Count; i++) {
    Want = Next;
    Next = (Next == 0x7f) ? 1 : (uint8_t)(Next + 1);
    if (i == Count - 1) {
      Want |= 0x80;
    }

    rtkbt_frag_get (Len, i, &Idx, &Off, &FLen);
    if ((Idx != Want) || (Off != Sum) || (FLen > RTKBT_FRAG_LEN) || ((i < Count - 1) && (FLen != RTKBT_FRAG_LEN))) {
      Bad = 1;
    }

    Sum += FLen;
  }

  CHECK (!Bad);
  CHECK (Sum == Len);
  CHECK (Count == Len / RTKBT_FRAG_LEN + 1);
}

/* rtkbt_epatch_image_len + rtkbt_epatch_build_into (the kernel path) against rtkbt_epatch_build, ROM 0 and 1. */
static void
TestBuildInto (
  const uint8_t              *Fw,
  size_t                     FwLen,
  const struct rtkbt_epatch  *Ep,
  const uint8_t              *Cfg,
  size_t                     CfgLen
  )
{
  uint8_t  *Img;
  uint8_t  *Buf;
  size_t   ImgLen;
  size_t   Len;
  size_t   OutLen;
  char     Err[160];
  int      Sel;
  int      Sel2;
  uint8_t  Rom;

  for (Rom = 0; Rom <= 1; Rom++) {
    CHECK (rtkbt_epatch_build (Fw, FwLen, Ep, Rom, Cfg, CfgLen, &Img, &ImgLen, &Sel, Err, sizeof (Err)) == 0);
    Sel2 = -1;
    Len = rtkbt_epatch_image_len (Fw, FwLen, Ep, Rom, CfgLen, &Sel2, Err, sizeof (Err));
    CHECK ((Len == ImgLen) && (Sel2 == Sel));
    Buf = malloc (Len + 8);
    CHECK (Buf != NULL);
    if ((Img == NULL) || (Buf == NULL)) {
      free (Img);
      free (Buf);
      continue;
    }

    memset (Buf, 0xcc, Len + 8);
    Sel2 = -1;
    CHECK (rtkbt_epatch_build_into (Fw, FwLen, Ep, Rom, Cfg, CfgLen, Buf, Len, &OutLen, &Sel2, Err, sizeof (Err)) == 0);
    CHECK ((OutLen == Len) && (Sel2 == Sel) && (memcmp (Buf, Img, Len) == 0));
    CHECK ((Buf[Len] == 0xcc) && (Buf[Len + 7] == 0xcc));     // nothing written past the image
    // one byte short (in the config, then in the patch): refused, nothing claimed
    CHECK (rtkbt_epatch_build_into (Fw, FwLen, Ep, Rom, Cfg, CfgLen, Buf, Len - 1, &OutLen, NULL, Err, sizeof (Err)) != 0);
    CHECK (OutLen == 0);
    CHECK (rtkbt_epatch_build_into (Fw, FwLen, Ep, Rom, Cfg, CfgLen, Buf, Len - CfgLen - 1, &OutLen, NULL, Err,
             sizeof (Err)) != 0);
    CHECK (rtkbt_epatch_build_into (Fw, FwLen, Ep, Rom, Cfg, CfgLen, NULL, Len, &OutLen, NULL, Err, sizeof (Err)) != 0);
    free (Img);
    free (Buf);
  }

  // an error message is cut to the buffer and terminated
  memset (Err, 'x', sizeof (Err));
  CHECK (rtkbt_epatch_image_len (Fw, FwLen, Ep, 5, 0, NULL, Err, 8) == 0);
  CHECK ((Err[7] == 0) && (Err[8] == 'x'));
}

static void
TestRealFile (
  const char  *FwPath,
  const char  *CfgPath
  )
{
  uint8_t              *Fw;
  uint8_t              *Cfg;
  uint8_t              *Img;
  size_t               FwLen;
  size_t               CfgLen;
  size_t               ImgLen;
  struct rtkbt_epatch  Ep;
  char                 Err[160];
  int                  Sel;
  uint8_t              Idx;
  size_t               Off;
  size_t               FLen;

  Fw = ReadAll (FwPath, &FwLen);
  Cfg = ReadAll (CfgPath, &CfgLen);
  CHECK (Fw != NULL);
  CHECK (Cfg != NULL);
  if ((Fw == NULL) || (Cfg == NULL)) {
    return;
  }

  CHECK (FwLen == 74324);
  CHECK (rtkbt_epatch_parse (Fw, FwLen, &Ep, Err, sizeof (Err)) == 0);
  CHECK (Ep.fw_version == 0xdbc6b20f);
  CHECK (Ep.project_id == RTKBT_PROJECT_8852B);
  CHECK (rtkbt_project_lmp_subver (Ep.project_id) == RTKBT_LMP_SUBVER_8852);
  CHECK (Ep.num_patches == 2);
  CHECK ((Ep.chip_id[0] == 1) && (Ep.length[0] == 16133) && (Ep.offset[0] == 48));
  CHECK ((Ep.chip_id[1] == 2) && (Ep.length[1] == 57997) && (Ep.offset[1] == 16256));

  // the config is the empty one (signature, length 0)
  CHECK (CfgLen == 6);
  CHECK (rtkbt_config_check (Cfg, CfgLen, Err, sizeof (Err)) == 0);

  // ROM version 1 -> chip id 2 -> patch 1, fw_version in its last 4 bytes, config after it
  CHECK (rtkbt_epatch_build (Fw, FwLen, &Ep, 1, Cfg, CfgLen, &Img, &ImgLen, &Sel, Err, sizeof (Err)) == 0);
  CHECK (Sel == 1);
  CHECK (ImgLen == 57997 + 6);
  CHECK (memcmp (Img, Fw + 16256, 57997 - 4) == 0);
  CHECK ((Img[57993] == 0x0f) && (Img[57994] == 0xb2) && (Img[57995] == 0xc6) && (Img[57996] == 0xdb));
  CHECK (memcmp (Img + 57997, Cfg, 6) == 0);
  CheckFrags (ImgLen);
  CHECK (rtkbt_frag_count (ImgLen) == 231);
  rtkbt_frag_get (ImgLen, 127, &Idx, &Off, &FLen);
  CHECK (Idx == 0x7f);
  rtkbt_frag_get (ImgLen, 128, &Idx, &Off, &FLen);
  CHECK (Idx == 0x01);
  rtkbt_frag_get (ImgLen, 230, &Idx, &Off, &FLen);
  CHECK ((Idx == (0x80 | 103)) && (Off == 57960) && (FLen == 43));
  free (Img);

  // without the config
  CHECK (rtkbt_epatch_build (Fw, FwLen, &Ep, 1, NULL, 0, &Img, &ImgLen, &Sel, Err, sizeof (Err)) == 0);
  CHECK (ImgLen == 57997);
  free (Img);

  // ROM version 0 -> patch 0
  CHECK (rtkbt_epatch_build (Fw, FwLen, &Ep, 0, Cfg, CfgLen, &Img, &ImgLen, &Sel, Err, sizeof (Err)) == 0);
  CHECK ((Sel == 0) && (ImgLen == 16133 + 6));
  CHECK (memcmp (Img, Fw + 48, 16133 - 4) == 0);
  CHECK ((Img[16129] == 0x0f) && (Img[16132] == 0xdb));
  CheckFrags (ImgLen);
  free (Img);

  // no patch for ROM version 2
  CHECK (rtkbt_epatch_build (Fw, FwLen, &Ep, 2, Cfg, CfgLen, &Img, &ImgLen, &Sel, Err, sizeof (Err)) != 0);
  CHECK (Img == NULL);
  CHECK (rtkbt_epatch_image_len (Fw, FwLen, &Ep, 2, CfgLen, &Sel, Err, sizeof (Err)) == 0);

  // the filter driver's path: size first, then build into its own buffer; the same bytes as rtkbt_epatch_build
  TestBuildInto (Fw, FwLen, &Ep, Cfg, CfgLen);
  free (Fw);
  free (Cfg);
}

/* A minimal v1 file: one patch for chip id 1 (8 bytes), then the extension: project id, signature. */
static size_t
MakeSynthetic (
  uint8_t  *B,
  uint8_t  Project
  )
{
  size_t  n;

  memcpy (B, "Realtech", 8);
  B[8] = 0x78; B[9] = 0x56; B[10] = 0x34; B[11] = 0x12;   // fw_version 0x12345678
  B[12] = 1; B[13] = 0;                                    // one patch
  B[14] = 1; B[15] = 0;                                    // chip id 1
  B[16] = 8; B[17] = 0;                                    // length 8
  B[18] = 22; B[19] = 0; B[20] = 0; B[21] = 0;             // offset 22
  memcpy (B + 22, "\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7", 8);
  n = 30;
  B[n++] = 0xee;                                           // a byte that the walk does not reach
  B[n++] = Project;                                        // data
  B[n++] = 1;                                              // length
  B[n++] = 0;                                              // opcode 0: project id
  memcpy (B + n, "\x51\x04\xfd\x77", 4);
  return n + 4;
}

static void
TestSynthetic (
  void
  )
{
  uint8_t              B[64];
  uint8_t              C[16];
  uint8_t              *Img;
  size_t               Len;
  size_t               ImgLen;
  struct rtkbt_epatch  Ep;
  char                 Err[160];
  int                  Sel;

  Len = MakeSynthetic (B, 20);
  CHECK (rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) == 0);
  CHECK ((Ep.project_id == 20) && (Ep.fw_version == 0x12345678) && (Ep.num_patches == 1));
  CHECK (rtkbt_epatch_build (B, Len, &Ep, 0, NULL, 0, &Img, &ImgLen, &Sel, Err, sizeof (Err)) == 0);
  CHECK ((ImgLen == 8) && (memcmp (Img, "\xa0\xa1\xa2\xa3\x78\x56\x34\x12", 8) == 0));
  free (Img);

  // another project id is parsed (the tool refuses it, not the parser); an unknown one is an error
  Len = MakeSynthetic (B, 8);
  CHECK ((rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) == 0) && (Ep.project_id == 8));
  CHECK (rtkbt_project_lmp_subver (8) == 0x8822);
  Len = MakeSynthetic (B, 99);
  CHECK (rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) != 0);

  // instructions before the project id are skipped: 0xff ends the walk without one
  Len = MakeSynthetic (B, 20);
  B[Len - 5] = 0xff;
  CHECK (rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) != 0);

  // a zero-length instruction
  Len = MakeSynthetic (B, 20);
  B[Len - 5] = 3;
  B[Len - 6] = 0;
  CHECK (rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) != 0);

  // bad signatures
  Len = MakeSynthetic (B, 20);
  B[0] = 'r';
  CHECK (rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) != 0);
  Len = MakeSynthetic (B, 20);
  memcpy (B, "RTBTCore", 8);
  CHECK (rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) != 0);
  Len = MakeSynthetic (B, 20);
  B[Len - 1] = 0;
  CHECK (rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) != 0);
  CHECK (rtkbt_epatch_parse (B, 8, &Ep, Err, sizeof (Err)) != 0);

  // patch outside the file, patch shorter than the version
  Len = MakeSynthetic (B, 20);
  B[18] = 60;
  CHECK (rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) == 0);
  CHECK (rtkbt_epatch_build (B, Len, &Ep, 0, NULL, 0, &Img, &ImgLen, &Sel, Err, sizeof (Err)) != 0);
  Len = MakeSynthetic (B, 20);
  B[16] = 3;
  CHECK (rtkbt_epatch_parse (B, Len, &Ep, Err, sizeof (Err)) == 0);
  CHECK (rtkbt_epatch_build (B, Len, &Ep, 0, NULL, 0, &Img, &ImgLen, &Sel, Err, sizeof (Err)) != 0);

  // config checks
  memcpy (C, "\x55\xab\x23\x87\x04\x00\x10\x00\x01\x5a", 10);
  CHECK (rtkbt_config_check (C, 10, Err, sizeof (Err)) == 1);
  CHECK (rtkbt_config_check (C, 9, Err, sizeof (Err)) < 0);
  C[8] = 2;
  CHECK (rtkbt_config_check (C, 10, Err, sizeof (Err)) < 0);
  C[0] = 0;
  CHECK (rtkbt_config_check (C, 10, Err, sizeof (Err)) < 0);
}

static void
TestFragEdges (
  void
  )
{
  uint8_t  Idx;
  size_t   Off;
  size_t   FLen;
  size_t   Len;

  // one short fragment: index 0x80
  CHECK (rtkbt_frag_count (10) == 1);
  rtkbt_frag_get (10, 0, &Idx, &Off, &FLen);
  CHECK ((Idx == 0x80) && (Off == 0) && (FLen == 10));

  // a multiple of 252 ends with an empty fragment, as in btrtl (len / 252 + 1 fragments)
  CHECK (rtkbt_frag_count (504) == 3);
  rtkbt_frag_get (504, 2, &Idx, &Off, &FLen);
  CHECK ((Idx == 0x82) && (Off == 504) && (FLen == 0));

  // long images: the index wraps 0x7f -> 1 more than once
  for (Len = 1; Len < 200000; Len += 997) {
    CheckFrags (Len);
  }

  CheckFrags (127 * 252);
  CheckFrags (128 * 252);
  CheckFrags (255 * 252 + 1);
  rtkbt_frag_get (300 * 252, 254, &Idx, &Off, &FLen);
  CHECK (Idx == 0x7f);
  rtkbt_frag_get (300 * 252, 255, &Idx, &Off, &FLen);
  CHECK (Idx == 0x01);
}

static void
TestEvents (
  void
  )
{
  uint8_t                B[64];
  size_t                 ROff;
  size_t                 RLen;
  uint8_t                St;
  enum rtkbt_evt_layout  L;

  // raw Command Complete for 0xfc6d: status 0, version 1
  memcpy (B, "\x0e\x05\x01\x6d\xfc\x00\x01", 7);
  // the filter driver's exact match on what the interrupt endpoint delivered
  CHECK ((rtkbt_evt_match (B, 7, 0xfc6d, &ROff, &RLen, &St) == 1) && (ROff == 5) && (RLen == 2));
  CHECK (rtkbt_evt_match (B, 6, 0xfc6d, &ROff, &RLen, &St) == 0);       // incomplete
  CHECK (rtkbt_evt_match (B, 7, 0xfc20, &ROff, &RLen, &St) == 0);       // another opcode
  CHECK (rtkbt_evt_match (B, 0, 0xfc6d, &ROff, &RLen, &St) == 0);
  CHECK (rtkbt_evt_match (B + 1, 6, 0xfc6d, &ROff, &RLen, &St) == 0);   // no scanning into the buffer
  // HCI_Read_Local_Version of the ROM, as the filter sees it
  memcpy (B + 16, "\x0e\x0c\x01\x01\x10\x00\x0b\x0b\x00\x0b\x5d\x00\x52\x88", 14);
  CHECK ((rtkbt_evt_match (B + 16, 14, 0x1001, &ROff, &RLen, &St) == 1) && (ROff == 5) && (RLen == 9));
  CHECK (rtkbt_is_rom_8852b (0x8852, 0x000b, 0x0b));
  CHECK (!rtkbt_is_rom_8852b (0xb20f, 0xdbc6, 0x0b));
  CHECK (!rtkbt_is_rom_8852b (0x8852, 0x000c, 0x0b));
  CHECK (!rtkbt_is_rom_8852b (0x8852, 0x000b, 0x0c));
  // Command Status for 0x1001 with an error
  memcpy (B + 32, "\x0f\x04\x01\x01\x01\x10", 6);
  CHECK ((rtkbt_evt_match (B + 32, 6, 0x1001, &ROff, &RLen, &St) == 2) && (St == 0x01));
  CHECK (rtkbt_evt_find (B, 7, 0xfc6d, &ROff, &RLen, &St, &L) == 1);
  CHECK ((L == RTKBT_EVT_RAW) && (ROff == 5) && (RLen == 2) && (B[ROff + 1] == 1));
  // trailing bytes in the buffer do not matter
  CHECK (rtkbt_evt_find (B, 20, 0xfc6d, &ROff, &RLen, &St, &L) == 1);
  // another opcode
  CHECK (rtkbt_evt_find (B, 7, 0xfc20, &ROff, &RLen, &St, &L) == 0);
  // truncated
  CHECK (rtkbt_evt_find (B, 6, 0xfc6d, &ROff, &RLen, &St, &L) == 0);

  // BTH_VENDOR_EVENT_INFO: address, size 7, the event
  memset (B, 0, sizeof (B));
  memcpy (B, "\xeb\xb9\xef\xb7\x3c\x00\x00\x00\x07\x00\x00\x00", 12);
  memcpy (B + 12, "\x0e\x05\x01\x20\xfc\x00\x85", 7);
  CHECK (rtkbt_evt_find (B, 19, 0xfc20, &ROff, &RLen, &St, &L) == 1);
  CHECK ((L == RTKBT_EVT_INFO) && (ROff == 17) && (RLen == 2) && (B[ROff] == 0) && (B[ROff + 1] == 0x85));

  // Command Status with an error (unknown command)
  memcpy (B, "\x0f\x04\x01\x01\x66\xfc", 6);
  CHECK (rtkbt_evt_find (B, 6, 0xfc66, &ROff, &RLen, &St, &L) == 2);
  CHECK (St == 0x01);

  // no event header
  memcpy (B, "\x01\x61\xfc\x00\x00\x00", 6);
  CHECK (rtkbt_evt_find (B, 6, 0xfc61, &ROff, &RLen, &St, &L) == 1);
  CHECK ((L == RTKBT_EVT_NOHDR) && (ROff == 3) && (RLen == 3));

  // somewhere further in
  memcpy (B, "\x00\x00\x00\x0e\x04\x01\x01\x10\x00", 9);
  CHECK (rtkbt_evt_find (B, 9, 0x1001, &ROff, &RLen, &St, &L) == 1);
  CHECK ((L == RTKBT_EVT_SCAN) && (ROff == 8) && (RLen == 1));

  CHECK (rtkbt_evt_find (B, 0, 0x1001, &ROff, &RLen, &St, &L) == 0);
}

int
main (
  int   Argc,
  char  **Argv
  )
{
  if (Argc != 3) {
    printf ("usage: test-epatch <rtl8852bu_fw.bin> <rtl8852bu_config.bin>\n");
    return 2;
  }

  TestRealFile (Argv[1], Argv[2]);
  TestSynthetic ();
  TestFragEdges ();
  TestEvents ();
  printf ("%s: %d checks, %d failed\n", (mFails == 0) ? "PASS" : "FAIL", mChecks, mFails);
  return (mFails == 0) ? 0 : 1;
}
