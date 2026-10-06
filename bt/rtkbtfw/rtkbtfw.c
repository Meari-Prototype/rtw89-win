// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtkbtfw: user-mode patch loader for the Bluetooth half of the RTL8852BE (USB 13D3:3572) under the in-box
 * Windows stack (bth.inf / BTHUSB / bthport).
 *
 * Every vendor command goes through IOCTL_BTH_HCI_VENDOR_COMMAND on the bthport radio interface
 * (GUID_BTHPORT_DEVICE_INTERFACE); that needs an administrator with SE_LOAD_DRIVER_NAME enabled. The download
 * follows Linux btrtl (read for reference only): 0xFC6D reads the ROM version, 0xFC61 reads the security project
 * (key id; the config file is appended only when it is 0), the patch for "ROM version + 1" is sent with 0xFC20 in
 * 252-byte fragments (epatch.c), each acknowledged by a Command Complete with status 0.
 *
 * bthport has already initialised the controller on its ROM firmware and keeps what it read then (the local
 * version, features, its own settings), so after a download the device is restarted (pnputil /restart-device)
 * to make bthport initialise the patched controller again - the counterpart of the HCI_Reset and the standard
 * init that Linux runs after btrtl's setup. The tool then reads the local version once more: lmpSubversion has to
 * be something else than the ROM's 0x8852.
 *
 * Failure policy: before the last fragment has gone out the controller still runs its ROM (the patch is only
 * taken on the fragment with bit 7 set), so nothing is undone and "load" can simply run again (it restarts at
 * index 0). If the last fragment has gone out but its completion is missing or bad, the patch may be running,
 * so the tool sends 0xFC66 (btrtl's "drop the loaded patch") and restarts the device, which leaves the
 * controller on its ROM and bthport consistent with it.
 *
 * Observed: the controller loses the patch at every D3 (radio off, any device restart, the one above included), so
 * "load" cannot leave a patch in place for bthport; the lower filter driver bt\rtkbtflt loads it at every start,
 * before BTHUSB sees the device. "info" also prints the filter's results from Services\rtkbtflt\Parameters; "load"
 * stays as a diagnostic.
 */
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <bcrypt.h>
#include <initguid.h>
#include <bthsdpdef.h>
#include <bthdef.h>
#include <bthioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "epatch.h"

#define RTKBTFW_VERSION   "0.2"

#define EXIT_OK           0
#define EXIT_USAGE        1
#define EXIT_NOCHANGE     2   /* failed before anything was sent that changes the controller */
#define EXIT_DOWNLOAD     3   /* the download failed (see the message for the state left behind) */
#define EXIT_VERIFY       4   /* downloaded, but the restarted radio does not report a patched version */

#define VSC_HDR_LEN       FIELD_OFFSET (BTH_VENDOR_SPECIFIC_COMMAND, Data)

C_ASSERT (VSC_HDR_LEN == 9);
C_ASSERT (sizeof (BTH_COMMAND_HEADER) == 3);

static const uint8_t  mRegSecProj[5] = { 0x10, 0xa4, 0xad, 0x00, 0xb0 };   /* btrtl RTL_SEC_PROJ */

/* how a command's input buffer is sized; see HciCmd */
enum {
  IN_EXACT = 0,     /* header + parameters */
  IN_PADDED,        /* at least sizeof (BTH_VENDOR_SPECIFIC_COMMAND), for commands without parameters */
  IN_PATTERN,       /* header + parameters + one pattern on the Command Complete's opcode */
  IN_MODES
};

static const char  *mInModeName[IN_MODES] = { "exact", "padded", "pattern" };

static struct {
  HANDLE    Radio;
  WCHAR     InstanceId[256];
  WCHAR     Path[512];
  USHORT    Mfg;
  int       Verbose;
  DWORD     TimeoutMs;
  int       ForcePattern;
  int       ModeNoParam;      /* input sizing that worked for commands without parameters, -1 = not known */
  int       ModeParam;        /* the same for commands with parameters */
  int       Privilege;
  uint8_t   IoIn[VSC_HDR_LEN + 255 + 8];
  uint8_t   IoOut[1024];
} g;

/* options */
static struct {
  int           Force;
  int           DryRun;
  int           NoConfig;
  int           NoRestart;
  int           NoDrop;
  int           Rom;          /* -1 = read it from the controller */
  const WCHAR   *Config;
} o;

static void
Hex (
  const char     *Prefix,
  const uint8_t  *P,
  size_t         Len
  )
{
  size_t  i;

  printf ("%s(%zu bytes)", Prefix, Len);
  for (i = 0; i < Len; i++) {
    printf ("%s%02x", ((i % 32) == 0) ? "\n    " : " ", P[i]);
  }

  printf ("\n");
}

static const char *
Win32Text (
  DWORD  Err
  )
{
  static char  Buf[256];
  DWORD        n;

  n = FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Err, 0, Buf, sizeof (Buf), NULL);
  while ((n > 0) && ((Buf[n - 1] == '\n') || (Buf[n - 1] == '\r') || (Buf[n - 1] == ' ') || (Buf[n - 1] == '.'))) {
    Buf[--n] = 0;
  }

  if (n == 0) {
    snprintf (Buf, sizeof (Buf), "Win32 error %lu", Err);
  }

  return Buf;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* files                                                                                                         */

static int
ReadFileAll (
  const WCHAR  *Path,
  uint8_t      **Data,
  size_t       *Len
  )
{
  FILE   *f;
  long   n;

  *Data = NULL;
  *Len = 0;
  if (_wfopen_s (&f, Path, L"rb") != 0) {
    return -1;
  }

  fseek (f, 0, SEEK_END);
  n = ftell (f);
  fseek (f, 0, SEEK_SET);
  if ((n <= 0) || (n > 16 * 1024 * 1024)) {
    fclose (f);
    return -1;
  }

  *Data = malloc ((size_t)n);
  if ((*Data == NULL) || (fread (*Data, 1, (size_t)n, f) != (size_t)n)) {
    free (*Data);
    *Data = NULL;
    fclose (f);
    return -1;
  }

  fclose (f);
  *Len = (size_t)n;
  return 0;
}

static void
Sha256Hex (
  const uint8_t  *P,
  size_t         Len,
  char           Out[65]
  )
{
  uint8_t  H[32];
  int      i;

  strcpy_s (Out, 65, "(sha256 unavailable)");
  if (BCryptHash (BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)P, (ULONG)Len, H, sizeof (H)) == 0) {
    for (i = 0; i < 32; i++) {
      snprintf (Out + 2 * i, 3, "%02x", H[i]);
    }
  }
}

