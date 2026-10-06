// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win - RTL8852BE (PCI\VEN_10EC&DEV_B852) Wi-Fi driver for the ROCK 5T, Windows ARM64.
 * Shared declarations of the Windows side (KMDF glue, log, diagnostics, and the C entry points of the C++ WiFiCx
 * front end in wificx\). C linkage throughout, so the C++ files can include it as well.
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>

EXTERN_C_START

#define RTW_POOL_TAG  'w9tr'

#define RTW_STR2(x)  #x
#define RTW_STR(x)   RTW_STR2 (x)
#ifdef RTW_VERSION_RAW
#define RTW_VERSION  RTW_STR (RTW_VERSION_RAW)   // build.ps1 passes the package version
#else
#define RTW_VERSION  "dev"
#endif

typedef struct _RTW_DEVICE {
  PVOID                            WdfTriageInfoPtr;   // must stay first: NetAdapterCx reads WDF_TRIAGE_INFO here
  WDFDEVICE                        Device;
  BOOLEAN                          WifiMode;     // this start of the device runs WiFiCx (driver.c, guard)
  PVOID                            Wifi;         // the front end's context (wificx\), NULL in diagnostic mode
  WDFDEVICE                        Cdo;          // control device \\.\rtw89win (diagnostics, both modes)
  KSPIN_LOCK                       BarLock;      // Bar/BarLen for IOCTL_RTW_READ32 against unmapping
  BUS_INTERFACE_STANDARD           Bus;          // PCI config space access
  BOOLEAN                          BusValid;
  PHYSICAL_ADDRESS                 BarPhys;      // BAR2 (MMIO), CPU physical address
  SIZE_T                           BarLen;
  volatile UCHAR                   *Bar;
  CM_PARTIAL_RESOURCE_DESCRIPTOR   IntRes;       // translated line interrupt, connected only while probed
  BOOLEAN                          IntResValid;
  // the Linux side (rtw89), started by RtwProbe (probe.c, os.c)
  WDFWAITLOCK                      CtlLock;      // serializes probe/remove/start/stop/monitor/scan (PASSIVE_LEVEL)
  BOOLEAN                          Probed;
  PKINTERRUPT                      Interrupt;
  KDPC                             IsrDpc;
  PDMA_ADAPTER                     Dma;
  // the guard against crash loops (driver.c)
  BOOLEAN                          GuardPending; // WiFiCx, but ArmRunning = 1 is written in PrepareHardware (early boot)
  BOOLEAN                          Guarded;      // diagnostic mode because the guard tripped
  // the power plan's wireless adapter power saving mode (driver.c), between PrepareHardware and ReleaseHardware
  PVOID                            PowerSetting;
} RTW_DEVICE, *PRTW_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME (RTW_DEVICE, RtwGetDevice)
C_ASSERT (FIELD_OFFSET (RTW_DEVICE, WdfTriageInfoPtr) == 0);

/* Context of the control device: the one PnP device it serves (NULL once that is going away). */
typedef struct _RTW_CDO {
  PRTW_DEVICE                      Dev;
} RTW_CDO, *PRTW_CDO;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME (RTW_CDO, RtwGetCdo)

/* log.c: in-memory log ring, read with IOCTL_RTW_LOG_READ (tools\rtwdiag.ps1 log) */
VOID
RtwLog (
  _In_z_ _Printf_format_string_ PCSTR  Format,
  ...
  );

VOID
RtwLogRead (
  _Inout_ ULONG64                  *From,
  _Out_ ULONG64                    *Dropped,
  _Out_writes_bytes_to_ (OutLen, *Copied) PCHAR  Out,
  _In_ ULONG                       OutLen,
  _Out_ ULONG                      *Copied
  );

/* hw.c */
NTSTATUS
RtwHwMap (
  _In_ PRTW_DEVICE   Dev,
  _In_ WDFCMRESLIST  Raw,
  _In_ WDFCMRESLIST  Translated
  );

