// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * Diagnostic IOCTLs on \\.\rtw89win (see rtw89win_ioctl.h), the control device of driver.c. LOG_READ, READ32,
 * BSS_LIST and MODE only read and may run at any time; PROBE and REMOVE start and stop rtw89 (probe.c),
 * START/STOP/MONITOR/SCAN drive the mac layer (rtw_linux_* in glue.h). Those control IOCTLs take Dev->CtlLock, so
 * they run one at a time and never overlap EvtDeviceReleaseHardware. The queue is parallel and passive-level, so a
 * long scan does not hold up log reads. TXPROBE is a control IOCTL as well.
 * When the device runs WiFiCx (armed), the front end owns rtw89 and the mac layer, so the control IOCTLs are
 * refused with -EBUSY (STATUS_DEVICE_BUSY for REMOVE and STOP, which have no output).
 * LINK, RADIO, BTC, AP and SER work in both modes under CtlLock, so the mac layer cannot go away while they read it.
 * BSS_IES may run at any time (the BSS table is static); MODE appends the per-message WDI counters when the caller
 * made room for them.
 */
#include "rtw89win.h"
#include "rtw89win_ioctl.h"
#include "../compat/include/rtw/glue.h"

C_ASSERT (sizeof (struct rtw_scan_param) == 12);
C_ASSERT (sizeof (struct rtw_scan_result) == 16);
C_ASSERT (sizeof (struct rtw_bss_list) == 40);
C_ASSERT (sizeof (struct rtw_bss) == 64);
C_ASSERT (FIELD_OFFSET (struct rtw_bss, ssid) == 24);
C_ASSERT (FIELD_OFFSET (struct rtw_bss, age_ms) == 56);
C_ASSERT (sizeof (RTW_MODE_OUT) == 48);
C_ASSERT (sizeof (struct rtw_txprobe_param) == 12);
C_ASSERT (sizeof (struct rtw_txprobe_result) == 40);
C_ASSERT (sizeof (struct rtw_link_stats) == 288);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, phy) == 256);        // tools\rtwdiag.ps1 link reads these offsets
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, rx_amsdu) == 264);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, tx_kbps) == 284);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, mfp) == 192);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, mgmt_protected_tx) == 196);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, tx_data) == 16);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, tx_protected) == 112);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, auth) == 152);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, restarts) == 156);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, rx_reordered) == 160);
C_ASSERT (FIELD_OFFSET (struct rtw_link_stats, ba_rx_tids) == 188);
C_ASSERT (sizeof (struct rtw_radio_info) == 200);
C_ASSERT (FIELD_OFFSET (struct rtw_radio_info, reg_changes) == 24);
C_ASSERT (FIELD_OFFSET (struct rtw_radio_info, ps_changes) == 36);
C_ASSERT (FIELD_OFFSET (struct rtw_radio_info, bt_fw_ver) == 48);
C_ASSERT (FIELD_OFFSET (struct rtw_radio_info, btc_tdma) == 60);
C_ASSERT (FIELD_OFFSET (struct rtw_radio_info, btc_c2h) == 76);
C_ASSERT (FIELD_OFFSET (struct rtw_radio_info, btc_notify) == 104);
C_ASSERT (sizeof (struct rtw_bss_rec) == 32);
C_ASSERT (sizeof (RTW_WDI_COUNTS) == 2048);
C_ASSERT (sizeof (struct rtw_ap_stats) == 340);
C_ASSERT (FIELD_OFFSET (struct rtw_ap_stats, clients) == 148);
C_ASSERT (sizeof (struct rtw_ap_client) == 24);
C_ASSERT (sizeof (RTW_WFD_OUT) == 32);

#define EBUSY   16

/* the shortest LINK output accepted: an older rtwdiag.ps1 asks for the original 160-byte layout */
#define RTW_LINK_MIN_OUT   160

/* Input buffer of exactly Len bytes. */
static NTSTATUS
GetInput (
  _In_ WDFREQUEST  Request,
  _In_ size_t      Len,
  _Out_ PVOID      *Buf
  )
{
  size_t    Got;
  NTSTATUS  Status;

  Status = WdfRequestRetrieveInputBuffer (Request, Len, Buf, &Got);
  if (NT_SUCCESS (Status) && (Got != Len)) {
    Status = STATUS_INVALID_BUFFER_SIZE;
  }

  return Status;
}

