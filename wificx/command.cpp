// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: WiFiCx commands (WIFIREQUEST = a WDI message). EvtWifiDeviceSendCommand runs at
 * IRQL <= DISPATCH_LEVEL and only queues the request; the work item handles them one by one at PASSIVE_LEVEL with
 * Dev->CtlLock held (wifi.h). Every request is completed exactly once (WifiRequestComplete).
 *
 * Property messages (SET_ADAPTER_CONFIGURATION, ABORT_TASK, ...) are complete with their M3. Task messages
 * (TASK_SET_RADIO_STATE, TASK_DOT11_RESET, TASK_SCAN) get M3 when accepted, then the work, then their completion
 * indication (M4) with the request's PortId/TransactionId/IhvSpecificId; a failed M3 has no M4 (sample
 * wifitransition.cpp ExecuteSteps). The request and its buffer belong to WiFiCx again once completed, so
 * everything needed later is copied out first. Messages not handled here get STATUS_NOT_SUPPORTED, like the sample.
 * TASK_CONNECT/TASK_DISCONNECT (connect.cpp) and their events; SET_RECEIVE_PACKET_FILTER and SET_MULTICAST_LIST are
 * accepted. Reset and radio off end a connection first.
 * SET_PRIVACY_EXEMPTION_LIST, SET_ADD_CIPHER_KEYS, SET_DELETE_CIPHER_KEYS, SET_DEFAULT_KEY_ID (security.cpp).
 * SET_CONNECTION_QUALITY steers the power save policy; GET_AUTO_POWER_SAVE reports the power save state;
 * SET_SAE_AUTH_PARAMS carries the OS's SAE steps (sae.cpp).
 * With Wi-Fi Direct on, messages for the Wi-Fi Direct device and role ports go to wfd.cpp/ap.cpp first; radio off
 * stops a running GO; the work item also takes the GO's indications (ap.cpp).
 */
#include "wifi.h"
#include <ntstrsafe.h>

#define RTW_WIFI_LOG_PER_ID     4       // requests logged per message id

/* NDIS_STATUS from the TLV parser to the NTSTATUS for M3 (sample umkmfusion.h ConvertNDISSTATUSToNTSTATUS) */
static NTSTATUS
NdisToNt (
  _In_ NDIS_STATUS  Status
  )
{
  if (Status == NDIS_STATUS_BUFFER_TOO_SHORT) {
    return STATUS_BUFFER_TOO_SMALL;
  }

  if (NT_SUCCESS (Status) && (Status != NDIS_STATUS_SUCCESS)) {
    return STATUS_UNSUCCESSFUL;     // an NDIS error code that NT_SUCCESS would take for success
  }

  return (NTSTATUS)Status;
}

NTSTATUS
RtwWifiErrnoStatus (
  _In_ LONG  Ret
  )
{
  switch (-Ret) {
    case 12:  return STATUS_INSUFFICIENT_RESOURCES;   // ENOMEM
    case 16:  return STATUS_DEVICE_BUSY;              // EBUSY
    case 19:  return STATUS_DEVICE_NOT_READY;         // ENODEV
    case 107: return STATUS_INVALID_DEVICE_STATE;     // ENOTCONN
    default:  return STATUS_UNSUCCESSFUL;
  }
}

VOID
RtwWifiComplete (
  _In_ PRTW_WIFI    W,
  _In_ WIFIREQUEST  Request,
  _In_ NTSTATUS     Status,
  _In_ UINT         BytesWritten
  )
{
  WifiRequestComplete (Request, Status, BytesWritten);
  InterlockedIncrement (&W->Completed);
}

/* like Dispatch's default: logged for the first few of an id, counted */
VOID
RtwWifiCompleteUnsupported (
  _In_ PRTW_WIFI    W,
  _In_ WIFIREQUEST  Request,
  _In_ UINT16       Id
  )
{
  ULONG  Seen;

  Seen = (Id < RTW_WIFI_MSG_IDS) ? W->MsgUnsupported[Id]++ : 0;
  if (Seen < RTW_WIFI_LOG_PER_ID) {
    RtwLog ("wdi %u: not supported on this port", Id);
  }

  InterlockedIncrement (&W->Unsupported);
  RtwWifiComplete (W, Request, STATUS_NOT_SUPPORTED, sizeof (WDI_MESSAGE_HEADER));
}

