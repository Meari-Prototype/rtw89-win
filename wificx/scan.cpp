// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: WDI_TASK_SCAN and WDI_ABORT_TASK on the asynchronous firmware scan of the mac layer
 * (rtw_linux_scan_start). At the end the BSSs heard since the scan started go up as WDI_INDICATION_BSS_ENTRY_LIST
 * (unsolicited: TransactionId 0, like the sample), then WDI_INDICATION_SCAN_COMPLETE (M4). The mac layer's
 * completion (ScanDone) and the timeout (ScanTimer) only set a flag and queue the command work item, which finishes
 * the task (wifi.h); everything here runs with Dev->CtlLock held unless noted.
 */
#include "wifi.h"

#define RTW_SCAN_TIMEOUT_MS         15000
#define RTW_SCAN_TIMEOUT_MIN_MS     10000
#define RTW_SCAN_TIMEOUT_MAX_MS     30000
#define RTW_SCAN_DWELL_MIN_MS       45      // rtw89's own active dwell (RTW89_CHANNEL_TIME)
#define RTW_SCAN_DWELL_MAX_MS       110
#define RTW_SCAN_LOG_FIRST          8       // scans logged in full; later only every RTW_SCAN_LOG_EVERY-th
#define RTW_SCAN_LOG_EVERY          32

static BOOLEAN
ScanLogged (
  _In_ ULONG  Seq
  )
{
  return (Seq <= RTW_SCAN_LOG_FIRST) || ((Seq % RTW_SCAN_LOG_EVERY) == 0);
}

/* From the mac layer with its lock held (PASSIVE_LEVEL): record and signal only. */
static void
ScanDone (
  void      *Ctx,
  int32_t   Result,
  uint32_t  Aborted,
  uint64_t  StartNs
  )
{
  PRTW_WIFI  W;
  BOOLEAN    Enqueue;
  KIRQL      Irql;

  UNREFERENCED_PARAMETER (Result);
  UNREFERENCED_PARAMETER (StartNs);

  W = (PRTW_WIFI)Ctx;
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->EvScanDone    = TRUE;
  W->EvScanAborted = (Aborted != 0);
  Enqueue          = !W->Stopping;
  KeReleaseSpinLock (&W->Lock, Irql);
  if (Enqueue) {
    WdfWorkItemEnqueue (W->CmdWork);
  }
}

/* DISPATCH_LEVEL: the scan did not end in time; the work item cancels it. */
VOID
RtwWifiScanTimer (
  _In_ WDFTIMER  Timer
  )
{
  PRTW_WIFI  W;
  BOOLEAN    Enqueue;
  KIRQL      Irql;

  W = RtwGetWifi ((WDFDEVICE)WdfTimerGetParentObject (Timer));
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->EvScanTimeout = TRUE;
  Enqueue          = !W->Stopping;
  KeReleaseSpinLock (&W->Lock, Irql);
  if (Enqueue) {
    WdfWorkItemEnqueue (W->CmdWork);
  }
}

/* ---- BSS list ---- */
static WDI_BAND_ID
WdiBand (
  _In_ UINT8  Nl80211Band
  )
{
  // NL80211_BAND_2GHZ = 0, NL80211_BAND_5GHZ = 1; 6 GHz (3) is not among the reported bands
  switch (Nl80211Band) {
    case 0:  return WDI_BAND_ID_2400;
    case 1:  return WDI_BAND_ID_5000;
    default: return WDI_BAND_ID_UNKNOWN;
  }
}

static BOOLEAN
Wanted (
  _In_ const RTW_SCAN_TASK       *S,
  _In_ const struct rtw_bss_rec  *R
  )
{
  if ((R->channel == 0) || (WdiBand (R->band) == WDI_BAND_ID_UNKNOWN)) {
    return FALSE;
  }

  return !S->BssidFilter || (RtlCompareMemory (R->bssid, S->Bssid, sizeof (S->Bssid)) == sizeof (S->Bssid));
}

/* One WDI_INDICATION_BSS_ENTRY_LIST for Count records. The frame bodies are not copied into the parameters (they
   point into the export buffer); the generator copies them into the TLV stream. */