static NTSTATUS
LogRead (
  _In_ WDFREQUEST  Request,
  _Out_ size_t     *Info
  )
{
  ULONG64           *From;
  ULONG64           Pos;
  RTW_LOG_READ_OUT  *Out;
  size_t            OutLen;
  ULONG             Copied;
  NTSTATUS          Status;

  Status = WdfRequestRetrieveInputBuffer (Request, sizeof (ULONG64), (PVOID *)&From, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  // METHOD_BUFFERED: input and output share the system buffer, so take From before writing.
  Pos    = *From;
  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (RTW_LOG_READ_OUT) + 1, (PVOID *)&Out, &OutLen);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  OutLen = min (OutLen, 0x100000);
  RtwLogRead (&Pos, &Out->Dropped, (PCHAR)(Out + 1), (ULONG)(OutLen - sizeof (*Out)), &Copied);
  Out->Next = Pos;
  *Info     = sizeof (*Out) + Copied;
  return STATUS_SUCCESS;
}

static NTSTATUS
Read32 (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  ULONG     *In;
  ULONG     *Out;
  ULONG     Offset;
  NTSTATUS  Status;

  Status = WdfRequestRetrieveInputBuffer (Request, sizeof (ULONG), (PVOID *)&In, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Offset = *In;
  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (ULONG), (PVOID *)&Out, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Status = RtwHwRead32Checked (Dev, Offset, Out);
  if (NT_SUCCESS (Status)) {
    *Info = sizeof (ULONG);
  }

  return Status;
}

static NTSTATUS
Probe (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  LONG      *Out;
  NTSTATUS  Status;

  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (LONG), (PVOID *)&Out, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtwLog ("IOCTL probe");
  *Out = RtwProbe (Dev);
  RtwLog ("IOCTL probe: %d", *Out);
  *Info = sizeof (LONG);
  return STATUS_SUCCESS;
}

static NTSTATUS
Start (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  LONG      *Out;
  NTSTATUS  Status;

  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (LONG), (PVOID *)&Out, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtwLog ("IOCTL start");
  *Out = Dev->Probed ? rtw_linux_start () : -19;   // -ENODEV
  RtwLog ("IOCTL start: %d", *Out);
  *Info = sizeof (LONG);
  return STATUS_SUCCESS;
}

static NTSTATUS
Monitor (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  ULONG     *In;
  ULONG     Channel;
  LONG      *Out;
  NTSTATUS  Status;

  Status = GetInput (Request, sizeof (ULONG), (PVOID *)&In);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Channel = *In;
  Status  = WdfRequestRetrieveOutputBuffer (Request, sizeof (LONG), (PVOID *)&Out, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtwLog ("IOCTL monitor %u", Channel);
  *Out = Dev->Probed ? rtw_linux_monitor (Channel) : -19;   // -ENODEV
  RtwLog ("IOCTL monitor: %d", *Out);
  *Info = sizeof (LONG);
  return STATUS_SUCCESS;
}

static NTSTATUS
Scan (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  struct rtw_scan_param   *In;
  struct rtw_scan_param   Param;
  struct rtw_scan_result  *Out;
  struct rtw_scan_result  Result;
  NTSTATUS                Status;

  Status = GetInput (Request, sizeof (Param), (PVOID *)&In);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Param  = *In;       // the output shares the system buffer
  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (Result), (PVOID *)&Out, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtwLog ("IOCTL scan: flags %#x, dwell %u ms, timeout %u ms", Param.flags, Param.dwell_ms, Param.timeout_ms);
  RtlZeroMemory (&Result, sizeof (Result));
  if (Dev->Probed) {
    rtw_linux_scan (&Param, &Result);
  } else {
    Result.result = -19;   // -ENODEV
  }

  RtwLog ("IOCTL scan: %d, %u ms, %u BSS%s", Result.result, Result.duration_ms, Result.bss_count,
          Result.aborted ? ", aborted" : "");
  *Out  = Result;
  *Info = sizeof (Result);
  return STATUS_SUCCESS;
}

static NTSTATUS
BssList (
  _In_ WDFREQUEST  Request,
  _Out_ size_t     *Info
  )
{
  struct rtw_bss_list  *Out;
  size_t               OutLen;
  ULONG                Max;
  ULONG                n;
  NTSTATUS             Status;

  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (*Out), (PVOID *)&Out, &OutLen);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Max   = (ULONG)min ((OutLen - sizeof (*Out)) / sizeof (struct rtw_bss), 4096);
  n     = rtw_linux_bss_get (Out, (struct rtw_bss *)(Out + 1), Max);
  *Info = sizeof (*Out) + (size_t)n * sizeof (struct rtw_bss);
  return STATUS_SUCCESS;
}

static NTSTATUS
Mode (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  RTW_MODE_OUT    *Out;
  RTW_WDI_COUNTS  *Counts;
  size_t          OutLen;
  NTSTATUS        Status;

  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (*Out), (PVOID *)&Out, &OutLen);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  // the per-message counters follow when the caller made room for them
  Counts = (OutLen >= sizeof (*Out) + sizeof (*Counts)) ? (RTW_WDI_COUNTS *)(Out + 1) : NULL;
  RtlZeroMemory (Out, sizeof (*Out));
  if (Counts != NULL) {
    RtlZeroMemory (Counts, sizeof (*Counts));
  }

  Out->AdapterPort = MAXULONG;
  if (Dev->Wifi != NULL) {
    RtwWifiGetMode (Dev, Out, Counts);
  }

  Out->Mode  = Dev->WifiMode ? RTW_MODE_WIFICX : Dev->Guarded ? RTW_MODE_GUARDED : RTW_MODE_DIAGNOSTIC;
  *Info      = sizeof (*Out) + ((Counts != NULL) ? sizeof (*Counts) : 0);
  return STATUS_SUCCESS;
}

