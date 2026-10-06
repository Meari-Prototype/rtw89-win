// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: Wi-Fi Direct, as far as Windows' mobile hotspot needs it. With
 * Parameters\WifiDirect on (driver\probe.c) PrepareHardware reports WIFI_WIFIDIRECT_CAPABILITIES (one role, a GO,
 * no P2P client, WPA2-PSK/CCMP); WiFiCx then creates the Wi-Fi Direct device (a WIFIDIRECTDEVICE: a control port, no
 * data path, no rtw89 interface), and the role NETADAPTER (adapter.cpp) when the hotspot starts, on which it runs
 * TASK_START_AP (ap.cpp). Only what the hotspot needs is handled here; the P2P discovery, negotiation, action frame
 * and service messages are refused (STATUS_NOT_SUPPORTED). With WifiDirect off nothing of this exists: no
 * capabilities, no objects, and every message goes the station's way (W->Wfd stays FALSE).
 * Everything runs in the command work item with Dev->CtlLock held, except the WiFiCx callbacks (PASSIVE_LEVEL).
 */
#include "wifi.h"

/* ---- the Wi-Fi Direct device ---- */
static EVT_WDF_OBJECT_CONTEXT_CLEANUP  WfdCleanup;

typedef struct _RTW_WFD_DEVICE {
  PRTW_WIFI  W;
} RTW_WFD_DEVICE, *PRTW_WFD_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME (RTW_WFD_DEVICE, RtwGetWfdDevice)

static VOID
WfdCleanup (
  _In_ WDFOBJECT  Object
  )
{
  PRTW_WIFI  W;
  KIRQL      Irql;

  W = RtwGetWfdDevice (Object)->W;
  if (W == NULL) {
    return;
  }

  KeAcquireSpinLock (&W->Lock, &Irql);
  if (W->WfdDevice == (WIFIDIRECTDEVICE)Object) {
    W->WfdDevice = NULL;
    W->WfdUp     = FALSE;
  }

  KeReleaseSpinLock (&W->Lock, Irql);
  RtwLog ("WiFiCx: Wi-Fi Direct device gone");
}

/* WiFiCx asks for the Wi-Fi Direct device (writing-a-wificx-client-driver, "Wi-Fi Direct event callback for
   WfdDevice"). Without Wi-Fi Direct nothing is created (like the sample). A failure is logged and
   success returned all the same, so the station port never suffers from it. PASSIVE_LEVEL. */
NTSTATUS
RtwEvtWifiCreateWifiDirectDevice (
  _In_ WDFDEVICE                   Device,
  _Inout_ WIFIDIRECT_DEVICE_INIT   *WifiDirectDeviceInit
  )
{
  WDF_OBJECT_ATTRIBUTES  Attributes;
  WIFIDIRECTDEVICE       Wfd;
  PRTW_WIFI              W;
  NTSTATUS               Status;
  ULONG                  Port;
  KIRQL                  Irql;

  W = RtwGetWifi (Device);
  if (!W->Wfd) {
    return STATUS_SUCCESS;
  }

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attributes, RTW_WFD_DEVICE);
  Attributes.EvtCleanupCallback = WfdCleanup;
  Status = WifiDirectDeviceCreate (WifiDirectDeviceInit, &Attributes, &Wfd);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: WifiDirectDeviceCreate failed 0x%08x (no mobile hotspot)", Status);
    return STATUS_SUCCESS;
  }

  RtwGetWfdDevice (Wfd)->W = W;
  Status = WifiDirectDeviceInitialize (Wfd);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: WifiDirectDeviceInitialize failed 0x%08x (no mobile hotspot)", Status);
    return STATUS_SUCCESS;
  }

  Port = WifiDirectDeviceGetPortId (Wfd);
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->WfdDevice = Wfd;
  W->WfdPortId = Port;
  W->WfdUp     = TRUE;
  KeReleaseSpinLock (&W->Lock, Irql);
  RtwLog ("WiFiCx: Wi-Fi Direct device created, port %u, %02x:%02x:%02x:%02x:%02x:%02x", Port, W->WfdAddr[0],
          W->WfdAddr[1], W->WfdAddr[2], W->WfdAddr[3], W->WfdAddr[4], W->WfdAddr[5]);
  return STATUS_SUCCESS;
}