/* An indication: WDI header + TLVs in a WDFMEMORY; WiFiCx copies it during the call (sample wifirequest.cpp). */
VOID
RtwWifiIndicate (
  _In_ PRTW_WIFI                     W,
  _In_ UINT16                        MessageId,
  _In_ UINT16                        PortId,
  _In_ UINT32                        TransactionId,
  _In_ UINT32                        IhvSpecificId,
  _In_ NDIS_STATUS                   Status,
  _In_reads_bytes_opt_ (TlvLen) const UINT8  *Tlv,
  _In_ ULONG                         TlvLen
  )
{
  WDF_OBJECT_ATTRIBUTES  Attributes;
  WDFMEMORY              Memory;
  PWDI_MESSAGE_HEADER    Header;
  NTSTATUS               NtStatus;

  WDF_OBJECT_ATTRIBUTES_INIT (&Attributes);
  Attributes.ParentObject = W->Device;
  NtStatus = WdfMemoryCreate (
               &Attributes,
               NonPagedPoolNx,
               RTW_POOL_TAG,
               sizeof (WDI_MESSAGE_HEADER) + TlvLen,
               &Memory,
               (PVOID *)&Header
               );
  if (!NT_SUCCESS (NtStatus)) {
    RtwLog ("WiFiCx: indication %u not sent (0x%08x)", MessageId, NtStatus);
    return;
  }

  RtlZeroMemory (Header, sizeof (*Header));
  Header->PortId        = PortId;
  Header->Status        = Status;
  Header->TransactionId = TransactionId;
  Header->IhvSpecificId = IhvSpecificId;
  if ((Tlv != NULL) && (TlvLen != 0)) {
    RtlCopyMemory (Header + 1, Tlv, TlvLen);
  }

  WifiDeviceReceiveIndication (W->Device, MessageId, Memory);
  WdfObjectDelete (Memory);
}

/* The station adapter exists and has this port: port indications and port tasks are fine. */
BOOLEAN
RtwWifiPortUp (
  _In_ PRTW_WIFI  W,
  _In_ UINT16     PortId
  )
{
  BOOLEAN  Up;
  KIRQL    Irql;

  KeAcquireSpinLock (&W->Lock, &Irql);
  Up = W->AdapterUp && (W->PortId == PortId);
  KeReleaseSpinLock (&W->Lock, Irql);
  return Up;
}

/* The radio needs the mac layer started (drv_start + station interface; rtw89 then idles in IPS). */
LONG
RtwWifiEnsureStarted (
  _In_ PRTW_WIFI  W
  )
{
  LONG  Ret;

  if (W->Started) {
    return 0;
  }

  if (!W->HwReady || W->PoweredDown || !W->Dev->Probed) {
    return -RTW_ENODEV;
  }

  Ret = rtw_linux_start ();
  if ((Ret == 0) || (Ret == -RTW_EBUSY)) {      // -EBUSY: started already
    W->Started = TRUE;
    return 0;
  }

  RtwLog ("WiFiCx: rtw_linux_start failed: %d", Ret);
  return Ret;
}

/* Callers stop a running scan task first (RtwWifiScanStop). */
VOID
RtwWifiStopMac (
  _In_ PRTW_WIFI  W
  )
{
  if (W->Started) {
    rtw_linux_stop ();
    W->Started = FALSE;
  }
}

/* ---- TLV dump (Parameters\WdiDump = 1): the first message of each id goes to the log ---- */
static VOID __stdcall
DumpLine (
  _In_ UINT_PTR  Context,
  _In_z_ _Printf_format_string_ PCSTR  Format,
  ...
  )
{
  CHAR     Line[200];
  va_list  Args;

  UNREFERENCED_PARAMETER (Context);
  Line[0] = '\0';
  va_start (Args, Format);
  (VOID)RtlStringCbVPrintfA (Line, sizeof (Line), Format, Args);     // truncated lines are fine
  va_end (Args);
  if (Line[0] != '\0') {
    RtwLog ("wdi dump: %s", Line);
  }
}