/* Both modes, CtlLock held (the mac layer cannot go away meanwhile). */
static NTSTATUS
Radio (
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  struct rtw_radio_info  *Out;
  NTSTATUS               Status;

  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (*Out), (PVOID *)&Out, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  rtw_linux_radio_info (Out);     // only the power save policy when nothing is probed
  *Info = sizeof (*Out);
  return STATUS_SUCCESS;
}

/* rtw89's coexistence report. Both modes, CtlLock held. */
static NTSTATUS
Btc (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  LONG      *Out;
  size_t    OutLen;
  size_t    Room;
  NTSTATUS  Status;

  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (LONG), (PVOID *)&Out, &OutLen);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Room = min (OutLen - sizeof (LONG), 0x10000);
  *Out = Dev->Probed ? rtw_linux_btc_dump ((char *)(Out + 1), (uint32_t)Room) : -19;   // -ENODEV
  *Info = sizeof (LONG) + ((*Out > 0) ? (size_t)*Out : 0);
  return STATUS_SUCCESS;
}

/* The BSS table with its frame bodies. Any time, no lock (the table is static). */
static NTSTATUS
BssIes (
  _In_ WDFREQUEST  Request,
  _Out_ size_t     *Info
  )
{
  ULONG     *In;
  ULONG     *Out;
  uint32_t  Cursor;
  size_t    OutLen;
  NTSTATUS  Status;

  Status = GetInput (Request, sizeof (ULONG), (PVOID *)&In);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Cursor = *In;       // the output shares the system buffer
  Status = WdfRequestRetrieveOutputBuffer (Request, 2 * sizeof (ULONG), (PVOID *)&Out, &OutLen);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  OutLen = min (OutLen, 0x100000);
  Out[1] = 0;
  *Info  = 2 * sizeof (ULONG) + rtw_linux_bss_export (0, &Cursor, Out + 2, (uint32_t)(OutLen - 2 * sizeof (ULONG)));
  Out[0] = Cursor;
  return STATUS_SUCCESS;
}

/* Diagnostic mode, CtlLock held. */
static NTSTATUS
TxProbe (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  struct rtw_txprobe_param   *In;
  struct rtw_txprobe_param   Param;
  struct rtw_txprobe_result  *Out;
  struct rtw_txprobe_result  Result;
  NTSTATUS                   Status;

  Status = GetInput (Request, sizeof (Param), (PVOID *)&In);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  Param  = *In;       // the output shares the system buffer
  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (Result), (PVOID *)&Out, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtwLog ("IOCTL txprobe: %u frames, %u ms apart, wait %u ms", Param.count, Param.interval_ms, Param.wait_ms);
  RtlZeroMemory (&Result, sizeof (Result));
  if (Dev->Probed) {
    rtw_linux_txprobe (&Param, &Result);
  } else {
    Result.result = -19;   // -ENODEV
  }

  *Out  = Result;
  *Info = sizeof (Result);
  return STATUS_SUCCESS;
}

