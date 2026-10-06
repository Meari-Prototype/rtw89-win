// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: connections. WDI_TASK_CONNECT and WDI_TASK_DISCONNECT on the mac layer's station MLME
 * (rtw_linux_connect/disconnect, mac\mlme.c), and the indications that go with them:
 * TASK_CONNECT: M3 -> WDI_INDICATION_ASSOCIATION_RESULT per candidate tried -> (connected) LINK_STATE_CHANGE ->
 * CONNECT_COMPLETE (M4). TASK_DISCONNECT: M3 -> deauthentication -> DISASSOCIATION (by host) -> DISCONNECT_COMPLETE.
 * A connection that ends by itself (deauthentication or disassociation by the AP, beacon loss) is reported with
 * WDI_INDICATION_DISASSOCIATION.
 * Accepted: open networks (open system authentication, no cipher), WPA2-Personal (RSNA-PSK, CCMP as unicast and
 * multicast cipher) and, with Parameters\Pmf 2 and the crypto self-tests passed, WPA3-Personal (WPA3_SAE, CCMP, AKM
 * SAE, MFPEnabled). A request may allow both SAE and WPA2-PSK; each candidate then gets the first of them it offers
 * (mac layer). MFPEnabled lets the mac layer negotiate management frame protection with a BSS that has MFPC;
 * ASSOCIATION_RESULT says SAE and BIP when it was used. SAE's frames go back and forth with the OS while the connect
 * task runs (sae.cpp). Anything else (OWE, 802.1X, TKIP, WEP) is refused in the M3 with STATUS_NOT_SUPPORTED, with
 * no M4 and nothing on the air.
 * A connect flagged as a roam (Wlansvc's reconnect after a lost link) is an ordinary connect once the port is idle;
 * while it is still connected or connecting it is refused as busy.
 * Optional gates, refused the same way: AllowConnect 0 (the volatile Parameters\Allow\AllowConnect if present, else
 * Parameters\AllowConnect; neither present: allowed), and the volatile Parameters\Allow\AllowSsid (REG_SZ): only a
 * request for exactly that SSID goes on.
 * Target check (only the network the OS asked for): a candidate must be in AllowedBSSIDs if the OS gave that list,
 * not in DisallowedBSSIDs, and (mac layer) carry the requested SSID.
 * The mac layer's events (ConnEvent) and the timeout only set flags and queue the command work item, which runs
 * RtwWifiConnEvents with Dev->CtlLock held (wifi.h); everything here runs that way.
 */
#include "wifi.h"

#define RTW_EID_RSN             48
#define RTW_EID_VENDOR          221
#define RTW_CAP_PRIVACY         0x0010
#define RTW_REASON_LEAVING      3       // WLAN_REASON_DEAUTH_LEAVING
#define RTW_EINVAL              22
#define RTW_CONN_IES_MAX        256     // the OS's association request elements we take along

/* ---- the optional gate ---- */
/* FALSE only while an AllowConnect that is present says 0: the volatile Parameters\Allow\AllowConnect if there is
   one, else the persistent Parameters\AllowConnect. Neither present: connections are allowed. */
static BOOLEAN
AllowConnect (
  VOID
  )
{
  DECLARE_CONST_UNICODE_STRING (Name, L"AllowConnect");
  DECLARE_CONST_UNICODE_STRING (SubName, L"Allow");
  WDFKEY  Key;
  WDFKEY  Sub;
  ULONG   Value;
  ULONG   Volatile;

  Value = 1;
  if (!NT_SUCCESS (WdfDriverOpenParametersRegistryKey (WdfGetDriver (), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
    return TRUE;
  }

  if (!NT_SUCCESS (WdfRegistryQueryULong (Key, &Name, &Value))) {
    Value = 1;
  }

  if (NT_SUCCESS (WdfRegistryOpenKey (Key, &SubName, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Sub))) {
    if (NT_SUCCESS (WdfRegistryQueryULong (Sub, &Name, &Volatile))) {
      Value = Volatile;
    }

    WdfRegistryClose (Sub);
  }

  WdfRegistryClose (Key);
  return Value != 0;
}

/* The optional SSID pin: TRUE if Parameters\Allow\AllowSsid is absent, or present and equal (as UTF-8) to Ssid. */
static BOOLEAN
SsidAllowed (
  _In_reads_bytes_ (SsidLen) const UINT8  *Ssid,
  _In_ ULONG                              SsidLen,
  _Out_ BOOLEAN                           *Pinned
  )
{
  DECLARE_CONST_UNICODE_STRING (Name, L"AllowSsid");
  DECLARE_CONST_UNICODE_STRING (SubName, L"Allow");
  WCHAR     Value[34];
  CHAR      Utf8[3 * 32];
  WDFKEY    Key;
  WDFKEY    Sub;
  ULONG     Bytes;
  ULONG     Type;
  ULONG     Chars;
  ULONG     Utf8Len;
  NTSTATUS  Status;

  *Pinned = FALSE;
  if (!NT_SUCCESS (WdfDriverOpenParametersRegistryKey (WdfGetDriver (), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
    return TRUE;
  }

  Status = WdfRegistryOpenKey (Key, &SubName, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Sub);
  WdfRegistryClose (Key);
  if (!NT_SUCCESS (Status)) {
    return TRUE;
  }

  RtlZeroMemory (Value, sizeof (Value));
  Bytes  = 0;
  Type   = REG_NONE;
  Status = WdfRegistryQueryValue (Sub, &Name, sizeof (Value) - sizeof (WCHAR), Value, &Bytes, &Type);
  WdfRegistryClose (Sub);
  if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {
    return TRUE;
  }

  // present: from here on, anything but an exact match refuses
  *Pinned = TRUE;
  if (!NT_SUCCESS (Status) || (Type != REG_SZ)) {
    return FALSE;
  }

  Chars = Bytes / sizeof (WCHAR);
  while ((Chars != 0) && (Value[Chars - 1] == L'\0')) {
    Chars--;
  }

  Utf8Len = 0;
  if (!NT_SUCCESS (RtlUnicodeToUTF8N (Utf8, sizeof (Utf8), &Utf8Len, Value, Chars * sizeof (WCHAR)))) {
    return FALSE;
  }

  return (Utf8Len == SsidLen) && (RtlCompareMemory (Utf8, Ssid, SsidLen) == SsidLen);
}

/* A beacon/probe response body that may be a WPA2 network: Privacy bit and an RSN element (the mac layer checks
   the element's ciphers and AKMs) */
static BOOLEAN
BodyHasRsn (
  _In_reads_bytes_ (Len) const UINT8  *Body,
  _In_ ULONG                          Len
  )
{
  ULONG  Off;

  if ((Len < 12) || !((Body[10] | (Body[11] << 8)) & RTW_CAP_PRIVACY)) {
    return FALSE;
  }

  for (Off = 12; Off + 2 <= Len && Off + 2 + Body[Off + 1] <= Len; Off += 2 + Body[Off + 1]) {
    if (Body[Off] == RTW_EID_RSN) {
      return TRUE;
    }
  }

  return FALSE;
}

static const UINT8  RtwBroadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Address in List; with Wildcard (AllowedBSSIDs) the broadcast address in the list matches any BSSID, the desired
   BSSID list's default (the WDI sample's PlatformIsInDesiredBSSIDList) */
static BOOLEAN
AddressListed (
  _In_ const WDI_ADDRESS_LIST_CONTAINER  *List,
  _In_reads_ (6) const UINT8             *Address,
  _In_ BOOLEAN                           Wildcard
  )
{
  ULONG  i;

  for (i = 0; i < List->ElementCount; i++) {
    if ((RtlCompareMemory (List->pElements[i].Address, Address, 6) == 6) ||
        (Wildcard && (RtlCompareMemory (List->pElements[i].Address, RtwBroadcast, 6) == 6)))
    {
      return TRUE;
    }
  }

  return FALSE;
}

/* A beacon/probe response body (from the Timestamp on) of an open BSS: no Privacy bit, no RSN or WPA element */
static BOOLEAN
BodyIsOpen (
  _In_reads_bytes_ (Len) const UINT8  *Body,
  _In_ ULONG                          Len
  )
{
  ULONG  Off;

  if ((Len < 12) || ((Body[10] | (Body[11] << 8)) & RTW_CAP_PRIVACY)) {
    return FALSE;
  }

  for (Off = 12; Off + 2 <= Len && Off + 2 + Body[Off + 1] <= Len; Off += 2 + Body[Off + 1]) {
    if (Body[Off] == RTW_EID_RSN) {
      return FALSE;
    }

    if ((Body[Off] == RTW_EID_VENDOR) && (Body[Off + 1] >= 4) && (Body[Off + 2] == 0x00) && (Body[Off + 3] == 0x50) &&
        (Body[Off + 4] == 0xf2) && (Body[Off + 5] == 1))
    {
      return FALSE;       // WPA (vendor element)
    }
  }

  return TRUE;
}

static WDI_BAND_ID
WdiBand (
  _In_ UINT8  Nl80211Band
  )
{
  return (Nl80211Band == 0) ? WDI_BAND_ID_2400 : (Nl80211Band == 1) ? WDI_BAND_ID_5000 : WDI_BAND_ID_UNKNOWN;
}

static UINT32
LinkQuality (
  _In_ LONG  Rssi
  )
{
  // like the BSS list: 0..100 over -100..-50 dBm
  Rssi = (Rssi != 0) ? Rssi : -100;
  return (UINT32)min (max (2 * (Rssi + 100), 0), 100);
}

/* ---- indications ---- */
static VOID
IndicateAssociation (
  _In_ PRTW_WIFI                     W,
  _In_ const struct rtw_conn_info    *Info,
  _In_ BOOLEAN                       Success,
  _In_opt_ PUCHAR                    Frames
  )
{
  WDI_INDICATION_ASSOCIATION_RESULT_LIST  P;
  WDI_ASSOCIATION_RESULT_CONTAINER        *E;
  WDI_PHY_TYPE                            Phy;
  NDIS_STATUS                             Status;
  UINT8                                   *Tlv;
  ULONG                                   TlvLen;

  Status = P.AssociationResults.AllocateElements (1, 0);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: no memory for an association result");
    return;
  }

  E = &P.AssociationResults.pElements[0];
  RtlCopyMemory (E->BSSID.Address, Info->bssid, sizeof (E->BSSID.Address));
  E->AssociationResultParameters.AssociationStatus = Success ? WDI_ASSOC_STATUS_SUCCESS : (WDI_ASSOC_STATUS)Info->status;
  // a success is 0 even when the connection ended in the same run of the mac layer (its reason is in status_code then)
  E->AssociationResultParameters.StatusCode = Success ? 0 : Info->status_code;
  E->AssociationResultParameters.ReAssociation = FALSE;
  // what was negotiated (the RSN element of our request): PSK or SAE, and BIP with management frame protection
  E->AssociationResultParameters.AuthAlgorithm = (Info->auth == RTW_AUTH_SAE) ? WDI_AUTH_ALGO_WPA3_SAE :
                                                 (Info->auth == RTW_AUTH_RSN_PSK) ? WDI_AUTH_ALGO_RSNA_PSK :
                                                                                    WDI_AUTH_ALGO_80211_OPEN;
  E->AssociationResultParameters.UnicastCipherAlgorithm = (WDI_CIPHER_ALGORITHM)Info->pairwise;
  E->AssociationResultParameters.MulticastDataCipherAlgorithm = (WDI_CIPHER_ALGORITHM)Info->group;
  E->AssociationResultParameters.MulticastMgmtCipherAlgorithm = (WDI_CIPHER_ALGORITHM)Info->mgmt_group;
  // the OS authorizes the port itself: open at once, WPA2/WPA3 after its 4-way handshake
  E->AssociationResultParameters.PortAuthorized = FALSE;
  E->AssociationResultParameters.WMMQoSEnabled = Info->wmm;
  E->AssociationResultParameters.DSInfo = WDI_DS_UNKNOWN;
  E->AssociationResultParameters.BandID = WdiBand (Info->band);
  E->AssociationResultParameters.IHVAssociationStatus = Success ? 0 : (UINT32)Info->status;
  if (Success && (Frames != NULL)) {
    // the bodies follow each other in the order of the *_len fields
    if (Info->assoc_req_len != 0) {
      E->AssociationRequestFrame.SimpleAssign (Frames, Info->assoc_req_len);
      E->Optional.AssociationRequestFrame_IsPresent = 1;
    }

    Frames += Info->assoc_req_len;
    if (Info->assoc_resp_len != 0) {
      E->AssociationResponseFrame.SimpleAssign (Frames, Info->assoc_resp_len);
      E->Optional.AssociationResponseFrame_IsPresent = 1;
    }

    Frames += Info->assoc_resp_len;
    if (Info->auth_resp_len != 0) {
      E->AuthenticationResponseFrame.SimpleAssign (Frames, Info->auth_resp_len);
      E->Optional.AuthenticationResponseFrame_IsPresent = 1;
    }

    Frames += Info->auth_resp_len;
    if (Info->bss_body_len != 0) {
      E->BeaconProbeResponse.SimpleAssign (Frames, Info->bss_body_len);
      E->Optional.BeaconProbeResponse_IsPresent = 1;
    }
  }

  // required since WDI 1.1.4: the PHY in use
  Phy = (Info->phy == RTW_PHY_VHT) ? WDI_PHY_TYPE_VHT : Info->ht ? WDI_PHY_TYPE_HT :
        (Info->band == 0) ? WDI_PHY_TYPE_ERP : WDI_PHY_TYPE_OFDM;
  E->ActivePhyTypeList.SimpleAssign (&Phy, 1);

  Tlv    = NULL;
  TlvLen = 0;
  Status = GenerateWdiIndicationAssociationResultFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: ASSOCIATION_RESULT not generated (0x%08x)", Status);
    return;
  }

  RtwWifiIndicate (W, WDI_INDICATION_ASSOCIATION_RESULT, W->Conn.PortId, WDI_TRANSACTION_ID_UNSOLICIT,
                   W->Conn.IhvSpecificId, NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
  RtwLog ("WiFiCx: association result %02x:%02x:%02x:%02x:%02x:%02x: status %d (802.11 %u)%s%s", Info->bssid[0],
          Info->bssid[1], Info->bssid[2], Info->bssid[3], Info->bssid[4], Info->bssid[5],
          Success ? 0 : Info->status, Success ? 0 : Info->status_code, Success ? ", connected" : "",
          !Success ? "" : (Info->auth == RTW_AUTH_SAE) ? ", WPA3-SAE, MFP" : Info->mfp ? ", MFP" : "");
}   // P's destructor frees the entry; the frames and the PHY list are not owned (SimpleAssign)

static VOID
IndicateLinkState (
  _In_ PRTW_WIFI                   W,
  _In_ const struct rtw_conn_info  *Info
  )
{
  WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS  P;
  WDI_LINK_INFO_CONTAINER                      Link;
  NDIS_STATUS                                  Status;
  UINT8                                        *Tlv;
  ULONG                                        TlvLen;
  UINT32                                       Kbps;

  // kbit/s (the sample reports 30000). rtw89's rate report comes with traffic; until it comes: VHT and HT40 the top
  // rate of the mode on 2 streams with short GI, HT20 65 Mbps, legacy 54 Mbps
  if (Info->tx_kbps != 0) {
    Kbps = Info->tx_kbps;
  } else if (Info->phy == RTW_PHY_VHT) {
    Kbps = (Info->width_mhz >= 80) ? 866700 : (Info->width_mhz >= 40) ? 400000 : 173300;
  } else if (Info->ht) {
    Kbps = (Info->width_mhz >= 40) ? 300000 : 65000;
  } else {
    Kbps = 54000;
  }
  RtlCopyMemory (P.LinkStateChangeParameters.PeerMACAddress.Address, Info->bssid, 6);
  P.LinkStateChangeParameters.TxLinkSpeed = Kbps;
  P.LinkStateChangeParameters.RxLinkSpeed = (Info->rx_kbps != 0) ? Info->rx_kbps : Kbps;
  P.LinkStateChangeParameters.LinkQuality = (UINT8)LinkQuality (Info->rssi);
  Link.LinkID = 0;
  RtlCopyMemory (Link.LocalLinkMACAddress.Address, W->Caps->addr, 6);
  RtlCopyMemory (Link.PeerLinkMACAddress.Address, Info->bssid, 6);
  Link.ChannelNumber = Info->channel;
  Link.BandId        = WdiBand (Info->band);
  Link.RSSI          = (Info->rssi != 0) ? Info->rssi : -100;
  Link.Bandwidth     = (Info->width_mhz != 0) ? Info->width_mhz : 20;   // MHz the station uses with this AP
  Link.TxMCS         = 0;
  Link.RxMCS         = 0;
  P.LinkInfo.SimpleAssign (&Link, 1);       // required since WDI 2.0.5

  Tlv    = NULL;
  TlvLen = 0;
  Status = GenerateWdiIndicationLinkStateChangeFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: LINK_STATE_CHANGE not generated (0x%08x)", Status);
    return;
  }

  RtwWifiIndicate (W, WDI_INDICATION_LINK_STATE_CHANGE, W->Conn.PortId, WDI_TRANSACTION_ID_UNSOLICIT,
                   W->Conn.IhvSpecificId, NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
}

/* Frame: the deauthentication/disassociation body the AP sent (reason code on), if any */
static VOID
IndicateDisassociation (
  _In_ PRTW_WIFI                  W,
  _In_ UINT32                     WabiReason,
  _In_reads_bytes_opt_ (FrameLen) PUCHAR  Frame,
  _In_ ULONG                      FrameLen
  )
{
  WDI_INDICATION_DISASSOCIATION_PARAMETERS  P;
  NDIS_STATUS                               Status;
  UINT8                                     *Tlv;
  ULONG                                     TlvLen;

  if (!RtwWifiPortUp (W, W->Conn.PortId)) {
    return;
  }

  RtlCopyMemory (P.DisconnectIndicationParameters.MacAddress.Address, W->Conn.Bssid, 6);
  P.DisconnectIndicationParameters.DisassociationWABIReason = (WDI_ASSOC_STATUS)WabiReason;
  if ((Frame != NULL) && (FrameLen != 0)) {
    if (WabiReason == WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED) {
      P.DeauthFrame.SimpleAssign (Frame, FrameLen);
      P.Optional.DeauthFrame_IsPresent = 1;
    } else if (WabiReason == WDI_ASSOC_STATUS_PEER_DISASSOCIATED) {
      P.DisassociationFrame.SimpleAssign (Frame, FrameLen);
      P.Optional.DisassociationFrame_IsPresent = 1;
    }
  }

  Tlv    = NULL;
  TlvLen = 0;
  Status = GenerateWdiIndicationDisassociationFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: DISASSOCIATION not generated (0x%08x)", Status);
    return;
  }

  RtwWifiIndicate (W, WDI_INDICATION_DISASSOCIATION, W->Conn.PortId, WDI_TRANSACTION_ID_UNSOLICIT,
                   W->Conn.IhvSpecificId, NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
  RtwLog ("WiFiCx: disassociation indicated, reason %u", WabiReason);
}

/* the M4 of the running connect task */
static VOID
ConnectComplete (
  _In_ PRTW_WIFI    W,
  _In_ NDIS_STATUS  Status,
  _In_ BOOLEAN      Indicate
  )
{
  RTW_CONN_TASK  C;

  C                 = W->Conn;
  W->Conn.Active    = FALSE;
  WdfTimerStop (W->ConnTimer, FALSE);
  if (Indicate && RtwWifiPortUp (W, C.PortId)) {
    RtwWifiIndicate (W, WDI_INDICATION_CONNECT_COMPLETE, C.PortId, C.TransactionId, C.IhvSpecificId, Status, NULL, 0);
  }

  RtwLog ("WiFiCx: connect %u done after %llu ms: status %#x%s", C.Seq, (rtwk_time_ns () - C.StartNs) / 1000000,
          Status, Indicate ? "" : " (not indicated)");
}

/* ---- events from the mac layer (with its lock held, PASSIVE_LEVEL): record and signal only ---- */
static void
ConnEvent (
  void      *Ctx,
  uint32_t  Events
  )
{
  PRTW_WIFI  W;
  BOOLEAN    Enqueue;
  KIRQL      Irql;

  W = (PRTW_WIFI)Ctx;
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->EvConn      = TRUE;
  W->EvConnBits |= Events;
  Enqueue        = !W->Stopping;
  KeReleaseSpinLock (&W->Lock, Irql);
  if (Enqueue) {
    WdfWorkItemEnqueue (W->CmdWork);
  }
}

/* DISPATCH_LEVEL: the connect task took too long; the work item cancels it. */
VOID
RtwWifiConnTimer (
  _In_ WDFTIMER  Timer
  )
{
  PRTW_WIFI  W;
  BOOLEAN    Enqueue;
  KIRQL      Irql;

  W = RtwGetWifi ((WDFDEVICE)WdfTimerGetParentObject (Timer));
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->EvConnTimeout = TRUE;
  Enqueue          = !W->Stopping;
  KeReleaseSpinLock (&W->Lock, Irql);
  if (Enqueue) {
    WdfWorkItemEnqueue (W->CmdWork);
  }
}

/* events that came in while we were calling the mac layer ourselves */
static ULONG
TakeEvents (
  _In_ PRTW_WIFI  W
  )
{
  ULONG  Events;
  KIRQL  Irql;

  KeAcquireSpinLock (&W->Lock, &Irql);
  Events        = W->EvConnBits;
  W->EvConnBits = 0;
  W->EvConn     = FALSE;
  KeReleaseSpinLock (&W->Lock, Irql);
  return Events;
}

static VOID
ConnEventsEx (
  _In_ PRTW_WIFI  W,
  _In_ ULONG      Events,
  _In_ BOOLEAN    Timeout,
  _In_ BOOLEAN    Indicate
  )
{
  struct rtw_conn_info  Info;
  struct rtw_sae_ind    Sae;
  LONG                  Ret;

  if (Timeout && W->Conn.Active) {
    RtwLog ("WiFiCx: connect %u timed out; cancelling", W->Conn.Seq);
    W->Conn.AbortRequested = TRUE;
    rtw_linux_connect_cancel ();      // ConnEvent runs inside: take its events now
    Events |= TakeEvents (W);
  }

  // SAE steps for the OS come before the candidate's result (an error indication precedes its failure)
  if (Events & RTW_CONN_EV_SAE) {
    if (Indicate) {
      RtwWifiSaeIndicate (W);
    } else {
      while (rtw_linux_sae_get (&Sae, W->SaeBuf, RTW_WIFI_SAE_BUF) == 1) {
        // nobody to tell: dropped
      }
    }
  }

  // the candidates that failed, in order
  if (Events & (RTW_CONN_EV_ATTEMPT | RTW_CONN_EV_FAILED | RTW_CONN_EV_CONNECTED)) {
    while (rtw_linux_conn_get (&Info, NULL, 0) == 1) {
      if (W->Conn.Active && Indicate && RtwWifiPortUp (W, W->Conn.PortId)) {
        IndicateAssociation (W, &Info, FALSE, NULL);
      }
    }
  }

  if (Events & RTW_CONN_EV_CONNECTED) {
    Ret = rtw_linux_conn_get (&Info, W->ConnBuf, RTW_WIFI_CONN_BUF);
    if ((Ret != 0) || !W->Conn.Active || W->Conn.AbortRequested) {
      // nobody waits for this connection any more (or its result cannot be read): leave again
      RtwLog ("WiFiCx: connected without a connect task (%d); disconnecting", Ret);
      rtw_linux_disconnect (RTW_REASON_LEAVING, TRUE);
      Events &= ~(ULONG)(RTW_CONN_EV_LOST | RTW_CONN_EV_LINK);
      if (W->Conn.Active) {
        ConnectComplete (W, NDIS_STATUS_REQUEST_ABORTED, Indicate);
      }
    } else {
      RtlCopyMemory (W->Conn.Bssid, Info.bssid, sizeof (W->Conn.Bssid));
      W->Conn.Connected = TRUE;
      if (Indicate && RtwWifiPortUp (W, W->Conn.PortId)) {
        IndicateAssociation (W, &Info, TRUE, W->ConnBuf);
        IndicateLinkState (W, &Info);
      }

      ConnectComplete (W, NDIS_STATUS_SUCCESS, Indicate);
    }
  }

  if ((Events & RTW_CONN_EV_FAILED) && W->Conn.Active) {
    ConnectComplete (W, W->Conn.AbortRequested ? NDIS_STATUS_REQUEST_ABORTED : NDIS_STATUS_FAILURE, Indicate);
  }

  if ((Events & RTW_CONN_EV_LOST) && W->Conn.Connected) {
    W->Conn.Connected = FALSE;
    Ret = rtw_linux_conn_get (&Info, W->ConnBuf, RTW_WIFI_CONN_BUF);
    if (Indicate) {
      if (Ret == 0) {
        IndicateDisassociation (W, (UINT32)Info.status,
                                W->ConnBuf + Info.assoc_req_len + Info.assoc_resp_len + Info.auth_resp_len +
                                Info.bss_body_len, Info.lost_frame_len);
      } else {
        IndicateDisassociation (W, WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN, NULL, 0);
      }
    }
  } else if ((Events & RTW_CONN_EV_LINK) && W->Conn.Connected && Indicate &&
             (rtw_linux_conn_get (&Info, NULL, 0) == 0) && RtwWifiPortUp (W, W->Conn.PortId))
  {
    IndicateLinkState (W, &Info);
  }
}

VOID
RtwWifiConnEvents (
  _In_ PRTW_WIFI  W,
  _In_ ULONG      Events,
  _In_ BOOLEAN    Timeout
  )
{
  ConnEventsEx (W, Events, Timeout, TRUE);
}

VOID
RtwWifiConnStop (
  _In_ PRTW_WIFI  W,
  _In_ UINT32     WabiReason,
  _In_ BOOLEAN    Indicate
  )
{
  if (W->Conn.Active) {
    W->Conn.AbortRequested = TRUE;
    rtw_linux_connect_cancel ();
    ConnEventsEx (W, TakeEvents (W), FALSE, Indicate);
  }

  if (W->Conn.Connected) {
    rtw_linux_disconnect (RTW_REASON_LEAVING, TRUE);
    (VOID)TakeEvents (W);     // nothing comes from a disconnect we asked for
    if (Indicate) {
      IndicateDisassociation (W, WabiReason, NULL, 0);
    }

    W->Conn.Connected = FALSE;
  }

  if (W->Conn.Active) {
    ConnectComplete (W, NDIS_STATUS_REQUEST_ABORTED, Indicate);
  }
}

BOOLEAN
RtwWifiConnAbort (
  _In_ PRTW_WIFI  W,
  _In_ UINT32     TransactionId
  )
{
  if (!W->Conn.Active || (W->Conn.TransactionId != TransactionId)) {
    return FALSE;
  }

  RtwLog ("WiFiCx: abort of connect %u", W->Conn.Seq);
  W->Conn.AbortRequested = TRUE;
  rtw_linux_connect_cancel ();        // ConnEvent queues the work item, which sends the M4 after this M3
  return TRUE;
}

/* ---- the tasks ---- */
VOID
RtwWifiTaskConnect (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  )
{
  WDI_TASK_CONNECT_PARAMETERS        P;
  const WDI_CONNECT_PARAMETERS_CONTAINER  *Cp;
  const WDI_CONNECT_BSS_ENTRY_CONTAINER  *B;
  WDI_MESSAGE_HEADER                 H;
  struct rtw_connect_req             Rq;
  const UINT8                        *Body;
  NDIS_STATUS                        Parsed;
  NTSTATUS                           Status;
  BOOLEAN                            OpenAuth;
  BOOLEAN                            RsnPsk;
  BOOLEAN                            NoCipher;
  BOOLEAN                            Ccmp;
  BOOLEAN                            McNone;
  BOOLEAN                            McCcmp;
  BOOLEAN                            AkmPsk;
  BOOLEAN                            Sae;
  BOOLEAN                            AkmSae;
  BOOLEAN                            SaeOk;
  BOOLEAN                            PskOk;
  BOOLEAN                            Mfp;
  BOOLEAN                            Supported;
  BOOLEAN                            Pinned;
  BOOLEAN                            AllowedAny;
  BOOLEAN                            AllowedWild;
  ULONG                              OtherAuth;
  ULONG                              BodyLen;
  ULONG                              Used;
  ULONG                              Seq;
  ULONG                              Cands;
  ULONG                              Skipped;
  ULONG                              i;
  LONG                               Ret;
  KIRQL                              Irql;

  H      = *Header;
  Parsed = ParseWdiTaskConnectToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiTaskConnectToIhv (&P);
    RtwLog ("WiFiCx: TASK_CONNECT not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, (Parsed == NDIS_STATUS_RESOURCES) ? STATUS_INSUFFICIENT_RESOURCES : STATUS_INVALID_PARAMETER, 0);
    return;
  }

  // what the attempt needs comes out of the request before M3
  Cp = &P.ConnectParameters;
  RtlZeroMemory (&Rq, sizeof (Rq));
  OpenAuth  = FALSE;
  RsnPsk    = FALSE;
  Sae       = FALSE;
  OtherAuth = 0;
  for (i = 0; i < Cp->AuthenticationAlgorithms.ElementCount; i++) {
    switch (Cp->AuthenticationAlgorithms.pElements[i]) {
      case WDI_AUTH_ALGO_80211_OPEN: OpenAuth = TRUE; break;
      case WDI_AUTH_ALGO_RSNA_PSK:   RsnPsk = TRUE; break;
      case WDI_AUTH_ALGO_WPA3_SAE:   Sae = TRUE; break;
      default:                       OtherAuth++; break;
    }
  }

  NoCipher = FALSE;
  Ccmp     = FALSE;
  for (i = 0; i < Cp->UnicastCipherAlgorithms.ElementCount; i++) {
    NoCipher = NoCipher || (Cp->UnicastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_NONE);
    Ccmp     = Ccmp || (Cp->UnicastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_CCMP);
  }

  McNone = FALSE;
  McCcmp = FALSE;
  for (i = 0; i < Cp->MulticastCipherAlgorithms.ElementCount; i++) {
    McNone = McNone || (Cp->MulticastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_NONE);
    McCcmp = McCcmp || (Cp->MulticastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_CCMP);
  }

  // an AKM list, if the OS gives one, must offer PSK (00-0F-AC:2) or SAE (00-0F-AC:8)
  AkmPsk = !Cp->Optional.RsnaAkmSuites_IsPresent;
  AkmSae = !Cp->Optional.RsnaAkmSuites_IsPresent;
  for (i = 0; Cp->Optional.RsnaAkmSuites_IsPresent && (i < Cp->RsnaAkmSuites.ElementCount); i++) {
    AkmPsk = AkmPsk || (Cp->RsnaAkmSuites.pElements[i] == rsna_akm_psk);
    AkmSae = AkmSae || (Cp->RsnaAkmSuites.pElements[i] == rsna_akm_sae_pmk256);
  }

  // MFP when the OS lets us offer it and it is on here; SAE needs it (WPA3), and Parameters\Pmf 2
  Mfp   = Cp->ConnectionSettings.MFPEnabled && (rtw_linux_get_pmf () != RTW_PMF_OFF);
  SaeOk = Sae && AkmSae && Mfp && (rtw_linux_get_pmf () == RTW_PMF_SAE);
  PskOk = RsnPsk && AkmPsk;

  // WPA3/WPA2-Personal first (each candidate gets the first of SAE, PSK it offers), else an open network; nothing else
  Supported = TRUE;
  if ((SaeOk || PskOk) && Ccmp && McCcmp) {
    Rq.auth  = SaeOk ? RTW_AUTH_SAE : RTW_AUTH_RSN_PSK;
    Rq.auths = (uint8_t)((SaeOk ? RTW_AUTH_BIT (RTW_AUTH_SAE) : 0) | (PskOk ? RTW_AUTH_BIT (RTW_AUTH_RSN_PSK) : 0));
  } else if (OpenAuth && NoCipher && (McNone || (Cp->MulticastCipherAlgorithms.ElementCount == 0))) {
    Rq.auth = RTW_AUTH_OPEN;
  } else {
    Supported = FALSE;
  }

  if (Cp->ConnectionSettings.ExcludeUnencrypted) {
    Rq.flags |= RTW_CONN_F_EXCLUDE_UNENCRYPTED;
  }

  if (Mfp && (Rq.auth != RTW_AUTH_OPEN)) {
    Rq.flags |= RTW_CONN_F_MFP;
  }

  if (Cp->ConnectionSettings.HiddenNetwork) {
    Rq.flags |= RTW_CONN_F_HIDDEN;
  }

  // the PHYs the OS allows, if it says: no VHT unless listed (or "any")
  if (Cp->Optional.ActivePhyTypeList_IsPresent && (Cp->ActivePhyTypeList.ElementCount != 0)) {
    BOOLEAN  VhtOk = FALSE;

    for (i = 0; i < Cp->ActivePhyTypeList.ElementCount; i++) {
      VhtOk = VhtOk || (Cp->ActivePhyTypeList.pElements[i] == WDI_PHY_TYPE_VHT) ||
              (Cp->ActivePhyTypeList.pElements[i] == WDI_PHY_TYPE_ANY);
    }

    RtwLog ("WiFiCx: connect: the OS's PHY list has %u entries%s", Cp->ActivePhyTypeList.ElementCount,
            VhtOk ? "" : ", no VHT: HT at most");
    if (!VhtOk) {
      Rq.flags |= RTW_CONN_F_NO_VHT;
    }
  }

  if (Cp->SSIDList.ElementCount != 0) {
    Rq.ssid_len = (uint8_t)min (Cp->SSIDList.pElements[0].ElementCount, sizeof (Rq.ssid));
    if (Rq.ssid_len != 0) {
      RtlCopyMemory (Rq.ssid, Cp->SSIDList.pElements[0].pElements, Rq.ssid_len);
    }
  }

  // AllowedBSSIDs restricts unless absent, empty or holding the broadcast address (any BSSID)
  AllowedWild = Cp->Optional.AllowedBSSIDs_IsPresent && AddressListed (&Cp->AllowedBSSIDs, RtwBroadcast, FALSE);
  AllowedAny  = !Cp->Optional.AllowedBSSIDs_IsPresent || (Cp->AllowedBSSIDs.ElementCount == 0) || AllowedWild;

  // the candidates, in the OS's order: only BSSs it allows, offering what it asked for (the mac layer checks the
  // SSID and the RSN element in detail); their beacon/probe response bodies go to ConnBuf
  Used    = 0;
  Skipped = 0;
  Cands   = P.PreferredBSSEntryList.ElementCount;
  for (i = 0; Supported && (i < P.PreferredBSSEntryList.ElementCount) && (Rq.n_cands < RTW_CONN_MAX_CANDS); i++) {
    B = &P.PreferredBSSEntryList.pElements[i];
    if (B->Optional.ProbeResponseFrame_IsPresent && (B->ProbeResponseFrame.ElementCount != 0)) {
      Body    = B->ProbeResponseFrame.pElements;
      BodyLen = B->ProbeResponseFrame.ElementCount;
    } else if (B->Optional.BeaconFrame_IsPresent && (B->BeaconFrame.ElementCount != 0)) {
      Body    = B->BeaconFrame.pElements;
      BodyLen = B->BeaconFrame.ElementCount;
    } else {
      Skipped++;
      continue;
    }

    if (((B->ChannelInfo.BandId != WDI_BAND_ID_2400) && (B->ChannelInfo.BandId != WDI_BAND_ID_5000)) ||
        (B->ChannelInfo.ChannelNumber == 0) || (B->ChannelInfo.ChannelNumber > 255) || (BodyLen > 2304) ||
        (BodyLen > RTW_WIFI_CONN_BUF - RTW_CONN_IES_MAX - Used) ||
        (!AllowedAny && !AddressListed (&Cp->AllowedBSSIDs, B->BSSID.Address, TRUE)) ||
        (Cp->Optional.DisallowedBSSIDs_IsPresent && AddressListed (&Cp->DisallowedBSSIDs, B->BSSID.Address, FALSE)) ||
        ((Rq.auth == RTW_AUTH_OPEN) ? !BodyIsOpen (Body, BodyLen) : !BodyHasRsn (Body, BodyLen)))
    {
      Skipped++;
      continue;
    }

    RtlCopyMemory (W->ConnBuf + Used, Body, BodyLen);
    RtlCopyMemory (Rq.cands[Rq.n_cands].bssid, B->BSSID.Address, 6);
    Rq.cands[Rq.n_cands].band     = (B->ChannelInfo.BandId == WDI_BAND_ID_2400) ? 0 : 1;
    Rq.cands[Rq.n_cands].channel  = (uint8_t)B->ChannelInfo.ChannelNumber;
    Rq.cands[Rq.n_cands].body_off = Used;
    Rq.cands[Rq.n_cands].body_len = (uint16_t)BodyLen;
    Rq.n_cands++;
    Used += BodyLen;
  }

  // the OS's own elements for the association request (vendor specific), after the bodies
  if (Supported && Cp->Optional.AssociationRequestVendorIE_IsPresent && (Cp->AssociationRequestVendorIE.ElementCount != 0)) {
    if (Cp->AssociationRequestVendorIE.ElementCount <= RTW_CONN_IES_MAX) {
      RtlCopyMemory (W->ConnBuf + Used, Cp->AssociationRequestVendorIE.pElements, Cp->AssociationRequestVendorIE.ElementCount);
      Rq.ies    = W->ConnBuf + Used;
      Rq.ie_len = Cp->AssociationRequestVendorIE.ElementCount;
    } else {
      RtwLog ("WiFiCx: association request elements of %u bytes left out", Cp->AssociationRequestVendorIE.ElementCount);
    }
  }

  Seq = W->Conn.Seq + 1;
  RtwLog ("WiFiCx: connect %u on port %u: SSID length %u, %u candidates (%u usable), auth open %u RSNA-PSK %u "
          "WPA3-SAE %u other %u, unicast none %u CCMP %u, multicast none %u CCMP %u, AKM list %s%s, exclude "
          "unencrypted %u, MFP %u, hidden %u, roam %u, %u allowed%s / %u disallowed BSSIDs", Seq, H.PortId,
          Rq.ssid_len, Cands, Rq.n_cands, OpenAuth, RsnPsk, Sae, OtherAuth, NoCipher, Ccmp, McNone, McCcmp,
          Cp->Optional.RsnaAkmSuites_IsPresent ? (AkmPsk ? "with PSK" : "without PSK") : "none",
          Cp->Optional.RsnaAkmSuites_IsPresent ? (AkmSae ? ", SAE" : ", no SAE") : "",
          Cp->ConnectionSettings.ExcludeUnencrypted, Cp->ConnectionSettings.MFPEnabled,
          Cp->ConnectionSettings.HiddenNetwork, Cp->ConnectionSettings.RoamRequest,
          Cp->Optional.AllowedBSSIDs_IsPresent ? Cp->AllowedBSSIDs.ElementCount : 0,
          AllowedWild ? " (allowed list has wildcard)" :
          (Cp->Optional.AllowedBSSIDs_IsPresent && (Cp->AllowedBSSIDs.ElementCount == 0)) ? " (empty: any)" : "",
          Cp->Optional.DisallowedBSSIDs_IsPresent ? Cp->DisallowedBSSIDs.ElementCount : 0);
  // RoamRequest: after a lost link (beacon loss, a hardware restart) Wlansvc reconnects with it set. The port is idle
  // by then, so it is an ordinary association; a roam from a live connection meets the busy check below.

  CleanupParsedWdiTaskConnectToIhv (&P);
  Rq.bodies     = W->ConnBuf;
  Rq.bodies_len = Used;

  // the gate: nothing reaches the mac layer (and the air) unless allowed (by default), supported and the network
  // pinned (if so)
  if (!AllowConnect ()) {
    RtwLog ("WiFiCx: connect refused (AllowConnect=0)");
    RtwWifiComplete (W, Request, STATUS_NOT_SUPPORTED, sizeof (WDI_MESSAGE_HEADER));     // no M4
    return;
  }

  if (!SsidAllowed (Rq.ssid, Rq.ssid_len, &Pinned)) {
    RtwLog ("WiFiCx: connect refused: not the SSID in Parameters\\Allow\\AllowSsid");
    RtwWifiComplete (W, Request, STATUS_NOT_SUPPORTED, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  if (!Supported || (Rq.n_cands == 0)) {
    RtwLog ("WiFiCx: connect refused: %s (%u candidates skipped)",
            !Supported ? "neither WPA2/WPA3-Personal (RSNA-PSK or SAE with MFP, CCMP) nor open" : "no usable candidate",
            Skipped);
    RtwWifiComplete (W, Request, STATUS_NOT_SUPPORTED, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  if (Pinned) {
    Rq.flags |= RTW_CONN_F_PINNED;    // the mac layer then wants the SSID in the body, hidden network or not
  }

  RtwLog ("WiFiCx: connect %u: %s%s, %u candidates%s", Seq,
          (Rq.auth == RTW_AUTH_OPEN) ? "open" : !PskOk ? "WPA3-SAE/CCMP" : SaeOk ? "WPA3-SAE or WPA2-PSK/CCMP" :
          "WPA2-PSK/CCMP", (Rq.flags & RTW_CONN_F_MFP) ? ", MFP allowed" : "", Rq.n_cands,
          Pinned ? ", SSID pinned by AllowSsid" : "");

  if (!RtwWifiPortUp (W, H.PortId) || !W->HwReady || W->PoweredDown) {
    Status = STATUS_DEVICE_NOT_READY;
  } else if (!W->RadioOn) {
    Status = STATUS_INVALID_DEVICE_STATE;
  } else if (W->Conn.Active || W->Conn.Connected) {
    Status = STATUS_DEVICE_BUSY;      // WiFiCx disconnects first; not expected
  } else {
    RtwWifiScanStop (W, TRUE);        // its M4 goes first
    RtwWifiApBeforeConnect (W, &Rq);  // a running GO keeps the channel, or stops (nothing without one)
    Ret = RtwWifiEnsureStarted (W);
    if (Ret == 0) {
      (VOID)TakeEvents (W);           // nothing stale may end this task
      KeAcquireSpinLock (&W->Lock, &Irql);
      W->EvConnTimeout = FALSE;
      KeReleaseSpinLock (&W->Lock, Irql);
      Ret = rtw_linux_connect (&Rq, ConnEvent, W);
    }

    Status = (Ret == 0) ? STATUS_SUCCESS : (Ret == -RTW_EINVAL) ? STATUS_INVALID_PARAMETER : RtwWifiErrnoStatus (Ret);
    if (Ret != 0) {
      RtwLog ("WiFiCx: connect %u not started: %d", Seq, Ret);
    }
  }

  if (!NT_SUCCESS (Status)) {
    RtwWifiComplete (W, Request, Status, sizeof (WDI_MESSAGE_HEADER));    // no M4 after a failed M3
    return;
  }

  RtlZeroMemory (&W->Conn, sizeof (W->Conn));
  W->Conn.Active        = TRUE;
  W->Conn.PortId        = H.PortId;
  W->Conn.TransactionId = H.TransactionId;
  W->Conn.IhvSpecificId = H.IhvSpecificId;
  W->Conn.StartNs       = rtwk_time_ns ();
  W->Conn.Seq           = Seq;
  W->Conn.Auth          = Rq.auth;
  WdfTimerStart (W->ConnTimer, WDF_REL_TIMEOUT_IN_MS (RTW_WIFI_CONN_TIMEOUT_MS));
  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));    // M3; the work item sends the rest
}

VOID
RtwWifiTaskDisconnect (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  )
{
  WDI_TASK_DISCONNECT_PARAMETERS  P;
  WDI_MESSAGE_HEADER              H;
  NDIS_STATUS                     Parsed;
  UINT16                          Reason;

  H      = *Header;
  Parsed = ParseWdiTaskDisconnectToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  Reason = P.DisconnectParameters.Disassociation80211Reason;
  CleanupParsedWdiTaskDisconnectToIhv (&P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: TASK_DISCONNECT not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }

  if (!RtwWifiPortUp (W, H.PortId)) {
    RtwWifiComplete (W, Request, STATUS_DEVICE_NOT_READY, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3
  RtwLog ("WiFiCx: disconnect on port %u, reason %u%s", H.PortId, Reason,
          W->Conn.Connected ? "" : W->Conn.Active ? " (while connecting)" : " (not connected)");
  if (Reason == 0) {
    Reason = RTW_REASON_LEAVING;
  }

  if (W->Conn.Active) {
    W->Conn.AbortRequested = TRUE;
    rtw_linux_connect_cancel ();
    ConnEventsEx (W, TakeEvents (W), FALSE, TRUE);
  }

  if (W->Conn.Connected) {
    rtw_linux_disconnect (Reason, TRUE);
    (VOID)TakeEvents (W);
    IndicateDisassociation (W, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST, NULL, 0);
    W->Conn.Connected = FALSE;
  }

  RtwWifiIndicate (W, WDI_INDICATION_DISCONNECT_COMPLETE, H.PortId, H.TransactionId, H.IhvSpecificId,
                   NDIS_STATUS_SUCCESS, NULL, 0);                                    // M4
}