ULONG
RtwWifiPortKind (
  _In_ PRTW_WIFI  W,
  _In_ UINT16     PortId
  )
{
  ULONG  Kind;
  KIRQL  Irql;

  KeAcquireSpinLock (&W->Lock, &Irql);
  if (W->AdapterUp && (W->PortId == PortId)) {
    Kind = RTW_PORT_STA;
  } else if (W->RoleUp && (W->RolePortId == PortId)) {
    Kind = RTW_PORT_ROLE;
  } else if (W->WfdUp && (W->WfdPortId == PortId)) {
    Kind = RTW_PORT_WFD;
  } else {
    Kind = RTW_PORT_NONE;
  }

  KeReleaseSpinLock (&W->Lock, Irql);
  return Kind;
}

/* ---- capabilities ---- */
VOID
RtwWifiReportWfdCapabilities (
  _In_ PRTW_WIFI  W
  )
{
  WIFI_WIFIDIRECT_CAPABILITIES  Caps;
  DOT11_AUTH_CIPHER_PAIR        Algos[1];
  WDI_MAC_ADDRESS               Iface;
  UCHAR                         Chans[64];
  ULONG                         n5;
  NTSTATUS                      Status;

  W->Wfd = FALSE;
  if (rtw_linux_get_wfd () != RTW_WFD_ON) {
    return;
  }

  // the P2P device address: the efuse address, locally administered; the GO's interface address: that with one more
  // bit changed, so it differs from both
  RtlCopyMemory (W->WfdAddr, W->Caps->addr, 6);
  W->WfdAddr[0] |= 0x02;
  RtlCopyMemory (W->RoleAddr, W->WfdAddr, 6);
  W->RoleAddr[4] ^= 0x80;
  RtlCopyMemory (Iface.Address, W->RoleAddr, 6);

  n5 = rtw_linux_ap_channels (1, Chans, sizeof (Chans));
  Algos[0].AuthAlgoId   = DOT11_AUTH_ALGO_RSNA_PSK;
  Algos[0].CipherAlgoId = DOT11_CIPHER_ALGO_CCMP;

  WIFI_WIFIDIRECT_CAPABILITIES_INIT (&Caps);
  Caps.WFDRoleCount                        = 1;       // rtw89: two interfaces, the station has one
  Caps.ConcurrentGOCount                   = 1;
  // Observed on Windows 11 26100: with ConcurrentClientCount 0 or ScanSSIDListSize below 4 the Native WiFi filter
  // (nwifi.sys) fails to attach (0xC0000001) and Wlansvc gets no WLAN interface at all, the station included.
  // So one P2P client is claimed; the role port still refuses TASK_CONNECT (wfd.cpp RtwWifiWfdDispatch).
  Caps.ConcurrentClientCount               = 1;
  RtlCopyMemory (Caps.DeviceAddress, W->WfdAddr, 6);
  Caps.DiscoveryFilterListSize             = 0;
  Caps.GOClientTableSize                   = RTW_AP_MAX_CLIENTS;
  Caps.MaxVendorSpecificExtensionIESize    = RTW_AP_IES_MAX;
  Caps.GOon5GHzBandSupported               = (n5 != 0);
  Caps.GOon6GHzBandSupported               = FALSE;
  Caps.ScanSSIDListSize                    = 4;       // see above; 1 and 2 are refused; the ports never scan
  Caps.DesiredSSIDListSize                 = 1;
  Caps.PrivacyExemptionListSize            = RTW_EXEMPT_MAX;
  Caps.AssociationTableSize                = RTW_AP_MAX_CLIENTS;
  Caps.DefaultKeyTableSize                 = 4;
  Caps.WEPKeyValueMaxLength                = 0;
  Caps.NumSupportedUnicastAlgorithms       = 1;
  Caps.UnicastAlgorithms                   = Algos;
  Caps.NumSupportedMulticastDataAlgorithms = 1;
  Caps.MulticastDataAlgorithms             = Algos;
  Caps.NumInterfaceAddresses               = 1;
  Caps.InterfaceAddressList                = &Iface;
  Status = WifiDeviceSetWiFiDirectCapabilities (W->Device, &Caps);
  if (!NT_SUCCESS (Status)) {
    // the station goes on without the hotspot
    RtwLog ("WiFiCx: WifiDeviceSetWiFiDirectCapabilities failed 0x%08x; no mobile hotspot", Status);
    return;
  }

  W->Wfd = TRUE;
  RtwLog ("WiFiCx: Wi-Fi Direct capabilities reported: 1 GO role, %u clients, WPA2-PSK/CCMP, GO on 5 GHz %s "
          "(%u channels)", RTW_AP_MAX_CLIENTS, Caps.GOon5GHzBandSupported ? "yes" : "no", n5);
}