/* Both modes, CtlLock held (rtw89 and the mac layer cannot go away meanwhile). Fills the caller's buffer up to
   min(its length, sizeof), so a script built for the older (160-byte) layout still works. */
static NTSTATUS
Link (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  struct rtw_link_stats  Stats;
  PVOID                  Out;
  size_t                 OutLen;
  size_t                 Copy;
  NTSTATUS               Status;

  Status = WdfRequestRetrieveOutputBuffer (Request, RTW_LINK_MIN_OUT, &Out, &OutLen);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtlZeroMemory (&Stats, sizeof (Stats));
  if (Dev->Probed) {
    rtw_linux_link_stats (&Stats);
  }

  Copy = min (OutLen, sizeof (Stats));
  RtlCopyMemory (Out, &Stats, Copy);
  *Info = Copy;
  return STATUS_SUCCESS;
}

/* The mobile hotspot's group owner, and the front end's Wi-Fi Direct ports when there is room. Both modes, CtlLock
   held (the mac layer cannot go away meanwhile). */
static NTSTATUS
Ap (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  struct rtw_ap_stats  *Out;
  size_t               OutLen;
  NTSTATUS             Status;

  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (*Out), (PVOID *)&Out, &OutLen);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtlZeroMemory (Out, sizeof (*Out));
  if (Dev->Probed) {
    rtw_linux_ap_stats (Out);
  }

  *Info = sizeof (*Out);
  if (OutLen >= sizeof (*Out) + sizeof (RTW_WFD_OUT)) {
    RtwWifiGetWfd (Dev, (RTW_WFD_OUT *)(Out + 1));
    *Info += sizeof (RTW_WFD_OUT);
  }

  return STATUS_SUCCESS;
}

/* Trigger a hardware restart (SER) through the firmware. Both modes, CtlLock held. */
static NTSTATUS
Ser (
  _In_ PRTW_DEVICE  Dev,
  _In_ WDFREQUEST   Request,
  _Out_ size_t      *Info
  )
{
  LONG      *Out;
  NTSTATUS  Status;

  Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (LONG), (PVOID *)&Out, NULL);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtwLog ("IOCTL ser");
  *Out = Dev->Probed ? rtw_linux_ser_trigger () : -19;   // -ENODEV
  RtwLog ("IOCTL ser: %d", *Out);
  *Info = sizeof (LONG);
  return STATUS_SUCCESS;
}

/* WiFiCx mode: the WiFiCx front end owns rtw89; a control IOCTL answers -EBUSY where it has a result. */
static NTSTATUS
Refuse (
  _In_ WDFREQUEST  Request,
  _In_ ULONG       IoControlCode,
  _Out_ size_t     *Info
  )
{
  struct rtw_scan_result     *Result;
  struct rtw_txprobe_result  *Probe;
  LONG                       *Out;
  NTSTATUS                   Status;

  RtwLog ("IOCTL %#x refused: WiFiCx owns the device", IoControlCode);
  switch (IoControlCode) {
    case IOCTL_RTW_REMOVE:
    case IOCTL_RTW_STOP:
      return STATUS_DEVICE_BUSY;

    case IOCTL_RTW_SCAN:
      Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (*Result), (PVOID *)&Result, NULL);
      if (NT_SUCCESS (Status)) {
        RtlZeroMemory (Result, sizeof (*Result));
        Result->result = -EBUSY;
        *Info          = sizeof (*Result);
      }

      return Status;

    case IOCTL_RTW_TXPROBE:
      Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (*Probe), (PVOID *)&Probe, NULL);
      if (NT_SUCCESS (Status)) {
        RtlZeroMemory (Probe, sizeof (*Probe));
        Probe->result = -EBUSY;
        *Info         = sizeof (*Probe);
      }

      return Status;

    default:
      Status = WdfRequestRetrieveOutputBuffer (Request, sizeof (LONG), (PVOID *)&Out, NULL);
      if (NT_SUCCESS (Status)) {
        *Out  = -EBUSY;
        *Info = sizeof (LONG);
      }

      return Status;
  }
}

