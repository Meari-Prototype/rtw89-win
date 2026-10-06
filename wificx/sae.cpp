// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: SAE authentication (WPA3-Personal, IEEE 802.11-2020 12.4). The OS computes SAE; the
 * mac layer (mac\sae.c) sends and receives the authentication frames of the running TASK_CONNECT:
 *  - WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED (unsolicited, the connect task's port): what the mac layer queued
 *    (rtw_linux_sae_get) - commit parameters needed, the AP's commit or confirm frame (the body from the algorithm
 *    number on), an error with its WDI SAE status, or a request for a new confirm. Microsoft's WiFiCx sample
 *    (wifihal.cpp WifiIhvSetSaeAuthParams) carries the AP's confirm in SAECommitFrame; the TLV also has a
 *    SAEConfirmFrame field. Which one the OS reads is not documented in the WDK, so a confirm goes in both.
 *  - WDI_SET_SAE_AUTH_PARAMS (a property: M3 only): the OS's commit or confirm parameters, or success/failure. It
 *    comes with a transaction ID of its own (observed on Windows 11 26100: never the connect task's, so the sample's
 *    check against that ID would refuse every exchange); without a running connect task it gets
 *    STATUS_INVALID_DEVICE_REQUEST, and the mac layer takes it only for the candidate in SAE and in the matching state.
 * Scalars, elements, tokens and confirms are not logged (only types, groups, lengths and statuses); the TLV dump
 * leaves WDI_SET_SAE_AUTH_PARAMS out (command.cpp). Everything runs in the command work item with Dev->CtlLock held.
 */
#include "wifi.h"

#define RTW_ENOTCONN            107
#define RTW_EINVAL              22
#define RTW_AKM_OUI_MASK        0x00ffffffu     // RSNA_AKM_SUITE: OUI in the low three octets, the type above
#define RTW_AKM_OUI_IEEE        0x00ac0f00u     // RSNA_OUI_PREFIX (00-0F-AC)

/* the mac layer's SAE indications for the running connect task, in order */
VOID
RtwWifiSaeIndicate (
  _In_ PRTW_WIFI  W
  )
{
  struct rtw_sae_ind  Ind;
  NDIS_STATUS         Status;
  UINT8               *Tlv;
  ULONG               TlvLen;
  LONG                Ret;

  for ( ; ; ) {
    WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED_PARAMETERS  P;

    Ret = rtw_linux_sae_get (&Ind, W->SaeBuf, RTW_WIFI_SAE_BUF);
    if (Ret != 1) {
      if (Ret < 0) {
        RtwLog ("WiFiCx: SAE indication not taken: %d", Ret);
      }

      return;
    }

    if (!W->Conn.Active || !RtwWifiPortUp (W, W->Conn.PortId)) {
      continue;               // nobody waits for it any more
    }

    RtlCopyMemory (P.BssId.Address, Ind.bssid, sizeof (P.BssId.Address));
    P.SAEIndicationType = (WDI_SAE_INDICATION_TYPE)Ind.type;
    if ((Ind.type == RTW_SAE_IND_COMMIT) && (Ind.frame_len != 0)) {
      P.SAECommitFrame.SimpleAssign (W->SaeBuf, Ind.frame_len);
      P.Optional.SAECommitFrame_IsPresent = 1;
    } else if ((Ind.type == RTW_SAE_IND_CONFIRM) && (Ind.frame_len != 0)) {
      P.SAEConfirmFrame.SimpleAssign (W->SaeBuf, Ind.frame_len);
      P.Optional.SAEConfirmFrame_IsPresent = 1;
      P.SAECommitFrame.SimpleAssign (W->SaeBuf, Ind.frame_len);     // where the sample puts it
      P.Optional.SAECommitFrame_IsPresent = 1;
    }

    if (Ind.has_status) {
      P.SAEStatus = (WDI_SAE_STATUS)Ind.status;
      P.Optional.SAEStatus_IsPresent = 1;
    }

    Tlv    = NULL;
    TlvLen = 0;
    Status = GenerateWdiIndicationSaeAuthParamsNeededFromIhv (&P, 0, &W->Tlv, &TlvLen, &Tlv);
    if (Status != NDIS_STATUS_SUCCESS) {
      RtwLog ("WiFiCx: SAE_AUTH_PARAMS_NEEDED not generated (0x%08x)", Status);
      continue;
    }

    RtwWifiIndicate (W, WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED, W->Conn.PortId, WDI_TRANSACTION_ID_UNSOLICIT,
                     W->Conn.IhvSpecificId, NDIS_STATUS_SUCCESS, Tlv, TlvLen);
    FreeGenerated (Tlv);
    if (Ind.has_status) {
      RtwLog ("WiFiCx: SAE indication for %02x:%02x:%02x:%02x:%02x:%02x: type %u, frame %u bytes, status %u",
              Ind.bssid[0], Ind.bssid[1], Ind.bssid[2], Ind.bssid[3], Ind.bssid[4], Ind.bssid[5], Ind.type,
              Ind.frame_len, Ind.status);
    } else {
      RtwLog ("WiFiCx: SAE indication for %02x:%02x:%02x:%02x:%02x:%02x: type %u, frame %u bytes", Ind.bssid[0],
              Ind.bssid[1], Ind.bssid[2], Ind.bssid[3], Ind.bssid[4], Ind.bssid[5], Ind.type, Ind.frame_len);
    }
  }
}

