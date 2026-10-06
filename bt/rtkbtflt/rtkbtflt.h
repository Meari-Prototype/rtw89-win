// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtkbtflt - lower filter under the in-box BTHUSB for the Bluetooth half of the RTL8852BE (USB 13D3:3572) on the
 * ROCK 5T, Windows ARM64.
 *
 * The in-box stack (bth.inf / BTHUSB / bthport) cannot load Realtek's patch, and the controller's ROM firmware cannot
 * scan or pair. This filter sits between BTHUSB and the USB hub's PDO and, in its D0 entry - after the PDO has started
 * or powered up and before BTHUSB has seen the IRP - checks the controller with HCI_Read_Local_Version and, when it runs
 * its ROM, downloads rtl8852bu_fw.bin (+ config) the way Linux btrtl does, so that bthport always initialises a patched
 * controller. Every failure leaves the device as it is (pass-through): BTHUSB then runs the ROM, as without the filter.
 */
#ifndef RTKBTFLT_H_
#define RTKBTFLT_H_

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbdlib.h>
#include <ntstrsafe.h>
#include "../rtkbtfw/epatch.h"

#define FLT_POOL_TAG          'fBkR'

#define FLT_STR2(x)           #x
#define FLT_STR(x)            FLT_STR2 (x)
#ifdef FLT_VERSION_RAW
#define FLT_VERSION           FLT_STR (FLT_VERSION_RAW)    // build.ps1 passes the package version
#else
#define FLT_VERSION           "dev"
#endif

#define FLT_FW_NAME           L"rtl8852bu_fw.bin"
#define FLT_CFG_NAME          L"rtl8852bu_config.bin"
#define FLT_FILE_MAX          (1024 * 1024)

#define FLT_LOG_SIZE          4096        // text log of one session (Parameters\LastSession)
#define FLT_ACC_SIZE          1024        // HCI event bytes received and not yet consumed
#define FLT_READ_SIZE         512         // our interrupt IN transfer (an HCI event is at most 257 bytes)
#define FLT_CMD_MAX           (3 + 255)   // opcode, parameter length, parameters

// timeouts (ms)
#define FLT_T_SEND            1000        // one control transfer (a command) or descriptor request
#define FLT_T_SELECT          3000        // our SELECT_CONFIGURATION
#define FLT_T_FIRST_EVT       2000        // the Command Complete of the first command of a session
#define FLT_T_EVT             1000        // any other Command Complete
#define FLT_T_DOWNLOAD        20000       // all of the 0xFC20 fragments
#define FLT_T_READ_STOP       5000        // our interrupt read to come back after its cancel
#define FLT_T_HIVE_START      30000       // the registry hive to become writable, at a start (early boot)
#define FLT_T_HIVE_RESUME     5000        // the same, at a D3 -> D0
#define FLT_T_GUARD_HOLD      60000       // the guard stays set this long after a session that changed the device

typedef enum {
  FltModeNormal = 0,
  FltModeUser,                // Parameters\PassThrough = 1
  FltModeGuard,               // the crash loop guard tripped (BadSessions)
  FltModeBroken               // a resource of the filter could not be set up
} FLT_MODE;

// Parameters\LastResult
typedef enum {
  FltResultPatched = 0,       // downloaded, the controller reports the patch
  FltResultAlreadyPatched,    // the controller runs a patch already: nothing sent
  FltResultNotRom,            // not Realtek, or not the 8852B USB's ROM: nothing sent
  FltResultFailedRom,         // failed before the last fragment (or the patch was not taken): the controller runs its ROM
  FltResultUnknown,           // the last fragment went out without a good completion and no version could be read
  FltResultNoVersion,         // no answer to HCI_Read_Local_Version
  FltResultNoFirmware,        // no usable patch file in the driver directory
  FltResultPassThrough,       // pass-through mode (PassThrough = 1, the guard, a broken setup)
  FltResultUsbError,          // our own USB requests failed (descriptor, configuration, interrupt read)
  FltResultHive               // the registry could not record the session (guard): nothing sent
} FLT_RESULT;

