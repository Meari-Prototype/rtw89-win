// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: private declarations. C++ (MSVC), kernel mode, no exceptions, no RTTI, no global
 * constructors. The structure follows Microsoft's WiFiCx sample (github.com/microsoft/Windows-driver-samples,
 * network/wlan/wificx, MS-PL; no sample code is copied).
 *
 * M3 and M4 are WDI's names for the two answers to a task: M3 completes the command (WifiRequestComplete), M4 is the
 * task's completion indication (e.g. WDI_INDICATION_SCAN_COMPLETE). A property message has only its M3.
 *
 * Threads and locks:
 *  - EvtWifiDeviceSendCommand (IRQL <= DISPATCH_LEVEL) only queues the WIFIREQUEST; one PASSIVE work item
 *    (CmdWork) handles requests and scan events in order, holding Dev->CtlLock for each, so a task's M3
 *    (WifiRequestComplete) always comes before its indications and its M4. A KMDF work item enqueued while its
 *    callback runs is run again, possibly on another thread at the same time; RTW_WIFI.WorkRunning keeps that
 *    second run out, so requests and events are taken and handled by one thread at a time.
 *  - RTW_WIFI.Lock (spin lock) guards the request ring, the event flags and the adapter fields; everything else is
 *    owned by whoever holds Dev->CtlLock (the work item, PrepareHardware, ReleaseHardware, D0Entry/D0Exit).
 *  - Lock order: Dev->CtlLock, then the mac layer's wiphy->mtx (rtw_linux_*). The mac layer calls back
 *    (ScanDone, ConnEvent) with wiphy->mtx held and never takes Dev->CtlLock.
 *  - Data path (adapter.cpp): NetAdapterCx's queue callbacks run at IRQL <= DISPATCH_LEVEL without
 *    Dev->CtlLock; the mac layer's tx_ready/rx_ready callbacks only kick DpDpc (RTW_WIFI.DpLock).
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <wificx.h>
#include <TlvGeneratorParser.hpp>

#include "../driver/rtw89win.h"
#include "../driver/rtw89win_ioctl.h"
#include "../compat/include/rtw/glue.h"