/* <dir of Fw>\<name with "_fw" replaced by "_config">, if Fw's name contains "_fw." */
static int
DefaultConfigPath (
  const WCHAR  *Fw,
  WCHAR        *Out,
  size_t       Cap
  )
{
  const WCHAR  *Name;
  const WCHAR  *Tag;
  size_t       Pre;

  Name = wcsrchr (Fw, L'\\');
  if ((Name == NULL) || ((wcsrchr (Fw, L'/') != NULL) && (wcsrchr (Fw, L'/') > Name))) {
    Name = wcsrchr (Fw, L'/');
  }

  Name = (Name == NULL) ? Fw : Name + 1;
  Tag = wcsstr (Name, L"_fw.");
  if (Tag == NULL) {
    return -1;
  }

  Pre = (size_t)(Tag - Fw);
  if (Pre + wcslen (L"_config") + wcslen (Tag + 3) + 1 > Cap) {
    return -1;
  }

  wcsncpy_s (Out, Cap, Fw, Pre);
  wcscat_s (Out, Cap, L"_config");
  wcscat_s (Out, Cap, Tag + 3);
  return 0;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the radio                                                                                                     */

static int
EnablePrivilege (
  void
  )
{
  HANDLE            Tok;
  TOKEN_PRIVILEGES  Tp;
  DWORD             Err;

  if (g.Privilege) {
    return 0;
  }

  if (!OpenProcessToken (GetCurrentProcess (), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Tok)) {
    printf ("error: OpenProcessToken: %s\n", Win32Text (GetLastError ()));
    return -1;
  }

  Tp.PrivilegeCount = 1;
  Tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValueW (NULL, SE_LOAD_DRIVER_NAME, &Tp.Privileges[0].Luid) ||
      !AdjustTokenPrivileges (Tok, FALSE, &Tp, sizeof (Tp), NULL, NULL)) {
    Err = GetLastError ();
  } else {
    Err = GetLastError ();    // ERROR_NOT_ALL_ASSIGNED when the token does not hold it
  }

  CloseHandle (Tok);
  if (Err != ERROR_SUCCESS) {
    printf ("error: cannot enable SeLoadDriverPrivilege (%s); run from an elevated administrator prompt\n",
      Win32Text (Err));
    return -1;
  }

  g.Privilege = 1;
  return 0;
}

static void
CloseRadio (
  void
  )
{
  if (g.Radio != NULL) {
    CloseHandle (g.Radio);
    g.Radio = NULL;
  }
}