static VOID
IndicateList (
  _In_ PRTW_WIFI                               W,
  _In_ const RTW_SCAN_TASK                     *S,
  _In_reads_ (Count) const struct rtw_bss_rec  *const *Recs,
  _In_ ULONG                                   Count
  )
{
  WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS  P;
  WDI_BSS_ENTRY_CONTAINER                   *E;
  const struct rtw_bss_rec                  *R;
  UINT8                                     *Body;
  UINT8                                     *Tlv;
  ULONG                                     TlvLen;
  NDIS_STATUS                               Status;
  LONG                                      Rssi;
  ULONG                                     i;

  Status = P.DeviceDescriptor.AllocateElements (Count, 0);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: no memory for %u BSS entries", Count);
    return;
  }

  P.Optional.DeviceDescriptor_IsPresent = 1;
  for (i = 0; i < Count; i++) {
    R    = Recs[i];
    E    = &P.DeviceDescriptor.pElements[i];
    Body = (UINT8 *)(R + 1);
    RtlCopyMemory (E->BSSID.Address, R->bssid, sizeof (E->BSSID.Address));
    if (R->beacon_len != 0) {
      E->BeaconFrame.SimpleAssign (Body, R->beacon_len);
      E->Optional.BeaconFrame_IsPresent = 1;
    }

    if (R->presp_len != 0) {
      E->ProbeResponseFrame.SimpleAssign (Body + R->beacon_len, R->presp_len);
      E->Optional.ProbeResponseFrame_IsPresent = 1;
    }

    // link quality 0..100 over -100..-50 dBm; no reading counts as -100 dBm
    Rssi                        = (R->signal != 0) ? R->signal : -100;
    E->SignalInfo.RSSI          = Rssi;
    E->SignalInfo.LinkQuality   = (UINT32)min (max (2 * (Rssi + 100), 0), 100);
    E->ChannelInfo.ChannelNumber = R->channel;
    E->ChannelInfo.BandId       = WdiBand (R->band);
  }

  Tlv    = NULL;
  TlvLen = 0;
  Status = GenerateWdiIndicationBssEntryListFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: BSS_ENTRY_LIST not generated (0x%08x)", Status);
    return;
  }

  RtwWifiIndicate (W, WDI_INDICATION_BSS_ENTRY_LIST, S->PortId, WDI_TRANSACTION_ID_UNSOLICIT, S->IhvSpecificId,
                   NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
}   // P's destructor frees the entry array (not the bodies: SimpleAssign does not own them)

/* Indicates the BSSs exported since the scan started, RTW_WIFI_BSS_PER_IND at a time. Returns how many. */
static ULONG
IndicateBss (
  _In_ PRTW_WIFI            W,
  _In_ const RTW_SCAN_TASK  *S
  )
{
  const struct rtw_bss_rec  *Recs[RTW_WIFI_BSS_PER_IND];
  const struct rtw_bss_rec  *R;
  UINT32                    Cursor;
  ULONG                     Len;
  ULONG                     Off;
  ULONG                     Size;
  ULONG                     Count;
  ULONG                     Body;
  ULONG                     Total;

  Total  = 0;
  Cursor = 0;
  while (Cursor < RTW_BSS_EXPORT_END) {
    Len = rtw_linux_bss_export (S->StartNs, &Cursor, W->ExportBuf, RTW_WIFI_EXPORT_SIZE);
    if (Len == 0) {
      break;      // nothing left (a record always fits into the 64 KB buffer)
    }

    Off   = 0;
    Count = 0;
    Body  = 0;
    while (Off + sizeof (*R) <= Len) {
      R    = (const struct rtw_bss_rec *)(W->ExportBuf + Off);
      Size = ALIGN_UP_BY (sizeof (*R) + R->beacon_len + R->presp_len, RTW_BSS_REC_ALIGN);
      if ((Count == RTW_WIFI_BSS_PER_IND) ||
          ((Count != 0) && (Body + R->beacon_len + R->presp_len > RTW_WIFI_BODY_PER_IND)))
      {
        IndicateList (W, S, Recs, Count);
        Total += Count;
        Count  = 0;
        Body   = 0;
      }

      if (Wanted (S, R)) {
        Recs[Count++] = R;
        Body         += R->beacon_len + R->presp_len;
      }

      Off += Size;
    }

    if (Count != 0) {
      IndicateList (W, S, Recs, Count);
      Total += Count;
    }
  }

  return Total;
}

