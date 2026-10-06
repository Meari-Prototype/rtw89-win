// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: the station NETADAPTER (WDI port) and its data path.
 * TX: the OS's native 802.11 data frames (fragments in its buffers) go to rtw_linux_tx_data(), which copies them,
 * so packets go back at once. While not connected they are dropped (and counted). When the mac layer is busy
 * (rtw89 stopped its queues, or 64 frames in flight) the rest stays in the ring until its tx_ready callback.
 * RX: rtw_linux_rx_data() copies one MSDU (24-byte data header, LLC/SNAP, payload) per OS buffer; its rx_ready
 * callback asks NetAdapterCx for another Advance. Nothing arrives while not connected.
 * The mac layer's callbacks run inside its and rtw89's locks: they only set a kick and queue DpDpc, which calls
 * NetAdapterCx. DpLock keeps the queue handles valid for the DPC; the queues' cleanup clears them.
 * Each TX packet's ms_packet_wifiexemptionaction (when WiFiCx enables it) goes to the mac layer, which decides
 * from it whether the frame goes protected; on RX the extension (if enabled) gets the exemption action an
 * unprotected frame was let up with, else WDI_EXEMPT_NO_EXEMPTION.
 * Structure from the WiFiCx sample (device.cpp, adapter.cpp) and netvadapterlibrary (adapter.cpp, txqueue.cpp,
 * rxqueue.cpp).
 * The Wi-Fi Direct role adapter (the mobile hotspot's GO port) uses the same queue code with Role set: its frames
 * go to the mac layer's GO data path (rtw_linux_ap_*), its queues and kicks are W->Ap*. Without Wi-Fi Direct WiFiCx
 * never asks for one.
 */
#include "wifi.h"
#include <net/virtualaddress.h>
#include <net/ieee8021q.h>
#include <net/wifi/exemptionaction.h>

#define RTW_LINK_SPEED      144400000ULL    // HT20 2SS short GI
#define RTW_LINK_SPEED_40   400000000ULL    // VHT40 MCS9 2SS short GI (HT40: 300)
#define RTW_LINK_SPEED_80   866700000ULL    // VHT80 MCS9 2SS short GI
#define RTW_MTU             1500
#define RTW_RX_BUFFER_SIZE  4096
#define RTW_TX_MAX_FRAGS    8
#define RTW_ENOTCONN        107

typedef struct _RTW_ADAPTER {
  PRTW_WIFI  W;
  BOOLEAN    Role;          // the Wi-Fi Direct role adapter (our GO), else the station's
} RTW_ADAPTER, *PRTW_ADAPTER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME (RTW_ADAPTER, RtwGetAdapter)

typedef struct _RTW_QUEUE {
  NET_RING_COLLECTION const  *Rings;
  PRTW_WIFI                  W;
  BOOLEAN                    Tx;
  BOOLEAN                    Role;                // a queue of the role adapter (the GO's data path)
  NET_EXTENSION              VirtualAddress;    // ms_fragment_virtualaddress
  NET_EXTENSION              Ieee8021q;         // ms_packet_ieee8021q (TX priority), if enabled
  NET_EXTENSION              Exemption;         // ms_packet_wifiexemptionaction, if enabled
} RTW_QUEUE, *PRTW_QUEUE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME (RTW_QUEUE, RtwGetQueue)

static EVT_WDF_OBJECT_CONTEXT_CLEANUP              AdapterCleanup;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP              QueueCleanup;
static EVT_NET_ADAPTER_CREATE_TXQUEUE              CreateTxQueue;
static EVT_NET_ADAPTER_CREATE_RXQUEUE              CreateRxQueue;
static EVT_NET_ADAPTER_SET_RECEIVE_FILTER          SetReceiveFilter;
static EVT_PACKET_QUEUE_ADVANCE                    TxAdvance;
static EVT_PACKET_QUEUE_CANCEL                     TxCancel;
static EVT_PACKET_QUEUE_ADVANCE                    RxAdvance;
static EVT_PACKET_QUEUE_CANCEL                     RxCancel;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED   SetNotify;
static KDEFERRED_ROUTINE                           DpDpc;

/* ---- the mac layer's callbacks (any IRQL <= DISPATCH_LEVEL, inside its locks) ---- */

static VOID
Kick (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    Tx,
  _In_ BOOLEAN    Role
  )
{
  KIRQL  Irql;

  KeAcquireSpinLock (&W->DpLock, &Irql);
  if (Role) {
    if (Tx) {
      W->ApTxKick = TRUE;
    } else {
      W->ApRxKick = TRUE;
    }
  } else if (Tx) {
    W->TxKick = TRUE;
  } else {
    W->RxKick = TRUE;
  }

  KeReleaseSpinLock (&W->DpLock, Irql);
  KeInsertQueueDpc (&W->DpDpc, NULL, NULL);
}

static void
DpTxReady (
  void  *Ctx
  )
{
  Kick ((PRTW_WIFI)Ctx, TRUE, FALSE);
}

static void
DpRxReady (
  void  *Ctx
  )
{
  Kick ((PRTW_WIFI)Ctx, FALSE, FALSE);
}

/* the GO port's (rtw_linux_ap_dp_attach) */
static void
DpApTxReady (
  void  *Ctx
  )
{
  Kick ((PRTW_WIFI)Ctx, TRUE, TRUE);
}

static void
DpApRxReady (
  void  *Ctx
  )
{
  Kick ((PRTW_WIFI)Ctx, FALSE, TRUE);
}

/* DISPATCH_LEVEL: ask NetAdapterCx for an Advance of the queues that were kicked and want notifications */
static VOID
DpDpc (
  _In_ PKDPC  Dpc,
  _In_opt_ PVOID  Context,
  _In_opt_ PVOID  Arg1,
  _In_opt_ PVOID  Arg2
  )
{
  PRTW_WIFI  W;

  UNREFERENCED_PARAMETER (Dpc);
  UNREFERENCED_PARAMETER (Arg1);
  UNREFERENCED_PARAMETER (Arg2);

  // the enables are NetAdapterCx's (SetNotify); a notification too many only costs an Advance
  W = (PRTW_WIFI)Context;
  KeAcquireSpinLockAtDpcLevel (&W->DpLock);
  if (W->TxKick && W->TxNotify && (W->TxQueue != NULL)) {
    NetTxQueueNotifyMoreCompletedPacketsAvailable (W->TxQueue);
  }

  if (W->RxKick && W->RxNotify && (W->RxQueue != NULL)) {
    NetRxQueueNotifyMoreReceivedPacketsAvailable (W->RxQueue);
  }

  // the role adapter's queues
  if (W->ApTxKick && W->ApTxNotify && (W->ApTxQueue != NULL)) {
    NetTxQueueNotifyMoreCompletedPacketsAvailable (W->ApTxQueue);
  }

  if (W->ApRxKick && W->ApRxNotify && (W->ApRxQueue != NULL)) {
    NetRxQueueNotifyMoreReceivedPacketsAvailable (W->ApRxQueue);
  }

  W->TxKick   = FALSE;
  W->RxKick   = FALSE;
  W->ApTxKick = FALSE;
  W->ApRxKick = FALSE;
  KeReleaseSpinLockFromDpcLevel (&W->DpLock);
}

VOID
RtwWifiDpInit (
  _In_ PRTW_WIFI  W
  )
{
  KeInitializeSpinLock (&W->DpLock);
  KeInitializeDpc (&W->DpDpc, DpDpc, W);
}

/* ---- data path ---- */

/* Gives every packet and fragment the OS posted back to it. */
static VOID
ReturnAll (
  _In_ NET_RING_COLLECTION const  *Rings
  )
{
  NET_RING  *Packets;
  NET_RING  *Fragments;

  Packets               = Rings->Rings[NetRingTypePacket];
  Fragments             = Rings->Rings[NetRingTypeFragment];
  Packets->BeginIndex   = Packets->EndIndex;
  Packets->NextIndex    = Packets->EndIndex;
  Fragments->BeginIndex = Fragments->EndIndex;
  Fragments->NextIndex  = Fragments->EndIndex;
}

/* One OS frame to the mac layer. Returns its result (-EBUSY: keep the packet in the ring). */
static LONG
TxOne (
  _In_ PRTW_QUEUE   Q,
  _In_ NET_PACKET   *Packet,
  _In_ UINT32       PacketIndex
  )
{
  NET_RING         *Fragments;
  struct rtw_frag  Frags[RTW_TX_MAX_FRAGS];
  NET_FRAGMENT     *Fragment;
  UINT32           Index;
  UINT32           Flags;
  UINT8            Prio;
  ULONG            i;

  Fragments = Q->Rings->Rings[NetRingTypeFragment];
  if ((Packet->FragmentCount == 0) || (Packet->FragmentCount > RTW_TX_MAX_FRAGS)) {
    // counted as a frame we cannot send
    return Q->Role ? rtw_linux_ap_tx_data (Frags, 0, 0, 0) : rtw_linux_tx_data (Frags, 0, 0, 0);
  }

  Index = Packet->FragmentIndex;
  for (i = 0; i < Packet->FragmentCount; i++) {
    Fragment          = NetRingGetFragmentAtIndex (Fragments, Index);
    Frags[i].p        = (PUCHAR)NetExtensionGetFragmentVirtualAddress (&Q->VirtualAddress, Index)->VirtualAddress +
                        Fragment->Offset;
    Frags[i].len      = (uint32_t)Fragment->ValidLength;
    Frags[i].reserved = 0;
    Index             = NetRingIncrementIndex (Fragments, Index);
  }

  Prio = 0;
  if (Q->Ieee8021q.Enabled) {
    Prio = (UINT8)NetExtensionGetPacketIeee8021Q (&Q->Ieee8021q, PacketIndex)->PriorityCodePoint;
  }

  // the OS's exemption action for this frame (WDI_EXEMPTION_ACTION_TYPE); the mac layer applies its list
  // otherwise
  Flags = 0;
  if (Q->Exemption.Enabled) {
    Flags = RTW_TX_EXEMPT_VALID | RTW_TX_EXEMPT (WifiExtensionGetExemptionAction (&Q->Exemption, PacketIndex)->ExemptionAction);
  }

  if (Q->Role) {
    return rtw_linux_ap_tx_data (Frags, Packet->FragmentCount, Prio, Flags);       // our GO's
  }

  return rtw_linux_tx_data (Frags, Packet->FragmentCount, Prio, Flags);
}

/* <= DISPATCH_LEVEL. Packets go back in order once the mac layer took (or dropped) them. */
static VOID
TxAdvance (
  _In_ NETPACKETQUEUE  Queue
  )
{
  PRTW_QUEUE  Q;
  NET_RING    *Packets;
  NET_RING    *Fragments;
  NET_PACKET  *Packet;
  UINT32      i;
  ULONG       Dropped;
  LONG        Ready;

  Q         = RtwGetQueue (Queue);
  Packets   = Q->Rings->Rings[NetRingTypePacket];
  Fragments = Q->Rings->Rings[NetRingTypeFragment];
  Ready     = Q->Role ? rtw_linux_ap_tx_ready () : rtw_linux_tx_ready ();
  Dropped   = 0;
  for (i = Packets->BeginIndex; i != Packets->EndIndex; i = NetRingIncrementIndex (Packets, i)) {
    Packet = NetRingGetPacketAtIndex (Packets, i);
    if (!Packet->Ignore) {
      if (Ready == -RTW_EBUSY) {
        break;                  // tx_ready follows
      }

      if (Ready == -RTW_ENOTCONN) {
        Dropped++;
      } else if (TxOne (Q, Packet, i) == -RTW_EBUSY) {
        Ready = -RTW_EBUSY;
        break;
      }
    }

    if (Packet->FragmentCount != 0) {
      Fragments->BeginIndex = NetRingAdvanceIndex (Fragments, Packet->FragmentIndex, Packet->FragmentCount);
    }
  }

  Packets->BeginIndex = i;
  if (Dropped != 0) {
    if (Q->Role) {
      rtw_linux_ap_tx_drop (Dropped);
    } else {
      rtw_linux_tx_drop (Dropped);
    }
  }
}

static VOID
TxCancel (
  _In_ NETPACKETQUEUE  Queue
  )
{
  ReturnAll (RtwGetQueue (Queue)->Rings);
}

/* <= DISPATCH_LEVEL. One received MSDU per OS buffer (system managed, one fragment each). */
static VOID
RxAdvance (
  _In_ NETPACKETQUEUE  Queue
  )
{
  PRTW_QUEUE    Q;
  NET_RING      *Packets;
  NET_RING      *Fragments;
  NET_PACKET    *Packet;
  NET_FRAGMENT  *Fragment;
  PUCHAR        Va;
  UINT32        Len;
  UINT32        Info;

  Q         = RtwGetQueue (Queue);
  Packets   = Q->Rings->Rings[NetRingTypePacket];
  Fragments = Q->Rings->Rings[NetRingTypeFragment];
  while ((Packets->BeginIndex != Packets->EndIndex) && (Fragments->BeginIndex != Fragments->EndIndex)) {
    Fragment = NetRingGetFragmentAtIndex (Fragments, Fragments->BeginIndex);
    Va       = (PUCHAR)NetExtensionGetFragmentVirtualAddress (&Q->VirtualAddress, Fragments->BeginIndex)->VirtualAddress;
    Len      = Q->Role ? rtw_linux_ap_rx_data (Va, (uint32_t)Fragment->Capacity, &Info) :
                         rtw_linux_rx_data (Va, (uint32_t)Fragment->Capacity, &Info);
    if (Len == 0) {
      break;
    }

    if (Q->Exemption.Enabled) {
      WifiExtensionGetExemptionAction (&Q->Exemption, Packets->BeginIndex)->ExemptionAction =
        (Info & RTW_RX_EXEMPT) ? (UINT8)RTW_RX_EXEMPT_GET (Info) : (UINT8)WDI_EXEMPT_NO_EXEMPTION;
    }

    Fragment->ValidLength = Len;
    Fragment->Offset      = 0;
    Packet                = NetRingGetPacketAtIndex (Packets, Packets->BeginIndex);
    RtlZeroMemory (&Packet->Layout, sizeof (Packet->Layout));
    Packet->FragmentIndex             = Fragments->BeginIndex;
    Packet->FragmentCount             = 1;
    Packet->Ignore                    = 0;
    Packet->Layout.Layer2Type         = NetPacketLayer2TypeIeee80211;
    Packet->Layout.Layer2HeaderLength = 24;
    Fragments->BeginIndex = NetRingIncrementIndex (Fragments, Fragments->BeginIndex);
    Packets->BeginIndex   = NetRingIncrementIndex (Packets, Packets->BeginIndex);
  }
}

/* All buffers must go back, else stopping the queue (and removing the device) hangs: mark the packets ignored and
   return every packet and fragment (netvadapterlibrary netpacketlibrary.h CancelRxPackets). What the mac layer
   still holds for the OS is dropped. */
static VOID
RxCancel (
  _In_ NETPACKETQUEUE  Queue
  )
{
  NET_RING_COLLECTION const  *Rings;
  NET_RING                   *Packets;
  UINT32                     i;

  if (RtwGetQueue (Queue)->Role) {
    rtw_linux_ap_rx_flush ();
  } else {
    rtw_linux_rx_flush ();
  }

  Rings   = RtwGetQueue (Queue)->Rings;
  Packets = Rings->Rings[NetRingTypePacket];
  for (i = Packets->BeginIndex; i != Packets->EndIndex; i = NetRingIncrementIndex (Packets, i)) {
    NetRingGetPacketAtIndex (Packets, i)->Ignore = 1;
  }

  ReturnAll (Rings);
}

/* NetAdapterCx waits for us: if there is work already, ask for the next Advance (through the DPC). */
static VOID
SetNotify (
  _In_ NETPACKETQUEUE  Queue,
  _In_ BOOLEAN         NotificationEnabled
  )
{
  PRTW_QUEUE  Q;
  PRTW_WIFI   W;
  BOOLEAN     Work;
  KIRQL       Irql;

  Q = RtwGetQueue (Queue);
  W = Q->W;
  KeAcquireSpinLock (&W->DpLock, &Irql);
  if (Q->Role) {
    if (Q->Tx) {
      W->ApTxNotify = NotificationEnabled;
    } else {
      W->ApRxNotify = NotificationEnabled;
    }
  } else if (Q->Tx) {
    W->TxNotify = NotificationEnabled;
  } else {
    W->RxNotify = NotificationEnabled;
  }

  KeReleaseSpinLock (&W->DpLock, Irql);
  if (NotificationEnabled) {
    if (Q->Role) {
      Work = Q->Tx ? (rtw_linux_ap_tx_ready () != -RTW_EBUSY) : (rtw_linux_ap_rx_pending () != 0);
    } else {
      Work = Q->Tx ? (rtw_linux_tx_ready () != -RTW_EBUSY) : (rtw_linux_rx_pending () != 0);
    }

    if (Work) {
      Kick (W, Q->Tx, Q->Role);
    }
  }
}

/* The queue goes away (data path stopped): the DPC must not touch it any more. */
static VOID
QueueCleanup (
  _In_ WDFOBJECT  Object
  )
{
  PRTW_QUEUE  Q;
  KIRQL       Irql;

  Q = RtwGetQueue ((NETPACKETQUEUE)Object);
  if (Q->W == NULL) {
    return;
  }

  KeAcquireSpinLock (&Q->W->DpLock, &Irql);
  if (Q->Role) {
    if (Q->Tx && (Q->W->ApTxQueue == (NETPACKETQUEUE)Object)) {
      Q->W->ApTxQueue  = NULL;
      Q->W->ApTxNotify = FALSE;
    } else if (!Q->Tx && (Q->W->ApRxQueue == (NETPACKETQUEUE)Object)) {
      Q->W->ApRxQueue  = NULL;
      Q->W->ApRxNotify = FALSE;
    }
  } else if (Q->Tx && (Q->W->TxQueue == (NETPACKETQUEUE)Object)) {
    Q->W->TxQueue  = NULL;
    Q->W->TxNotify = FALSE;
  } else if (!Q->Tx && (Q->W->RxQueue == (NETPACKETQUEUE)Object)) {
    Q->W->RxQueue  = NULL;
    Q->W->RxNotify = FALSE;
  }

  KeReleaseSpinLock (&Q->W->DpLock, Irql);
}

static NTSTATUS
CreateTxQueue (
  _In_ NETADAPTER         Adapter,
  _Inout_ NETTXQUEUE_INIT  *Init
  )
{
  NET_PACKET_QUEUE_CONFIG  Config;
  NET_EXTENSION_QUERY      Query;
  WDF_OBJECT_ATTRIBUTES    Attributes;
  NETPACKETQUEUE           Queue;
  PRTW_QUEUE               Q;
  PRTW_WIFI                W;
  NTSTATUS                 Status;
  BOOLEAN                  Role;
  KIRQL                    Irql;

  W    = RtwGetAdapter (Adapter)->W;
  Role = RtwGetAdapter (Adapter)->Role;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attributes, RTW_QUEUE);
  Attributes.EvtCleanupCallback = QueueCleanup;
  NET_PACKET_QUEUE_CONFIG_INIT (&Config, TxAdvance, SetNotify, TxCancel);
  Status = NetTxQueueCreate (Init, &Attributes, &Config, &Queue);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: NetTxQueueCreate failed 0x%08x", Status);
    return Status;
  }

  Q        = RtwGetQueue (Queue);
  Q->Rings = NetTxQueueGetRingCollection (Queue);
  Q->Tx    = TRUE;
  Q->Role  = Role;
  NET_EXTENSION_QUERY_INIT (&Query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                            NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
  NetTxQueueGetExtension (Queue, &Query, &Q->VirtualAddress);
  NET_EXTENSION_QUERY_INIT (&Query, NET_PACKET_EXTENSION_IEEE8021Q_NAME, NET_PACKET_EXTENSION_IEEE8021Q_VERSION_1,
                            NetExtensionTypePacket);
  NetTxQueueGetExtension (Queue, &Query, &Q->Ieee8021q);
  NET_EXTENSION_QUERY_INIT (&Query, NET_PACKET_EXTENSION_WIFI_EXEMPTION_ACTION_NAME,
                            NET_PACKET_EXTENSION_WIFI_EXEMPTION_ACTION_VERSION_1, NetExtensionTypePacket);
  NetTxQueueGetExtension (Queue, &Query, &Q->Exemption);
  if (!Q->VirtualAddress.Enabled) {
    RtwLog ("WiFiCx: TX queue without virtual addresses");
    return STATUS_NOT_SUPPORTED;
  }

  KeAcquireSpinLock (&W->DpLock, &Irql);
  Q->W = W;
  if (Role) {
    W->ApTxQueue  = Queue;
    W->ApTxNotify = FALSE;
  } else {
    W->TxQueue  = Queue;
    W->TxNotify = FALSE;
  }

  KeReleaseSpinLock (&W->DpLock, Irql);
  RtwLog ("WiFiCx: %sTX queue created (802.1Q priority %s, exemption action %s)", Role ? "GO " : "",
          Q->Ieee8021q.Enabled ? "on" : "off", Q->Exemption.Enabled ? "on" : "off");
  return STATUS_SUCCESS;
}

