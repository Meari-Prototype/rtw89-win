// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: the group owner on the Wi-Fi Direct role port, for Windows' mobile hotspot, on the mac
 * layer's GO (mac\ap.c, rtw_linux_ap_*):
 *  TASK_START_AP: M3 -> channel (ChooseChannel) -> rtw_linux_ap_start -> INDICATION_P2P_GROUP_OPERATING_CHANNEL -> M4.
 *  A client: the mac layer answers its open system authentication and checks its (re)association request ->
 *  INDICATION_AP_ASSOCIATION_REQUEST_RECEIVED -> the OS decides: TASK_SEND_AP_ASSOCIATION_RESPONSE (M3) ->
 *  rtw_linux_ap_assoc_resp -> the response's tx status -> SEND_AP_ASSOCIATION_RESPONSE_COMPLETE (M4, with the
 *  response and the beacon's body, which the OS's authenticator takes our RSN element from) -> the OS runs the
 *  4-way handshake over the role port's data path and sets the keys (security.cpp, by port).
 *  A client gone (its deauthentication/disassociation, inactivity, no key in time): INDICATION_DISASSOCIATION.
 *  TASK_DISCONNECT (a client), TASK_STOP_AP, TASK_DOT11_RESET of the role port stop clients or the GO.
 *  The mac layer stopping the GO itself (hardware restart, regulatory domain): DISASSOCIATION for each client,
 *  INDICATION_STOP_AP, and after a restart INDICATION_CAN_SUSTAIN_AP.
 * Radio off and the station taking another channel stop the GO with INDICATION_STOP_AP; power down and release stop it
 * without indications. The mac layer's indications only set a flag and queue the command work item (ApEvent), which
 * runs RtwWifiApEvents with Dev->CtlLock held; everything here runs that way.
 */
#include "wifi.h"

#define RTW_EINVAL              22
#define RTW_ENOENT              2
#define RTW_ERANGE              34
#define RTW_EXDEV               18
#define RTW_AP_STOP_IHV_RESET   ((UINT32)WDI_STOP_AP_REASON_IHV_START + 1)   // a hardware restart (SER) stopped the GO

static const UINT8  RtwApBroadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- helpers ---- */
static BOOLEAN
RolePortUp (
  _In_ PRTW_WIFI  W,
  _In_ UINT16     PortId
  )
{
  return RtwWifiPortKind (W, PortId) == RTW_PORT_ROLE;
}

static ULONG
PeerIndex (
  _In_ PRTW_WIFI              W,
  _In_reads_ (6) const UINT8  *Peer
  )
{
  ULONG  i;

  for (i = 0; i < W->Ap.NumPeers; i++) {
    if (RtlCompareMemory (W->Ap.Peers[i], Peer, 6) == 6) {
      return i;
    }
  }

  return MAXULONG;
}

static VOID
PeerAdd (
  _In_ PRTW_WIFI              W,
  _In_reads_ (6) const UINT8  *Peer
  )
{
  if ((PeerIndex (W, Peer) == MAXULONG) && (W->Ap.NumPeers < RTW_AP_MAX_CLIENTS)) {
    RtlCopyMemory (W->Ap.Peers[W->Ap.NumPeers++], Peer, 6);
  }
}

/* TRUE if it was there */
static BOOLEAN
PeerRemove (
  _In_ PRTW_WIFI              W,
  _In_reads_ (6) const UINT8  *Peer
  )
{
  ULONG  i;

  i = PeerIndex (W, Peer);
  if (i == MAXULONG) {
    return FALSE;
  }

  W->Ap.NumPeers--;
  if (i != W->Ap.NumPeers) {
    RtlCopyMemory (W->Ap.Peers[i], W->Ap.Peers[W->Ap.NumPeers], 6);
  }

  return TRUE;
}

static WDI_BAND_ID
ApWdiBand (
  _In_ UCHAR  Band
  )
{
  return (Band == 0) ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
}

/* the global operating class (802.11-2020 Table E-4) of a 20 MHz channel */
static UINT8
OperatingClass (
  _In_ UCHAR  Band,
  _In_ UCHAR  Channel
  )
{
  if (Band == 0) {
    return 81;
  }

  return (Channel <= 48) ? 115 : (Channel <= 64) ? 118 : (Channel <= 144) ? 121 : (Channel <= 161) ? 124 : 125;
}

/* ---- indications ---- */
static VOID
IndicateDisassociation (
  _In_ PRTW_WIFI                     W,
  _In_reads_ (6) const UINT8         *Peer,
  _In_ UINT32                        WabiReason,
  _In_reads_bytes_opt_ (FrameLen) const UINT8  *Frame,
  _In_ ULONG                         FrameLen
  )
{
  WDI_INDICATION_DISASSOCIATION_PARAMETERS  P;
  NDIS_STATUS                               Status;
  UINT8                                     *Tlv;
  ULONG                                     TlvLen;

  if (!RolePortUp (W, W->Ap.PortId)) {
    return;
  }

  RtlCopyMemory (P.DisconnectIndicationParameters.MacAddress.Address, Peer, 6);
  P.DisconnectIndicationParameters.DisassociationWABIReason = (WDI_ASSOC_STATUS)WabiReason;
  if ((Frame != NULL) && (FrameLen != 0)) {
    if (WabiReason == WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED) {
      P.DeauthFrame.SimpleAssign ((UINT8 *)Frame, FrameLen);
      P.Optional.DeauthFrame_IsPresent = 1;
    } else if (WabiReason == WDI_ASSOC_STATUS_PEER_DISASSOCIATED) {
      P.DisassociationFrame.SimpleAssign ((UINT8 *)Frame, FrameLen);
      P.Optional.DisassociationFrame_IsPresent = 1;
    }
  }

  Tlv    = NULL;
  TlvLen = 0;
  Status = GenerateWdiIndicationDisassociationFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: GO DISASSOCIATION not generated (0x%08x)", Status);
    return;
  }

  RtwWifiIndicate (W, WDI_INDICATION_DISASSOCIATION, W->Ap.PortId, WDI_TRANSACTION_ID_UNSOLICIT, 0,
                   NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
  RtwLog ("WiFiCx: GO client %02x:%02x:%02x:%02x:%02x:%02x disassociation indicated, reason %u", Peer[0], Peer[1],
          Peer[2], Peer[3], Peer[4], Peer[5], WabiReason);
}