VOID
RtwHwUnmap (
  _In_ PRTW_DEVICE  Dev
  );

VOID
RtwHwLogChipInfo (
  _In_ PRTW_DEVICE  Dev
  );

ULONG
RtwRead32 (
  _In_ PRTW_DEVICE  Dev,
  _In_ ULONG        Offset
  );

/* any IRQL <= DISPATCH_LEVEL, at any time: STATUS_INVALID_PARAMETER if not mapped or out of range */
NTSTATUS
RtwHwRead32Checked (
  _In_ PRTW_DEVICE  Dev,
  _In_ ULONG        Offset,
  _Out_ ULONG       *Value
  );

/* diag.c */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL  RtwEvtIoDeviceControl;

/* os.c: the rtwk_* primitives of compat\include\rtw\os.h for one device */
VOID
RtwOsInit (
  _In_ PDRIVER_OBJECT  DriverObject
  );

NTSTATUS
RtwOsAttach (
  _In_ PRTW_DEVICE  Dev
  );

VOID
RtwOsDetach (
  _In_ PRTW_DEVICE  Dev
  );

/* crypto.c: the AES primitives for management frame protection (kernel CNG); DriverEntry opens them and runs
   their known-answer tests, unload closes them. PASSIVE_LEVEL. */
VOID
RtwCryptoInit (
  VOID
  );

VOID
RtwCryptoExit (
  VOID
  );

/* probe.c: start and stop rtw89 on the device (PASSIVE_LEVEL); return 0 or a negative Linux errno */
LONG
RtwProbe (
  _In_ PRTW_DEVICE  Dev
  );

VOID
RtwRemove (
  _In_ PRTW_DEVICE  Dev
  );

VOID
RtwLinuxExit (
  VOID
  );

/*
 * wificx\: the WiFiCx front end (C++), used only when the device was armed. All PASSIVE_LEVEL.
 * RtwWifiPreDeviceCreate: NetDeviceInitConfig + WifiDeviceInitConfig, before WdfDeviceCreate.
 * RtwWifiPostDeviceCreate: WifiDeviceInitialize, the command work item; sets Dev->Wifi.
 * RtwWifiPrepareHardware: probe (firmware, efuse) and report the capabilities; takes Dev->CtlLock.
 * RtwWifiReleaseHardware: stops command processing and the scan, stops the mac layer; before RtwRemove.
 * RtwWifiD0Entry/D0Exit: allow/stop hardware use (D0Exit stops the mac layer).
 */
NTSTATUS
RtwWifiPreDeviceCreate (
  _Inout_ PWDFDEVICE_INIT  DeviceInit
  );

NTSTATUS
RtwWifiPostDeviceCreate (
  _In_ PRTW_DEVICE  Dev
  );

NTSTATUS
RtwWifiPrepareHardware (
  _In_ PRTW_DEVICE  Dev
  );

VOID
RtwWifiReleaseHardware (
  _In_ PRTW_DEVICE  Dev
  );

VOID
RtwWifiD0Entry (
  _In_ PRTW_DEVICE  Dev
  );

VOID
RtwWifiD0Exit (
  _In_ PRTW_DEVICE  Dev
  );

/* IOCTL_RTW_MODE (rtw89win_ioctl.h); any time, reads counters only. Counts: per message id, if not NULL. */
struct _RTW_MODE_OUT;
struct _RTW_WDI_COUNTS;

VOID
RtwWifiGetMode (
  _In_ PRTW_DEVICE                    Dev,
  _Out_ struct _RTW_MODE_OUT          *Out,
  _Out_opt_ struct _RTW_WDI_COUNTS    *Counts
  );

/* IOCTL_RTW_AP (rtw89win_ioctl.h): the front end's Wi-Fi Direct ports and GO; any time, a snapshot */
struct _RTW_WFD_OUT;

VOID
RtwWifiGetWfd (
  _In_ PRTW_DEVICE              Dev,
  _Out_ struct _RTW_WFD_OUT     *Out
  );

EXTERN_C_END