VOID
RtwEvtIoDeviceControl (
  _In_ WDFQUEUE    Queue,
  _In_ WDFREQUEST  Request,
  _In_ size_t      OutputBufferLength,
  _In_ size_t      InputBufferLength,
  _In_ ULONG       IoControlCode
  )
{
  PRTW_DEVICE  Dev;
  size_t       Info;
  NTSTATUS     Status;

  UNREFERENCED_PARAMETER (OutputBufferLength);
  UNREFERENCED_PARAMETER (InputBufferLength);

  // the PnP device this control device serves; NULL only while it is being deleted
  Dev  = RtwGetCdo (WdfIoQueueGetDevice (Queue))->Dev;
  Info = 0;
  if ((Dev == NULL) && (IoControlCode != IOCTL_RTW_LOG_READ)) {
    WdfRequestComplete (Request, STATUS_DEVICE_NOT_READY);
    return;
  }

  switch (IoControlCode) {
    case IOCTL_RTW_LOG_READ:
      Status = LogRead (Request, &Info);
      break;

    case IOCTL_RTW_MODE:
      Status = Mode (Dev, Request, &Info);
      break;

    case IOCTL_RTW_READ32:
      Status = Read32 (Dev, Request, &Info);
      break;

    case IOCTL_RTW_BSS_LIST:
      Status = BssList (Request, &Info);
      break;

    case IOCTL_RTW_LINK:
      WdfWaitLockAcquire (Dev->CtlLock, NULL);
      Status = Link (Dev, Request, &Info);
      WdfWaitLockRelease (Dev->CtlLock);
      break;

    case IOCTL_RTW_SER:
      WdfWaitLockAcquire (Dev->CtlLock, NULL);
      Status = Ser (Dev, Request, &Info);
      WdfWaitLockRelease (Dev->CtlLock);
      break;

    case IOCTL_RTW_RADIO:
      WdfWaitLockAcquire (Dev->CtlLock, NULL);
      Status = Radio (Request, &Info);
      WdfWaitLockRelease (Dev->CtlLock);
      break;

    case IOCTL_RTW_BTC:
      WdfWaitLockAcquire (Dev->CtlLock, NULL);
      Status = Btc (Dev, Request, &Info);
      WdfWaitLockRelease (Dev->CtlLock);
      break;

    case IOCTL_RTW_BSS_IES:
      Status = BssIes (Request, &Info);
      break;

    case IOCTL_RTW_AP:
      WdfWaitLockAcquire (Dev->CtlLock, NULL);
      Status = Ap (Dev, Request, &Info);
      WdfWaitLockRelease (Dev->CtlLock);
      break;

    case IOCTL_RTW_PROBE:
    case IOCTL_RTW_REMOVE:
    case IOCTL_RTW_START:
    case IOCTL_RTW_STOP:
    case IOCTL_RTW_MONITOR:
    case IOCTL_RTW_SCAN:
    case IOCTL_RTW_TXPROBE:
      if (Dev->WifiMode) {
        Status = Refuse (Request, IoControlCode, &Info);
        break;
      }

      WdfWaitLockAcquire (Dev->CtlLock, NULL);
      switch (IoControlCode) {
        case IOCTL_RTW_PROBE:
          Status = Probe (Dev, Request, &Info);
          break;

        case IOCTL_RTW_REMOVE:
          RtwLog ("IOCTL remove");
          RtwRemove (Dev);
          Status = STATUS_SUCCESS;
          break;

        case IOCTL_RTW_START:
          Status = Start (Dev, Request, &Info);
          break;

        case IOCTL_RTW_STOP:
          RtwLog ("IOCTL stop");
          if (Dev->Probed) {
            rtw_linux_stop ();
          }

          Status = STATUS_SUCCESS;
          break;

        case IOCTL_RTW_MONITOR:
          Status = Monitor (Dev, Request, &Info);
          break;

        case IOCTL_RTW_TXPROBE:
          Status = TxProbe (Dev, Request, &Info);
          break;

        default:
          Status = Scan (Dev, Request, &Info);
          break;
      }

      WdfWaitLockRelease (Dev->CtlLock);
      break;

    default:
      Status = STATUS_INVALID_DEVICE_REQUEST;
      break;
  }

  WdfRequestCompleteWithInformation (Request, Status, Info);
}