/* Ends the scan task: BSS list and SCAN_COMPLETE if Indicate and the port is still there. */
static VOID
ScanFinish (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    MacAborted,
  _In_ BOOLEAN    Indicate
  )
{
  RTW_SCAN_TASK  S;
  NDIS_STATUS    Status;
  ULONG          Count;
  ULONG64        Ms;

  S               = W->Scan;
  W->Scan.Active  = FALSE;
  WdfTimerStop (W->ScanTimer, TRUE);
  Status = (S.AbortRequested || MacAborted) ? NDIS_STATUS_REQUEST_ABORTED : NDIS_STATUS_SUCCESS;
  Count  = 0;
  Indicate = Indicate && RtwWifiPortUp (W, S.PortId);
  if (Indicate) {
    Count = IndicateBss (W, &S);
    RtwWifiIndicate (W, WDI_INDICATION_SCAN_COMPLETE, S.PortId, S.TransactionId, S.IhvSpecificId, Status, NULL, 0);
  }

  InterlockedIncrement (&W->Scans);
  InterlockedExchange (&W->LastScanBss, (LONG)Count);
  if (ScanLogged (S.Seq) || (Status != NDIS_STATUS_SUCCESS)) {
    Ms = (rtwk_time_ns () - S.StartNs) / 1000000;
    RtwLog ("WiFiCx: scan %u done after %llu ms: %u BSS indicated, status %#x%s", S.Seq, Ms, Count, Status,
            Indicate ? "" : ", no indications (port gone or stopping)");
  }
}

/* Mac layer finished (Done) and/or the timer fired (Timeout); from the command work item. */
VOID
RtwWifiScanEvents (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    Done,
  _In_ BOOLEAN    Aborted,
  _In_ BOOLEAN    Timeout
  )
{
  if (!W->Scan.Active) {
    return;       // a task finished already (cancelled by radio off, reset, abort, power down)
  }

  if (Timeout && !Done) {
    RtwLog ("WiFiCx: scan %u timed out; cancelling", W->Scan.Seq);
    W->Scan.AbortRequested = TRUE;
    rtw_linux_scan_cancel ();     // ScanDone runs inside; its event finds no task afterwards
    Done    = TRUE;
    Aborted = TRUE;
  }

  if (Done) {
    ScanFinish (W, Aborted, TRUE);
  }
}

/* Stops a running scan task now (radio off, reset, D0Exit, ReleaseHardware): aborted. */
VOID
RtwWifiScanStop (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    Indicate
  )
{
  if (!W->Scan.Active) {
    return;
  }

  W->Scan.AbortRequested = TRUE;
  rtw_linux_scan_cancel ();
  ScanFinish (W, TRUE, Indicate);
}