static NTSTATUS
CreateRxQueue (
  _In_ NETADAPTER         Adapter,
  _Inout_ NETRXQUEUE_INIT  *Init
  )
{
  NET_PACKET_QUEUE_CONFIG  Config;
  NET_EXTENSION_QUERY      Query;
  WDF_OBJECT_ATTRIBUTES    Attributes;
  NETPACKETQUEUE           Queue;
  PRTW_QUEUE               Q;
  PRTW_WIFI                W;
  NTSTATUS                 Status;
  BOOLEAN                  Role;
  KIRQL                    Irql;

  W    = RtwGetAdapter (Adapter)->W;
  Role = RtwGetAdapter (Adapter)->Role;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attributes, RTW_QUEUE);
  Attributes.EvtCleanupCallback = QueueCleanup;
  NET_PACKET_QUEUE_CONFIG_INIT (&Config, RxAdvance, SetNotify, RxCancel);
  Status = NetRxQueueCreate (Init, &Attributes, &Config, &Queue);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: NetRxQueueCreate failed 0x%08x", Status);
    return Status;
  }

  Q        = RtwGetQueue (Queue);
  Q->Rings = NetRxQueueGetRingCollection (Queue);
  Q->Tx    = FALSE;
  Q->Role  = Role;
  NET_EXTENSION_QUERY_INIT (&Query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                            NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
  NetRxQueueGetExtension (Queue, &Query, &Q->VirtualAddress);
  NET_EXTENSION_QUERY_INIT (&Query, NET_PACKET_EXTENSION_WIFI_EXEMPTION_ACTION_NAME,
                            NET_PACKET_EXTENSION_WIFI_EXEMPTION_ACTION_VERSION_1, NetExtensionTypePacket);
  NetRxQueueGetExtension (Queue, &Query, &Q->Exemption);
  if (!Q->VirtualAddress.Enabled) {
    RtwLog ("WiFiCx: RX queue without virtual addresses");
    return STATUS_NOT_SUPPORTED;
  }

  KeAcquireSpinLock (&W->DpLock, &Irql);
  Q->W = W;
  if (Role) {
    W->ApRxQueue  = Queue;
    W->ApRxNotify = FALSE;
  } else {
    W->RxQueue  = Queue;
    W->RxNotify = FALSE;
  }

  KeReleaseSpinLock (&W->DpLock, Irql);
  RtwLog ("WiFiCx: %sRX queue created (exemption action %s)", Role ? "GO " : "", Q->Exemption.Enabled ? "on" : "off");
  return STATUS_SUCCESS;
}