/* INDICATION_STOP_AP or INDICATION_CAN_SUSTAIN_AP with a reason */
static VOID
IndicateApReason (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    Sustain,
  _In_ UINT32     Reason
  )
{
  WDI_INDICATION_STOP_AP_PARAMETERS          Stop;
  WDI_INDICATION_CAN_SUSTAIN_AP_PARAMETERS   Can;
  NDIS_STATUS                                Status;
  UINT8                                      *Tlv;
  ULONG                                      TlvLen;

  if (!RolePortUp (W, W->Ap.PortId)) {
    return;
  }

  Tlv    = NULL;
  TlvLen = 0;
  if (Sustain) {
    Can.Reason = Reason;
    Status     = GenerateWdiIndicationCanSustainApFromIhv (&Can, 0, &W->Tlv, &TlvLen, &Tlv);
  } else {
    Stop.Reason = Reason;
    Status      = GenerateWdiIndicationStopApFromIhv (&Stop, 0, &W->Tlv, &TlvLen, &Tlv);
  }

  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: %s not generated (0x%08x)", Sustain ? "CAN_SUSTAIN_AP" : "STOP_AP", Status);
    return;
  }

  RtwWifiIndicate (W, Sustain ? WDI_INDICATION_CAN_SUSTAIN_AP : WDI_INDICATION_STOP_AP, W->Ap.PortId,
                   WDI_TRANSACTION_ID_UNSOLICIT, 0, NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
  RtwLog ("WiFiCx: %s indicated, reason %#x", Sustain ? "CAN_SUSTAIN_AP" : "STOP_AP", Reason);
}

/* the M4 of the running association response task: the result (Ind with its frames in W->ApBuf), or an empty one with
   Status (failure, aborted) when Ind is NULL */