VOID
RtwWifiTaskScan (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  )
{
  WDI_SCAN_PARAMETERS                  P;
  const WDI_SCAN_BAND_CHANNEL_LIST_CONTAINER  *B;
  WDI_MESSAGE_HEADER                   H;
  struct rtw_scan_req                  Rq;
  NDIS_STATUS                          Parsed;
  NTSTATUS                             Status;
  LONG                                 Ret;
  UINT8                                Band;
  ULONG                                TimeoutMs;
  ULONG                                Seq;
  ULONG                                n;
  ULONG                                i;
  ULONG                                j;
  BOOLEAN                              Broadcast;
  BOOLEAN                              Zero;
  UCHAR                                Bssid[6];
  ULONG64                              Start;
  KIRQL                                Irql;

  // everything the scan needs comes out of the request before M3
  Start  = 0;
  H      = *Header;
  Parsed = ParseWdiTaskScanToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiTaskScanToIhv (&P);
    RtwLog ("WiFiCx: TASK_SCAN not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, (Parsed == NDIS_STATUS_RESOURCES) ? STATUS_INSUFFICIENT_RESOURCES : STATUS_INVALID_PARAMETER, 0);
    return;
  }

  RtlZeroMemory (&Rq, sizeof (Rq));
  if (P.ScanModeParameters.AllowedScanType == WDI_SCAN_TYPE_PASSIVE_ONLY) {
    Rq.flags |= RTW_SCAN_PASSIVE;
  }

  for (i = 0; (i < P.SSIDList.ElementCount) && (Rq.n_ssids < RTW_SCAN_MAX_SSIDS); i++) {
    n = min (P.SSIDList.pElements[i].ElementCount, sizeof (Rq.ssids[0].ssid));
    Rq.ssids[Rq.n_ssids].len = (uint8_t)n;
    if (n != 0) {
      RtlCopyMemory (Rq.ssids[Rq.n_ssids].ssid, P.SSIDList.pElements[i].pElements, n);
    }

    Rq.n_ssids++;
  }

  if (P.Optional.BandChannelList_IsPresent) {
    for (i = 0; i < P.BandChannelList.ElementCount; i++) {
      B = &P.BandChannelList.pElements[i];
      if (B->BandID == WDI_BAND_ID_2400) {
        Band = 0;       // NL80211_BAND_2GHZ
      } else if (B->BandID == WDI_BAND_ID_5000) {
        Band = 1;       // NL80211_BAND_5GHZ
      } else {
        continue;
      }

      // only channels the capabilities listed (others would leave the mac layer nothing to scan)
      for (j = 0; (j < B->ChannelList.ElementCount) && (Rq.n_channels < RTW_SCAN_MAX_CHANNELS); j++) {
        for (n = 0; n < W->Caps->n_chans[Band]; n++) {
          if (W->Caps->chans[Band][n].number == B->ChannelList.pElements[j]) {
            Rq.channels[Rq.n_channels].band   = Band;
            Rq.channels[Rq.n_channels].number = W->Caps->chans[Band][n].number;
            Rq.n_channels++;
            break;
          }
        }
      }
    }

    if (Rq.n_channels == 0) {
      RtwLog ("WiFiCx: scan channel list (%u bands) has no channel of ours; scanning all",
              P.BandChannelList.ElementCount);
    }
  }

  // the active dwell time applies to every channel (rtw89 has one period per request); passive (NO_IR) channels
  // still get at least 105 ms from rtw89. 0 keeps rtw89's 45 ms.
  if (P.DwellTime.ActiveChannelDwellTime != 0) {
    Rq.dwell_ms = min (max (P.DwellTime.ActiveChannelDwellTime, RTW_SCAN_DWELL_MIN_MS), RTW_SCAN_DWELL_MAX_MS);
  }

  TimeoutMs = RTW_SCAN_TIMEOUT_MS;
  if (P.DwellTime.MaximumScanTime != 0) {
    TimeoutMs = min (max (P.DwellTime.MaximumScanTime + 5000, RTW_SCAN_TIMEOUT_MIN_MS), RTW_SCAN_TIMEOUT_MAX_MS);
  }

  Broadcast = TRUE;
  Zero      = TRUE;
  for (i = 0; i < sizeof (P.BSSID.Address); i++) {
    Broadcast = Broadcast && (P.BSSID.Address[i] == 0xFF);
    Zero      = Zero && (P.BSSID.Address[i] == 0);
  }

  Seq = W->ScanSeq + 1;
  if (ScanLogged (Seq)) {
    RtwLog (
      "WiFiCx: scan %u on port %u: %u SSIDs, type %u, %u channels%s, dwell %u/%u ms (used %u), max %u ms, "
      "trigger %u, repeat %u, live %u%s",
      Seq, H.PortId, Rq.n_ssids, (ULONG)P.ScanModeParameters.AllowedScanType, Rq.n_channels,
      P.Optional.BandChannelList_IsPresent ? "" : " (all)", P.DwellTime.ActiveChannelDwellTime,
      P.DwellTime.PassiveChannelDwellTime, Rq.dwell_ms, P.DwellTime.MaximumScanTime, (ULONG)P.ScanModeParameters.ScanTrigger,
      (ULONG)P.ScanModeParameters.ScanRepeatCount, (ULONG)P.ScanModeParameters.LiveUpdatesNeeded,
      (Broadcast || Zero) ? "" : ", one BSSID"
      );
  }

  RtlCopyMemory (Bssid, P.BSSID.Address, sizeof (Bssid));
  CleanupParsedWdiTaskScanToIhv (&P);

  if (!RtwWifiPortUp (W, H.PortId) || !W->HwReady || W->PoweredDown) {
    Status = STATUS_DEVICE_NOT_READY;
  } else if (!W->RadioOn) {
    Status = STATUS_INVALID_DEVICE_STATE;
  } else if (W->Scan.Active || W->Conn.Active) {
    Status = STATUS_DEVICE_BUSY;      // WiFiCx runs one task at a time; not expected
  } else {
    Ret = RtwWifiEnsureStarted (W);
    if (Ret == 0) {
      // no stale event of an earlier scan may end this one; its ScanDone has run already
      KeAcquireSpinLock (&W->Lock, &Irql);
      W->EvScanDone    = FALSE;
      W->EvScanAborted = FALSE;
      W->EvScanTimeout = FALSE;
      KeReleaseSpinLock (&W->Lock, Irql);
      Start = rtwk_time_ns ();      // report what is heard from here on
      RtwWifiApScanLimit (W, &Rq);  // a GO with clients stays on its channel (nothing without one)
      Ret   = rtw_linux_scan_start (&Rq, ScanDone, W);
    }

    Status = (Ret == 0) ? STATUS_SUCCESS : RtwWifiErrnoStatus (Ret);
    if (Ret != 0) {
      RtwLog ("WiFiCx: scan %u not started: %d", Seq, Ret);
    }
  }

  if (!NT_SUCCESS (Status)) {
    RtwWifiComplete (W, Request, Status, sizeof (WDI_MESSAGE_HEADER));    // no M4 after a failed M3
    return;
  }

  RtlZeroMemory (&W->Scan, sizeof (W->Scan));
  RtlCopyMemory (W->Scan.Bssid, Bssid, sizeof (Bssid));
  W->Scan.BssidFilter   = !Broadcast && !Zero;
  W->ScanSeq            = Seq;
  W->Scan.Active        = TRUE;
  W->Scan.PortId        = H.PortId;
  W->Scan.TransactionId = H.TransactionId;
  W->Scan.IhvSpecificId = H.IhvSpecificId;
  W->Scan.StartNs       = Start;
  W->Scan.Seq           = Seq;
  WdfTimerStart (W->ScanTimer, WDF_REL_TIMEOUT_IN_MS (TimeoutMs));
  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));    // M3; the work item sends M4
}