/* The receive filter is accepted and ignored. */
static VOID
SetReceiveFilter (
  _In_ NETADAPTER        Adapter,
  _In_ NETRECEIVEFILTER  Handle
  )
{
  UNREFERENCED_PARAMETER (Adapter);
  UNREFERENCED_PARAMETER (Handle);
}

/* ---- the adapter ---- */

/* The adapter object goes away (port deleted, device removed): no more indications for its port. */
static VOID
AdapterCleanup (
  _In_ WDFOBJECT  Object
  )
{
  PRTW_WIFI  W;
  BOOLEAN    Enqueue;
  KIRQL      Irql;

  W = RtwGetAdapter ((NETADAPTER)Object)->W;
  if (W == NULL) {
    return;
  }

  if (RtwGetAdapter ((NETADAPTER)Object)->Role) {
    // the role port goes; a GO still running stops in the command work item (no CtlLock here: WiFiCx may
    // delete the adapter while a command is being handled)
    KeAcquireSpinLock (&W->Lock, &Irql);
    Enqueue = FALSE;
    if (W->RoleAdapter == (NETADAPTER)Object) {
      W->RoleAdapter   = NULL;
      W->RoleUp        = FALSE;
      W->EvRoleGone    = TRUE;
      Enqueue          = !W->Stopping;
    }

    KeReleaseSpinLock (&W->Lock, Irql);
    if (KeGetCurrentIrql () == PASSIVE_LEVEL) {
      rtw_linux_ap_dp_attach (NULL);
      KeFlushQueuedDpcs ();
    }

    if (Enqueue) {
      WdfWorkItemEnqueue (W->CmdWork);
    }

    RtwLog ("WiFiCx: role adapter gone");
    return;
  }

  KeAcquireSpinLock (&W->Lock, &Irql);
  if (W->Adapter == (NETADAPTER)Object) {
    W->Adapter   = NULL;
    W->AdapterUp = FALSE;
  }

  KeReleaseSpinLock (&W->Lock, Irql);

  // no more data path callbacks, and none of their DPCs still running (ReleaseHardware flushes them as well)
  if (KeGetCurrentIrql () == PASSIVE_LEVEL) {
    rtw_linux_dp_attach (NULL);
    KeFlushQueuedDpcs ();
  }

  RtwLog ("WiFiCx: adapter gone");
}