/* ---- messages ---- */
/* WDI_SET_ADVERTISEMENT_INFORMATION: the OS's additional beacon and probe response elements (WPS, P2P) go into the
   GO's frames; the P2P device information is only logged */
static VOID
SetAdvertisementInformation (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_SET_ADVERTISEMENT_INFORMATION_PARAMETERS  P;
  const UINT8                                   *Beacon;
  const UINT8                                   *Presp;
  NDIS_STATUS                                   Status;
  ULONG                                         BeaconLen;
  ULONG                                         PrespLen;
  LONG                                          Ret;

  Status = ParseWdiSetAdvertisementInformationToIhv (InLen - sizeof (*Header), (const UINT8 *)(Header + 1), &W->Tlv,
                                                     &P);
  if (Status != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiSetAdvertisementInformationToIhv (&P);
    RtwLog ("WiFiCx: SET_ADVERTISEMENT_INFORMATION not parsed (0x%08x)", Status);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  Ret = 0;
  if (P.Optional.AdditionalIEs_IsPresent) {
    Beacon    = P.AdditionalIEs.Optional.AdditionalBeaconIEs_IsPresent ? P.AdditionalIEs.AdditionalBeaconIEs.pElements : NULL;
    BeaconLen = P.AdditionalIEs.Optional.AdditionalBeaconIEs_IsPresent ? P.AdditionalIEs.AdditionalBeaconIEs.ElementCount : 0;
    Presp     = P.AdditionalIEs.Optional.AdditionalProbeResponseIEs_IsPresent ?
                P.AdditionalIEs.AdditionalProbeResponseIEs.pElements : NULL;
    PrespLen  = P.AdditionalIEs.Optional.AdditionalProbeResponseIEs_IsPresent ?
                P.AdditionalIEs.AdditionalProbeResponseIEs.ElementCount : 0;
    if (BeaconLen > RTW_AP_IES_MAX) {
      RtwLog ("WiFiCx: additional beacon elements of %u bytes left out", BeaconLen);
      BeaconLen = 0;
    }

    if (PrespLen > RTW_AP_IES_MAX) {
      RtwLog ("WiFiCx: additional probe response elements of %u bytes left out", PrespLen);
      PrespLen = 0;
    }

    Ret = rtw_linux_ap_set_ies (Beacon, BeaconLen, Presp, PrespLen);
    RtwLog ("WiFiCx: advertisement on port %u: %u bytes beacon, %u bytes probe response elements: %d", Header->PortId,
            BeaconLen, PrespLen, Ret);
  }

  RtwLog ("WiFiCx: advertisement: device information %u, capability %u, GO capability %u, services %u",
          P.Optional.DeviceInformation_IsPresent, P.Optional.DeviceCapability_IsPresent,
          P.Optional.GroupOwnerCapability_IsPresent, P.Optional.AdvertisedServices_IsPresent);
  CleanupParsedWdiSetAdvertisementInformationToIhv (&P);
  // elements that do not fit are only logged: the hotspot works without them
  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
}

/* WDI_TASK_CHANGE_OPERATION_MODE: only the modes the ports already have (P2P device, GO) */
static VOID
TaskChangeOperationMode (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen,
  _In_ ULONG                      Kind
  )
{
  WDI_TASK_CHANGE_OPERATION_MODE_PARAMETERS  P;
  WDI_MESSAGE_HEADER                         H;
  NDIS_STATUS                                Status;
  ULONG                                      Mode;

  H      = *Header;
  Status = ParseWdiTaskChangeOperationModeToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  Mode   = (ULONG)P.OperationMode;
  CleanupParsedWdiTaskChangeOperationModeToIhv (&P);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }

  RtwLog ("WiFiCx: operation mode %#x on port %u", Mode, H.PortId);
  if (!((Kind == RTW_PORT_ROLE) && (Mode == WDI_OPERATION_MODE_P2P_GO)) &&
      !((Kind == RTW_PORT_WFD) && (Mode == WDI_OPERATION_MODE_P2P_DEVICE)))
  {
    RtwWifiCompleteUnsupported (W, Request, WDI_TASK_CHANGE_OPERATION_MODE);
    return;
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3
  RtwWifiIndicate (W, WDI_INDICATION_CHANGE_OPERATION_MODE_COMPLETE, H.PortId, H.TransactionId, H.IhvSpecificId,
                   NDIS_STATUS_SUCCESS, NULL, 0);                                 // M4
}

/* WDI_TASK_DOT11_RESET of the Wi-Fi Direct device port: nothing runs on it */
static VOID
WfdDot11Reset (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header
  )
{
  WDI_MESSAGE_HEADER  H;

  H = *Header;
  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3
  RtwLog ("WiFiCx: dot11 reset on the Wi-Fi Direct device port %u", H.PortId);
  RtwWifiIndicate (W, WDI_INDICATION_DOT11_RESET_COMPLETE, H.PortId, H.TransactionId, H.IhvSpecificId,
                   NDIS_STATUS_SUCCESS, NULL, 0);                                 // M4
}

BOOLEAN
RtwWifiWfdDispatch (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen,
  _In_ UINT16              Id
  )
{
  ULONG  Kind;

  Kind = RtwWifiPortKind (W, Header->PortId);
  if ((Kind != RTW_PORT_WFD) && (Kind != RTW_PORT_ROLE)) {
    return FALSE;               // the station's and the adapter's messages: Dispatch takes them
  }

  switch (Id) {
    case WDI_TASK_START_AP:
    case WDI_TASK_STOP_AP:
    case WDI_TASK_SEND_AP_ASSOCIATION_RESPONSE:
      if (Kind != RTW_PORT_ROLE) {
        RtwWifiCompleteUnsupported (W, Request, Id);
      } else if (Id == WDI_TASK_START_AP) {
        RtwWifiApTaskStartAp (W, Request, Header, InLen);
      } else if (Id == WDI_TASK_STOP_AP) {
        RtwWifiApTaskStopAp (W, Request, Header, InLen);
      } else {
        RtwWifiApTaskSendAssocResponse (W, Request, Header, InLen);
      }

      return TRUE;

    case WDI_TASK_DOT11_RESET:
      if (Kind == RTW_PORT_ROLE) {
        RtwWifiApTaskDot11Reset (W, Request, Header, InLen);
      } else {
        WfdDot11Reset (W, Request, Header);
      }

      return TRUE;

    case WDI_TASK_DISCONNECT:
      if (Kind == RTW_PORT_ROLE) {
        RtwWifiApTaskDisconnect (W, Request, Header, InLen);
      } else {
        RtwWifiCompleteUnsupported (W, Request, Id);
      }

      return TRUE;

    case WDI_SET_ADVERTISEMENT_INFORMATION:
      SetAdvertisementInformation (W, Request, Header, InLen);
      return TRUE;

    case WDI_SET_P2P_LISTEN_STATE:
    case WDI_SET_P2P_WPS_ENABLED:
      // accepted and not acted on: no P2P discovery here; WPS needs nothing beyond open system authentication
      RtwLog ("wdi %u on port %u: accepted, nothing to do", Id, Header->PortId);
      RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
      return TRUE;

    case WDI_TASK_CHANGE_OPERATION_MODE:
      TaskChangeOperationMode (W, Request, Header, InLen, Kind);
      return TRUE;

    case WDI_TASK_CONNECT:      // a P2P client
    case WDI_TASK_SCAN:
    case WDI_TASK_P2P_DISCOVER:
    case WDI_TASK_P2P_SEND_REQUEST_ACTION_FRAME:
    case WDI_TASK_P2P_SEND_RESPONSE_ACTION_FRAME:
    case WDI_SET_P2P_START_BACKGROUND_DISCOVERY:
    case WDI_SET_P2P_STOP_BACKGROUND_DISCOVERY:
    case WDI_SET_SAE_AUTH_PARAMS:   // sae.cpp would feed it to the station's connection
      RtwWifiCompleteUnsupported (W, Request, Id);
      return TRUE;

    case WDI_SET_CONNECTION_QUALITY:
      // the station's power save policy follows only the station port's hint (command.cpp SetConnectionQuality)
      RtwLog ("wdi %u on port %u: accepted, the station's power save policy left alone", Id, Header->PortId);
      RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
      return TRUE;

    case WDI_SET_PRIVACY_EXEMPTION_LIST:
    case WDI_SET_ADD_CIPHER_KEYS:
    case WDI_SET_DELETE_CIPHER_KEYS:
    case WDI_SET_DEFAULT_KEY_ID:
      if (Kind == RTW_PORT_ROLE) {
        return FALSE;           // the GO's keys and exemption list (security.cpp, by port)
      }

      // the Wi-Fi Direct device has no data path: security.cpp would take these for the station's connection
      if (Id == WDI_SET_ADD_CIPHER_KEYS) {
        RtwWifiCompleteUnsupported (W, Request, Id);
      } else {
        RtwLog ("wdi %u on the Wi-Fi Direct device port %u: accepted, nothing to do", Id, Header->PortId);
        RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
      }

      return TRUE;

    default:
      // packet filters and the rest: Dispatch takes them
      return FALSE;
  }
}