static VOID
AssocResponseComplete (
  _In_ PRTW_WIFI                  W,
  _In_opt_ const struct rtw_ap_ind *Ind,
  _In_ NDIS_STATUS                Status
  )
{
  WDI_INDICATION_SEND_AP_ASSOCIATION_RESPONSE_COMPLETE_PARAMETERS  P;
  WDI_PHY_TYPE                                                     Phy;
  NDIS_STATUS                                                      Gen;
  UINT8                                                            *Tlv;
  ULONG                                                            TlvLen;

  W->Ap.RespActive  = FALSE;
  W->Ap.RespAborted = FALSE;
  if (!RolePortUp (W, W->Ap.RespPort)) {
    return;
  }

  if (Ind == NULL) {
    RtwWifiIndicate (W, WDI_INDICATION_SEND_AP_ASSOCIATION_RESPONSE_COMPLETE, W->Ap.RespPort, W->Ap.RespTid,
                     W->Ap.RespIhv, Status, NULL, 0);
    RtwLog ("WiFiCx: association response task done: status %#x", Status);
    return;
  }

  RtlCopyMemory (P.AssocResponseResult.PeerMACAddress.Address, Ind->peer, 6);
  P.AssocResponseResult.IsReassociationRequest   = Ind->reassoc;
  P.AssocResponseResult.IsReAssociationResponse  = Ind->reassoc;
  P.AssocResponseResult.AuthAlgorithm            = W->Ap.Rsn ? WDI_AUTH_ALGO_RSNA_PSK : WDI_AUTH_ALGO_80211_OPEN;
  P.AssocResponseResult.UnicastCipherAlgorithm   = W->Ap.Rsn ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
  P.AssocResponseResult.MulticastCipherAlgorithm = W->Ap.Rsn ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
  P.AssocResponseFrame.SimpleAssign (W->ApBuf, Ind->frame_len);
  // the beacon's body with its fixed fields, like the WDI sample (the OS reads our RSN element from it)
  P.BeaconIEs.SimpleAssign (W->ApBuf + Ind->frame_len, Ind->ies_len);
  // required since WDI 1.1.5: the PHY in use
  Phy = Ind->ht ? WDI_PHY_TYPE_HT : (Ind->band == 0) ? WDI_PHY_TYPE_ERP : WDI_PHY_TYPE_OFDM;
  P.ActivePhyList.SimpleAssign (&Phy, 1);
  Tlv    = NULL;
  TlvLen = 0;
  Gen    = GenerateWdiIndicationSendApAssociationResponseCompleteFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
  if (Gen != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: SEND_AP_ASSOCIATION_RESPONSE_COMPLETE not generated (0x%08x)", Gen);
    RtwWifiIndicate (W, WDI_INDICATION_SEND_AP_ASSOCIATION_RESPONSE_COMPLETE, W->Ap.RespPort, W->Ap.RespTid,
                     W->Ap.RespIhv, NDIS_STATUS_FAILURE, NULL, 0);
    return;
  }

  RtwWifiIndicate (W, WDI_INDICATION_SEND_AP_ASSOCIATION_RESPONSE_COMPLETE, W->Ap.RespPort, W->Ap.RespTid,
                   W->Ap.RespIhv, NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
  RtwLog ("WiFiCx: association response task done: %s, %u bytes response, %u bytes beacon body",
          Ind->accepted ? "associated" : "refused", Ind->frame_len, Ind->ies_len);
}

static VOID
IndicateAssocRequest (
  _In_ PRTW_WIFI                  W,
  _In_ const struct rtw_ap_ind    *Ind
  )
{
  WDI_INDICATION_AP_ASSOCIATION_REQUEST_PARAMETERS  P;
  NDIS_STATUS                                       Status;
  UINT8                                             *Tlv;
  ULONG                                             TlvLen;

  if (!W->Ap.Running || !RolePortUp (W, W->Ap.PortId)) {
    (VOID)rtw_linux_ap_disconnect (Ind->peer, 1);         // nobody to decide: the client goes
    return;
  }

  RtlCopyMemory (P.IncomingRequestInfo.AssocRequestParams.PeerMacAddress.Address, Ind->peer, 6);
  P.IncomingRequestInfo.AssocRequestParams.IsReassociationRequest = Ind->reassoc;
  P.IncomingRequestInfo.AssocRequestFrame.SimpleAssign (W->ApBuf, Ind->frame_len);
  Tlv    = NULL;
  TlvLen = 0;
  Status = GenerateWdiIndicationApAssociationRequestReceivedFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: AP_ASSOCIATION_REQUEST_RECEIVED not generated (0x%08x)", Status);
    return;
  }

  RtwWifiIndicate (W, WDI_INDICATION_AP_ASSOCIATION_REQUEST_RECEIVED, W->Ap.PortId, WDI_TRANSACTION_ID_UNSOLICIT, 0,
                   NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
  RtwLog ("WiFiCx: GO association request of %02x:%02x:%02x:%02x:%02x:%02x indicated (%u bytes%s)", Ind->peer[0],
          Ind->peer[1], Ind->peer[2], Ind->peer[3], Ind->peer[4], Ind->peer[5], Ind->frame_len,
          Ind->reassoc ? ", reassociation" : "");
}

/* the GO goes (mac layer included): every client the OS knows gets DISASSOCIATION (Wabi) when Indicate. A running
   association response task gets its M4 (aborted) whenever its port is still there, unless the device is going
   (ReleaseHardware): a task without M4 would hang (D0Exit sends the station's connect M4 likewise). */
static VOID
ApStopGo (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    Indicate,
  _In_ UINT32     Wabi
  )
{
  ULONG  i;

  if (W->Ap.RespActive) {
    if (Indicate || !W->Stopping) {
      AssocResponseComplete (W, NULL, NDIS_STATUS_REQUEST_ABORTED);
    }

    W->Ap.RespActive = FALSE;
  }

  rtw_linux_ap_stop (TRUE);
  if (Indicate) {
    for (i = 0; i < W->Ap.NumPeers; i++) {
      IndicateDisassociation (W, W->Ap.Peers[i], Wabi, NULL, 0);
    }
  }

  W->Ap.NumPeers = 0;
  if (W->Ap.Running) {
    RtwLog ("WiFiCx: GO %u stopped", W->Ap.Seq);
  }

  W->Ap.Running = FALSE;
}

/* ---- events from the mac layer (with its lock held, PASSIVE_LEVEL): record and signal only ---- */
static void
ApEvent (
  void      *Ctx,
  uint32_t  Events
  )
{
  PRTW_WIFI  W;
  BOOLEAN    Enqueue;
  KIRQL      Irql;

  UNREFERENCED_PARAMETER (Events);
  W = (PRTW_WIFI)Ctx;
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->EvAp  = TRUE;
  Enqueue  = !W->Stopping;
  KeReleaseSpinLock (&W->Lock, Irql);
  if (Enqueue) {
    WdfWorkItemEnqueue (W->CmdWork);
  }
}

VOID
RtwWifiApEvents (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    RoleGone
  )
{
  struct rtw_ap_ind  Ind;
  UINT32             Wabi;
  LONG               Ret;

  if (RoleGone && (W->Ap.Running || W->Ap.RespActive)) {
    RtwLog ("WiFiCx: the role port went while the GO ran; stopping it");
    ApStopGo (W, FALSE, 0);
  }

  if (W->Ap.RespActive && W->Ap.RespAborted) {
    AssocResponseComplete (W, NULL, NDIS_STATUS_REQUEST_ABORTED);
  }

  for ( ; ; ) {
    RtlZeroMemory (&Ind, sizeof (Ind));
    Ret = rtw_linux_ap_get_ind (&Ind, W->ApBuf, RTW_WIFI_AP_BUF);
    if (Ret == 0) {
      break;
    }

    if (Ret < 0) {
      RtwLog ("WiFiCx: GO indication %u dropped: %d", Ind.type, Ret);
      continue;
    }

    switch (Ind.type) {
      case RTW_AP_IND_ASSOC_REQ:
        IndicateAssocRequest (W, &Ind);
        break;

      case RTW_AP_IND_ASSOC_DONE:
        if (W->Ap.RespActive && (RtlCompareMemory (W->Ap.RespPeer, Ind.peer, 6) == 6)) {
          if ((Ind.status == 0) && Ind.accepted) {
            PeerAdd (W, Ind.peer);
          }

          if (Ind.status == 0) {
            AssocResponseComplete (W, &Ind, NDIS_STATUS_SUCCESS);
          } else {
            AssocResponseComplete (W, NULL, NDIS_STATUS_FAILURE);
          }
        } else if ((Ind.status == 0) && Ind.accepted) {
          // nobody waits for it any more (aborted): the client must not stay associated behind the OS's back
          RtwLog ("WiFiCx: association completed without a task; disconnecting the client");
          (VOID)rtw_linux_ap_disconnect (Ind.peer, 1);
        }

        break;

      case RTW_AP_IND_LEFT:
        if (PeerRemove (W, Ind.peer)) {
          IndicateDisassociation (W, Ind.peer, (UINT32)Ind.status, (Ind.frame_len != 0) ? W->ApBuf : NULL,
                                  Ind.frame_len);
        }

        break;

      case RTW_AP_IND_STOPPED:
        RtwLog ("WiFiCx: the mac layer stopped GO %u (%s)", W->Ap.Seq,
                (Ind.status == RTW_AP_STOP_RESTART) ? "hardware restart" : "channel no longer allowed");
        if (!W->Ap.Running) {
          break;
        }

        Wabi = (Ind.status == RTW_AP_STOP_RESTART) ? WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET :
                                                     WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN;
        ApStopGo (W, TRUE, Wabi);
        if (Ind.status == RTW_AP_STOP_RESTART) {
          IndicateApReason (W, FALSE, RTW_AP_STOP_IHV_RESET);
          IndicateApReason (W, TRUE, RTW_AP_STOP_IHV_RESET);      // the restart is done: it may start again
        } else {
          IndicateApReason (W, FALSE, WDI_STOP_AP_REASON_FREQUENCY_NOT_AVAILABLE);
        }

        break;

      default:
        break;
    }
  }
}

VOID
RtwWifiApStop (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    Indicate,
  _In_ UINT32     StopReason
  )
{
  BOOLEAN  Running;

  if (!W->Ap.Running && !W->Ap.RespActive) {
    return;
  }

  Running = W->Ap.Running;
  ApStopGo (W, Indicate, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
  if (Indicate && Running && (StopReason != 0)) {
    IndicateApReason (W, FALSE, StopReason);
  }
}

BOOLEAN
RtwWifiApAbort (
  _In_ PRTW_WIFI  W,
  _In_ UINT32     Oid,
  _In_ UINT32     TransactionId
  )
{
  KIRQL  Irql;

  if (((Oid != WDI_TASK_SEND_AP_ASSOCIATION_RESPONSE) && (Oid != WDI_DEFINE_OID (WDI_TASK_SEND_AP_ASSOCIATION_RESPONSE))) ||
      !W->Ap.RespActive || (W->Ap.RespTid != TransactionId))
  {
    return FALSE;
  }

  // the port must be clean afterwards: the client goes; the M4 (aborted) follows from the work item, after this M3
  RtwLog ("WiFiCx: abort of the association response task");
  (VOID)rtw_linux_ap_disconnect (W->Ap.RespPeer, 1);
  W->Ap.RespAborted = TRUE;
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->EvAp = TRUE;
  KeReleaseSpinLock (&W->Lock, Irql);
  WdfWorkItemEnqueue (W->CmdWork);
  return TRUE;
}

/* ---- the channel ---- */
#define RTW_AP_CANDS            96

typedef struct _RTW_AP_CHANS {
  ULONG  Count;
  UCHAR  Band[RTW_AP_CANDS];
  UCHAR  Channel[RTW_AP_CANDS];
} RTW_AP_CHANS;

static BOOLEAN
ChansHave (
  _In_ const RTW_AP_CHANS  *C,
  _In_ UCHAR               Band,
  _In_ UCHAR               Channel
  )
{
  ULONG  i;

  for (i = 0; i < C->Count; i++) {
    if ((C->Band[i] == Band) && (C->Channel[i] == Channel)) {
      return TRUE;
    }
  }

  return FALSE;
}

static VOID
ChansAdd (
  _Inout_ RTW_AP_CHANS  *C,
  _In_ UCHAR            Band,
  _In_ UCHAR            Channel
  )
{
  if ((C->Count < RTW_AP_CANDS) && !ChansHave (C, Band, Channel)) {
    C->Band[C->Count]    = Band;
    C->Channel[C->Count] = Channel;
    C->Count++;
  }
}

/* BSSs heard on or next to the channel (2.4 GHz: within 4 channels), from the scans' BSS table */
static ULONG
ChannelLoad (
  _In_reads_ (Count) const struct rtw_bss  *Bss,
  _In_ ULONG                               Count,
  _In_ UCHAR                               Band,
  _In_ UCHAR                               Channel
  )
{
  ULONG  Load;
  ULONG  i;
  LONG   d;

  Load = 0;
  for (i = 0; i < Count; i++) {
    if (Bss[i].band != Band) {
      continue;
    }

    d = (LONG)Bss[i].channel - (LONG)Channel;
    if ((Band == 0) ? ((d >= -4) && (d <= 4)) : (d == 0)) {
      Load += (Bss[i].age_ms < 600000) ? 2 : 1;
    }
  }

  return Load;
}

/* picks the GO's channel: NDIS_STATUS_SUCCESS with *Band, *Channel, or an NDIS_STATUS_DOT11_AP_* */
static NDIS_STATUS
ChooseChannel (
  _In_ PRTW_WIFI              W,
  _In_ const RTW_AP_CHANS     *Wanted,       // the OS's candidates, its advertised channel first; empty = any
  _In_ BOOLEAN                Advertised,    // Wanted's first entry is the OS's advertised operating channel
  _In_ BOOLEAN                MustUse,
  _Out_ UCHAR                 *Band,
  _Out_ UCHAR                 *Channel
  )
{
  RTW_AP_CHANS          Allowed;
  RTW_AP_CHANS          Cands;
  struct rtw_bss_list   List;
  struct rtw_bss        *Bss;
  UCHAR                 Chans[64];
  UCHAR                 StaBand;
  UCHAR                 StaChannel;
  ULONG                 n;
  ULONG                 i;
  ULONG                 Best;
  ULONG                 BestLoad;
  ULONG                 Load;
  ULONG                 NumBss;
  UCHAR                 b;
  BOOLEAN               Have5;

  UNREFERENCED_PARAMETER (W);
  *Band    = 0;
  *Channel = 0;
  RtlZeroMemory (&Allowed, sizeof (Allowed));
  for (b = 0; b < 2; b++) {
    n = rtw_linux_ap_channels (b, Chans, sizeof (Chans));
    for (i = 0; i < n; i++) {
      ChansAdd (&Allowed, b, Chans[i]);
    }
  }

  // the candidates the rules allow, in the OS's order
  RtlZeroMemory (&Cands, sizeof (Cands));
  for (i = 0; i < ((Wanted->Count != 0) ? Wanted->Count : Allowed.Count); i++) {
    b = (Wanted->Count != 0) ? Wanted->Band[i] : Allowed.Band[i];
    if (ChansHave (&Allowed, b, (Wanted->Count != 0) ? Wanted->Channel[i] : Allowed.Channel[i])) {
      ChansAdd (&Cands, b, (Wanted->Count != 0) ? Wanted->Channel[i] : Allowed.Channel[i]);
    }
  }

  // the station is on a channel: the GO can only share it (no MCC)
  if (rtw_linux_sta_channel (&StaBand, &StaChannel)) {
    if (!ChansHave (&Allowed, StaBand, StaChannel)) {
      return NDIS_STATUS_DOT11_AP_CHANNEL_NOT_ALLOWED;
    }

    if (!ChansHave (&Cands, StaBand, StaChannel) && MustUse) {
      for (i = 0; i < Cands.Count && (Cands.Band[i] != StaBand); i++) {
      }

      return (i == Cands.Count) ? NDIS_STATUS_DOT11_AP_BAND_CURRENTLY_NOT_AVAILABLE :
                                  NDIS_STATUS_DOT11_AP_CHANNEL_CURRENTLY_NOT_AVAILABLE;
    }

    *Band    = StaBand;
    *Channel = StaChannel;
    return NDIS_STATUS_SUCCESS;
  }

  if (Cands.Count == 0) {
    return NDIS_STATUS_DOT11_AP_BAND_NOT_ALLOWED;
  }

  // the OS named one and it is allowed: that one. A band list alone names no channel (observed: the mobile hotspot's
  // band setting sends just a band): its first channel is no choice, the least crowded below is.
  if (Advertised && (Wanted->Count != 0) && ChansHave (&Cands, Wanted->Band[0], Wanted->Channel[0])) {
    *Band    = Wanted->Band[0];
    *Channel = Wanted->Channel[0];
    return NDIS_STATUS_SUCCESS;
  }

  // else the least crowded, 5 GHz first; on 2.4 GHz only 1, 6, 11 when they are candidates
  Have5 = FALSE;
  for (i = 0; i < Cands.Count; i++) {
    Have5 = Have5 || (Cands.Band[i] == 1);
  }

  NumBss = 0;
  Bss    = (struct rtw_bss *)ExAllocatePool2 (POOL_FLAG_NON_PAGED, sizeof (*Bss) * 128, RTW_POOL_TAG);
  if (Bss != NULL) {
    NumBss = rtw_linux_bss_get (&List, Bss, 128);
  }

  Best     = MAXULONG;
  BestLoad = MAXULONG;
  for (i = 0; i < Cands.Count; i++) {
    if ((Have5 && (Cands.Band[i] != 1)) ||
        ((Cands.Band[i] == 0) && (Cands.Channel[i] != 1) && (Cands.Channel[i] != 6) && (Cands.Channel[i] != 11) &&
         (ChansHave (&Cands, 0, 1) || ChansHave (&Cands, 0, 6) || ChansHave (&Cands, 0, 11))))
    {
      continue;
    }

    Load = (Bss != NULL) ? ChannelLoad (Bss, NumBss, Cands.Band[i], Cands.Channel[i]) : 0;
    if (Load < BestLoad) {
      Best     = i;
      BestLoad = Load;
    }
  }

  if (Bss != NULL) {
    ExFreePoolWithTag (Bss, RTW_POOL_TAG);
  }

  if (Best == MAXULONG) {
    Best = 0;
  }

  *Band    = Cands.Band[Best];
  *Channel = Cands.Channel[Best];
  return NDIS_STATUS_SUCCESS;
}

/*
 * INDICATION_LINK_STATE_CHANGE on the role port: WiFiCx turns the port's link speeds from it into the
 * NetAdapter's link state (CPort::OnLinkStateChange -> UpdateLinkState: NDIS_STATUS_LINK_STATE, connected); without it
 * the role adapter stays "media disconnected" below the Native WiFi filter, which drops every frame the OS sends on
 * the GO port (pktmon: DropReason Media Disconnected), the 4-way handshake's message 1 first. The station's comes with
 * its association (connect.cpp IndicateLinkState); the GO's once START_AP succeeded, with our own address as the peer
 * (Peer NULL). The speeds are what our HT20 two-stream GO can do.
 */
static VOID
IndicateApLinkState (
  _In_ PRTW_WIFI                   W,
  _In_reads_opt_ (6) const UINT8   *Peer
  )
{
  WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS  P;
  WDI_LINK_INFO_CONTAINER                      Link;
  NDIS_STATUS                                  Status;
  UINT8                                        *Tlv;
  ULONG                                        TlvLen;
  UINT32                                       Kbps;

  if (!W->Ap.Running || !RolePortUp (W, W->Ap.PortId)) {
    return;
  }

  Kbps = 144400;                                // HT20, two streams, short guard interval
  RtlCopyMemory (P.LinkStateChangeParameters.PeerMACAddress.Address, (Peer != NULL) ? Peer : W->RoleAddr, 6);
  P.LinkStateChangeParameters.TxLinkSpeed = Kbps;
  P.LinkStateChangeParameters.RxLinkSpeed = Kbps;
  P.LinkStateChangeParameters.LinkQuality = 100;
  Link.LinkID = 0;
  RtlCopyMemory (Link.LocalLinkMACAddress.Address, W->RoleAddr, 6);
  RtlCopyMemory (Link.PeerLinkMACAddress.Address, (Peer != NULL) ? Peer : W->RoleAddr, 6);
  Link.ChannelNumber = W->Ap.Channel;
  Link.BandId        = ApWdiBand (W->Ap.Band);
  Link.RSSI          = -50;
  Link.Bandwidth     = 20;
  Link.TxMCS         = 0;
  Link.RxMCS         = 0;
  P.LinkInfo.SimpleAssign (&Link, 1);           // required since WDI 2.0.5

  Tlv    = NULL;
  TlvLen = 0;
  Status = GenerateWdiIndicationLinkStateChangeFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: GO LINK_STATE_CHANGE not generated (0x%08x)", Status);
    return;
  }

  RtwWifiIndicate (W, WDI_INDICATION_LINK_STATE_CHANGE, W->Ap.PortId, WDI_TRANSACTION_ID_UNSOLICIT, 0,
                   NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
}   // P's destructor leaves the link list alone (SimpleAssign)

static VOID
IndicateOperatingChannel (
  _In_ PRTW_WIFI                  W,
  _In_ const WDI_MESSAGE_HEADER   *H
  )
{
  WDI_INDICATION_P2P_GROUP_OPERATING_CHANNEL_PARAMETERS  P;
  struct rtw_radio_info                                  Radio;
  NDIS_STATUS                                            Status;
  UINT8                                                  *Tlv;
  ULONG                                                  TlvLen;

  // the country string with 0x04: the operating classes are the global ones (802.11-2020 Table E-4)
  RtlZeroMemory (&Radio, sizeof (Radio));
  rtw_linux_radio_info (&Radio);
  if ((Radio.reg_cur[0] >= 'A') && (Radio.reg_cur[0] <= 'Z')) {
    P.Channel.CountryOrRegionString[0] = (UINT8)Radio.reg_cur[0];
    P.Channel.CountryOrRegionString[1] = (UINT8)Radio.reg_cur[1];
  } else {
    P.Channel.CountryOrRegionString[0] = 'X';         // no country ("XX": not a country, 802.11 Annex E)
    P.Channel.CountryOrRegionString[1] = 'X';
  }

  P.Channel.CountryOrRegionString[2] = 0x04;
  P.Channel.OperatingClass           = OperatingClass (W->Ap.Band, W->Ap.Channel);
  P.Channel.ChannelNumber            = W->Ap.Channel;
  P.IndicateReason                   = WDI_P2P_CHANNEL_INDICATE_REASON_NEW_CONNECTION;
  P.APBandInformation.BandID         = ApWdiBand (W->Ap.Band);
  P.APBandInformation.IsPCPinPBSS    = 0;
  Tlv    = NULL;
  TlvLen = 0;
  Status = GenerateWdiIndicationP2pGroupOperatingChannelFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: P2P_GROUP_OPERATING_CHANNEL not generated (0x%08x)", Status);
    return;
  }

  // during START_AP, before its M4 (the WDI sample passes the task's transaction id)
  RtwWifiIndicate (W, WDI_INDICATION_P2P_GROUP_OPERATING_CHANNEL, H->PortId, H->TransactionId, H->IhvSpecificId,
                   NDIS_STATUS_SUCCESS, Tlv, TlvLen);
  FreeGenerated (Tlv);
}

/* ---- the tasks ---- */
VOID
RtwWifiApTaskStartAp (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  )
{
  WDI_TASK_START_AP_PARAMETERS  P;
  WDI_MESSAGE_HEADER            H;
  struct rtw_ap_start           Req;
  RTW_AP_CHANS                  Wanted;
  UCHAR                         Chans[64];
  NDIS_STATUS                   Parsed;
  NDIS_STATUS                   Result;
  NTSTATUS                      Status;
  BOOLEAN                       Psk;
  BOOLEAN                       Open;
  BOOLEAN                       UniCcmp;
  BOOLEAN                       UniNone;
  BOOLEAN                       McCcmp;
  BOOLEAN                       McNone;
  BOOLEAN                       MustUse;
  BOOLEAN                       Advertised;
  BOOLEAN                       Rsn;
  BOOLEAN                       Supported;
  ULONG                         n;
  ULONG                         i;
  ULONG                         j;
  UCHAR                         Band;
  UCHAR                         Channel;
  LONG                          Ret;

  H      = *Header;
  Parsed = ParseWdiTaskStartApToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiTaskStartApToIhv (&P);
    RtwLog ("WiFiCx: TASK_START_AP not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }

  RtlZeroMemory (&Req, sizeof (Req));
  RtlZeroMemory (&Wanted, sizeof (Wanted));
  Psk     = FALSE;
  Open    = FALSE;
  for (i = 0; i < P.AuthenticationAlgorithms.ElementCount; i++) {
    Psk  = Psk || (P.AuthenticationAlgorithms.pElements[i] == WDI_AUTH_ALGO_RSNA_PSK);
    Open = Open || (P.AuthenticationAlgorithms.pElements[i] == WDI_AUTH_ALGO_80211_OPEN);
  }

  UniCcmp = FALSE;
  UniNone = FALSE;
  for (i = 0; i < P.UnicastCipherAlgorithms.ElementCount; i++) {
    UniCcmp = UniCcmp || (P.UnicastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_CCMP);
    UniNone = UniNone || (P.UnicastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_NONE);
  }

  McCcmp = FALSE;
  McNone = FALSE;
  for (i = 0; i < P.MulticastCipherAlgorithms.ElementCount; i++) {
    McCcmp = McCcmp || (P.MulticastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_CCMP);
    McNone = McNone || (P.MulticastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_NONE);
  }

  // WPA2-Personal (PSK, CCMP) or an open GO; nothing else (no WPA3-SAE on the GO side: the Wi-Fi Direct
  // capabilities do not report it, and WDI defines its SAE messages for the station only)
  Rsn       = Psk && UniCcmp && McCcmp;
  Supported = Rsn || (Open && UniNone && (McNone || (P.MulticastCipherAlgorithms.ElementCount == 0)));
  Req.ssid_len = (uint8_t)min (P.DesiredSSID.ElementCount, sizeof (Req.ssid));
  if (Req.ssid_len != 0) {
    RtlCopyMemory (Req.ssid, P.DesiredSSID.pElements, Req.ssid_len);
  }

  Req.beacon_int  = (uint16_t)min (P.StaticParameters.BeaconPeriod, 1000u);
  Req.dtim_period = (uint8_t)min (P.StaticParameters.DTIMPeriod, 255u);
  Req.flags       = (uint8_t)((Rsn ? RTW_AP_F_RSN : 0) |
                              (P.StaticParameters.ExcludeUnencrypted ? RTW_AP_F_EXCLUDE_UNENCRYPTED : 0) |
                              (P.StaticParameters.DataRateSupport802_11b ? RTW_AP_F_11B : 0));
  MustUse = (P.StaticParameters.MustUseSpecifiedChannels != 0);

  // the OS's channels: its advertised one first, then the band/channel list (a band without channels: all of it)
  Advertised = FALSE;
  if (P.Optional.AdvertisedOperatingChannel_IsPresent && (P.AdvertisedOperatingChannel.ChannelNumber != 0) &&
      (P.AdvertisedOperatingChannel.ChannelNumber < 256))
  {
    ChansAdd (&Wanted, (P.AdvertisedOperatingChannel.ChannelNumber <= 14) ? 0 : 1,
              (UCHAR)P.AdvertisedOperatingChannel.ChannelNumber);
    Advertised = TRUE;
  }

  if (P.Optional.APBandChannelList_IsPresent) {
    for (i = 0; i < P.APBandChannelList.ElementCount; i++) {
      const WDI_AP_BAND_CHANNEL_LIST_CONTAINER  *B;
      UCHAR                                     b;

      B = &P.APBandChannelList.pElements[i];
      if ((B->BandID != WDI_BAND_ID_2400) && (B->BandID != WDI_BAND_ID_5000)) {
        continue;
      }

      b = (B->BandID == WDI_BAND_ID_2400) ? 0 : 1;
      if (B->Optional.ChannelList_IsPresent && (B->ChannelList.ElementCount != 0)) {
        for (j = 0; j < B->ChannelList.ElementCount; j++) {
          if ((B->ChannelList.pElements[j] != 0) && (B->ChannelList.pElements[j] < 256)) {
            ChansAdd (&Wanted, b, (UCHAR)B->ChannelList.pElements[j]);
          }
        }
      } else {
        n = rtw_linux_ap_channels (b, Chans, sizeof (Chans));
        for (j = 0; j < n; j++) {
          ChansAdd (&Wanted, b, Chans[j]);
        }
      }
    }
  }

  RtwLog ("WiFiCx: START_AP on port %u: SSID length %u, beacon %u, DTIM %u, auth PSK %u open %u, unicast CCMP %u "
          "none %u, multicast CCMP %u none %u, exclude unencrypted %u, 11b %u, non-P2P clients %u, must use channels "
          "%u, prefer over station %u, advertised channel %s%u, %u band lists, %u channels wanted", H.PortId,
          Req.ssid_len, P.StaticParameters.BeaconPeriod, P.StaticParameters.DTIMPeriod, Psk, Open, UniCcmp, UniNone,
          McCcmp, McNone, P.StaticParameters.ExcludeUnencrypted, P.StaticParameters.DataRateSupport802_11b,
          P.StaticParameters.AcceptNonP2PClients, MustUse, P.StaticParameters.PreferOverStation,
          P.Optional.AdvertisedOperatingChannel_IsPresent ? "" : "(none) ",
          P.AdvertisedOperatingChannel.ChannelNumber,
          P.Optional.APBandChannelList_IsPresent ? P.APBandChannelList.ElementCount : 0, Wanted.Count);
  CleanupParsedWdiTaskStartApToIhv (&P);

  if (!RolePortUp (W, H.PortId) || !W->HwReady || W->PoweredDown) {
    Status = STATUS_DEVICE_NOT_READY;
  } else if (!W->RadioOn) {
    Status = STATUS_INVALID_DEVICE_STATE;
  } else if (W->Ap.Running) {
    Status = STATUS_DEVICE_BUSY;        // WiFiCx stops it first; not expected
  } else if (!Supported || (Req.ssid_len == 0)) {
    Status = Supported ? STATUS_INVALID_PARAMETER : STATUS_NOT_SUPPORTED;
  } else {
    Status = STATUS_SUCCESS;
  }

  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: START_AP refused (0x%08x)", Status);
    RtwWifiComplete (W, Request, Status, sizeof (WDI_MESSAGE_HEADER));    // no M4 after a failed M3
    return;
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3
  W->Ap.Seq++;
  RtwWifiScanStop (W, TRUE);            // its M4 first; the GO's start would abort it anyway
  Result = ChooseChannel (W, &Wanted, Advertised, MustUse, &Band, &Channel);
  if (Result == NDIS_STATUS_SUCCESS) {
    RtlCopyMemory (Req.addr, W->RoleAddr, 6);
    Req.band    = Band;
    Req.channel = Channel;
    Ret = RtwWifiEnsureStarted (W);
    if (Ret == 0) {
      Ret = rtw_linux_ap_start (&Req, ApEvent, W);
    }

    if (Ret == -RTW_ERANGE) {
      Result = NDIS_STATUS_DOT11_AP_CHANNEL_NOT_ALLOWED;
    } else if (Ret == -RTW_EXDEV) {
      Result = NDIS_STATUS_DOT11_AP_CHANNEL_CURRENTLY_NOT_AVAILABLE;
    } else if (Ret != 0) {
      Result = NDIS_STATUS_FAILURE;
    }

    RtwLog ("WiFiCx: GO %u on %s channel %u: %d", W->Ap.Seq, (Band == 0) ? "2.4 GHz" : "5 GHz", Channel, Ret);
  } else {
    RtwLog ("WiFiCx: GO %u: no channel (%#x)", W->Ap.Seq, Result);
  }

  if (Result == NDIS_STATUS_SUCCESS) {
    W->Ap.Running  = TRUE;
    W->Ap.Rsn      = Rsn;
    W->Ap.PortId   = H.PortId;
    W->Ap.Band     = Band;
    W->Ap.Channel  = Channel;
    W->Ap.NumPeers = 0;
    IndicateOperatingChannel (W, &H);
    IndicateApLinkState (W, NULL);
  }

  RtwWifiIndicate (W, WDI_INDICATION_START_AP_COMPLETE, H.PortId, H.TransactionId, H.IhvSpecificId, Result, NULL,
                   0);                                                            // M4
}

VOID
RtwWifiApTaskStopAp (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  )
{
  WDI_MESSAGE_HEADER  H;

  UNREFERENCED_PARAMETER (InLen);
  H = *Header;
  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3
  RtwLog ("WiFiCx: STOP_AP on port %u%s", H.PortId, W->Ap.Running ? "" : " (not running)");
  ApStopGo (W, TRUE, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
  RtwWifiIndicate (W, WDI_INDICATION_STOP_AP_COMPLETE, H.PortId, H.TransactionId, H.IhvSpecificId,
                   NDIS_STATUS_SUCCESS, NULL, 0);                                 // M4
}

VOID
RtwWifiApTaskDot11Reset (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  )
{
  WDI_TASK_DOT11_RESET_PARAMETERS  P;
  WDI_MESSAGE_HEADER               H;
  NDIS_STATUS                      Status;
  BOOLEAN                          DefaultMib;
  const UINT8                      *A;

  H          = *Header;
  DefaultMib = FALSE;
  Status     = ParseWdiTaskDot11ResetToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Status == NDIS_STATUS_SUCCESS) {
    DefaultMib = (P.Dot11ResetParameters.SetDefaultMIB != 0);
    if (P.Optional.ResetMACAddress_IsPresent) {
      // the OS names the role port's address: the GO's BSSID from the next START_AP on
      A = P.ResetMACAddress.Address;
      RtwLog ("WiFiCx: role port reset gives MAC %02x:%02x:%02x:%02x:%02x:%02x (ours %02x:%02x:%02x:%02x:%02x:%02x)",
              A[0], A[1], A[2], A[3], A[4], A[5], W->RoleAddr[0], W->RoleAddr[1], W->RoleAddr[2], W->RoleAddr[3],
              W->RoleAddr[4], W->RoleAddr[5]);
      if (!(A[0] & 1) && ((A[0] | A[1] | A[2] | A[3] | A[4] | A[5]) != 0)) {
        RtlCopyMemory (W->RoleAddr, A, 6);
      }
    }
  }

  CleanupParsedWdiTaskDot11ResetToIhv (&P);
  if (Status != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: role TASK_DOT11_RESET not parsed (0x%08x)", Status);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3
  RtwLog ("WiFiCx: dot11 reset on the role port %u, default MIB %u%s", H.PortId, DefaultMib,
          W->Ap.Running ? ", GO running: stopping it" : "");
  // like STOP_AP, without INDICATION_STOP_AP; the GO port's exemption list is a MIB value
  ApStopGo (W, TRUE, WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET);
  if (DefaultMib) {
    rtw_linux_ap_set_exemptions (NULL, 0);
  }

  RtwWifiIndicate (W, WDI_INDICATION_DOT11_RESET_COMPLETE, H.PortId, H.TransactionId, H.IhvSpecificId,
                   NDIS_STATUS_SUCCESS, NULL, 0);                                 // M4
}

VOID
RtwWifiApTaskSendAssocResponse (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  )
{
  WDI_TASK_SEND_AP_ASSOCIATION_RESPONSE_PARAMETERS  P;
  WDI_MESSAGE_HEADER                                H;
  NDIS_STATUS                                       Parsed;
  NTSTATUS                                          Status;
  UCHAR                                             Peer[6];
  UCHAR                                             Ies[RTW_AP_IES_MAX];
  ULONG                                             IeLen;
  BOOLEAN                                           Accept;
  BOOLEAN                                           Reassoc;
  UINT16                                            Reason;
  LONG                                              Ret;

  H      = *Header;
  Parsed = ParseWdiTaskSendApAssociationResponseToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiTaskSendApAssociationResponseToIhv (&P);
    RtwLog ("WiFiCx: TASK_SEND_AP_ASSOCIATION_RESPONSE not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }

  RtlCopyMemory (Peer, P.IncomingRequestInfo.AssocRequestParams.PeerMacAddress.Address, 6);
  Reassoc = (P.IncomingRequestInfo.AssocRequestParams.IsReassociationRequest != 0);
  Accept  = (P.AssocResponseParameters.AcceptRequest != 0);
  Reason  = P.AssocResponseParameters.ReasonCode;
  IeLen   = 0;
  if (P.Optional.VendorIEs_IsPresent && (P.VendorIEs.ElementCount != 0)) {
    if (P.VendorIEs.ElementCount <= sizeof (Ies)) {
      IeLen = P.VendorIEs.ElementCount;
      RtlCopyMemory (Ies, P.VendorIEs.pElements, IeLen);
    } else {
      RtwLog ("WiFiCx: association response elements of %u bytes left out", P.VendorIEs.ElementCount);
    }
  }

  CleanupParsedWdiTaskSendApAssociationResponseToIhv (&P);
  RtwLog ("WiFiCx: association response to %02x:%02x:%02x:%02x:%02x:%02x: accept %u, code %u, %u bytes of "
          "elements", Peer[0], Peer[1], Peer[2], Peer[3], Peer[4], Peer[5], Accept, Reason, IeLen);
  if (!RolePortUp (W, H.PortId)) {
    Status = STATUS_DEVICE_NOT_READY;
  } else if (!W->Ap.Running) {
    Status = STATUS_INVALID_DEVICE_STATE;
  } else if (W->Ap.RespActive) {
    Status = STATUS_DEVICE_BUSY;
  } else {
    Status = STATUS_SUCCESS;
  }

  if (!NT_SUCCESS (Status)) {
    RtwWifiComplete (W, Request, Status, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3
  W->Ap.RespActive  = TRUE;
  W->Ap.RespAborted = FALSE;
  W->Ap.RespReassoc = Reassoc;
  W->Ap.RespPort    = H.PortId;
  W->Ap.RespTid     = H.TransactionId;
  W->Ap.RespIhv     = H.IhvSpecificId;
  RtlCopyMemory (W->Ap.RespPeer, Peer, 6);
  Ret = rtw_linux_ap_assoc_resp (Peer, Accept, Accept ? 0 : ((Reason != 0) ? Reason : 1), (IeLen != 0) ? Ies : NULL,
                                 IeLen);
  if (Ret != 0) {
    RtwLog ("WiFiCx: association response not sent: %d", Ret);
    AssocResponseComplete (W, NULL, (Ret == -RTW_ENOENT) ? NDIS_STATUS_INVALID_STATE : NDIS_STATUS_FAILURE);
  }

  // else the mac layer's RTW_AP_IND_ASSOC_DONE completes it (RtwWifiApEvents)
}

VOID
RtwWifiApTaskDisconnect (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  )
{
  WDI_TASK_DISCONNECT_PARAMETERS  P;
  WDI_MESSAGE_HEADER              H;
  NDIS_STATUS                     Parsed;
  UCHAR                           Peer[6];
  UINT16                          Reason;
  ULONG                           i;
  LONG                            Ret;

  H      = *Header;
  Parsed = ParseWdiTaskDisconnectToIhv (InLen - sizeof (H), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  RtlCopyMemory (Peer, P.DisconnectParameters.MacAddress.Address, 6);
  Reason = P.DisconnectParameters.Disassociation80211Reason;
  CleanupParsedWdiTaskDisconnectToIhv (&P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: role TASK_DISCONNECT not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, 0);
    return;
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));      // M3
  Ret = W->Ap.Running ? rtw_linux_ap_disconnect (Peer, Reason) : -RTW_ENOENT;
  RtwLog ("WiFiCx: GO disconnect of %02x:%02x:%02x:%02x:%02x:%02x, reason %u: %d", Peer[0], Peer[1], Peer[2],
          Peer[3], Peer[4], Peer[5], Reason, Ret);
  if (RtlCompareMemory (Peer, RtwApBroadcast, 6) == 6) {
    for (i = 0; i < W->Ap.NumPeers; i++) {
      IndicateDisassociation (W, W->Ap.Peers[i], WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST, NULL, 0);
    }

    W->Ap.NumPeers = 0;
  } else if (PeerRemove (W, Peer)) {
    IndicateDisassociation (W, Peer, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST, NULL, 0);
  }

  RtwWifiIndicate (W, WDI_INDICATION_DISCONNECT_COMPLETE, H.PortId, H.TransactionId, H.IhvSpecificId,
                   NDIS_STATUS_SUCCESS, NULL, 0);                                 // M4
}

/* ---- the station meets the GO ---- */
VOID
RtwWifiApBeforeConnect (
  _In_ PRTW_WIFI                  W,
  _Inout_ struct rtw_connect_req  *Rq
  )
{
  ULONG  Kept;
  ULONG  i;

  if (!W->Ap.Running) {
    return;
  }

  // one channel at a time (rtw89 allows an AP only with a single channel, no MCC): the candidates on the GO's
  // channel, else the station takes precedence and the GO stops
  Kept = 0;
  for (i = 0; i < Rq->n_cands; i++) {
    if ((Rq->cands[i].band == W->Ap.Band) && (Rq->cands[i].channel == W->Ap.Channel)) {
      Rq->cands[Kept++] = Rq->cands[i];
    }
  }

  if (Kept != 0) {
    RtwLog ("WiFiCx: connect while the GO runs: %u of %u candidates on its channel %u", Kept, Rq->n_cands,
            W->Ap.Channel);
    Rq->n_cands = (uint8_t)Kept;
    return;
  }

  RtwLog ("WiFiCx: connect while the GO runs on channel %u: no candidate there; the GO stops", W->Ap.Channel);
  RtwWifiApStop (W, TRUE, WDI_STOP_AP_REASON_CHANNEL_NOT_AVAILABLE);
}

VOID
RtwWifiApScanLimit (
  _In_ PRTW_WIFI               W,
  _Inout_ struct rtw_scan_req  *Rq
  )
{
  if (!W->Ap.Running || (rtw_linux_ap_clients () == 0)) {
    return;
  }

  // the firmware would leave the GO's channel; its clients would miss beacons
  Rq->n_channels           = 1;
  Rq->channels[0].band     = W->Ap.Band;
  Rq->channels[0].number   = W->Ap.Channel;
  Rq->flags               |= RTW_SCAN_PASSIVE;
}