/* Capabilities and addresses before NetAdapterStart (netvadapterlibrary ConfigureDataCapabilities). The link state
   is left to WiFiCx (the sample does not set it either). */
static VOID
SetDataCapabilities (
  _In_ NETADAPTER       Adapter,
  _In_ const UCHAR      *Address
  )
{
  NET_ADAPTER_LINK_LAYER_CAPABILITIES      LinkCaps;
  NET_ADAPTER_TX_CAPABILITIES              TxCaps;
  NET_ADAPTER_RX_CAPABILITIES              RxCaps;
  NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES  FilterCaps;
  NET_ADAPTER_LINK_LAYER_ADDRESS           LinkAddress;
  ULONG64                                  Speed;

  // the station's: the top rate of the widest 5 GHz channel Parameters\Bandwidth allows; the GO's role adapter
  // stays at 20 MHz
  Speed = RtwGetAdapter (Adapter)->Role ? RTW_LINK_SPEED : (rtw_linux_get_wide () >= 80) ? RTW_LINK_SPEED_80 :
          (rtw_linux_get_wide () >= 40) ? RTW_LINK_SPEED_40 : RTW_LINK_SPEED;
  NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT (&LinkCaps, Speed, Speed);
  NetAdapterSetLinkLayerCapabilities (Adapter, &LinkCaps);
  NetAdapterSetLinkLayerMtuSize (Adapter, RTW_MTU);

  NET_ADAPTER_TX_CAPABILITIES_INIT (&TxCaps, 1);
  NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED (&RxCaps, RTW_RX_BUFFER_SIZE, 1);
  NetAdapterSetDataPathCapabilities (Adapter, &TxCaps, &RxCaps);

  NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT (&FilterCaps, SetReceiveFilter);
  FilterCaps.SupportedPacketFilters = NetPacketFilterFlagDirected | NetPacketFilterFlagMulticast |
                                      NetPacketFilterFlagAllMulticast | NetPacketFilterFlagBroadcast |
                                      NetPacketFilterFlagPromiscuous;
  FilterCaps.MaximumMulticastAddresses = 32;
  NetAdapterSetReceiveFilterCapabilities (Adapter, &FilterCaps);

  NET_ADAPTER_LINK_LAYER_ADDRESS_INIT (&LinkAddress, 6, Address);
  NetAdapterSetPermanentLinkLayerAddress (Adapter, &LinkAddress);
  NetAdapterSetCurrentLinkLayerAddress (Adapter, &LinkAddress);
}

