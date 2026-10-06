// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: keys and the privacy exemption list. Property messages, complete
 * with their M3; the work is in the mac layer (mac\key.c):
 *  - WDI_SET_PRIVACY_EXEMPTION_LIST: EtherTypes the data path may send/receive in clear (802.1X before the
 *    pairwise key); the EtherType comes big endian.
 *  - WDI_SET_ADD_CIPHER_KEYS: the pairwise key after the OS's 4-way handshake, group keys after it and after each
 *    group key handshake (with the receive sequence counter). CCMP only; with management frame protection also
 *    the integrity group key (IGTK, BIP-CMAC-128 in BIPKey, key ID 4 or 5, the IPN as receive sequence counter).
 *  - WDI_SET_DELETE_CIPHER_KEYS, WDI_SET_DEFAULT_KEY_ID (only logged: a station sends with the pairwise key).
 * The key bytes are copied once into a struct rtw_key on the stack, wiped after use and never logged; the TLV dump
 * (Parameters\WdiDump) leaves WDI_SET_ADD_CIPHER_KEYS out (command.cpp).
 * Everything runs in the command work item with Dev->CtlLock held (wifi.h).
 * On the Wi-Fi Direct role port (the mobile hotspot's GO) the same messages set the GO's keys (a client's pairwise
 * key by its PeerMacAddress, the group key the GO sends with) and the GO port's exemption list (rtw_linux_ap_*).
 */
#include "wifi.h"

#define RTW_ENOTCONN            107
#define RTW_EOPNOTSUPP          95
#define RTW_ENOENT              2
#define RTW_EINVAL              22

static NTSTATUS
KeyStatus (
  _In_ LONG  Ret
  )
{
  switch (-Ret) {
    case 0:              return STATUS_SUCCESS;
    case RTW_ENOTCONN:   return STATUS_INVALID_DEVICE_STATE;
    case RTW_EOPNOTSUPP: return STATUS_NOT_SUPPORTED;
    case RTW_EINVAL:     return STATUS_INVALID_PARAMETER;
    default:             return RtwWifiErrnoStatus (Ret);
  }
}

static const char *
KeyTypeName (
  _In_ UINT8  Type
  )
{
  return (Type == RTW_KEY_PAIRWISE) ? "pairwise" : (Type == RTW_KEY_IGTK) ? "integrity group" : "group";
}

/* pairwise, group or IGTK: the WDI key type, else (NONE) a unicast peer address means pairwise */
static UINT8
KeyType (
  _In_ WDI_CIPHER_KEY_TYPE   Type,
  _In_ BOOLEAN               PeerPresent,
  _In_reads_ (6) const UINT8 *Peer
  )
{
  switch (Type) {
    case WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY:
      return RTW_KEY_PAIRWISE;
    case WDI_CIPHER_KEY_TYPE_GROUP_KEY:
      return RTW_KEY_GROUP;
    case WDI_CIPHER_KEY_TYPE_IGTK:
      return RTW_KEY_IGTK;
    case WDI_CIPHER_KEY_TYPE_NONE:
      return (PeerPresent && !(Peer[0] & 1)) ? RTW_KEY_PAIRWISE : RTW_KEY_GROUP;
    default:
      return 0;                 // BIGTK: no beacon protection here
  }
}

/* volatile stores: not optimised away although the copy is dead afterwards */
static VOID
Wipe (
  _Out_writes_bytes_all_ (Len) PVOID  P,
  _In_ SIZE_T                        Len
  )
{
  volatile UCHAR  *V;

  V = (volatile UCHAR *)P;
  while (Len-- != 0) {
    *V++ = 0;
  }
}

VOID
RtwWifiSetPrivacyExemptionList (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_SET_PRIVACY_EXEMPTION_LIST_PARAMETERS  P;
  struct rtw_exemption                       List[RTW_EXEMPT_MAX];
  NDIS_STATUS                                Status;
  ULONG                                      n;
  ULONG                                      i;

  Status = ParseWdiSetPrivacyExemptionListToIhv (InLen - sizeof (*Header), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Status != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiSetPrivacyExemptionListToIhv (&P);
    RtwLog ("WiFiCx: SET_PRIVACY_EXEMPTION_LIST not parsed (0x%08x)", Status);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  n = 0;
  if (P.Optional.PrivacyExemptionEntry_IsPresent) {
    if (P.PrivacyExemptionEntry.ElementCount > RTW_EXEMPT_MAX) {
      RtwLog ("WiFiCx: privacy exemption list of %u entries, %u kept", P.PrivacyExemptionEntry.ElementCount,
              RTW_EXEMPT_MAX);
    }

    for (i = 0; (i < P.PrivacyExemptionEntry.ElementCount) && (n < RTW_EXEMPT_MAX); i++) {
      const WDI_PRIVACY_EXEMPTION_LIST_CONTAINER  *E;

      E                     = &P.PrivacyExemptionEntry.pElements[i];
      List[n].ethertype     = RtlUshortByteSwap (E->EtherType);     // big endian in the message (sample WDI_Cmds.c)
      List[n].action        = (uint8_t)E->ExemptionActionType;
      List[n].packet_type   = (uint8_t)E->ExemptionPacketType;
      n++;
    }
  }

  CleanupParsedWdiSetPrivacyExemptionListToIhv (&P);
  if (RtwWifiPortKind (W, Header->PortId) == RTW_PORT_ROLE) {
    rtw_linux_ap_set_exemptions (List, n);      // the GO port's own list
    RtwLog ("WiFiCx: GO privacy exemption list set, %u entries", n);
  } else {
    rtw_linux_set_exemptions (List, n);
    RtwLog ("WiFiCx: privacy exemption list set, %u entries", n);
  }

  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
}

VOID
RtwWifiSetAddCipherKeys (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_SET_ADD_CIPHER_KEYS_PARAMETERS  P;
  struct rtw_key                      Key;
  const WDI_PRIVATE_BYTE_BLOB         *Blob;
  NDIS_STATUS                         Parsed;
  NTSTATUS                            Status;
  BOOLEAN                             Ok;
  BOOLEAN                             Role;
  ULONG                               i;
  LONG                                Ret;

  Role   = (RtwWifiPortKind (W, Header->PortId) == RTW_PORT_ROLE);      // keys of our GO
  Parsed = ParseWdiSetAddCipherKeysToIhv (InLen - sizeof (*Header), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiSetAddCipherKeysToIhv (&P);
    RtwLog ("WiFiCx: SET_ADD_CIPHER_KEYS not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  Status = STATUS_SUCCESS;
  for (i = 0; i < P.SetCipherKey.ElementCount; i++) {
    const WDI_SET_ADD_CIPHER_KEYS_CONTAINER  *E;
    const WDI_CIPHER_KEY_TYPE_INFO_CONTAINER *T;

    E = &P.SetCipherKey.pElements[i];
    T = &E->CipherKeyTypeInfo;
    RtlZeroMemory (&Key, sizeof (Key));
    Key.type   = KeyType (T->KeyType, E->Optional.PeerMacAddress_IsPresent, E->PeerMacAddress.Address);
    Key.index  = E->Optional.CipherKeyID_IsPresent ? (uint8_t)min (E->CipherKeyID.CipherKeyID, 255u) : 0;
    Key.cipher = (uint8_t)min ((ULONG)T->CipherAlgorithm, 255u);
    if (E->Optional.PeerMacAddress_IsPresent && (Key.type == RTW_KEY_PAIRWISE)) {
      RtlCopyMemory (Key.peer, E->PeerMacAddress.Address, 6);
    }

    if (E->Optional.ReceiveSequenceCount_IsPresent) {
      RtlCopyMemory (Key.rsc, E->ReceiveSequenceCount.ReceiveSequenceCount, sizeof (Key.rsc));
      Key.flags |= RTW_KEY_F_RSC;
    }

    // CCMP keys in CCMPKey; the IGTK (BIP-CMAC-128) in BIPKey
    if (Key.type == RTW_KEY_IGTK) {
      Blob = E->Optional.BIPKey_IsPresent ? &E->BIPKey : NULL;
      Ok   = (T->CipherAlgorithm == WDI_CIPHER_ALGO_BIP);
    } else {
      Blob = E->Optional.CCMPKey_IsPresent ? &E->CCMPKey : NULL;
      Ok   = (Key.type != 0) && (T->CipherAlgorithm == WDI_CIPHER_ALGO_CCMP);
    }

    if (!Ok || (Blob == NULL) || (Blob->ElementCount == 0) || (Blob->ElementCount > sizeof (Key.key))) {
      Ret = -RTW_EOPNOTSUPP;
    } else if (Role && (Key.type == RTW_KEY_PAIRWISE) && !E->Optional.PeerMacAddress_IsPresent) {
      Ret = -RTW_EINVAL;      // a GO's pairwise key belongs to a client
    } else {
      Key.len = (uint8_t)Blob->ElementCount;
      RtlCopyMemory (Key.key, Blob->pElements, Key.len);
      Ret = Role ? rtw_linux_ap_add_key (&Key) : rtw_linux_add_key (&Key);
    }

    // type, ID, cipher, length and direction only: never the key
    RtwLog ("WiFiCx: %sadd key %u/%u: type %u (%s), ID %u%s, cipher %u, %u bytes, direction %u, static %u%s: %d",
            Role ? "GO " : "", i + 1,
            P.SetCipherKey.ElementCount, (ULONG)T->KeyType, (Key.type != 0) ? KeyTypeName (Key.type) : "not supported",
            Key.index, E->Optional.CipherKeyID_IsPresent ? "" : " (none given)", (ULONG)T->CipherAlgorithm,
            (Blob != NULL) ? Blob->ElementCount : 0, (ULONG)T->Direction, T->Static,
            E->Optional.ReceiveSequenceCount_IsPresent ? ", RSC given" : "", Ret);
    Wipe (&Key, sizeof (Key));
    if ((Ret != 0) && NT_SUCCESS (Status)) {
      Status = KeyStatus (Ret);
    }
  }

  CleanupParsedWdiSetAddCipherKeysToIhv (&P);
  RtwWifiComplete (W, Request, Status, sizeof (WDI_MESSAGE_HEADER));
}

VOID
RtwWifiSetDeleteCipherKeys (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_SET_DELETE_CIPHER_KEYS_PARAMETERS  P;
  NDIS_STATUS                            Parsed;
  ULONG                                  i;
  LONG                                   Ret;
  UINT8                                  Type;
  UINT8                                  Index;
  BOOLEAN                                Role;

  Role   = (RtwWifiPortKind (W, Header->PortId) == RTW_PORT_ROLE);      // keys of our GO
  Parsed = ParseWdiSetDeleteCipherKeysToIhv (InLen - sizeof (*Header), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if (Parsed != NDIS_STATUS_SUCCESS) {
    CleanupParsedWdiSetDeleteCipherKeysToIhv (&P);
    RtwLog ("WiFiCx: SET_DELETE_CIPHER_KEYS not parsed (0x%08x)", Parsed);
    RtwWifiComplete (W, Request, STATUS_INVALID_PARAMETER, sizeof (WDI_MESSAGE_HEADER));
    return;
  }

  // a key that is gone already (the connection ended, which takes every key with it) is fine
  for (i = 0; i < P.CipherKeyInfo.ElementCount; i++) {
    const WDI_SET_DELETE_CIPHER_KEYS_CONTAINER  *E;

    E     = &P.CipherKeyInfo.pElements[i];
    Type  = KeyType (E->CipherKeyTypeInfo.KeyType, E->Optional.PeerMacAddress_IsPresent, E->PeerMacAddress.Address);
    Index = E->Optional.CipherKeyID_IsPresent ? (UINT8)min (E->CipherKeyID.CipherKeyID, 255u) : 0;
    if (Type == 0) {
      Ret = -RTW_ENOENT;
    } else if (Role) {
      Ret = rtw_linux_ap_del_key (Type, Index, E->Optional.PeerMacAddress_IsPresent ? E->PeerMacAddress.Address : NULL);
    } else {
      Ret = rtw_linux_del_key (Type, Index);
    }
    RtwLog ("WiFiCx: delete key %u/%u: type %u (%s), ID %u: %d", i + 1, P.CipherKeyInfo.ElementCount,
            (ULONG)E->CipherKeyTypeInfo.KeyType, (Type != 0) ? KeyTypeName (Type) : "not supported", Index, Ret);
  }

  CleanupParsedWdiSetDeleteCipherKeysToIhv (&P);
  RtwWifiComplete (W, Request, STATUS_SUCCESS, sizeof (WDI_MESSAGE_HEADER));
}

/* A station sends with the pairwise key (and group-addressed frames go to the AP as unicast): nothing to switch. */
VOID
RtwWifiSetDefaultKeyId (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  )
{
  WDI_SET_DEFAULT_KEY_ID_PARAMETERS  P;
  NDIS_STATUS                        Parsed;

  Parsed = ParseWdiSetDefaultKeyIdToIhv (InLen - sizeof (*Header), (const UINT8 *)(Header + 1), &W->Tlv, &P);
  if ((Parsed == NDIS_STATUS_SUCCESS) && (RtwWifiPortKind (W, Header->PortId) == RTW_PORT_ROLE)) {
    // the GO sends its group frames with this group key
    RtwLog ("WiFiCx: GO default key ID %u: %d", P.DefaultKeyIdParameters.KeyID,
            rtw_linux_ap_set_tx_key ((uint8_t)min (P.DefaultKeyIdParameters.KeyID, 255u)));
  } else if (Parsed == NDIS_STATUS_SUCCESS) {
    RtwLog ("WiFiCx: default key ID %u (not used by a station)", P.DefaultKeyIdParameters.KeyID);
  } else {
    RtwLog ("WiFiCx: SET_DEFAULT_KEY_ID not parsed (0x%08x)", Parsed);
  }

  CleanupParsedWdiSetDefaultKeyIdToIhv (&P);
  RtwWifiComplete (W, Request, (Parsed == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER,
                   sizeof (WDI_MESSAGE_HEADER));
}
