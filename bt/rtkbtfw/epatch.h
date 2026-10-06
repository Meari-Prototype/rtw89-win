// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * Realtek Bluetooth patch file handling for rtkbtfw, without any Windows dependency so that the PC-side unit
 * test (test-epatch.c) runs the same code as the board-side tool.
 *
 * The rules follow Linux drivers/bluetooth/btrtl.c (v7.2), read for reference only:
 *  - "Realtech" (v1) epatch: signature[8], fw_version le32, num_patches le16, then chip_id le16[n],
 *    patch_length le16[n], patch_offset le32[n]. The patch whose chip_id is rom_version + 1 is used, with its last
 *    4 bytes replaced by fw_version. "RTBTCore" (v2) files are not handled (rtl8852bu_fw.bin is v1).
 *  - The file ends with the extension signature 51 04 fd 77; walking back from it, each instruction is
 *    opcode, length, data (read backwards); opcode 0 with length 1 carries the project id, 0xff ends the walk.
 *  - A config file (if any and if the controller's key id is 0) is appended after the patch as it is.
 *  - Download: HCI vendor command 0xFC20, parameters = index byte + up to 252 data bytes. There are len / 252 + 1
 *    fragments (so a length that is a multiple of 252 ends with an empty fragment, as in Linux); the index counts
 *    0, 1, ... 0x7f, then continues from 1; the last fragment has bit 7 of the index set.
 *
 * The filter driver: the same file is built into bt\rtkbtflt (kernel mode, _KERNEL_MODE). Nothing here uses the CRT
 * beyond memcmp/memcpy; the malloc-based rtkbt_epatch_build exists in user mode only.
 */
#ifndef RTKBT_EPATCH_H_
#define RTKBT_EPATCH_H_

#include <stddef.h>
#if defined (_KERNEL_MODE)
// the WDK's kernel CRT has no stdint.h (and the compiler's own clashes with it)
typedef unsigned char   uint8_t;
typedef unsigned short  uint16_t;
typedef unsigned int    uint32_t;
#else
#include <stdint.h>
#endif

#define RTKBT_FRAG_LEN            252
#define RTKBT_MAX_PATCHES         16
#define RTKBT_CONFIG_MAGIC        0x8723ab55u

/* the controller this tool is for: 8852B USB (btrtl IC_INFO (0x8852, 0xb, 0xb, HCI_USB)), project id 20 */
#define RTKBT_LMP_SUBVER_8852     0x8852
#define RTKBT_HCI_REV_8852B       0x000b
#define RTKBT_HCI_VER_8852B       0x0b
#define RTKBT_PROJECT_8852B       20
#define RTKBT_MFG_REALTEK         0x005d

/* HCI opcodes used by the tool */
#define RTKBT_OP_DOWNLOAD         0xfc20
#define RTKBT_OP_READ_REG16       0xfc61
#define RTKBT_OP_DROP_PATCH       0xfc66
#define RTKBT_OP_READ_ROM_VER     0xfc6d
#define RTKBT_OP_READ_LOCAL_VER   0x1001

struct rtkbt_epatch {
  uint32_t  fw_version;
  uint16_t  num_patches;
  int       project_id;
  uint16_t  chip_id[RTKBT_MAX_PATCHES];
  uint16_t  length[RTKBT_MAX_PATCHES];
  uint32_t  offset[RTKBT_MAX_PATCHES];
};

/* Parses a v1 epatch file. Returns 0, or -1 with a message in Err. */
int
rtkbt_epatch_parse (
  const uint8_t        *Fw,
  size_t               FwLen,
  struct rtkbt_epatch  *Info,
  char                 *Err,
  size_t               ErrLen
  );

/* Project id -> the LMP subversion of the ROM it belongs to (btrtl's table), or 0 if unknown. */
uint16_t
rtkbt_project_lmp_subver (
  int  ProjectId
  );

/* The controller reports the ROM firmware of the 8852B USB (btrtl's IC_INFO match)? */
int
rtkbt_is_rom_8852b (
  uint16_t  LmpSubver,
  uint16_t  HciRev,
  uint8_t   HciVer
  );

/*
 * The image to download for RomVersion: the patch whose chip id is RomVersion + 1, then CfgLen bytes of config.
 * Returns its length (> 0) and the patch's index, or 0 with a message in Err (no such patch, patch outside the file).
 */
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
  );

/*
 * Builds the image for RomVersion into Out (OutCap bytes): the selected patch with fw_version in its last 4 bytes,
 * then Cfg (may be NULL / 0). Returns 0 (*OutLen = the image length), or -1 with a message in Err.
 */
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
  );

#if !defined (_KERNEL_MODE)

/* rtkbt_epatch_build_into with *Out malloc'ed. Returns 0, or -1 with a message in Err (*Out = NULL). */
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
  );

#endif

/* Checks a config file: magic, total length; returns the number of entries, or -1 with a message in Err. */
int
rtkbt_config_check (
  const uint8_t  *Cfg,
  size_t         CfgLen,
  char           *Err,
  size_t         ErrLen
  );

/* Number of 0xFC20 fragments for an image of Len bytes. */
size_t
rtkbt_frag_count (
  size_t  Len
  );

/* Fragment I (0-based) of an image of Len bytes: the index byte, and the data range. */
void
rtkbt_frag_get (
  size_t   Len,
  size_t   I,
  uint8_t  *Index,
  size_t   *Offset,
  size_t   *FragLen
  );

/* Where the event was found in the IOCTL_BTH_HCI_VENDOR_COMMAND output buffer. */
enum rtkbt_evt_layout {
  RTKBT_EVT_NONE = 0,
  RTKBT_EVT_RAW,          /* the event itself: code, length, parameters */
  RTKBT_EVT_INFO,         /* BTH_VENDOR_EVENT_INFO: address u64, size u32, then the event */
  RTKBT_EVT_SCAN,         /* a consistent event found further into the buffer */
  RTKBT_EVT_NOHDR         /* no event header: Num_HCI_Command_Packets, opcode, return parameters */
};

/*
 * Finds the Command Complete (0x0e) or Command Status (0x0f) event for Opcode in an IOCTL output buffer.
 * Command Complete: returns 1, *RetOff / *RetLen = the return parameters (status first).
 * Command Status:   returns 2, *CmdStatus = its status byte (no return parameters).
 * Not found:        returns 0.
 */
int
rtkbt_evt_find (
  const uint8_t          *Buf,
  size_t                 Len,
  uint16_t               Opcode,
  size_t                 *RetOff,
  size_t                 *RetLen,
  uint8_t                *CmdStatus,
  enum rtkbt_evt_layout  *Layout
  );

/*
 * Buf[0..Len) starts with one raw HCI event (code, parameter length, parameters), as read from the USB interrupt
 * endpoint. Same return values as rtkbt_evt_find for that event alone; 0 also when it is incomplete.
 */
int
rtkbt_evt_match (
  const uint8_t  *Buf,
  size_t         Len,
  uint16_t       Opcode,
  size_t         *RetOff,
  size_t         *RetLen,
  uint8_t        *CmdStatus
  );

#endif