/* A blob of the parsed message, or none */
static VOID
SaeBlob (
  _In_ const WDI_BYTE_BLOB  &Blob,
  _In_ UINT32               Present,
  _Out_ const uint8_t       **Data,
  _Out_ uint16_t            *Len
  )
{
  if (!Present || (Blob.ElementCount == 0) || (Blob.ElementCount > 0xffff)) {
    *Data = NULL;
    *Len  = 0;
    return;
  }

  *Data = Blob.pElements;
  *Len  = (uint16_t)Blob.ElementCount;
}

VOID
RtwWifiSetSaeAuthParams (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_SET_SAE_AUTH_PARAMS_COMMAND  P;
  struct rtw_sae_params            Sp;
  NDIS_STATUS                      Parsed;
  NTSTATUS                         Status;
  UINT32                           Akm;
  LONG                             Ret;

  Parsed = ParseWdiSetSaeAuthParamsToIhv (InLen - sizeof (*Header), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiSetSaeAuthParamsToIhv (&P);
    RtwLog ("WiFiCx: SET_SAE_AUTH_PARAMS not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  // a direct property with its own transaction ID (the OS does not reuse the connect task's, whatever the sample
  // checks); it needs a running connect task, and the mac layer checks the BSSID and the SAE state
  if (!W->Conn.Active) {
    CleanupParsedWdiSetSaeAuthParamsToIhv (&P);
    RtwLog ("WiFiCx: SET_SAE_AUTH_PARAMS (transaction %u) without a running connect", Header->TransactionId);
    RtwWifiComplete (W, Request, STATUS_INVALID_DEVICE_REQUEST, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  RtlZeroMemory (&Sp, sizeof (Sp));
  RtlCopyMemory (Sp.bssid, P.BssId.Address, sizeof (Sp.bssid));
  Sp.type = (uint8_t)min ((ULONG)P.SAERequestType, 255u);
  if (P.Optional.SAECommitParams_IsPresent) {
    const WDI_SAE_COMMIT_PARAMS  &C = P.SAECommitParams;

    Sp.group       = C.FiniteCyclicGroup;
    Sp.status_code = C.StatusCode;
    SaeBlob (C.Scalar, C.Optional.Scalar_IsPresent, &Sp.scalar, &Sp.scalar_len);
    SaeBlob (C.Element, C.Optional.Element_IsPresent, &Sp.element, &Sp.element_len);
    SaeBlob (C.AntiCloggingToken, C.Optional.AntiCloggingToken_IsPresent, &Sp.token, &Sp.token_len);
    SaeBlob (C.RejectedGroups, C.Optional.RejectedGroups_IsPresent, &Sp.rejected, &Sp.rejected_len);
    if (C.Optional.RsnaAkmSuite_IsPresent && (C.RsnaAkmSuite.ElementCount != 0)) {
      Akm    = (UINT32)C.RsnaAkmSuite.pElements[0];     // the AKM of this exchange (a list in the TLV)
      Sp.akm = ((Akm & RTW_AKM_OUI_MASK) == RTW_AKM_OUI_IEEE) ? (uint8_t)(Akm >> 24) : 0xff;
    }
  }

  if (P.Optional.SAEConfirmParams_IsPresent) {
    Sp.send_confirm = P.SAEConfirmParams.SendConfirm;
    SaeBlob (P.SAEConfirmParams.Confirm, TRUE, &Sp.confirm, &Sp.confirm_len);
  }

  if (P.Optional.SAEStatus_IsPresent) {
    Sp.sae_status = (uint32_t)P.SAEStatus;
  }

  Ret = rtw_linux_sae_set (&Sp);      // copies what it keeps
  RtwLog ("WiFiCx: SAE parameters for connect %u: type %u, group %u, status code %u, AKM %u, send-confirm %u, SAE "
          "status %u: %d", W->Conn.Seq, Sp.type, Sp.group, Sp.status_code, Sp.akm, Sp.send_confirm, Sp.sae_status, Ret);
  CleanupParsedWdiSetSaeAuthParamsToIhv (&P);
  switch (-Ret) {
    case 0:              Status = STATUS_SUCCESS; break;
    case RTW_ENOTCONN:   Status = STATUS_INVALID_DEVICE_STATE; break;
    case RTW_EINVAL:     Status = STATUS_INVALID_PARAMETER; break;
    default:             Status = RtwWifiErrnoStatus (Ret); break;
  }

  RtwWifiComplete (W, Request, Status, sizeof (WDI_MESSAGE_HEADER));
}