typedef struct _FLT_DEVICE {
  WDFDEVICE            Device;
  PDEVICE_OBJECT       Lower;            // the next lower device object (the hub's PDO, or a filter on it)
  WDFIOTARGET          Target;           // the default I/O target (the same device), for our own requests
  FLT_MODE             Mode;
  BOOLEAN              ReadStuck;        // our interrupt read did not come back after its cancel: no more sessions
  ULONG                D2Resumes;        // D1/D2 -> D0 since the last session (nothing done for them)
  CHAR                 ModeNote[96];

  // the guard held after a session that changed the device, while BTHUSB starts on it (driver.c)
  WDFTIMER             GuardTimer;       // passive level, one shot: lets the guard go after FLT_T_GUARD_HOLD
  volatile LONG        GuardHeld;        // 1 = SessionRunning is still 1 for the last session
  ULONG64              GuardHeldAt;      // interrupt time the hold began
  BOOLEAN              ShutdownHook;     // this device's hold ends at shutdown (driver.c's control device)
  volatile LONG        BthSelects;       // BTHUSB's successful SELECT_CONFIGURATIONs since the last session
  volatile LONG        BthEvents;        // BTHUSB's event reads completed with data since the last session

  // the patch file and the config, read by the first session of a start (non-paged; freed in ReleaseHardware)
  BOOLEAN              FwTried;
  PUCHAR               Fw;
  SIZE_T               FwLen;
  PUCHAR               Cfg;
  SIZE_T               CfgLen;
  struct rtkbt_epatch  Ep;
  BOOLEAN              FwOk;
  CHAR                 FwNote[128];

  // the HCI event pipe (interrupt IN): from our own SELECT_CONFIGURATION at a start, then from BTHUSB's
  USBD_PIPE_HANDLE volatile  EventPipe;
  volatile LONG        EventInterface;   // its interface number, -1 = none
  volatile LONG        EventReadsBelow;  // BTHUSB's event reads passed down and not completed yet
  volatile LONG        Stolen;           // events taken out of BTHUSB's reads in this session

  // one session (D0 entry): the event stream, under Lock (completion routines run at DISPATCH_LEVEL)
  KSPIN_LOCK           Lock;
  BOOLEAN              SessionOpen;
  USHORT               WaitOpcode;       // the command whose Command Complete / Status is awaited, 0 = none
  ULONG                AccLen;
  UCHAR                Acc[FLT_ACC_SIZE];
  ULONG                Dropped;          // complete events that were not the awaited one
  UCHAR                LastDropped;      // the event code of the last of them
  ULONG                Overflows;
  KEVENT               DataEvent;        // synchronization: event bytes arrived

  // our interrupt IN read (one request, resubmitted while the session is open)
  WDFREQUEST           ReadRequest;
  WDFMEMORY            ReadUrbMemory;
  USBD_PIPE_HANDLE     ReadPipe;
  BOOLEAN              ReadPending;
  BOOLEAN              ReadStop;
  NTSTATUS             ReadStatus;       // of the last completion
  USBD_STATUS          ReadUsbdStatus;
  ULONG                Reads;            // completions in this session
  KEVENT               ReadIdle;         // notification: signalled while no read is pending
  URB                  ReadUrb;
  UCHAR                ReadBuf[FLT_READ_SIZE];

  // commands (control transfers), one at a time, PASSIVE_LEVEL
  URB                  CmdUrb;
  UCHAR                CmdBuf[FLT_CMD_MAX];

  // the session's text log
  ULONG64              LogT0;
  ULONG                LogLen;
  BOOLEAN              LogFull;
  CHAR                 Log[FLT_LOG_SIZE];
} FLT_DEVICE, *PFLT_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME (FLT_DEVICE, FltGetDevice)

/* driver.c */
_IRQL_requires_max_ (PASSIVE_LEVEL)
VOID
FltLog (
  _In_opt_ PFLT_DEVICE  Dev,
  _In_z_ _Printf_format_string_ PCSTR  Format,
  ...
  );

_IRQL_requires_max_ (PASSIVE_LEVEL)
VOID
FltSaveSession (
  _In_ PFLT_DEVICE  Dev,
  _In_ FLT_RESULT   Result,
  _In_ BOOLEAN      Downloaded,
  _In_ USHORT       HciRev,
  _In_ USHORT       LmpSubver,
  _In_ BOOLEAN      Held
  );

_IRQL_requires_max_ (PASSIVE_LEVEL)
VOID
FltSavePassThrough (
  _In_ PFLT_DEVICE  Dev
  );

_IRQL_requires_max_ (PASSIVE_LEVEL)
VOID
FltProgress (
  _In_ PFLT_DEVICE  Dev,
  _In_z_ PCSTR      Stage,
  _In_ BOOLEAN      Flush
  );

_IRQL_requires_max_ (PASSIVE_LEVEL)
NTSTATUS
FltGuardBegin (
  _In_ PFLT_DEVICE  Dev,
  _In_ ULONG        WaitMs
  );

_IRQL_requires_max_ (PASSIVE_LEVEL)
VOID
FltGuardEnd (
  _In_opt_ PFLT_DEVICE  Dev
  );

_IRQL_requires_max_ (PASSIVE_LEVEL)
VOID
FltGuardHold (
  _In_ PFLT_DEVICE  Dev
  );

_IRQL_requires_max_ (PASSIVE_LEVEL)
VOID
FltGuardRelease (
  _In_ PFLT_DEVICE  Dev,
  _In_z_ PCSTR      Why
  );

/* urb.c */
EVT_WDFDEVICE_WDM_IRP_PREPROCESS  FltEvtWdmInternalIoctl;

/* hci.c */
_IRQL_requires_max_ (PASSIVE_LEVEL)
VOID
FltLoadFirmware (
  _In_ PFLT_DEVICE  Dev
  );

_IRQL_requires_max_ (DISPATCH_LEVEL)
VOID
FltFreeFirmware (
  _In_ PFLT_DEVICE  Dev
  );

_IRQL_requires_max_ (PASSIVE_LEVEL)
VOID
FltSession (
  _In_ PFLT_DEVICE  Dev,
  _In_ BOOLEAN      Start
  );

_IRQL_requires_max_ (DISPATCH_LEVEL)
BOOLEAN
FltOfferStolen (
  _In_ PFLT_DEVICE                Dev,
  _In_reads_bytes_ (Len) PCUCHAR  Buf,
  _In_ ULONG                      Len
  );

#endif