VOID
RtwWifiAbortTask (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  )
{
  WDI_TASK_ABORT_PARAMETERS  P;
  NDIS_STATUS                Parsed;
  NDIS_OID                   Oid;
  UINT32                     Tid;
  UINT16                     Port;

  Parsed = ParseWdiAbortTaskToIhv (InLen - sizeof (*Header), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  Oid    = P.CancelParameters.OriginalTaskOID;
  Tid    = P.CancelParameters.OriginalTransactionId;
  Port   = P.CancelParameters.OriginalPortId;
  CleanupParsedWdiAbortTaskToIhv (&P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: ABORT_TASK not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  // the port is not compared: the transaction id names the task. Observed with OS WDI version 2.0.14: WiFiCx sends
  // the bare message id (4) as the OID, not WDI_DEFINE_OID (WDI_TASK_SCAN); take either.
  if (W->Scan.Active && ((Oid == WDI_TASK_SCAN) || (Oid == WDI_DEFINE_OID (WDI_TASK_SCAN))) &&
      (Tid == W->Scan.TransactionId))
  {
    RtwLog ("WiFiCx: abort of scan %u (tid %u, port %u)", W->Scan.Seq, Tid, Port);
    W->Scan.AbortRequested = TRUE;
    rtw_linux_scan_cancel ();     // ScanDone queues the work item, which sends the M4 after this M3
  } else if (((Oid == WDI_TASK_CONNECT) || (Oid == WDI_DEFINE_OID (WDI_TASK_CONNECT))) && RtwWifiConnAbort (W, Tid)) {
    // the M4 (aborted) follows from the work item
  } else if (RtwWifiApAbort (W, Oid, Tid)) {
    // the GO's association response task; its M4 (aborted) follows from the work item
  } else {
    RtwLog ("WiFiCx: abort of task %#x tid %u port %u: not running", Oid, Tid, Port);
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
}