/* ---- the property messages ---- */
static VOID
SetAdapterConfiguration (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_SET_FIRMWARE_CONFIGURATION_PARAMETERS  P;
  NDIS_STATUS                                Status;

  Status = ParseWdiSetAdapterConfigurationToIhv (
             InLen - sizeof (*Header),
             (const UINT8 *)(Header + 1),
             &W->Tlv,
             &P
             );
  if (Status != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiSetAdapterConfigurationToIhv (&P);
    RtwLog ("WiFiCx: SET_ADAPTER_CONFIGURATION not parsed (0x%08x)", Status);
    RtwWifiComplete (W, Request, NdisToNt (Status), sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  // nothing to configure: the address stays the efuse one, NLO is not supported
  if (P.Optional.MacAddress_IsPresent) {
    RtwLog (
      "WiFiCx: adapter configuration: MAC %02x:%02x:%02x:%02x:%02x:%02x (kept %02x:%02x:%02x:%02x:%02x:%02x)",
      P.MacAddress.Address[0], P.MacAddress.Address[1], P.MacAddress.Address[2],
      P.MacAddress.Address[3], P.MacAddress.Address[4], P.MacAddress.Address[5],
      W->Caps->addr[0], W->Caps->addr[1], W->Caps->addr[2], W->Caps->addr[3], W->Caps->addr[4], W->Caps->addr[5]
      );
  }

  RtwLog (
    "WiFiCx: adapter configuration: unreachable threshold %s%u, NLO scan mode %s%u, %u disallowed bands, PLDR %u",
    P.Optional.UnreachableDetectionThreshold_IsPresent ? "" : "(none) ",
    P.UnreachableDetectionThreshold,
    P.Optional.AdapterNloScanMode_IsPresent ? "" : "(none) ",
    (ULONG)P.AdapterNloScanMode,
    P.Optional.DisallowedBandList_IsPresent ? P.DisallowedBandList.ElementCount : 0,
    (ULONG)P.PLDRSupport
    );
  CleanupParsedWdiSetAdapterConfigurationToIhv (&P);
  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
}

/* A hint (power save vs. latency); the mac layer's power save policy follows it (mac\ps.c). */
static VOID
SetConnectionQuality (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_SET_CONNECTION_QUALITY_PARAMETERS  P;
  NDIS_STATUS                            Status;

  Status = ParseWdiSetConnectionQualityToIhv (InLen - sizeof (*Header), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Status == NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: connection quality hint %u", (ULONG)P.Quality);
    rtw_linux_set_ps_policy (RTW_PS_SRC_HINT, (uint32_t)P.Quality);
  }

  CleanupParsedWdiSetConnectionQualityToIhv (&P);
  RtwWifiComplete (W, Request, (Status == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS : NdisToNt (Status),
                   sizeof (WDI_MESSAGE_HEADER));
}

/* WDI_GET_AUTO_POWER_SAVE (the station capabilities say AutoPowerSaveMode): the state of 802.11 power save, as
   the mac layer and rtw89 have it (power save wanted, the chip in leisure power save now, time with it on). Like
   the WDI sample (WDI_Cmds.c Wdi_Get_Auto_Power_Save), the traffic counters stay 0. */
static VOID
GetAutoPowerSave (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ WDI_MESSAGE_HEADER         *Header,
  _In_ UINT                       OutLen
  )
{
  WDI_GET_AUTO_POWER_SAVE_RESULTS  R;
  struct rtw_radio_info            Info;
  NDIS_STATUS                      Status;
  UINT8                            *Tlv;
  ULONG                            TlvLen;

  rtw_linux_radio_info (&Info);
  R.AutoPowerSaveParameters.EnableAutoPSM           = Info.ps_policy;
  R.AutoPowerSaveParameters.BeaconInterval          = (UINT16)((Info.beacon_int * 1024U) / 1000U);
  R.AutoPowerSaveParameters.ListenInterval          = 1;          // rtw89 LPS wakes for every beacon
  R.AutoPowerSaveParameters.ListenIntervalInDx      = 255;        // no low power state of its own
  R.AutoPowerSaveParameters.PowerMode               = (Info.rtw_flags & RTW_RF_LEISURE_PS) ?
                                                      WDI_POWER_SAVE_LEVEL_MAX_PSP : WDI_POWER_SAVE_LEVEL_NO_POWER_SAVE;
  R.AutoPowerSaveParameters.PowerModeinDx           = R.AutoPowerSaveParameters.PowerMode;
  R.AutoPowerSaveParameters.Reason                  = WDI_POWER_MODE_REASON_CODE_NO_CHANGE;
  R.AutoPowerSaveParameters.MillisecondsSinceStart  = (rtwk_time_ns () - W->ReadyNs) / 1000000;
  R.AutoPowerSaveParameters.MillisecondsInPowerSave = Info.ps_ms;

  Tlv    = NULL;
  TlvLen = 0;
  Status = GenerateWdiGetAutoPowerSaveFromIhv (&R, sizeof (WDI_MESSAGE_HEADER), &W->Tlv, &TlvLen, &Tlv);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: GET_AUTO_POWER_SAVE not generated (0x%08x)", Status);
    RtwWifiComplete (W, Request, NdisToNt (Status), sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  if ((TlvLen < sizeof (WDI_MESSAGE_HEADER)) || (OutLen < TlvLen)) {
    FreeGenerated (Tlv);
    RtwWifiComplete (W, Request, STATUS_BUFFER_TOO_SMALL, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  // the request's header stays; the TLVs follow it in the same buffer
  RtlCopyMemory (Header + 1, Tlv + sizeof (WDI_MESSAGE_HEADER), TlvLen - sizeof (WDI_MESSAGE_HEADER));
  FreeGenerated (Tlv);
  RtwWifiComplete (W, Request, STATUS_SUCCESS, TlvLen);
}

/* ---- the task messages ---- */
static VOID
TaskSetRadioState (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_SET_RADIO_STATE_PARAMETERS          P;
  WDI_INDICATION_RADIO_STATUS_PARAMETERS  Radio;
  WDI_MESSAGE_HEADER                      H;
  NDIS_STATUS                             Status;
  NDIS_STATUS                             Result;
  BOOLEAN                                 On;
  UINT8                                   *Tlv;
  ULONG                                   TlvLen;

  H      = *Header;
  Status = ParseWdiTaskSetRadioStateToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  On     = (P.SoftwareRadioState != 0);
  CleanupParsedWdiTaskSetRadioStateToIhv (&P);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: TASK_SET_RADIO_STATE not parsed (0x%08x)", Status);
    RtwWifiComplete (W, Request, NdisToNt (Status), 0);
    return;
  }

  if (!W->HwReady || W->PoweredDown) {
    RtwWifiComplete (W, Request, STATUS_DEVICE_NOT_READY, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3

  Result = NDIS_STATUS_SUCCESS;
  if (!On) {
    RtwWifiApStop (W, TRUE, WDI_STOP_AP_REASON_FREQUENCY_NOT_AVAILABLE);   // the GO first (nothing without one)
    RtwWifiConnStop (W, WDI_ASSOC_STATUS_RADIO_OFF, TRUE);
    RtwWifiScanStop (W, TRUE);
    RtwWifiStopMac (W);
  } else if (RtwWifiEnsureStarted (W) != 0) {
    Result = NDIS_STATUS_FAILURE;
  }

  RtwLog ("WiFiCx: radio %s -> %s%s", W->RadioOn ? "on" : "off", On ? "on" : "off",
          (Result == NDIS_STATUS_SUCCESS) ? "" : " (start failed)");
  if (On != W->RadioOn) {
    W->RadioOn = On;
    Radio.RadioState.HardwareState = TRUE;
    Radio.RadioState.SoftwareState = On;
    Tlv    = NULL;
    TlvLen = 0;
    Status = GenerateWdiIndicationRadioStatusFromIhv (&Radio, 0, &W->Tlv, &TlvLen, &Tlv);
    if (Status == NDIS_STATUS_SUCCESS) {
      RtwWifiIndicate (W, WDI_INDICATION_RADIO_STATUS, H.PortId, WDI_TRANSACTION_ID_UNSOLICIT, H.IhvSpecificId,
                       NDIS_STATUS_SUCCESS, Tlv, TlvLen);
      FreeGenerated (Tlv);
    } else {
      RtwLog ("WiFiCx: RADIO_STATUS not generated (0x%08x)", Status);
    }
  }

  RtwWifiIndicate (W, WDI_INDICATION_SET_RADIO_STATE_COMPLETE, H.PortId, H.TransactionId, H.IhvSpecificId,
                   Result, NULL, 0);                                              // M4
}

static VOID
TaskDot11Reset (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_TASK_DOT11_RESET_PARAMETERS  P;
  WDI_MESSAGE_HEADER               H;
  NDIS_STATUS                      Status;
  NDIS_STATUS                      Result;
  BOOLEAN                          DefaultMib;

  H          = *Header;
  DefaultMib = FALSE;
  Status     = ParseWdiTaskDot11ResetToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Status == NDIS_STATUS_SUCCESS) {
    DefaultMib = (P.Dot11ResetParameters.SetDefaultMIB != 0);
    RtwLog ("WiFiCx: dot11 reset on port %u, default MIB %u%s", H.PortId, P.Dot11ResetParameters.SetDefaultMIB,
            P.Optional.ResetMACAddress_IsPresent ? ", MAC address given (kept the efuse one)" : "");
  }

  CleanupParsedWdiTaskDot11ResetToIhv (&P);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: TASK_DOT11_RESET not parsed (0x%08x)", Status);
    RtwWifiComplete (W, Request, NdisToNt (Status), 0);
    return;
  }

  if (!RtwWifiPortUp (W, H.PortId) || !W->HwReady || W->PoweredDown) {
    RtwWifiComplete (W, Request, STATUS_DEVICE_NOT_READY, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3

  // a connect task ends (aborted) and a connection is left (with all its keys, mac\key.c); a started mac layer
  // stays started. The exemption list is a MIB value: back to the default (none) on request.
  RtwWifiConnStop (W, WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET, TRUE);
  RtwWifiScanStop (W, TRUE);
  if (DefaultMib) {
    rtw_linux_set_exemptions (NULL, 0);
  }
  Result = NDIS_STATUS_SUCCESS;
  if (W->RadioOn && (RtwWifiEnsureStarted (W) != 0)) {
    Result = NDIS_STATUS_FAILURE;
  }

  RtwWifiIndicate (W, WDI_INDICATION_DOT11_RESET_COMPLETE, H.PortId, H.TransactionId, H.IhvSpecificId,
                   Result, NULL, 0);                                              // M4
}

/* ---- dispatch ---- */
static VOID
Dispatch (
  _In_ PRTW_WIFI    W,
  _In_ WIFIREQUEST  Request
  )
{
  PWDI_MESSAGE_HEADER  Header;
  UINT                 InLen;
  UINT                 OutLen;
  UINT16               Id;
  ULONG                Seen;

  InLen  = 0;
  OutLen = 0;
  Header = (PWDI_MESSAGE_HEADER)WifiRequestGetInOutBuffer (Request, &InLen, &OutLen);
  Id     = WifiRequestGetMessageId (Request);
  Seen   = 0;
  if (Id < RTW_WIFI_MSG_IDS) {
    Seen = W->MsgCount[Id]++;
  }

  if ((Header == NULL) || (InLen < sizeof (WDI_MESSAGE_HEADER))) {
    RtwLog ("wdi %u: %u bytes, no message header", Id, InLen);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }

  if (Seen < RTW_WIFI_LOG_PER_ID) {
    RtwLog ("wdi %u port %#x tid %u len %u/%u", Id, Header->PortId, Header->TransactionId, InLen, OutLen);
  }

  // never dump key material: SET_ADD_CIPHER_KEYS carries the keys, SET_SAE_AUTH_PARAMS SAE secrets
  if (W->WdiDump && (Seen == 0) && (Id != WDI_SET_ADD_CIPHER_KEYS) && (Id != WDI_SET_SAE_AUTH_PARAMS)) {
    DumpMessageTlvByteStream (Id, TRUE, W->Tlv.PeerVersion, InLen - sizeof (WDI_MESSAGE_HEADER),
                              (const UINT8 *)(Header + 1), 0, DumpLine);
  }

  // the Wi-Fi Direct device and role ports (wfd.cpp); with Parameters\WifiDirect = 0 there are none
  if (W->Wfd && RtwWifiWfdDispatch (W, Request, Header, InLen, Id)) {
    return;
  }

  switch (Id) {
    case WDI_SET_ADAPTER_CONFIGURATION:
      SetAdapterConfiguration (W, Request, Header, InLen);
      break;

    case WDI_TASK_SET_RADIO_STATE:
      TaskSetRadioState (W, Request, Header, InLen);
      break;

    case WDI_TASK_DOT11_RESET:
      TaskDot11Reset (W, Request, Header, InLen);
      break;

    case WDI_TASK_SCAN:
      RtwWifiTaskScan (W, Request, Header, InLen);
      break;

    case WDI_ABORT_TASK:
      RtwWifiAbortTask (W, Request, Header, InLen);
      break;

    case WDI_SET_FLUSH_BSS_ENTRY:
      // the OS keeps the BSS list (BSSListCachemanagement = FALSE): nothing cached here to flush
      RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
      break;

    case WDI_TASK_CONNECT:
      RtwWifiTaskConnect (W, Request, Header, InLen);
      break;

    case WDI_TASK_DISCONNECT:
      RtwWifiTaskDisconnect (W, Request, Header, InLen);
      break;

    case WDI_SET_CONNECTION_QUALITY:
      SetConnectionQuality (W, Request, Header, InLen);
      break;

    case WDI_GET_AUTO_POWER_SAVE:
      GetAutoPowerSave (W, Request, Header, OutLen);
      break;

    case WDI_SET_PRIVACY_EXEMPTION_LIST:
      RtwWifiSetPrivacyExemptionList (W, Request, Header, InLen);
      break;

    case WDI_SET_ADD_CIPHER_KEYS:
      RtwWifiSetAddCipherKeys (W, Request, Header, InLen);
      break;

    case WDI_SET_DELETE_CIPHER_KEYS:
      RtwWifiSetDeleteCipherKeys (W, Request, Header, InLen);
      break;

    case WDI_SET_DEFAULT_KEY_ID:
      RtwWifiSetDefaultKeyId (W, Request, Header, InLen);
      break;

    case WDI_SET_SAE_AUTH_PARAMS:
      RtwWifiSetSaeAuthParams (W, Request, Header, InLen);
      break;

    case WDI_SET_RECEIVE_PACKET_FILTER:
    case WDI_SET_MULTICAST_LIST:
      // accepted and not acted on: every group frame goes up and the OS filters
      RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
      break;

    default:
      if (Seen < RTW_WIFI_LOG_PER_ID) {
        RtwLog ("wdi %u: not supported", Id);
      }

      InterlockedIncrement (&W->Unsupported);
      if (Id < RTW_WIFI_MSG_IDS) {
        W->MsgUnsupported[Id]++;
      }

      RtwWifiComplete (W, Request, STATUS_NOT_SUPPORTED, sizeof (WDI_MESSAGE_HEADER));
      break;
  }
}

/* IRQL <= DISPATCH_LEVEL: queue for the work item, nothing else. */
VOID
RtwEvtWifiSendCommand (
  _In_ WDFDEVICE    Device,
  _In_ WIFIREQUEST  SendRequest
  )
{
  PRTW_WIFI  W;
  NTSTATUS   Fail;
  KIRQL      Irql;

  W = RtwGetWifi (Device);
  InterlockedIncrement (&W->Received);
  Fail = STATUS_SUCCESS;
  KeAcquireSpinLock (&W->Lock, &Irql);
  if (W->Stopping) {
    Fail = STATUS_DEVICE_NOT_READY;
  } else if (W->RingCount == RTW_WIFI_QUEUE_LEN) {
    Fail = STATUS_INSUFFICIENT_RESOURCES;
  } else {
    W->Ring[(W->RingHead + W->RingCount) % RTW_WIFI_QUEUE_LEN] = SendRequest;
    W->RingCount++;
  }

  KeReleaseSpinLock (&W->Lock, Irql);
  if (Fail != STATUS_SUCCESS) {
    RtwLog ("wdi %u refused (0x%08x)", WifiRequestGetMessageId (SendRequest), Fail);
    RtwWifiComplete (W, SendRequest, Fail, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  WdfWorkItemEnqueue (W->CmdWork);
}

/* The command thread: scan events first, then the next request, each under Dev->CtlLock. While stopping
   (ReleaseHardware) the queued requests are failed instead. An enqueue during a run starts another run, which
   leaves at once when this one is still in its loop: this one sees the new work before it lets go of WorkRunning
   (both under W->Lock), so nothing is left behind and only one thread takes requests and events. */
VOID
RtwWifiCmdWork (
  _In_ WDFWORKITEM  WorkItem
  )
{
  PRTW_WIFI    W;
  WIFIREQUEST  Request;
  BOOLEAN      Stopping;
  BOOLEAN      Done;
  BOOLEAN      Aborted;
  BOOLEAN      Timeout;
  BOOLEAN      ConnTimeout;
  BOOLEAN      ApEvents;
  BOOLEAN      RoleGone;
  ULONG        ConnEvents;
  KIRQL        Irql;

  W = RtwGetWifi ((WDFDEVICE)WdfWorkItemGetParentObject (WorkItem));
  KeAcquireSpinLock (&W->Lock, &Irql);
  if (W->WorkRunning) {
    KeReleaseSpinLock (&W->Lock, Irql);
    return;
  }

  W->WorkRunning = TRUE;
  KeReleaseSpinLock (&W->Lock, Irql);
  for ( ; ; ) {
    Request = NULL;
    KeAcquireSpinLock (&W->Lock, &Irql);
    Stopping         = W->Stopping;
    Done             = W->EvScanDone;
    Aborted          = W->EvScanAborted;
    Timeout          = W->EvScanTimeout;
    W->EvScanDone    = FALSE;
    W->EvScanAborted = FALSE;
    W->EvScanTimeout = FALSE;
    ConnEvents       = W->EvConnBits;
    ConnTimeout      = W->EvConnTimeout;
    W->EvConnBits    = 0;
    W->EvConn        = FALSE;
    W->EvConnTimeout = FALSE;
    ApEvents         = W->EvAp;
    RoleGone         = W->EvRoleGone;
    W->EvAp          = FALSE;
    W->EvRoleGone    = FALSE;
    if (W->RingCount != 0) {
      Request     = W->Ring[W->RingHead];
      W->RingHead = (W->RingHead + 1) % RTW_WIFI_QUEUE_LEN;
      W->RingCount--;
    }

    if ((Request == NULL) &&
        (Stopping || (!Done && !Timeout && (ConnEvents == 0) && !ConnTimeout && !ApEvents && !RoleGone)))
    {
      W->WorkRunning = FALSE;     // nothing left; a later enqueue starts a new run
      KeReleaseSpinLock (&W->Lock, Irql);
      break;
    }

    KeReleaseSpinLock (&W->Lock, Irql);
    if (Stopping) {
      // ReleaseHardware deals with the scan itself
      RtwWifiComplete (W, Request, STATUS_DEVICE_REMOVED, sizeof (WDI_MESSAGE_HEADER));
      continue;
    }

    WdfWaitLockAcquire (W->Dev->CtlLock, NULL);
    if (Done || Timeout) {
      RtwWifiScanEvents (W, Done, Aborted, Timeout);
    }

    if ((ConnEvents != 0) || ConnTimeout) {
      RtwWifiConnEvents (W, ConnEvents, ConnTimeout);
    }

    if (ApEvents || RoleGone) {
      RtwWifiApEvents (W, RoleGone);
    }

    if (Request != NULL) {
      Dispatch (W, Request);
    }

    WdfWaitLockRelease (W->Dev->CtlLock);
  }
}