// shared with the clang side (static asserts in mac\hw.c, mac\bss.c); TlvGeneratorParser.hpp changes the packing
// around its own declarations, so check the layout as MSVC sees it here
C_ASSERT (sizeof (struct rtw_caps) == 594);
C_ASSERT (FIELD_OFFSET (struct rtw_caps, chans) == 82);
C_ASSERT (sizeof (struct rtw_scan_req) == 276);
C_ASSERT (sizeof (struct rtw_bss_rec) == 32);
C_ASSERT (FIELD_OFFSET (struct rtw_bss_rec, last_ns) == 24);
C_ASSERT (sizeof (struct rtw_frag) == 16);
C_ASSERT (sizeof (struct rtw_conn_cand) == 16);
C_ASSERT (sizeof (struct rtw_connect_req) == 128);
C_ASSERT (FIELD_OFFSET (struct rtw_connect_req, bodies) == 104);
C_ASSERT (FIELD_OFFSET (struct rtw_connect_req, ies) == 120);
C_ASSERT (sizeof (struct rtw_conn_info) == 48);
C_ASSERT (FIELD_OFFSET (struct rtw_conn_info, mgmt_group) == 43);
C_ASSERT (FIELD_OFFSET (struct rtw_conn_info, phy) == 19);
C_ASSERT (FIELD_OFFSET (struct rtw_conn_info, width_mhz) == 45);
C_ASSERT (FIELD_OFFSET (struct rtw_connect_req, auths) == 116);
C_ASSERT (sizeof (struct rtw_link_stats) == 288);
C_ASSERT (sizeof (struct rtw_key) == 52);
C_ASSERT (sizeof (struct rtw_sae_ind) == 16);
C_ASSERT (sizeof (struct rtw_sae_params) == 72);
C_ASSERT (FIELD_OFFSET (struct rtw_sae_params, scalar) == 32);
C_ASSERT (sizeof (struct rtw_exemption) == 4);
C_ASSERT (sizeof (struct rtw_radio_info) == 200);
// the connection quality hints go to the mac layer as they are
C_ASSERT (WDI_CONNECTION_QUALITY_AUTO_POWER_SAVE == 1);
C_ASSERT (WDI_CONNECTION_QUALITY_NO_POWER_SAVE == 4);
// the mac layer's values are the WDI ones (the front end passes them through)
C_ASSERT (RTW_CIPHER_CCMP == WDI_CIPHER_ALGO_CCMP);
C_ASSERT (RTW_CIPHER_NONE == WDI_CIPHER_ALGO_NONE);
C_ASSERT (RTW_CIPHER_BIP == WDI_CIPHER_ALGO_BIP);
C_ASSERT (RTW_SAE_IND_PARAMS == WDI_SAE_INDICATION_TYPE_COMMIT_REQUEST_PARAMS_NEEDED);
C_ASSERT (RTW_SAE_IND_COMMIT == WDI_SAE_INDICATION_TYPE_COMMIT_FRAME);
C_ASSERT (RTW_SAE_IND_CONFIRM == WDI_SAE_INDICATION_TYPE_CONFIRM_FRAME);
C_ASSERT (RTW_SAE_IND_ERROR == WDI_SAE_INDICATION_TYPE_ERROR);
C_ASSERT (RTW_SAE_IND_RESEND == WDI_SAE_INDICATION_TYPE_CONFIRM_REQUEST_RESEND_REQUEST);
C_ASSERT (RTW_SAE_REQ_COMMIT == WDI_SAE_REQUEST_TYPE_COMMIT_PARAMS);
C_ASSERT (RTW_SAE_REQ_CONFIRM == WDI_SAE_REQUEST_TYPE_CONFIRM_PARAMS);
C_ASSERT (RTW_SAE_REQ_FAILURE == WDI_SAE_REQUEST_TYPE_FAILURE);
C_ASSERT (RTW_SAE_REQ_SUCCESS == WDI_SAE_REQUEST_TYPE_SUCCESS);
C_ASSERT (RTW_SAE_REQ_COMMIT_H2E == WDI_SAE_REQUEST_TYPE_COMMIT_H2E_PARAMS);
C_ASSERT (RTW_EXEMPT_NONE == WDI_EXEMPT_NO_EXEMPTION);
C_ASSERT (RTW_EXEMPT_ALWAYS == WDI_EXEMPT_ALWAYS);
C_ASSERT (RTW_EXEMPT_NO_KEY == WDI_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE);
C_ASSERT (RTW_EXEMPT_UNICAST == WDI_EXEMPT_PACKET_TYPE_UNICAST);
C_ASSERT (RTW_EXEMPT_MULTICAST == WDI_EXEMPT_PACKET_TYPE_MULTICAST);
C_ASSERT (RTW_EXEMPT_BOTH == WDI_EXEMPT_PACKET_TYPE_BOTH);
// the group owner's shared layouts, and the WABI reasons the mac layer reports as they are
C_ASSERT (sizeof (struct rtw_ap_start) == 48);
C_ASSERT (sizeof (struct rtw_ap_ind) == 28);
C_ASSERT (sizeof (struct rtw_ap_client) == 24);
C_ASSERT (sizeof (struct rtw_ap_stats) == 340);
C_ASSERT (RTW_LOST_BY_RESET == WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET);
C_ASSERT (RTW_LOST_UNREACHABLE == WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN);
C_ASSERT (RTW_LOST_PEER_DEAUTH == WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED);
C_ASSERT (RTW_LOST_PEER_DISASSOC == WDI_ASSOC_STATUS_PEER_DISASSOCIATED);
C_ASSERT (RTW_LOST_BY_HOST == WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);

#define RTW_WIFI_QUEUE_LEN      32              // WIFIREQUESTs waiting for the work item
#define RTW_WIFI_EXPORT_SIZE    0x10000         // BSS export buffer (rtw_linux_bss_export)
#define RTW_WIFI_BSS_PER_IND    32              // BSS entries per WDI_INDICATION_BSS_ENTRY_LIST
#define RTW_WIFI_BODY_PER_IND   0x6000          // frame body bytes per indication
#define RTW_WIFI_MSG_IDS        256             // message ids we count (WDI ids are below 256)
C_ASSERT (RTW_WIFI_MSG_IDS == RTW_WDI_IDS);     // IOCTL_RTW_MODE copies the counters as they are