/* WiFiCx asks for the station port (WDI port = NETADAPTER). PASSIVE_LEVEL. */
NTSTATUS
RtwEvtWifiCreateAdapter (
  _In_ WDFDEVICE           Device,
  _Inout_ NETADAPTER_INIT  *AdapterInit
  )
{
  NET_ADAPTER_DATAPATH_CALLBACKS  Callbacks;
  struct rtw_dp_ops               DpOps;
  WDF_OBJECT_ATTRIBUTES           Attributes;
  WIFI_ADAPTER_TYPE               Type;
  NETADAPTER                      Adapter;
  PRTW_WIFI                       W;
  const UCHAR                     *Mac;
  ULONG                           Port;
  NTSTATUS                        Status;
  KIRQL                           Irql;

  W    = RtwGetWifi (Device);
  Type = WifiAdapterInitGetType (AdapterInit);
  if ((Type == WIFI_ADAPTER_WIFI_DIRECT_ROLE) && W->Wfd) {
    return RtwWifiCreateRoleAdapter (W, AdapterInit);      // the GO's port
  }

  if (Type != WIFI_ADAPTER_EXTENSIBLE_STATION) {
    RtwLog ("WiFiCx: adapter type %u not supported", (ULONG)Type);
    return STATUS_NOT_SUPPORTED;
  }

  // PrepareHardware read the efuse address before WiFiCx asks for the adapter
  if (!W->HwReady) {
    RtwLog ("WiFiCx: adapter requested before the hardware is ready");
    return STATUS_DEVICE_NOT_READY;
  }

  NET_ADAPTER_DATAPATH_CALLBACKS_INIT (&Callbacks, CreateTxQueue, CreateRxQueue);
  NetAdapterInitSetDatapathCallbacks (AdapterInit, &Callbacks);

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attributes, RTW_ADAPTER);
  Attributes.EvtCleanupCallback = AdapterCleanup;
  Status = NetAdapterCreate (AdapterInit, &Attributes, &Adapter);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: NetAdapterCreate failed 0x%08x", Status);
    return Status;
  }

  RtwGetAdapter (Adapter)->W = W;
  Status = WifiAdapterInitialize (Adapter);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: WifiAdapterInitialize failed 0x%08x", Status);
    return Status;
  }

  Mac = W->Caps->addr;
  SetDataCapabilities (Adapter, Mac);
  Status = NetAdapterStart (Adapter);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: NetAdapterStart failed 0x%08x", Status);
    return Status;
  }

  Port = WifiAdapterGetPortId (Adapter);
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->Adapter   = Adapter;
  W->PortId    = Port;
  W->AdapterUp = TRUE;
  KeReleaseSpinLock (&W->Lock, Irql);

  DpOps.tx_ready = DpTxReady;
  DpOps.rx_ready = DpRxReady;
  DpOps.ctx      = W;
  rtw_linux_dp_attach (&DpOps);
  RtwLog ("WiFiCx: adapter started, port %u, %02x:%02x:%02x:%02x:%02x:%02x", Port, Mac[0], Mac[1], Mac[2], Mac[3],
          Mac[4], Mac[5]);
  return STATUS_SUCCESS;
}