/* Opens the first present bthport radio interface; fills g.Path and g.InstanceId. */
static int
OpenRadio (
  int  Quiet
  )
{
  HDEVINFO                          Set;
  SP_DEVICE_INTERFACE_DATA          If;
  SP_DEVINFO_DATA                   Dev;
  PSP_DEVICE_INTERFACE_DETAIL_DATA_W  Detail;
  BYTE                              DetailBuf[sizeof (SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 1024];
  DWORD                             Err;
  static const DWORD                Access[] = { GENERIC_READ | GENERIC_WRITE, GENERIC_READ, 0 };
  int                               i;

  CloseRadio ();
  Set = SetupDiGetClassDevsW (&GUID_BTHPORT_DEVICE_INTERFACE, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (Set == INVALID_HANDLE_VALUE) {
    if (!Quiet) {
      printf ("error: SetupDiGetClassDevs: %s\n", Win32Text (GetLastError ()));
    }

    return -1;
  }

  If.cbSize = sizeof (If);
  if (!SetupDiEnumDeviceInterfaces (Set, NULL, &GUID_BTHPORT_DEVICE_INTERFACE, 0, &If)) {
    Err = GetLastError ();
    SetupDiDestroyDeviceInfoList (Set);
    if (!Quiet) {
      printf ("error: no Bluetooth radio present (%s)\n", Win32Text (Err));
    }

    return -1;
  }

  Detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)DetailBuf;
  Detail->cbSize = sizeof (*Detail);
  Dev.cbSize = sizeof (Dev);
  if (!SetupDiGetDeviceInterfaceDetailW (Set, &If, Detail, sizeof (DetailBuf), NULL, &Dev) ||
      !SetupDiGetDeviceInstanceIdW (Set, &Dev, g.InstanceId, ARRAYSIZE (g.InstanceId), NULL)) {
    Err = GetLastError ();
    SetupDiDestroyDeviceInfoList (Set);
    if (!Quiet) {
      printf ("error: radio interface details: %s\n", Win32Text (Err));
    }

    return -1;
  }

  wcscpy_s (g.Path, ARRAYSIZE (g.Path), Detail->DevicePath);
  SetupDiDestroyDeviceInfoList (Set);
  for (i = 0; i < ARRAYSIZE (Access); i++) {
    g.Radio = CreateFileW (g.Path, Access[i], FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED, NULL);
    if (g.Radio != INVALID_HANDLE_VALUE) {
      return 0;
    }
  }

  Err = GetLastError ();
  g.Radio = NULL;
  if (!Quiet) {
    printf ("error: opening the radio %ls: %s\n", g.Path, Win32Text (Err));
  }

  return -1;
}

/*
 * One IOCTL with a timeout. A request that times out is cancelled; if it does not complete after the
 * cancel either, the I/O buffers (static) still belong to the kernel, so the process exits.
 */
static DWORD
Ioctl (
  DWORD  Code,
  void   *In,
  DWORD  InLen,
  void   *Out,
  DWORD  OutLen,
  DWORD  *Ret
  )
{
  OVERLAPPED  Ov;
  DWORD       Err;

  *Ret = 0;
  ZeroMemory (&Ov, sizeof (Ov));
  Ov.hEvent = CreateEventW (NULL, TRUE, FALSE, NULL);
  if (Ov.hEvent == NULL) {
    return GetLastError ();
  }

  Err = ERROR_SUCCESS;
  if (!DeviceIoControl (g.Radio, Code, In, InLen, Out, OutLen, NULL, &Ov)) {
    Err = GetLastError ();
  }

  if ((Err == ERROR_SUCCESS) || (Err == ERROR_IO_PENDING)) {
    if (GetOverlappedResultEx (g.Radio, &Ov, Ret, g.TimeoutMs, FALSE)) {
      Err = ERROR_SUCCESS;
    } else {
      Err = GetLastError ();
      if (Err == WAIT_TIMEOUT) {
        CancelIoEx (g.Radio, &Ov);
        if (!GetOverlappedResultEx (g.Radio, &Ov, Ret, 5000, FALSE) && (GetLastError () == WAIT_TIMEOUT)) {
          printf ("fatal: IOCTL 0x%08lx neither completed nor could be cancelled; the Bluetooth stack looks stuck "
            "(restart the device or reboot)\n", Code);
          fflush (stdout);
          ExitProcess (EXIT_DOWNLOAD);
        }

        Err = ERROR_TIMEOUT;
      }
    }
  }

  CloseHandle (Ov.hEvent);
  return Err;
}

static int
GetLocalInfoEx (
  BTH_LOCAL_RADIO_INFO  *Info,
  int                   Quiet
  )
{
  DWORD  Ret;
  DWORD  Err;

  ZeroMemory (Info, sizeof (*Info));
  Err = Ioctl (IOCTL_BTH_GET_LOCAL_INFO, NULL, 0, Info, sizeof (*Info), &Ret);
  if (Err != ERROR_SUCCESS) {
    if (!Quiet) {
      printf ("error: IOCTL_BTH_GET_LOCAL_INFO: %s\n", Win32Text (Err));
    }

    return -1;
  }

  return 0;
}

static int
GetLocalInfo (
  BTH_LOCAL_RADIO_INFO  *Info
  )
{
  return GetLocalInfoEx (Info, 0);
}

static void
PrintLocalInfo (
  const BTH_LOCAL_RADIO_INFO  *Info
  )
{
  ULONGLONG  A;

  A = Info->localInfo.address;
  printf ("  address %02x:%02x:%02x:%02x:%02x:%02x, manufacturer 0x%04x\n", (UINT)((A >> 40) & 0xff),
    (UINT)((A >> 32) & 0xff), (UINT)((A >> 24) & 0xff), (UINT)((A >> 16) & 0xff), (UINT)((A >> 8) & 0xff),
    (UINT)(A & 0xff), Info->radioInfo.mfg);
  printf ("  hciVersion 0x%02x hciRevision 0x%04x lmpVersion 0x%02x lmpSubversion 0x%04x (bthport, read at its "
    "init)\n", Info->hciVersion, Info->hciRevision, Info->radioInfo.lmpVersion, Info->radioInfo.lmpSubversion);
  printf ("  lmpFeatures 0x%016llx, radio flags 0x%lx\n", Info->radioInfo.lmpSupportedFeatures, Info->flags);
}

static int
IsRomState (
  USHORT  Subver,
  USHORT  HciRev,
  UCHAR   HciVer
  )
{
  return (Subver == RTKBT_LMP_SUBVER_8852) && (HciRev == RTKBT_HCI_REV_8852B) && (HciVer == RTKBT_HCI_VER_8852B);
}

/*
 * Sends one HCI command through IOCTL_BTH_HCI_VENDOR_COMMAND and returns the return parameters of its Command
 * Complete event (status byte first) in Ret. Returns 0, or -1 with the reason printed (unless Quiet).
 *
 * The documentation does not say how a command without parameters is to be sized (the header is 9 bytes, the
 * structure 10), so the first such command tries "exact", then "padded", then "pattern" (one BTH_VENDOR_PATTERN
 * on the Command Complete's opcode, offset 1 of the event parameters) while the IOCTL rejects the buffer as
 * invalid or too small; the mode that worked is kept. -Pattern uses the pattern form for everything.
 */
static int
HciCmd (
  USHORT         Opcode,
  const uint8_t  *Params,
  uint8_t        PLen,
  uint8_t        *Ret,
  size_t         RetCap,
  size_t         *RetLen,
  int            Quiet
  )
{
  BTH_VENDOR_SPECIFIC_COMMAND  *Cmd;
  int                          *Known;
  int                          Mode;
  int                          Last;
  DWORD                        InLen;
  DWORD                        OutLen;
  DWORD                        Err;
  size_t                       ROff;
  size_t                       RLen;
  uint8_t                      CmdStatus;
  enum rtkbt_evt_layout        Layout;
  int                          R;

  *RetLen = 0;
  if (EnablePrivilege () != 0) {
    return -1;
  }

  Known = (PLen == 0) ? &g.ModeNoParam : &g.ModeParam;
  Mode = g.ForcePattern ? IN_PATTERN : ((*Known >= 0) ? *Known : IN_EXACT);
  Last = (g.ForcePattern || (*Known >= 0)) ? Mode : IN_PATTERN;
  for (;;) {
    if ((Mode == IN_PADDED) && (PLen > 0)) {
      Mode++;   // the same as exact once there are parameters
      continue;
    }

    ZeroMemory (g.IoIn, sizeof (g.IoIn));
    Cmd = (BTH_VENDOR_SPECIFIC_COMMAND *)g.IoIn;
    Cmd->ManufacturerId = g.Mfg;
    Cmd->LmpVersion = 0;
    Cmd->MatchAnySinglePattern = FALSE;
    Cmd->HciHeader.OpCode = Opcode;
    Cmd->HciHeader.TotalParameterLength = PLen;
    if (PLen > 0) {
      memcpy (Cmd->Data, Params, PLen);
    }

    InLen = VSC_HDR_LEN + PLen;
    if ((Mode == IN_PADDED) && (InLen < sizeof (BTH_VENDOR_SPECIFIC_COMMAND))) {
      InLen = sizeof (BTH_VENDOR_SPECIFIC_COMMAND);
    } else if (Mode == IN_PATTERN) {
      g.IoIn[InLen++] = 1;                        // Offset: after Num_HCI_Command_Packets
      g.IoIn[InLen++] = 2;                        // Size
      g.IoIn[InLen++] = (uint8_t)(Opcode & 0xff);
      g.IoIn[InLen++] = (uint8_t)(Opcode >> 8);
    }

    if (g.Verbose >= 2) {
      printf ("  -> opcode 0x%04x, input %s ", Opcode, mInModeName[Mode]);
      Hex ("", g.IoIn, InLen);
    }

    ZeroMemory (g.IoOut, sizeof (g.IoOut));
    Err = Ioctl (IOCTL_BTH_HCI_VENDOR_COMMAND, g.IoIn, InLen, g.IoOut, sizeof (g.IoOut), &OutLen);
    if ((Err == ERROR_INVALID_PARAMETER) || (Err == ERROR_INSUFFICIENT_BUFFER) || (Err == ERROR_BAD_LENGTH)) {
      if (Mode < Last) {
        if (g.Verbose) {
          printf ("  (opcode 0x%04x: input sizing \"%s\" refused: %s; trying the next)\n", Opcode, mInModeName[Mode],
            Win32Text (Err));
        }

        Mode++;
        continue;
      }
    }

    break;
  }

  if (Err != ERROR_SUCCESS) {
    if (!Quiet) {
      printf ("error: HCI command 0x%04x: IOCTL_BTH_HCI_VENDOR_COMMAND failed: %s (%lu)%s\n", Opcode, Win32Text (Err),
        Err, (Err == ERROR_PRIVILEGE_NOT_HELD) ? " - SeLoadDriverPrivilege is needed" : "");
    }

    return -1;
  }

  if (*Known < 0) {
    *Known = Mode;
    if (g.Verbose) {
      printf ("  (commands %s parameters: input sizing \"%s\")\n", (PLen == 0) ? "without" : "with", mInModeName[Mode]);
    }
  }

  if (g.Verbose >= 2) {
    Hex ("  <- output ", g.IoOut, OutLen);
  }

  R = rtkbt_evt_find (g.IoOut, OutLen, Opcode, &ROff, &RLen, &CmdStatus, &Layout);
  if (R == 2) {
    if (!Quiet) {
      printf ("error: HCI command 0x%04x: Command Status 0x%02x instead of a Command Complete%s\n", Opcode, CmdStatus,
        (CmdStatus != 0) ? " (the controller did not accept it)" : "");
    }

    return -1;
  }

  if (R == 0) {
    if (!Quiet) {
      printf ("error: HCI command 0x%04x: no Command Complete event in the IOCTL output", Opcode);
      Hex (" ", g.IoOut, OutLen);
    }

    return -1;
  }

  if (g.Verbose && (Layout != RTKBT_EVT_RAW)) {
    printf ("  (opcode 0x%04x: event found in layout %d of the output)\n", Opcode, (int)Layout);
  }

  if (RLen > RetCap) {
    RLen = RetCap;
  }

  memcpy (Ret, g.IoOut + ROff, RLen);
  *RetLen = RLen;
  return 0;
}

static int
ReadRomVersion (
  uint8_t  *Rom
  )
{
  uint8_t  R[8];
  size_t   Len;

  if (HciCmd (RTKBT_OP_READ_ROM_VER, NULL, 0, R, sizeof (R), &Len, 0) != 0) {
    return -1;
  }

  if ((Len != 2) || (R[0] != 0)) {
    printf ("error: 0xFC6D (read ROM version): status 0x%02x, %zu return bytes (expected status 0 and 2 bytes)\n",
      (Len > 0) ? R[0] : 0xff, Len);
    return -1;
  }

  *Rom = R[1];
  return 0;
}

static int
ReadKeyId (
  uint8_t  *KeyId
  )
{
  uint8_t  R[8];
  size_t   Len;

  if (HciCmd (RTKBT_OP_READ_REG16, mRegSecProj, sizeof (mRegSecProj), R, sizeof (R), &Len, 0) != 0) {
    return -1;
  }

  if ((Len != 3) || (R[0] != 0)) {
    printf ("error: 0xFC61 (read security project): status 0x%02x, %zu return bytes (expected status 0 and 3 "
      "bytes)\n", (Len > 0) ? R[0] : 0xff, Len);
    return -1;
  }

  *KeyId = R[1];
  return 0;
}

/* HCI_Read_Local_Version_Information sent through the vendor IOCTL; bthport may refuse a non-vendor opcode. */
static int
ReadLiveVersion (
  USHORT  *Subver,
  USHORT  *HciRev,
  UCHAR   *HciVer
  )
{
  uint8_t  R[16];
  size_t   Len;

  if (HciCmd (RTKBT_OP_READ_LOCAL_VER, NULL, 0, R, sizeof (R), &Len, 1) != 0) {
    return -1;
  }

  if ((Len != 9) || (R[0] != 0)) {
    return -1;
  }

  *HciVer = R[1];
  *HciRev = (USHORT)(R[2] | (R[3] << 8));
  *Subver = (USHORT)(R[7] | (R[8] << 8));
  return 0;
}

/* 0xFC66: btrtl's way to make a patched controller drop the patch and run its ROM again (sent, then 200 ms). */
static int
DropPatch (
  void
  )
{
  uint8_t  R[8];
  size_t   Len;
  int      Rc;

  printf ("sending 0xFC66 (drop the loaded patch)\n");
  Rc = HciCmd (RTKBT_OP_DROP_PATCH, NULL, 0, R, sizeof (R), &Len, 0);
  Sleep (200);
  if (Rc != 0) {
    printf ("  0xFC66 did not complete normally (btrtl does not wait for its completion either)\n");
    return -1;
  }

  printf ("  0xFC66 status 0x%02x\n", (Len > 0) ? R[0] : 0xff);
  return 0;
}

/*
 * pnputil /restart-device on the radio's device node, then waits (up to 60 s) for the bthport interface to come
 * back and prints the local version bthport read at its new init. The handle is closed first (an open handle
 * would veto the removal).
 */
static int
RestartRadio (
  BTH_LOCAL_RADIO_INFO  *After
  )
{
  WCHAR                Cmd[600];
  STARTUPINFOW         Si;
  PROCESS_INFORMATION  Pi;
  DWORD                Code;
  int                  i;

  CloseRadio ();
  swprintf_s (Cmd, ARRAYSIZE (Cmd), L"pnputil.exe /restart-device \"%ls\"", g.InstanceId);
  printf ("restarting the device: %ls\n", Cmd);
  fflush (stdout);
  ZeroMemory (&Si, sizeof (Si));
  Si.cb = sizeof (Si);
  if (!CreateProcessW (NULL, Cmd, NULL, NULL, FALSE, 0, NULL, NULL, &Si, &Pi)) {
    printf ("error: starting pnputil: %s\n", Win32Text (GetLastError ()));
    return -1;
  }

  if (WaitForSingleObject (Pi.hProcess, 120000) != WAIT_OBJECT_0) {
    printf ("error: pnputil did not finish within 120 s\n");
    CloseHandle (Pi.hThread);
    CloseHandle (Pi.hProcess);
    return -1;
  }

  GetExitCodeProcess (Pi.hProcess, &Code);
  CloseHandle (Pi.hThread);
  CloseHandle (Pi.hProcess);
  if (Code != 0) {
    printf ("error: pnputil /restart-device exit code %lu (3010 = a reboot is needed)\n", Code);
    return -1;
  }

  for (i = 0; i < 120; i++) {
    Sleep (500);
    if ((OpenRadio (1) == 0) && (GetLocalInfoEx (After, 1) == 0) && (After->radioInfo.lmpSubversion != 0)) {
      printf ("radio back after %.1f s:\n", (i + 1) * 0.5);
      PrintLocalInfo (After);
      return 0;
    }
  }

  printf ("error: the radio did not come back within 60 s of the restart\n");
  return -1;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the filter driver (bt\rtkbtflt)                                                                               */

#define FLT_PARAMS  L"SYSTEM\\CurrentControlSet\\Services\\rtkbtflt\\Parameters"

static int
FilterDword (
  const WCHAR  *Name,
  DWORD        *Value
  )
{
  DWORD  Len;

  Len = sizeof (*Value);
  return (RegGetValueW (HKEY_LOCAL_MACHINE, FLT_PARAMS, Name, RRF_RT_REG_DWORD, NULL, Value, &Len) == ERROR_SUCCESS) ?
         0 : -1;
}

/* Prints a REG_SZ of the filter's Parameters, each line indented; nothing if it is not there. */
static void
FilterText (
  const WCHAR  *Name,
  const char   *Title
  )
{
  static WCHAR  Buf[8192];
  DWORD         Len;
  WCHAR         *Line;
  WCHAR         *Next;

  Len = sizeof (Buf);
  if (RegGetValueW (HKEY_LOCAL_MACHINE, FLT_PARAMS, Name, RRF_RT_REG_SZ, NULL, Buf, &Len) != ERROR_SUCCESS) {
    return;
  }

  printf ("  %s:\n", Title);
  for (Line = Buf; (Line != NULL) && (*Line != 0); Line = Next) {
    Next = wcschr (Line, L'\n');
    if (Next != NULL) {
      *Next++ = 0;
    }

    if ((wcslen (Line) > 0) && (Line[wcslen (Line) - 1] == L'\r')) {
      Line[wcslen (Line) - 1] = 0;
    }

    printf ("    %ls\n", Line);
  }
}

/* The values the filter keeps under Services\rtkbtflt\Parameters (read-only). */
static void
PrintFilter (
  void
  )
{
  static const char  *Results[] = {
    "patched (downloaded)", "already patched (nothing sent)", "not this controller / not its ROM (nothing sent)",
    "download failed, the controller runs its ROM", "state unknown after the last fragment",
    "no answer to HCI_Read_Local_Version", "no usable patch file", "pass-through (PassThrough = 1, guard, or setup)",
    "USB error of the filter", "registry not writable (nothing sent)"
  };
  WCHAR  Ver[64];
  DWORD  Len;
  DWORD  V;
  DWORD  Rev;
  DWORD  Sv;

  Len = sizeof (Ver);
  if (RegGetValueW (HKEY_LOCAL_MACHINE, FLT_PARAMS, L"DriverVersion", RRF_RT_REG_SZ, NULL, Ver, &Len) != ERROR_SUCCESS) {
    printf ("filter rtkbtflt: not installed, or never loaded (no Services\\rtkbtflt\\Parameters\\DriverVersion)\n");
    return;
  }

  printf ("filter rtkbtflt %ls:", Ver);
  if (FilterDword (L"PassThrough", &V) == 0) {
    printf (" PassThrough %lu", V);
  }

  if (FilterDword (L"BadSessions", &V) == 0) {
    printf (", BadSessions %lu", V);
  }

  if (FilterDword (L"SessionRunning", &V) == 0) {
    printf (", SessionRunning %lu", V);
  }

  if (FilterDword (L"Downloads", &V) == 0) {
    printf (", Downloads %lu", V);
  }

  printf ("\n");
  if (FilterDword (L"LastResult", &V) == 0) {
    Rev = 0;
    Sv  = 0;
    (void)FilterDword (L"LastHciRev", &Rev);
    (void)FilterDword (L"LastLmpSubver", &Sv);
    printf ("  last session: result %lu (%s); the controller then: hci_rev 0x%04lx lmp_subver 0x%04lx\n", V,
      (V < ARRAYSIZE (Results)) ? Results[V] : "?", Rev, Sv);
  }

  FilterText (L"SessionStage", "SessionStage");
  FilterText (L"LastGuardRelease", "LastGuardRelease");
  if (g.Verbose) {
    FilterText (L"LastSession", "LastSession");
    FilterText (L"LastDownload", "LastDownload");
    FilterText (L"LastPassThrough", "LastPassThrough");
    FilterText (L"LastBadStage", "LastBadStage (a session that did not end in order)");
    FilterText (L"LastBadSession", "LastBadSession");
  }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* commands                                                                                                      */

static int
CmdInfo (
  void
  )
{
  BTH_LOCAL_RADIO_INFO  Info;
  uint8_t               Rom;
  uint8_t               KeyId;
  USHORT                Sv;
  USHORT                Rev;
  UCHAR                 Ver;

  PrintFilter ();
  if (OpenRadio (0) != 0) {
    return EXIT_NOCHANGE;
  }

  printf ("radio: %ls\n  interface %ls\n", g.InstanceId, g.Path);
  if (GetLocalInfo (&Info) != 0) {
    return EXIT_NOCHANGE;
  }

  PrintLocalInfo (&Info);
  g.Mfg = Info.radioInfo.mfg;
  if (IsRomState (Info.radioInfo.lmpSubversion, Info.hciRevision, Info.hciVersion)) {
    printf ("  state: ROM firmware of the 8852B USB (not patched)\n");
  } else if ((Info.radioInfo.mfg == RTKBT_MFG_REALTEK) && (Info.radioInfo.lmpSubversion != RTKBT_LMP_SUBVER_8852)) {
    printf ("  state: patched (lmpSubversion is not the ROM's 0x8852)\n");
  } else {
    printf ("  state: not the controller this tool is for (8852B USB: mfg 0x005d, subversion 0x8852, HCI "
      "revision 0x000b)\n");
  }

  if (Info.radioInfo.mfg != RTKBT_MFG_REALTEK) {
    printf ("not a Realtek radio: no vendor commands sent\n");
    return EXIT_OK;
  }

  if (ReadRomVersion (&Rom) == 0) {
    printf ("  ROM version (0xFC6D): %u -> patch chip id %u\n", Rom, Rom + 1);
  }

  if (ReadKeyId (&KeyId) == 0) {
    printf ("  security key id (0xFC61 RTL_SEC_PROJ): %u -> config file %s\n", KeyId,
      (KeyId == 0) ? "appended" : "not used");
  }

  if (ReadLiveVersion (&Sv, &Rev, &Ver) == 0) {
    printf ("  live version (0x1001 through the vendor IOCTL): hciVersion 0x%02x hciRevision 0x%04x lmpSubversion "
      "0x%04x\n", Ver, Rev, Sv);
    if ((Sv != Info.radioInfo.lmpSubversion) || (Rev != Info.hciRevision)) {
      printf ("  note: the controller and bthport's copy differ: bthport has not initialised the current firmware "
        "(rtkbtfw restart)\n");
    }
  } else {
    printf ("  live version: 0x1001 not available through the vendor IOCTL (only bthport's copy above)\n");
  }

  return EXIT_OK;
}

static void
PrintPlan (
  size_t  Len
  )
{
  size_t   Count;
  size_t   i;
  uint8_t  Idx;
  size_t   Off;
  size_t   FLen;

  Count = rtkbt_frag_count (Len);
  printf ("plan: %zu bytes in %zu fragments of 0xFC20 (%u data bytes each, the last %zu)\n", Len, Count,
    RTKBT_FRAG_LEN, Len % RTKBT_FRAG_LEN);
  for (i = 0; i < Count; i++) {
    if (!g.Verbose && (i > 2) && !((i >= 126) && (i <= 129)) && (i + 3 < Count)) {
      if (i == 3) {
        printf ("  ...\n");
      }

      continue;
    }

    rtkbt_frag_get (Len, i, &Idx, &Off, &FLen);
    printf ("  fragment %3zu: index 0x%02x, data %6zu..%6zu (%zu bytes)%s\n", i, Idx, Off, Off + FLen, FLen,
      ((Idx & 0x80) != 0) ? ", last" : "");
  }
}

/* Loads the firmware (and config) files, parses and checks them. */
static int
LoadFiles (
  const WCHAR          *FwPath,
  uint8_t              **Fw,
  size_t               *FwLen,
  struct rtkbt_epatch  *Ep,
  uint8_t              **Cfg,
  size_t               *CfgLen
  )
{
  char         Err[160];
  char         Sha[65];
  WCHAR        CfgPath[MAX_PATH];
  const WCHAR  *CPath;
  int          Entries;
  uint16_t     i;

  *Cfg = NULL;
  *CfgLen = 0;
  if (ReadFileAll (FwPath, Fw, FwLen) != 0) {
    printf ("error: cannot read %ls\n", FwPath);
    return -1;
  }

  Sha256Hex (*Fw, *FwLen, Sha);
  printf ("firmware %ls: %zu bytes, sha256 %s\n", FwPath, *FwLen, Sha);
  if (rtkbt_epatch_parse (*Fw, *FwLen, Ep, Err, sizeof (Err)) != 0) {
    printf ("error: %ls: %s\n", FwPath, Err);
    return -1;
  }

  printf ("  epatch v1, fw_version 0x%08x, project id %d (ROM subversion 0x%04x), %u patches\n", Ep->fw_version,
    Ep->project_id, rtkbt_project_lmp_subver (Ep->project_id), Ep->num_patches);
  for (i = 0; i < Ep->num_patches; i++) {
    printf ("    patch %u: chip id %u (ROM version %d), %u bytes at offset %u\n", i, Ep->chip_id[i],
      Ep->chip_id[i] - 1, Ep->length[i], Ep->offset[i]);
  }

  if (Ep->project_id != RTKBT_PROJECT_8852B) {
    printf ("error: the file is for project id %d, this tool only loads the 8852B (project id %d)\n", Ep->project_id,
      RTKBT_PROJECT_8852B);
    return -1;
  }

  if (o.NoConfig) {
    printf ("config: not used (-NoConfig)\n");
    return 0;
  }

  CPath = o.Config;
  if ((CPath == NULL) && (DefaultConfigPath (FwPath, CfgPath, ARRAYSIZE (CfgPath)) == 0) &&
      (GetFileAttributesW (CfgPath) != INVALID_FILE_ATTRIBUTES)) {
    CPath = CfgPath;
  }

  if (CPath == NULL) {
    printf ("config: none found next to the firmware (btrtl then sends the patch alone)\n");
    return 0;
  }

  if (ReadFileAll (CPath, Cfg, CfgLen) != 0) {
    printf ("error: cannot read the config %ls\n", CPath);
    return -1;
  }

  Entries = rtkbt_config_check (*Cfg, *CfgLen, Err, sizeof (Err));
  Sha256Hex (*Cfg, *CfgLen, Sha);
  if (Entries < 0) {
    printf ("error: config %ls: %s\n", CPath, Err);
    return -1;
  }

  printf ("config %ls: %zu bytes, %d entries, sha256 %s\n", CPath, *CfgLen, Entries, Sha);
  return 0;
}

static int
CmdParse (
  const WCHAR  *FwPath
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
  uint16_t             i;

  if (LoadFiles (FwPath, &Fw, &FwLen, &Ep, &Cfg, &CfgLen) != 0) {
    return EXIT_USAGE;
  }

  for (i = 0; i < Ep.num_patches; i++) {
    if ((o.Rom >= 0) && (Ep.chip_id[i] != o.Rom + 1)) {
      continue;
    }

    if (rtkbt_epatch_build (Fw, FwLen, &Ep, (uint8_t)(Ep.chip_id[i] - 1), Cfg, CfgLen, &Img, &ImgLen, &Sel, Err,
          sizeof (Err)) != 0) {
      printf ("ROM version %d: %s\n", Ep.chip_id[i] - 1, Err);
      continue;
    }

    printf ("\nROM version %d: patch %d + config %zu bytes = %zu bytes, ends with %02x %02x %02x %02x before the "
      "config\n", Ep.chip_id[i] - 1, Sel, CfgLen, ImgLen, Img[ImgLen - CfgLen - 4], Img[ImgLen - CfgLen - 3],
      Img[ImgLen - CfgLen - 2], Img[ImgLen - CfgLen - 1]);
    PrintPlan (ImgLen);
    free (Img);
  }

  free (Fw);
  free (Cfg);
  return EXIT_OK;
}

static int
CmdLoad (
  const WCHAR  *FwPath
  )
{
  uint8_t               *Fw;
  uint8_t               *Cfg;
  uint8_t               *Img;
  size_t                FwLen;
  size_t                CfgLen;
  size_t                ImgLen;
  struct rtkbt_epatch   Ep;
  BTH_LOCAL_RADIO_INFO  Info;
  BTH_LOCAL_RADIO_INFO  After;
  char                  Err[160];
  uint8_t               Rom;
  uint8_t               KeyId;
  USHORT                Sv;
  USHORT                Rev;
  UCHAR                 Ver;
  int                   Live;
  int                   RomOk;
  int                   Sel;
  size_t                Count;
  size_t                i;
  uint8_t               Idx;
  size_t                Off;
  size_t                FLen;
  uint8_t               Param[1 + RTKBT_FRAG_LEN];
  uint8_t               R[8];
  size_t                RLen;
  DWORD                 T0;
  int                   IdxWarned;

  if (LoadFiles (FwPath, &Fw, &FwLen, &Ep, &Cfg, &CfgLen) != 0) {
    return EXIT_USAGE;
  }

  //
  // -DryRun -Rom N: nothing is opened at all (works on a PC without the radio).
  //
  if (o.DryRun && (o.Rom >= 0)) {
    if (rtkbt_epatch_build (Fw, FwLen, &Ep, (uint8_t)o.Rom, Cfg, CfgLen, &Img, &ImgLen, &Sel, Err,
          sizeof (Err)) != 0) {
      printf ("error: %s\n", Err);
      return EXIT_NOCHANGE;
    }

    printf ("dry run for ROM version %d (given, the radio is not touched): patch %d, %zu bytes with the config\n",
      o.Rom, Sel, ImgLen);
    PrintPlan (ImgLen);
    return EXIT_OK;
  }

  if (OpenRadio (0) != 0) {
    return EXIT_NOCHANGE;
  }

  printf ("radio: %ls\n", g.InstanceId);
  if (GetLocalInfo (&Info) != 0) {
    return EXIT_NOCHANGE;
  }

  PrintLocalInfo (&Info);
  g.Mfg = Info.radioInfo.mfg;
  if (g.Mfg != RTKBT_MFG_REALTEK) {
    printf ("error: manufacturer 0x%04x is not Realtek (0x005d); nothing sent\n", g.Mfg);
    return EXIT_NOCHANGE;
  }

  //
  // 0xFC6D first: a plain read-only vendor command, which also settles how commands without parameters are
  // sized before the less certain 0x1001 is tried.
  //
  RomOk = (ReadRomVersion (&Rom) == 0);
  if (RomOk) {
    printf ("ROM version (0xFC6D): %u\n", Rom);
  }

  Sv = Info.radioInfo.lmpSubversion;
  Rev = Info.hciRevision;
  Ver = Info.hciVersion;
  Live = (ReadLiveVersion (&Sv, &Rev, &Ver) == 0);
  if (Live) {
    printf ("  live version (0x1001): hciVersion 0x%02x hciRevision 0x%04x lmpSubversion 0x%04x\n", Ver, Rev, Sv);
  } else {
    printf ("  (0x1001 not available through the vendor IOCTL: going by bthport's copy)\n");
  }

  if (!IsRomState (Sv, Rev, Ver)) {
    if (Sv == RTKBT_LMP_SUBVER_8852) {
      printf ("error: subversion 0x8852 but HCI revision 0x%04x / version 0x%02x: not the 8852B USB this file is "
        "for (0x000c would be the 8852BT, rtl8852btu_fw.bin); nothing sent\n", Rev, Ver);
      return EXIT_NOCHANGE;
    }

    if (!o.Force) {
      printf ("already patched (lmpSubversion 0x%04x, hciRevision 0x%04x): nothing to do (-Force drops the patch "
        "and downloads again)\n", Sv, Rev);
      if (Info.radioInfo.lmpSubversion == RTKBT_LMP_SUBVER_8852) {
        printf ("note: bthport still holds the ROM's version from its init: restart the device (rtkbtfw restart) so "
          "that it initialises the patched controller\n");
      }

      return EXIT_OK;
    }

    if (o.DryRun) {
      printf ("dry run: would send 0xFC66 (drop the patch) and download again\n");
    } else if (DropPatch () != 0) {
      printf ("error: could not drop the running patch; nothing downloaded\n");
      return EXIT_DOWNLOAD;
    } else if ((ReadLiveVersion (&Sv, &Rev, &Ver) == 0) && !IsRomState (Sv, Rev, Ver)) {
      printf ("error: after 0xFC66 the controller still reports lmpSubversion 0x%04x hciRevision 0x%04x\n", Sv, Rev);
      return EXIT_DOWNLOAD;
    } else {
      // as btrtl: the ROM version that counts is the one read on the ROM, after the drop
      RomOk = (ReadRomVersion (&Rom) == 0);
      if (RomOk) {
        printf ("ROM version (0xFC6D, after the drop): %u\n", Rom);
      }
    }
  }

  if (!RomOk) {
    printf ("error: no ROM version, so no patch can be chosen; nothing downloaded\n");
    return EXIT_NOCHANGE;
  }

  if (ReadKeyId (&KeyId) != 0) {
    return EXIT_NOCHANGE;
  }

  printf ("security key id (0xFC61): %u%s\n", KeyId, ((KeyId != 0) && (CfgLen > 0)) ? " -> config not appended" : "");
  if (KeyId != 0) {
    CfgLen = 0;
  }

  if (rtkbt_epatch_build (Fw, FwLen, &Ep, Rom, Cfg, CfgLen, &Img, &ImgLen, &Sel, Err, sizeof (Err)) != 0) {
    printf ("error: %s\n", Err);
    return EXIT_NOCHANGE;
  }

  printf ("image: patch %d (chip id %u, %u bytes) + config %zu bytes = %zu bytes; a patched controller usually "
    "reports hciRevision 0x%04x lmpSubversion 0x%04x (fw_version)\n", Sel, Ep.chip_id[Sel], Ep.length[Sel], CfgLen,
    ImgLen, Ep.fw_version >> 16, Ep.fw_version & 0xffff);
  if (o.DryRun) {
    PrintPlan (ImgLen);
    printf ("dry run: nothing downloaded\n");
    return EXIT_OK;
  }

  //
  // The download.
  //
  Count = rtkbt_frag_count (ImgLen);
  printf ("downloading %zu fragments\n", Count);
  fflush (stdout);
  T0 = GetTickCount ();
  IdxWarned = 0;
  for (i = 0; i < Count; i++) {
    rtkbt_frag_get (ImgLen, i, &Idx, &Off, &FLen);
    Param[0] = Idx;
    memcpy (Param + 1, Img + Off, FLen);
    if ((HciCmd (RTKBT_OP_DOWNLOAD, Param, (uint8_t)(1 + FLen), R, sizeof (R), &RLen, 0) != 0) ||
        (RLen < 1) || (R[0] != 0)) {
      if (RLen >= 1) {
        printf ("error: fragment %zu (index 0x%02x): status 0x%02x\n", i, Idx, R[0]);
      } else {
        printf ("error: fragment %zu (index 0x%02x) failed\n", i, Idx);
      }

      if (i + 1 < Count) {
        printf ("the last fragment was not sent: the controller still runs its ROM firmware and bthport's state "
          "is unchanged. Run \"load\" again (it starts over at index 0); if that keeps failing, restart the device "
          "(rtkbtfw restart) or power the board off and on.\n");
        return EXIT_DOWNLOAD;
      }

      printf ("the last fragment went out without a good completion: the patch may be running.\n");
      if (o.NoDrop) {
        printf ("-NoDrop: leaving it as it is; run \"rtkbtfw info\" and restart the device\n");
        return EXIT_DOWNLOAD;
      }

      DropPatch ();
      if (!o.NoRestart) {
        RestartRadio (&After);
      }

      return EXIT_DOWNLOAD;
    }

    if ((RLen >= 2) && (R[1] != Idx) && !IdxWarned) {
      printf ("note: fragment %zu: the completion echoes index 0x%02x for 0x%02x (btrtl does not check it)\n", i, R[1],
        Idx);
      IdxWarned = 1;
    }

    if (((i + 1) % 32 == 0) || (i + 1 == Count)) {
      printf ("  %zu/%zu\n", i + 1, Count);
      fflush (stdout);
    }
  }

  printf ("download done in %lu ms\n", GetTickCount () - T0);
  if (ReadLiveVersion (&Sv, &Rev, &Ver) == 0) {
    printf ("live version now: hciVersion 0x%02x hciRevision 0x%04x lmpSubversion 0x%04x (btrtl's \"fw version "
      "0x%04x%04x\")\n", Ver, Rev, Sv, Rev, Sv);
  }

  free (Img);
  free (Fw);
  free (Cfg);
  if (o.NoRestart) {
    printf ("-NoRestart: bthport still holds the ROM's version and settings; restart the device (rtkbtfw restart) "
      "before using Bluetooth\n");
    return EXIT_OK;
  }

  if (RestartRadio (&After) != 0) {
    return EXIT_VERIFY;
  }

  if (After.radioInfo.lmpSubversion == RTKBT_LMP_SUBVER_8852) {
    printf ("FAILED: after the restart the radio reports the ROM's lmpSubversion 0x8852 again - the patch did not "
      "survive the device restart (or was not taken)\n");
    return EXIT_VERIFY;
  }

  printf ("OK: patched, lmpSubversion 0x%04x hciRevision 0x%04x\n", After.radioInfo.lmpSubversion, After.hciRevision);
  return EXIT_OK;
}

static int
CmdDrop (
  void
  )
{
  BTH_LOCAL_RADIO_INFO  Info;

  if ((OpenRadio (0) != 0) || (GetLocalInfo (&Info) != 0)) {
    return EXIT_NOCHANGE;
  }

  g.Mfg = Info.radioInfo.mfg;
  if (g.Mfg != RTKBT_MFG_REALTEK) {
    printf ("error: not a Realtek radio\n");
    return EXIT_NOCHANGE;
  }

  DropPatch ();
  if (!o.NoRestart) {
    return (RestartRadio (&Info) == 0) ? EXIT_OK : EXIT_VERIFY;
  }

  return EXIT_OK;
}

static int
CmdRestart (
  void
  )
{
  BTH_LOCAL_RADIO_INFO  Info;

  if (OpenRadio (0) != 0) {
    return EXIT_NOCHANGE;
  }

  return (RestartRadio (&Info) == 0) ? EXIT_OK : EXIT_VERIFY;
}

static int
HexByte (
  const WCHAR  *S,
  uint8_t      *Out,
  size_t       Cap,
  size_t       *Len
  )
{
  unsigned  V;

  while (*S != 0) {
    if ((*S == L' ') || (*S == L':') || (*S == L',')) {
      S++;
      continue;
    }

    if ((swscanf_s (S, L"%2x", &V) != 1) || !iswxdigit (S[0]) || !iswxdigit (S[1]) || (*Len >= Cap)) {
      return -1;
    }

    Out[(*Len)++] = (uint8_t)V;
    S += 2;
  }

  return 0;
}

/* cmd <opcode> [hex bytes...]: one raw command, for experiments on the board. */
static int
CmdRaw (
  int    Argc,
  WCHAR  **Argv
  )
{
  BTH_LOCAL_RADIO_INFO  Info;
  uint8_t               P[255];
  size_t                PLen;
  uint8_t               R[260];
  size_t                RLen;
  unsigned long         Op;
  int                   i;

  if (Argc < 1) {
    printf ("usage: rtkbtfw cmd <opcode> [hex bytes...]\n");
    return EXIT_USAGE;
  }

  Op = wcstoul (Argv[0], NULL, 16);
  PLen = 0;
  for (i = 1; i < Argc; i++) {
    if (HexByte (Argv[i], P, sizeof (P), &PLen) != 0) {
      printf ("error: bad hex bytes \"%ls\"\n", Argv[i]);
      return EXIT_USAGE;
    }
  }

  if ((Op == 0) || (Op > 0xffff)) {
    printf ("error: bad opcode\n");
    return EXIT_USAGE;
  }

  if ((OpenRadio (0) != 0) || (GetLocalInfo (&Info) != 0)) {
    return EXIT_NOCHANGE;
  }

  g.Mfg = Info.radioInfo.mfg;
  if (g.Verbose < 2) {
    g.Verbose = 2;
  }

  if (HciCmd ((USHORT)Op, P, (uint8_t)PLen, R, sizeof (R), &RLen, 0) != 0) {
    return EXIT_DOWNLOAD;
  }

  Hex ("return parameters ", R, RLen);
  return EXIT_OK;
}

static void
Usage (
  void
  )
{
  printf (
    "rtkbtfw " RTKBTFW_VERSION " - Realtek 8852B USB Bluetooth patch loader for the in-box Windows stack\n"
    "usage (elevated; vendor commands need SeLoadDriverPrivilege):\n"
    "  rtkbtfw info                 local version (bthport), ROM version (0xFC6D), key id (0xFC61), and the\n"
    "                               rtkbtflt filter's last result (-v: its session logs)\n"
    "  rtkbtfw load <fw.bin>        download if the controller runs its ROM, restart the device, verify\n"
    "  rtkbtfw parse <fw.bin>       parse the file and print the download plan for each ROM version (no radio)\n"
    "  rtkbtfw drop                 0xFC66 (drop the patch), then restart the device\n"
    "  rtkbtfw restart              pnputil /restart-device on the radio, wait, print the local version\n"
    "  rtkbtfw cmd <op> [hex..]     one raw HCI command through the vendor IOCTL, raw output printed\n"
    "options:\n"
    "  -Config <file>   config to append (default: <fw name with _fw replaced by _config> if present)\n"
    "  -NoConfig        send the patch alone\n"
    "  -Force           load: drop a running patch (0xFC66) and download again\n"
    "  -DryRun          load: only the read-only queries and the plan; with -Rom N the radio is not opened\n"
    "  -Rom <n>         parse, load -DryRun: plan for ROM version n without opening the radio\n"
    "  -NoRestart       load/drop: do not restart the device afterwards\n"
    "  -NoDrop          load: no 0xFC66 when the last fragment fails\n"
    "  -Pattern         size every command with a Command Complete pattern\n"
    "  -Timeout <ms>    per command (default 5000)\n"
    "  -v, -vv          more output (-vv: every command and IOCTL output in hex)\n"
    "exit codes: 0 ok or nothing to do, 1 usage/file, 2 nothing changed, 3 download failed, 4 not patched after "
    "the restart\n");
}

int
wmain (
  int    Argc,
  WCHAR  **Argv
  )
{
  WCHAR        *Pos[64];
  int          NPos;
  int          i;
  const WCHAR  *A;
  int          Rc;

  setvbuf (stdout, NULL, _IONBF, 0);
  g.TimeoutMs = 5000;
  g.ModeNoParam = -1;
  g.ModeParam = -1;
  o.Rom = -1;
  NPos = 0;
  for (i = 1; i < Argc; i++) {
    A = Argv[i];
    if (((A[0] == L'-') || (A[0] == L'/')) && (NPos == 0 || _wcsicmp (Pos[0], L"cmd") != 0)) {
      A += (A[1] == L'-') ? 2 : 1;
      if (_wcsicmp (A, L"Force") == 0) {
        o.Force = 1;
      } else if (_wcsicmp (A, L"DryRun") == 0) {
        o.DryRun = 1;
      } else if (_wcsicmp (A, L"NoConfig") == 0) {
        o.NoConfig = 1;
      } else if (_wcsicmp (A, L"NoRestart") == 0) {
        o.NoRestart = 1;
      } else if (_wcsicmp (A, L"NoDrop") == 0) {
        o.NoDrop = 1;
      } else if (_wcsicmp (A, L"Pattern") == 0) {
        g.ForcePattern = 1;
      } else if (_wcsicmp (A, L"v") == 0) {
        g.Verbose = 1;
      } else if (_wcsicmp (A, L"vv") == 0) {
        g.Verbose = 2;
      } else if ((_wcsicmp (A, L"Config") == 0) && (i + 1 < Argc)) {
        o.Config = Argv[++i];
      } else if ((_wcsicmp (A, L"Rom") == 0) && (i + 1 < Argc)) {
        o.Rom = (int)wcstoul (Argv[++i], NULL, 0);
        if (o.Rom > 254) {
          Usage ();
          return EXIT_USAGE;
        }
      } else if ((_wcsicmp (A, L"Timeout") == 0) && (i + 1 < Argc)) {
        g.TimeoutMs = wcstoul (Argv[++i], NULL, 0);
      } else if ((_wcsicmp (A, L"h") == 0) || (_wcsicmp (A, L"help") == 0) || (_wcsicmp (A, L"?") == 0)) {
        Usage ();
        return EXIT_OK;
      } else {
        printf ("unknown option %ls\n", Argv[i]);
        Usage ();
        return EXIT_USAGE;
      }
    } else if (NPos < ARRAYSIZE (Pos)) {
      Pos[NPos++] = Argv[i];
    }
  }

  if (NPos == 0) {
    Usage ();
    return EXIT_USAGE;
  }

  if (g.TimeoutMs == 0) {
    g.TimeoutMs = 5000;
  }

  // a given ROM version only ever drives a plan, never a real download
  if ((o.Rom >= 0) && !o.DryRun && (_wcsicmp (Pos[0], L"parse") != 0)) {
    printf ("error: -Rom only goes with parse or load -DryRun\n");
    return EXIT_USAGE;
  }

  if (_wcsicmp (Pos[0], L"info") == 0) {
    Rc = CmdInfo ();
  } else if ((_wcsicmp (Pos[0], L"load") == 0) && (NPos == 2)) {
    Rc = CmdLoad (Pos[1]);
  } else if ((_wcsicmp (Pos[0], L"parse") == 0) && (NPos == 2)) {
    Rc = CmdParse (Pos[1]);
  } else if (_wcsicmp (Pos[0], L"drop") == 0) {
    Rc = CmdDrop ();
  } else if (_wcsicmp (Pos[0], L"restart") == 0) {
    Rc = CmdRestart ();
  } else if (_wcsicmp (Pos[0], L"cmd") == 0) {
    Rc = CmdRaw (NPos - 1, Pos + 1);
  } else {
    Usage ();
    return EXIT_USAGE;
  }

  CloseRadio ();
  return Rc;
}