// Linux errno values (rtw_linux_* results)
#define RTW_ENODEV              19
#define RTW_EBUSY               16

// NDIS_STATUS_REQUEST_ABORTED (shared\ndis\status.h), the status of an aborted task in its M4
#ifndef NDIS_STATUS_REQUEST_ABORTED
#define NDIS_STATUS_REQUEST_ABORTED  ((NDIS_STATUS)0xC001000CL)
#endif

/* The running WDI_TASK_SCAN (one at a time). */
typedef struct _RTW_SCAN_TASK {
  BOOLEAN   Active;
  BOOLEAN   AbortRequested;     // ABORT_TASK, radio off, reset or power down: M4 is NDIS_STATUS_REQUEST_ABORTED
  BOOLEAN   BssidFilter;        // the request named one BSSID
  UCHAR     Bssid[6];
  UINT16    PortId;             // from the request header, for the indications
  UINT32    TransactionId;
  UINT32    IhvSpecificId;
  ULONG64   StartNs;            // rtwk_time_ns() at the start: report what was heard since
  ULONG     Seq;                // scans started since the device was added
} RTW_SCAN_TASK;

/* The running WDI_TASK_CONNECT and the connection it made (connect.cpp). */
#define RTW_WIFI_CONN_BUF       0x4000          // candidate bodies, then the frames rtw_linux_conn_get returns
#define RTW_WIFI_CONN_TIMEOUT_MS 15000          // a TASK_CONNECT ends (aborted) after this at the latest
#define RTW_WIFI_SAE_BUF        1024            // an SAE frame body for the OS (mac\mac.h MAC_SAE_FRAME_MAX)

typedef struct _RTW_CONN_TASK {
  BOOLEAN   Active;             // TASK_CONNECT accepted (M3 done), its CONNECT_COMPLETE (M4) not yet sent
  BOOLEAN   Connected;          // the mac layer is connected; DISASSOCIATION not indicated yet
  BOOLEAN   AbortRequested;     // ABORT_TASK, reset, radio off or power down: M4 is NDIS_STATUS_REQUEST_ABORTED
  UINT16    PortId;             // of the task, for its indications and those of the connection
  UINT32    TransactionId;
  UINT32    IhvSpecificId;
  ULONG64   StartNs;
  ULONG     Seq;                // connect tasks since the device was added
  UCHAR     Bssid[6];           // the BSS connected to
  UCHAR     Auth;               // RTW_AUTH_* of the task
} RTW_CONN_TASK;

/* the WDI ports (RtwWifiPortKind) */
#define RTW_PORT_NONE           0
#define RTW_PORT_STA            1       // the station NETADAPTER
#define RTW_PORT_WFD            2       // the Wi-Fi Direct device (WIFIDIRECTDEVICE, control only)
#define RTW_PORT_ROLE           3       // the Wi-Fi Direct role NETADAPTER (our group owner)

#define RTW_WIFI_AP_BUF         0x1000  // frames of a GO indication (request, or response + beacon body)

/* The group owner on the role port (ap.cpp), and the TASK_SEND_AP_ASSOCIATION_RESPONSE that runs. */
typedef struct _RTW_AP_TASK {
  BOOLEAN   Running;            // START_AP succeeded; no STOP_AP, reset or stop by the mac layer since
  BOOLEAN   Rsn;                // WPA2-Personal (else open)
  UINT16    PortId;             // the role port it runs on
  UCHAR     Band;               // NL80211_BAND_*
  UCHAR     Channel;
  ULONG     Seq;                // START_AP tasks since the device was added
  BOOLEAN   RespActive;         // M3 of SEND_AP_ASSOCIATION_RESPONSE done, its M4 not yet
  BOOLEAN   RespAborted;        // ABORT_TASK named it: the work item sends its M4 (aborted)
  BOOLEAN   RespReassoc;
  UINT16    RespPort;
  UINT32    RespTid;
  UINT32    RespIhv;
  UCHAR     RespPeer[6];
  ULONG     NumPeers;           // the clients the OS knows as associated (DISASSOCIATION goes only for them)
  UCHAR     Peers[RTW_AP_MAX_CLIENTS][6];
} RTW_AP_TASK;