/* WiFiCx asks for the Wi-Fi Direct role port (the mobile hotspot's GO): like the station's, with the GO's interface
   address and the GO's data path (rtw_linux_ap_*). PASSIVE_LEVEL. Only while Wi-Fi Direct is on (W->Wfd). */
NTSTATUS
RtwWifiCreateRoleAdapter (
  _In_ PRTW_WIFI           W,
  _Inout_ NETADAPTER_INIT  *AdapterInit
  )
{
  NET_ADAPTER_DATAPATH_CALLBACKS  Callbacks;
  struct rtw_dp_ops               DpOps;
  WDF_OBJECT_ATTRIBUTES           Attributes;
  NETADAPTER                      Adapter;
  const UCHAR                     *Mac;
  ULONG                           Port;
  NTSTATUS                        Status;
  BOOLEAN                         Busy;
  KIRQL                           Irql;

  if (!W->HwReady) {
    RtwLog ("WiFiCx: role adapter requested before the hardware is ready");
    return STATUS_DEVICE_NOT_READY;
  }

  KeAcquireSpinLock (&W->Lock, &Irql);
  Busy = W->RoleUp;
  KeReleaseSpinLock (&W->Lock, Irql);
  if (Busy) {
    RtwLog ("WiFiCx: a second role adapter requested; only one is supported");
    return STATUS_NOT_SUPPORTED;
  }

  NET_ADAPTER_DATAPATH_CALLBACKS_INIT (&Callbacks, CreateTxQueue, CreateRxQueue);
  NetAdapterInitSetDatapathCallbacks (AdapterInit, &Callbacks);

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attributes, RTW_ADAPTER);
  Attributes.EvtCleanupCallback = AdapterCleanup;
  Status = NetAdapterCreate (AdapterInit, &Attributes, &Adapter);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: role NetAdapterCreate failed 0x%08x", Status);
    return Status;
  }

  // the context is zeroed: Role first, so a cleanup of a half-made adapter never touches the station's fields
  RtwGetAdapter (Adapter)->Role = TRUE;
  RtwGetAdapter (Adapter)->W    = W;
  Status = WifiAdapterInitialize (Adapter);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: role WifiAdapterInitialize failed 0x%08x", Status);
    return Status;
  }

  Mac = W->RoleAddr;
  SetDataCapabilities (Adapter, Mac);
  Status = NetAdapterStart (Adapter);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: role NetAdapterStart failed 0x%08x", Status);
    return Status;
  }

  Port = WifiAdapterGetPortId (Adapter);
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->RoleAdapter = Adapter;
  W->RolePortId  = Port;
  W->RoleUp      = TRUE;
  // EvRoleGone stays: a GO of the previous role port that the work item has not stopped yet must still stop (no GO
  // starts on this port before the work item has taken the event)
  KeReleaseSpinLock (&W->Lock, Irql);

  DpOps.tx_ready = DpApTxReady;
  DpOps.rx_ready = DpApRxReady;
  DpOps.ctx      = W;
  rtw_linux_ap_dp_attach (&DpOps);
  RtwLog ("WiFiCx: role adapter started, port %u, %02x:%02x:%02x:%02x:%02x:%02x", Port, Mac[0], Mac[1], Mac[2],
          Mac[3], Mac[4], Mac[5]);
  return STATUS_SUCCESS;
}