typedef struct _RTW_WIFI {
  PRTW_DEVICE       Dev;
  WDFDEVICE         Device;
  TLV_CONTEXT       Tlv;                // AllocationContext 0, PeerVersion = the OS's WDI version
  ULONG             OsWdiVersion;
  WDFWORKITEM       CmdWork;
  WDFTIMER          ScanTimer;

  // under Lock
  KSPIN_LOCK        Lock;
  WIFIREQUEST       Ring[RTW_WIFI_QUEUE_LEN];
  ULONG             RingHead;
  ULONG             RingCount;
  BOOLEAN           Stopping;           // ReleaseHardware: no new work, no more events
  BOOLEAN           WorkRunning;        // a CmdWork instance is in its loop (KMDF may run a requeued item in parallel)
  BOOLEAN           EvScanDone;         // the mac layer finished the scan (ScanDone)
  BOOLEAN           EvScanAborted;
  BOOLEAN           EvScanTimeout;      // ScanTimer fired
  BOOLEAN           AdapterUp;          // the station NETADAPTER exists (indications for its port are fine)
  NETADAPTER        Adapter;
  ULONG             PortId;

  // under Dev->CtlLock
  BOOLEAN           HwReady;            // probed and capabilities reported (PrepareHardware)
  BOOLEAN           RadioOn;            // software radio state from WiFiCx
  BOOLEAN           Started;            // rtw_linux_start done
  BOOLEAN           PoweredDown;        // between D0Exit and D0Entry
  BOOLEAN           WdiDump;            // Parameters\WdiDump = 1: dump the first message of each id
  RTW_SCAN_TASK     Scan;
  struct rtw_caps   *Caps;              // from PrepareHardware
  PUCHAR            ExportBuf;          // RTW_WIFI_EXPORT_SIZE
  ULONG             MsgCount[RTW_WIFI_MSG_IDS];
  ULONG             MsgUnsupported[RTW_WIFI_MSG_IDS];   // answered with STATUS_NOT_SUPPORTED, per id
  ULONG             ScanSeq;
  ULONG64           ReadyNs;            // rtwk_time_ns() when the hardware became ready (GET_AUTO_POWER_SAVE)

  // data path (adapter.cpp): the queues the mac layer's callbacks may wake, under DpLock. The callbacks only
  // set a kick and queue DpDpc, which calls NetAdapterCx outside every lock of the mac layer and rtw89.
  KSPIN_LOCK        DpLock;
  NETPACKETQUEUE    TxQueue;
  NETPACKETQUEUE    RxQueue;
  BOOLEAN           TxNotify;           // notification enabled by NetAdapterCx
  BOOLEAN           RxNotify;
  BOOLEAN           TxKick;             // DpDpc: call NotifyMore for this queue
  BOOLEAN           RxKick;
  KDPC              DpDpc;

  // connection (connect.cpp), under Dev->CtlLock unless noted
  RTW_CONN_TASK     Conn;
  BOOLEAN           EvConn;             // under Lock: the mac layer reported connection events
  ULONG             EvConnBits;         // under Lock: RTW_CONN_EV_*
  BOOLEAN           EvConnTimeout;      // under Lock: ConnTimer fired
  WDFTIMER          ConnTimer;
  PUCHAR            ConnBuf;            // RTW_WIFI_CONN_BUF: candidate bodies, then frames from rtw_linux_conn_get
  PUCHAR            SaeBuf;             // RTW_WIFI_SAE_BUF, the frame of an SAE indication (sae.cpp)

  // Wi-Fi Direct, for the mobile hotspot (wfd.cpp, ap.cpp). Nothing of it exists with Parameters\WifiDirect = 0.
  BOOLEAN           Wfd;                // under CtlLock: Wi-Fi Direct capabilities reported (PrepareHardware)
  UCHAR             WfdAddr[6];         // P2P device address
  UCHAR             RoleAddr[6];        // the GO's interface address (its BSSID)
  BOOLEAN           WfdUp;              // under Lock: the WIFIDIRECTDEVICE exists
  WIFIDIRECTDEVICE  WfdDevice;
  ULONG             WfdPortId;
  BOOLEAN           RoleUp;             // under Lock: the role NETADAPTER exists
  NETADAPTER        RoleAdapter;
  ULONG             RolePortId;
  BOOLEAN           EvAp;               // under Lock: the mac layer has GO indications (ApEvent)
  BOOLEAN           EvRoleGone;         // under Lock: the role adapter went away (a running GO has to stop)
  NETPACKETQUEUE    ApTxQueue;          // under DpLock: the role adapter's queues, like TxQueue/RxQueue
  NETPACKETQUEUE    ApRxQueue;
  BOOLEAN           ApTxNotify;
  BOOLEAN           ApRxNotify;
  BOOLEAN           ApTxKick;
  BOOLEAN           ApRxKick;
  RTW_AP_TASK       Ap;                 // under CtlLock
  PUCHAR            ApBuf;              // RTW_WIFI_AP_BUF

  // counters for IOCTL_RTW_MODE
  volatile LONG     Received;
  volatile LONG     Completed;
  volatile LONG     Unsupported;
  volatile LONG     Scans;
  volatile LONG     LastScanBss;
} RTW_WIFI, *PRTW_WIFI;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME (RTW_WIFI, RtwGetWifi)

/* adapter.cpp */
EVT_WIFI_DEVICE_CREATE_ADAPTER            RtwEvtWifiCreateAdapter;
EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE   RtwEvtWifiCreateWifiDirectDevice;

/* command.cpp */
EVT_WIFI_DEVICE_SEND_COMMAND              RtwEvtWifiSendCommand;
EVT_WDF_WORKITEM                          RtwWifiCmdWork;

VOID
RtwWifiComplete (
  _In_ PRTW_WIFI    W,
  _In_ WIFIREQUEST  Request,
  _In_ NTSTATUS     Status,
  _In_ UINT         BytesWritten
  );

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
  );

BOOLEAN
RtwWifiPortUp (
  _In_ PRTW_WIFI  W,
  _In_ UINT16     PortId
  );

LONG
RtwWifiEnsureStarted (
  _In_ PRTW_WIFI  W
  );

VOID
RtwWifiStopMac (
  _In_ PRTW_WIFI  W
  );

/* scan.cpp; all with Dev->CtlLock held */
VOID
RtwWifiTaskScan (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  );

VOID
RtwWifiAbortTask (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  );

VOID
RtwWifiScanEvents (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    Done,
  _In_ BOOLEAN    Aborted,
  _In_ BOOLEAN    Timeout
  );

VOID
RtwWifiScanStop (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    Indicate
  );

EVT_WDF_TIMER                             RtwWifiScanTimer;

/* adapter.cpp */
VOID
RtwWifiDpInit (
  _In_ PRTW_WIFI  W
  );

/* connect.cpp; all with Dev->CtlLock held */
VOID
RtwWifiTaskConnect (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  );

VOID
RtwWifiTaskDisconnect (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  );

VOID
RtwWifiConnEvents (
  _In_ PRTW_WIFI  W,
  _In_ ULONG      Events,
  _In_ BOOLEAN    Timeout
  );

/* Ends a running connect task (aborted) and a connection: reset, radio off, power down, release. With Indicate,
   a connection ends with WDI_INDICATION_DISASSOCIATION (WabiReason) while the port is there. */
VOID
RtwWifiConnStop (
  _In_ PRTW_WIFI  W,
  _In_ UINT32     WabiReason,
  _In_ BOOLEAN    Indicate
  );

/* ABORT_TASK of a TASK_CONNECT: TRUE if it named the running one (its M4 follows) */
BOOLEAN
RtwWifiConnAbort (
  _In_ PRTW_WIFI  W,
  _In_ UINT32     TransactionId
  );

EVT_WDF_TIMER                             RtwWifiConnTimer;

/* security.cpp; property messages, all with Dev->CtlLock held */
VOID
RtwWifiSetPrivacyExemptionList (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  );

VOID
RtwWifiSetAddCipherKeys (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  );

VOID
RtwWifiSetDeleteCipherKeys (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  );

VOID
RtwWifiSetDefaultKeyId (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  );

/* sae.cpp; with Dev->CtlLock held */
VOID
RtwWifiSetSaeAuthParams (
  _In_ PRTW_WIFI                  W,
  _In_ WIFIREQUEST                Request,
  _In_ const WDI_MESSAGE_HEADER   *Header,
  _In_ UINT                       InLen
  );

/* the SAE indications the mac layer queued (RTW_CONN_EV_SAE), for the running connect task */
VOID
RtwWifiSaeIndicate (
  _In_ PRTW_WIFI  W
  );

/* Linux errno to an NTSTATUS for M3 */
NTSTATUS
RtwWifiErrnoStatus (
  _In_ LONG  Ret
  );

/* command.cpp: STATUS_NOT_SUPPORTED, counted like the station's unsupported messages */
VOID
RtwWifiCompleteUnsupported (
  _In_ PRTW_WIFI    W,
  _In_ WIFIREQUEST  Request,
  _In_ UINT16       Id
  );

/* wfd.cpp; with Dev->CtlLock held unless noted */
ULONG
RtwWifiPortKind (                       // RTW_PORT_*; any IRQL <= DISPATCH_LEVEL
  _In_ PRTW_WIFI  W,
  _In_ UINT16     PortId
  );

VOID
RtwWifiReportWfdCapabilities (          // PrepareHardware: sets W->Wfd when Wi-Fi Direct is on and reported
  _In_ PRTW_WIFI  W
  );

/* messages for the Wi-Fi Direct device and role ports: TRUE if handled (completed) here */
BOOLEAN
RtwWifiWfdDispatch (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen,
  _In_ UINT16              Id
  );

/* adapter.cpp: the role NETADAPTER (WiFiCx's adapter callback for WIFI_ADAPTER_WIFI_DIRECT_ROLE) */
NTSTATUS
RtwWifiCreateRoleAdapter (
  _In_ PRTW_WIFI           W,
  _Inout_ NETADAPTER_INIT  *AdapterInit
  );

/* ap.cpp: the group owner; with Dev->CtlLock held */
VOID
RtwWifiApTaskStartAp (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  );

VOID
RtwWifiApTaskStopAp (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  );

VOID
RtwWifiApTaskSendAssocResponse (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  );

VOID
RtwWifiApTaskDisconnect (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  );

VOID
RtwWifiApTaskDot11Reset (
  _In_ PRTW_WIFI           W,
  _In_ WIFIREQUEST         Request,
  _In_ PWDI_MESSAGE_HEADER  Header,
  _In_ UINT                InLen
  );

/* the mac layer's GO indications (ApEvent) and a role adapter that went away */
VOID
RtwWifiApEvents (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    RoleGone
  );

/* Stops a running GO (and fails a running association response task): radio off, power down, release, the station
   taking another channel. With Indicate, while the role port is there: DISASSOCIATION for each client, then
   INDICATION_STOP_AP with StopReason (WDI_STOP_AP_REASON_*). */
VOID
RtwWifiApStop (
  _In_ PRTW_WIFI  W,
  _In_ BOOLEAN    Indicate,
  _In_ UINT32     StopReason
  );

/* ABORT_TASK of START_AP or SEND_AP_ASSOCIATION_RESPONSE: TRUE if it named the running one */
BOOLEAN
RtwWifiApAbort (
  _In_ PRTW_WIFI  W,
  _In_ UINT32     Oid,
  _In_ UINT32     TransactionId
  );

/* TASK_CONNECT while the GO runs: keep the candidates on its channel; none there: the GO stops (the station first) */
VOID
RtwWifiApBeforeConnect (
  _In_ PRTW_WIFI                  W,
  _Inout_ struct rtw_connect_req  *Rq
  );

/* TASK_SCAN while the GO has clients: only its channel, passive (the clients would miss beacons) */
VOID
RtwWifiApScanLimit (
  _In_ PRTW_WIFI               W,
  _Inout_ struct rtw_scan_req  *Rq
  );
