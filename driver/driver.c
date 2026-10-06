// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win - RTL8852BE (PCI\VEN_10EC&DEV_B852) Wi-Fi driver for the ROCK 5T, Windows ARM64.
 *
 * Every start runs the device as a WiFiCx/NetAdapterCx device (wificx\): PrepareHardware probes rtw89 (Linux v7.2,
 * rtw89hal.lib) and reports the capabilities, and WiFiCx sends the commands. A guard against crash loops (below)
 * stands in front of this; when it has tripped, the device starts in the diagnostic mode instead, without
 * NetAdapterCx/WiFiCx, until tools\rtwdiag.ps1 arm clears it. A diagnostic start binds the device, maps BAR2 and logs
 * PCI config and the chip version; nothing is written to the chip, no interrupt is connected and no DMA is set up.
 * There IOCTL_RTW_PROBE/REMOVE start and stop rtw89, and IOCTL_RTW_START/STOP/MONITOR/SCAN drive it through the mac
 * layer (interrupts, RX, H2C/C2H, firmware scan).
 * Release of the hardware and driver unload stop the mac layer before removing rtw89 (probe.c, compat\glue.c).
 * While the device has its hardware, the power plan's wireless adapter power saving mode goes to the mac layer's
 * power save policy. The AES primitives of management frame protection (crypto.c, kernel CNG) are opened and
 * self-tested at load.
 * Diagnostics: the control device \\.\rtw89win in both modes (IOCTLs in rtw89win_ioctl.h, client
 * tools\rtwdiag.ps1); it goes away with the PnP device. It also gets the system's shutdown notification (guard).
 */
#include "rtw89win.h"
#include <initguid.h>
#include <wdmguid.h>
#include "../compat/include/rtw/glue.h"

// power plan, "Wireless Adapter Settings" (19cbb8fa-5279-450e-9fac-8a3d5fedd0c1), "Power Saving Mode":
// 0 maximum performance, 1 low, 2 medium, 3 maximum power saving
DEFINE_GUID (RTW_GUID_WIFI_POWER_SAVING_MODE, 0x12bbebe6, 0x58d6, 0x4636, 0x95, 0xbb, 0x32, 0x17, 0xef, 0x86, 0x7c, 0x1a);

#define RTW_SAVED_LOG_SIZE 0x4000     // tail of the log kept in Parameters\SavedLog

DRIVER_INITIALIZE                  DriverEntry;
EVT_WDF_DRIVER_UNLOAD              RtwEvtDriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD          RtwEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE    RtwEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE    RtwEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY            RtwEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT             RtwEvtD0Exit;
EVT_WDF_OBJECT_CONTEXT_CLEANUP     RtwEvtDeviceCleanup;
EVT_WDF_DEVICE_SHUTDOWN_NOTIFICATION RtwEvtShutdown;
POWER_SETTING_CALLBACK             RtwPowerSettingCallback;

NTSTATUS
DriverEntry (
  _In_ PDRIVER_OBJECT   DriverObject,
  _In_ PUNICODE_STRING  RegistryPath
  )
{
  WDF_DRIVER_CONFIG  Config;
  NTSTATUS           Status;

  RtwLog ("rtw89win " RTW_VERSION " loaded");
  RtwOsInit (DriverObject);
  RtwCryptoInit ();               // AES for management frame protection, with its self-test

  WDF_DRIVER_CONFIG_INIT (&Config, RtwEvtDeviceAdd);
  Config.DriverPoolTag   = RTW_POOL_TAG;
  Config.EvtDriverUnload = RtwEvtDriverUnload;
  Status = WdfDriverCreate (DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WdfDriverCreate failed 0x%08x", Status);
    RtwCryptoExit ();
  }

  return Status;
}

VOID
RtwEvtDriverUnload (
  _In_ WDFDRIVER  Driver
  )
{
  UNREFERENCED_PARAMETER (Driver);

  RtwLinuxExit ();
  RtwCryptoExit ();
  RtwLog ("rtw89win unloaded");
}

/*
 * The guard against crash loops. Every start of the device runs WiFiCx, and records that
 * a WiFiCx session is running before anything touches the chip. Values under Parameters (this driver's own,
 * persistent):
 *  ArmRunning    1 (flushed to disk) before the chip is touched; 0 again only when that session ends in order:
 *                release of the hardware (restart, disable, removal, driver update), or the system's shutdown
 *                notification (a reboot or shutdown: devices are not released then, and their D0 exit comes after the
 *                registry's final flush). Still 1 at the next start = that session did not end in order (a crash,
 *                whenever it came, or a power cut).
 *  BootAttempts  sessions in a row that did not end in order; 0 again when one ends in order. At 2 the device
 *                starts in the diagnostic mode (the chip untouched but for reading registers) until rtwdiag.ps1 arm
 *                sets both values to 0: two crashes in a row, then no third. Written as 2 by hand, it stays at the
 *                end of the running session: the way into the diagnostic mode for tests (probe, monitor, txprobe).
 * Early in boot the hive cannot be flushed yet (ZwFlushKey: STATUS_REGISTRY_IO_FAILED). The device is then added
 * as a WiFiCx device all the same ("pending"), and PrepareHardware retries every 500 ms, for at most 60 s, before
 * the probe; if the hive stays read-only, the start fails without touching the chip (no WiFiCx, and no diagnostic
 * mode either, in this start; the next start tries again). Touching the chip unrecorded could loop on a crash at
 * every boot; and the device cannot change to the diagnostic mode within this start: that needs pci.sys to
 * re-enumerate it (WdfDeviceFailedAttemptRestart), after which the RTL8852BE was observed not to take its firmware
 * again until a power cycle ("FWDL path ready" errors).
 */
#define RTW_GUARD_RETRY_MS     500
#define RTW_GUARD_WAIT_S       60
#define RTW_GUARD_MAX          2

typedef enum {
  RtwModeDiag,
  RtwModeGuarded,           // diagnostic, because the guard tripped
  RtwModeWifi,
  RtwModeWifiPending        // WiFiCx, but ArmRunning = 1 is written in PrepareHardware
} RTW_MODE_DECISION;

DECLARE_CONST_UNICODE_STRING (mBootAttemptsName, L"BootAttempts");
DECLARE_CONST_UNICODE_STRING (mArmRunningName, L"ArmRunning");

// 1 while this driver's ArmRunning = 1 stands for a session of its own (one device); ended once (RtwGuardEnd)
static volatile LONG  mGuardRunning;

static ULONG
RtwRegGet (
  _In_ WDFKEY            Key,
  _In_ PCUNICODE_STRING  Name
  )
{
  ULONG  Value;

  if (!NT_SUCCESS (WdfRegistryQueryULong (Key, Name, &Value))) {
    Value = 0;
  }

  return Value;
}

/* Writes a persistent value and flushes the key. (A flush with nothing dirty can succeed even while the hive cannot
   be written back; the guard's flushes always follow a change.) */
static NTSTATUS
RtwRegSetFlushed (
  _In_ WDFKEY            Key,
  _In_ PCUNICODE_STRING  Name,
  _In_ ULONG             Value
  )
{
  NTSTATUS  Status;

  Status = WdfRegistryAssignULong (Key, Name, Value);
  if (NT_SUCCESS (Status)) {
    Status = ZwFlushKey (WdfRegistryWdmGetHandle (Key));
  }

  return Status;
}

/*
 * Records the start of a WiFiCx session, once the hive can be flushed: an ArmRunning still 1 means the last session
 * did not end in order, which adds one to BootAttempts; at RTW_GUARD_MAX the guard has tripped (STATUS_CANCELLED).
 * Otherwise ArmRunning = 1 reaches the disk before the chip is touched; if that flush fails, ArmRunning goes back to
 * 0 in memory, so a retry does not count a crash that did not happen. Every flush here follows a change of a value
 * (the hive is dirty), so a successful flush means it reached the disk.
 */
static NTSTATUS
RtwGuardBegin (
  _In_ WDFKEY  Key
  )
{
  ULONG     Attempts;
  NTSTATUS  Status;

  Attempts = RtwRegGet (Key, &mBootAttemptsName);
  if (RtwRegGet (Key, &mArmRunningName) != 0) {
    // both values change in memory, then reach the disk together (or later, lazily, still together: a retry reads
    // them consistent)
    Attempts++;
    Status = WdfRegistryAssignULong (Key, &mBootAttemptsName, Attempts);
    if (NT_SUCCESS (Status)) {
      Status = WdfRegistryAssignULong (Key, &mArmRunningName, 0);
    }

    if (NT_SUCCESS (Status)) {
      Status = ZwFlushKey (WdfRegistryWdmGetHandle (Key));
    }

    if (!NT_SUCCESS (Status)) {
      return Status;
    }

    RtwLog ("guard: the last WiFiCx session did not end in order (BootAttempts = %u)", Attempts);
  }

  if (Attempts >= RTW_GUARD_MAX) {
    return STATUS_CANCELLED;
  }

  Status = RtwRegSetFlushed (Key, &mArmRunningName, 1);
  if (!NT_SUCCESS (Status)) {
    (VOID)WdfRegistryAssignULong (Key, &mArmRunningName, 0);
  } else {
    InterlockedExchange (&mGuardRunning, 1);
  }

  return Status;
}

/* AddDevice, PASSIVE: the mode of this start. */
static RTW_MODE_DECISION
RtwGuardDecide (
  _In_ WDFDRIVER  Driver
  )
{
  WDFKEY    Key;
  ULONG     Attempts;
  NTSTATUS  Status;

  Status = WdfDriverOpenParametersRegistryKey (Driver, KEY_READ | KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &Key);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("mode: diagnostic (Parameters key not opened, 0x%08x: the guard cannot record a session)", Status);
    return RtwModeDiag;
  }

  // a tripped guard needs no writable hive to tell; it is recorded in memory (it reaches the disk lazily)
  Attempts = RtwRegGet (Key, &mBootAttemptsName) + ((RtwRegGet (Key, &mArmRunningName) != 0) ? 1 : 0);
  if (Attempts >= RTW_GUARD_MAX) {
    (VOID)WdfRegistryAssignULong (Key, &mBootAttemptsName, Attempts);
    (VOID)WdfRegistryAssignULong (Key, &mArmRunningName, 0);
    WdfRegistryClose (Key);
    RtwLog ("guard: the last %u WiFiCx sessions in a row did not end in order (Parameters\\BootAttempts = %u): "
            "diagnostic mode until rtwdiag.ps1 arm clears it", Attempts, Attempts);
    RtwLog ("mode: diagnostic (guard)");
    return RtwModeGuarded;
  }

  // WiFiCx now if ArmRunning = 1 reaches the disk; otherwise the device is still added as a WiFiCx device and
  // PrepareHardware waits for the hive before it touches the chip
  Status = RtwGuardBegin (Key);
  WdfRegistryClose (Key);
  if (NT_SUCCESS (Status)) {
    RtwLog ("mode: WiFiCx (guard: ArmRunning = 1, BootAttempts = %u)", Attempts);
    return RtwModeWifi;
  }

  RtwLog ("mode: WiFiCx, guard pending (registry not writable yet, 0x%08x): PrepareHardware waits for it", Status);
  return RtwModeWifiPending;
}

/* PrepareHardware of a pending start (PASSIVE): retries RtwGuardBegin every RTW_GUARD_RETRY_MS for at most
   RTW_GUARD_WAIT_S. Nothing touches the chip unless it succeeds; otherwise this start fails. */
static NTSTATUS
RtwGuardWait (
  _In_ PRTW_DEVICE  Dev
  )
{
  LARGE_INTEGER  Delay;
  WDFKEY         Key;
  ULONG64        Start;
  ULONG64        Ms;
  ULONG          Tries;
  NTSTATUS       Status;

  Status = WdfDriverOpenParametersRegistryKey (WdfDeviceGetDriver (Dev->Device), KEY_READ | KEY_SET_VALUE,
                                               WDF_NO_OBJECT_ATTRIBUTES, &Key);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("guard: Parameters key not opened (0x%08x); the start fails, the chip untouched", Status);
    return STATUS_DEVICE_NOT_READY;
  }

  Start          = KeQueryInterruptTime ();
  Delay.QuadPart = -10000LL * RTW_GUARD_RETRY_MS;
  for (Tries = 1; ; Tries++) {
    Status = RtwGuardBegin (Key);
    if (NT_SUCCESS (Status) || (Status == STATUS_CANCELLED) ||
        (KeQueryInterruptTime () - Start >= RTW_GUARD_WAIT_S * 10000000ULL)) {
      break;
    }

    KeDelayExecutionThread (KernelMode, FALSE, &Delay);
  }

  WdfRegistryClose (Key);
  Ms = (KeQueryInterruptTime () - Start) / 10000;
  if (NT_SUCCESS (Status)) {
    RtwLog ("guard: registry writable after %llu ms (%u tries); ArmRunning = 1", Ms, Tries);
    return STATUS_SUCCESS;
  }

  if (Status == STATUS_CANCELLED) {
    // AddDevice saw fewer; only another start in between could get here
    RtwLog ("guard: tripped meanwhile; the start fails, the chip untouched");
  } else {
    RtwLog ("guard: registry still not writable after %llu ms (0x%08x); the start fails, the chip untouched (the "
            "next start tries again)", Ms, Status);
  }

  return STATUS_DEVICE_NOT_READY;
}

/* The WiFiCx session ends in order (release of the hardware, the system's shutdown notification): ArmRunning and
   BootAttempts go back to 0. A BootAttempts at RTW_GUARD_MAX stays: a session never runs with it (it was written by
   hand, to get the diagnostic mode from the next start on). Once per session; returns whether this call ended one. */
static BOOLEAN
RtwGuardEnd (
  _In_ PCSTR  Why
  )
{
  WDFKEY    Key;
  ULONG     Attempts;
  NTSTATUS  Status;

  if (InterlockedExchange (&mGuardRunning, 0) == 0) {
    return FALSE;
  }

  Attempts = 0;
  Status   = WdfDriverOpenParametersRegistryKey (WdfGetDriver (), KEY_READ | KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES,
                                                 &Key);
  if (NT_SUCCESS (Status)) {
    Attempts = RtwRegGet (Key, &mBootAttemptsName);
    if (Attempts < RTW_GUARD_MAX) {
      Attempts = 0;
      Status   = WdfRegistryAssignULong (Key, &mBootAttemptsName, 0);
    }

    if (NT_SUCCESS (Status)) {
      Status = RtwRegSetFlushed (Key, &mArmRunningName, 0);
    }

    WdfRegistryClose (Key);
  }

  RtwLog ("guard: WiFiCx session ended in order (%s); ArmRunning 0, BootAttempts %u%s (0x%08x)", Why, Attempts,
          (Attempts != 0) ? " (set by hand: diagnostic mode from the next start)" : "", Status);
  return TRUE;
}

/* Keeps the tail of the log in Parameters\SavedLog (REG_BINARY; rtwdiag.ps1 log -Saved) when a WiFiCx start fails,
   when a WiFiCx device lets go of its hardware (removal and, without other devices, driver unload follow, and they
   take the log ring with them; the next start begins a new one) and at the shutdown notification. */
static VOID
RtwSaveLog (
  _In_ WDFDRIVER  Driver
  )
{
  DECLARE_CONST_UNICODE_STRING (Name, L"SavedLog");
  WDFKEY    Key;
  PCHAR     Buf;
  ULONG64   From;
  ULONG64   Dropped;
  ULONG     Copied;
  NTSTATUS  Status;

  // non-paged: RtwLogRead fills it under a spin lock
  Buf = ExAllocatePool2 (POOL_FLAG_NON_PAGED, RTW_SAVED_LOG_SIZE, RTW_POOL_TAG);
  if (Buf == NULL) {
    return;
  }

  From = MAXULONG64;          // a read from past the end only returns the end position
  RtwLogRead (&From, &Dropped, Buf, 0, &Copied);
  From = (From > RTW_SAVED_LOG_SIZE) ? (From - RTW_SAVED_LOG_SIZE) : 0;
  RtwLogRead (&From, &Dropped, Buf, RTW_SAVED_LOG_SIZE, &Copied);
  Status = WdfDriverOpenParametersRegistryKey (Driver, KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &Key);
  if (NT_SUCCESS (Status)) {
    Status = WdfRegistryAssignValue (Key, &Name, REG_BINARY, Copied, Buf);
    WdfRegistryClose (Key);
  }

  if (!NT_SUCCESS (Status)) {
    RtwLog ("SavedLog not saved (0x%08x)", Status);
  }

  ExFreePoolWithTag (Buf, RTW_POOL_TAG);
}

/*
 * The power plan's wireless adapter power saving mode, for the mac layer's power save policy when it follows the
 * OS (mac\ps.c). The power manager calls back once at registration and whenever the plan or the power source
 * changes, at PASSIVE_LEVEL; CtlLock keeps the call apart from probe and remove. Registered while the device has its
 * hardware (both modes); a failure only leaves the plan unknown (no power save when following the OS).
 */
NTSTATUS
RtwPowerSettingCallback (
  _In_ LPCGUID                          SettingGuid,
  _In_reads_bytes_ (ValueLength) PVOID  Value,
  _In_ ULONG                            ValueLength,
  _Inout_opt_ PVOID                     Context
  )
{
  PRTW_DEVICE  Dev;
  ULONG        Mode;

  Dev = (PRTW_DEVICE)Context;
  if ((Dev == NULL) || !IsEqualGUID (SettingGuid, &RTW_GUID_WIFI_POWER_SAVING_MODE)) {
    return STATUS_SUCCESS;
  }

  // observed on Windows 11 26100: this setting can arrive with 16 bytes that are not the plan's index (with the
  // plan's AC and DC values both 0); the mode stays unknown then
  if (ValueLength != sizeof (ULONG)) {
    RtwLog ("power plan: wireless adapter power saving mode not readable (%u bytes); unknown", ValueLength);
    return STATUS_SUCCESS;
  }

  Mode = *(const ULONG *)Value;
  RtwLog ("power plan: wireless adapter power saving mode %u", Mode);
  WdfWaitLockAcquire (Dev->CtlLock, NULL);
  rtw_linux_set_ps_policy (RTW_PS_SRC_PLAN, Mode);
  WdfWaitLockRelease (Dev->CtlLock);
  return STATUS_SUCCESS;
}

/* PrepareHardware, after the bring-up succeeded; without CtlLock (the first callback takes it) */
static VOID
RtwPowerSettingRegister (
  _In_ PRTW_DEVICE  Dev
  )
{
  NTSTATUS  Status;

  Status = PoRegisterPowerSettingCallback (WdfDeviceWdmGetDeviceObject (Dev->Device), &RTW_GUID_WIFI_POWER_SAVING_MODE,
                                           RtwPowerSettingCallback, Dev, &Dev->PowerSetting);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("power plan setting not registered (0x%08x): power saving mode unknown", Status);
    Dev->PowerSetting = NULL;
  }
}

/* ReleaseHardware, before anything takes CtlLock (a callback in progress may wait for it) */
static VOID
RtwPowerSettingUnregister (
  _In_ PRTW_DEVICE  Dev
  )
{
  if (Dev->PowerSetting != NULL) {
    (VOID)PoUnregisterPowerSettingCallback (Dev->PowerSetting);
    Dev->PowerSetting = NULL;
  }
}

/*
 * The system shuts down or restarts (IRP_MJ_SHUTDOWN, to the control device; PASSIVE). The registry is still
 * writable here, unlike in the D0 exit that follows later in the shutdown, so this is where a WiFiCx session that
 * runs until the end ends in order for the guard. A crash after this point in the shutdown is not counted (it cannot
 * loop: the next boot starts afresh).
 */
VOID
RtwEvtShutdown (
  _In_ WDFDEVICE  Device
  )
{
  UNREFERENCED_PARAMETER (Device);

  if (RtwGuardEnd ("shutdown")) {
    RtwSaveLog (WdfGetDriver ());
  }
}

/* The control device \\.\rtw89win (administrators and SYSTEM only). Failing to create it does not fail the device:
   only the diagnostics are missing then, and the shutdown notification (each reboot then counts as a session that
   did not end in order: after two, the guard trips). Deleted in RtwEvtDeviceCleanup. */
static VOID
RtwCdoCreate (
  _In_ WDFDRIVER    Driver,
  _In_ PRTW_DEVICE  Dev
  )
{
  DECLARE_CONST_UNICODE_STRING (Sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
  DECLARE_CONST_UNICODE_STRING (Name, L"\\Device\\rtw89win");
  DECLARE_CONST_UNICODE_STRING (SymLink, L"\\DosDevices\\rtw89win");
  PWDFDEVICE_INIT        Init;
  WDF_OBJECT_ATTRIBUTES  Attributes;
  WDF_IO_QUEUE_CONFIG    QueueConfig;
  WDFDEVICE              Cdo;
  NTSTATUS               Status;

  Init = WdfControlDeviceInitAllocate (Driver, &Sddl);
  if (Init == NULL) {
    RtwLog ("WdfControlDeviceInitAllocate failed; no diagnostics");
    return;
  }

  WdfDeviceInitSetCharacteristics (Init, FILE_DEVICE_SECURE_OPEN, TRUE);
  WdfControlDeviceInitSetShutdownNotification (Init, RtwEvtShutdown, WdfDeviceShutdown);
  Status = WdfDeviceInitAssignName (Init, &Name);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WdfDeviceInitAssignName failed 0x%08x; no diagnostics", Status);
    WdfDeviceInitFree (Init);
    return;
  }

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attributes, RTW_CDO);
  Status = WdfDeviceCreate (&Init, &Attributes, &Cdo);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("control device not created (0x%08x); no diagnostics", Status);
    WdfDeviceInitFree (Init);
    return;
  }

  RtwGetCdo (Cdo)->Dev = Dev;
  Status = WdfDeviceCreateSymbolicLink (Cdo, &SymLink);
  if (NT_SUCCESS (Status)) {
    // parallel, so log reads go on during a scan; the control IOCTLs serialize on CtlLock (diag.c)
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE (&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoDeviceControl = RtwEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT (&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfIoQueueCreate (Cdo, &QueueConfig, &Attributes, WDF_NO_HANDLE);
  }

  if (!NT_SUCCESS (Status)) {
    RtwLog ("control device setup failed 0x%08x; no diagnostics", Status);
    WdfObjectDelete (Cdo);
    return;
  }

  WdfControlFinishInitializing (Cdo);
  Dev->Cdo = Cdo;
}

NTSTATUS
RtwEvtDeviceAdd (
  _In_ WDFDRIVER        Driver,
  _Inout_ PWDFDEVICE_INIT  DeviceInit
  )
{
  WDF_PNPPOWER_EVENT_CALLBACKS  PnpPower;
  WDF_OBJECT_ATTRIBUTES         Attributes;
  WDFDEVICE                     Device;
  PRTW_DEVICE                   Dev;
  RTW_MODE_DECISION             Decision;
  BOOLEAN                       Wifi;
  NTSTATUS                      Status;

  // before anything that could touch the chip, and before the device is a NetAdapterCx/WiFiCx one
  Decision = RtwGuardDecide (Driver);
  Wifi     = (Decision == RtwModeWifi) || (Decision == RtwModeWifiPending);
  if (Wifi) {
    Status = RtwWifiPreDeviceCreate (DeviceInit);
    if (!NT_SUCCESS (Status)) {
      (VOID)RtwGuardEnd ("WiFiCx device setup failed");
      return Status;
    }
  }

  WDF_PNPPOWER_EVENT_CALLBACKS_INIT (&PnpPower);
  PnpPower.EvtDevicePrepareHardware = RtwEvtPrepareHardware;
  PnpPower.EvtDeviceReleaseHardware = RtwEvtReleaseHardware;
  if (Wifi) {
    PnpPower.EvtDeviceD0Entry = RtwEvtD0Entry;
    PnpPower.EvtDeviceD0Exit  = RtwEvtD0Exit;
  }

  WdfDeviceInitSetPnpPowerEventCallbacks (DeviceInit, &PnpPower);

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attributes, RTW_DEVICE);
  Attributes.EvtCleanupCallback = RtwEvtDeviceCleanup;
  Status = WdfDeviceCreate (&DeviceInit, &Attributes, &Device);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WdfDeviceCreate failed 0x%08x", Status);
    (VOID)RtwGuardEnd ("device not added");
    return Status;
  }

  Dev = RtwGetDevice (Device);
  Dev->WdfTriageInfoPtr = WdfGetTriageInfo ();
  Dev->Device           = Device;
  Dev->WifiMode         = Wifi;
  Dev->Guarded          = (Decision == RtwModeGuarded);
  Dev->GuardPending     = (Decision == RtwModeWifiPending);
  KeInitializeSpinLock (&Dev->BarLock);

  Status = WdfFdoQueryForInterface (
             Device,
             &GUID_BUS_INTERFACE_STANDARD,
             (PINTERFACE)&Dev->Bus,
             sizeof (BUS_INTERFACE_STANDARD),
             1,
             NULL
             );
  if (!NT_SUCCESS (Status)) {
    RtwLog ("no BUS_INTERFACE_STANDARD (0x%08x); PCI config space not logged", Status);
  } else {
    Dev->BusValid = TRUE;
  }

  WDF_OBJECT_ATTRIBUTES_INIT (&Attributes);
  Attributes.ParentObject = Device;
  Status = WdfWaitLockCreate (&Attributes, &Dev->CtlLock);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WdfWaitLockCreate failed 0x%08x", Status);
    (VOID)RtwGuardEnd ("device not added");
    return Status;
  }

  if (Wifi) {
    Status = RtwWifiPostDeviceCreate (Dev);
    if (!NT_SUCCESS (Status)) {
      (VOID)RtwGuardEnd ("device not added");
      return Status;
    }
  }

  RtwCdoCreate (Driver, Dev);
  return STATUS_SUCCESS;
}

NTSTATUS
RtwEvtPrepareHardware (
  _In_ WDFDEVICE     Device,
  _In_ WDFCMRESLIST  ResourcesRaw,
  _In_ WDFCMRESLIST  ResourcesTranslated
  )
{
  PRTW_DEVICE  Dev;
  NTSTATUS     Status;

  Dev = RtwGetDevice (Device);
  Status = RtwHwMap (Dev, ResourcesRaw, ResourcesTranslated);
  if (!NT_SUCCESS (Status)) {
    return Status;
  }

  RtwHwLogChipInfo (Dev);
  if (!Dev->WifiMode) {
    RtwPowerSettingRegister (Dev);
    return STATUS_SUCCESS;
  }

  // a start that waits for the hive: nothing below touches the chip before ArmRunning = 1 is on the disk
  if (Dev->GuardPending) {
    Status = RtwGuardWait (Dev);
    if (!NT_SUCCESS (Status)) {
      WdfWaitLockAcquire (Dev->CtlLock, NULL);
      RtwHwUnmap (Dev);
      WdfWaitLockRelease (Dev->CtlLock);
      RtwSaveLog (WdfDeviceGetDriver (Device));
      return Status;
    }

    Dev->GuardPending = FALSE;
  }

  // WiFiCx: firmware download and capabilities belong here (what IOCTL_RTW_PROBE does in diagnostic mode)
  Status = RtwWifiPrepareHardware (Dev);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx bring-up failed 0x%08x; the device will not start (the next start tries again)", Status);
    WdfWaitLockAcquire (Dev->CtlLock, NULL);
    RtwHwUnmap (Dev);
    WdfWaitLockRelease (Dev->CtlLock);
    (VOID)RtwGuardEnd ("bring-up failed, the chip stopped");
    RtwSaveLog (WdfDeviceGetDriver (Device));
  } else {
    RtwPowerSettingRegister (Dev);
  }

  return Status;
}

NTSTATUS
RtwEvtReleaseHardware (
  _In_ WDFDEVICE     Device,
  _In_ WDFCMRESLIST  ResourcesTranslated
  )
{
  PRTW_DEVICE  Dev;

  UNREFERENCED_PARAMETER (ResourcesTranslated);

  Dev = RtwGetDevice (Device);
  RtwPowerSettingUnregister (Dev);
  if (Dev->Wifi != NULL) {
    RtwWifiReleaseHardware (Dev);
  }

  // stops the mac layer and removes rtw89 if a control IOCTL left it running; waits for one in progress.
  // The BAR goes under the lock as well, so a later PROBE IOCTL finds no device.
  WdfWaitLockAcquire (Dev->CtlLock, NULL);
  RtwRemove (Dev);
  RtwHwUnmap (Dev);
  WdfWaitLockRelease (Dev->CtlLock);
  (VOID)RtwGuardEnd ("hardware released");
  if (Dev->WifiMode) {
    RtwSaveLog (WdfDeviceGetDriver (Device));     // the WiFiCx session's log outlives the driver
  }

  return STATUS_SUCCESS;
}

NTSTATUS
RtwEvtD0Entry (
  _In_ WDFDEVICE              Device,
  _In_ WDF_POWER_DEVICE_STATE  PreviousState
  )
{
  UNREFERENCED_PARAMETER (PreviousState);

  RtwWifiD0Entry (RtwGetDevice (Device));
  return STATUS_SUCCESS;
}

NTSTATUS
RtwEvtD0Exit (
  _In_ WDFDEVICE              Device,
  _In_ WDF_POWER_DEVICE_STATE  TargetState
  )
{
  // the guard's session ends at release of the hardware, or at shutdown in RtwEvtShutdown: the D0 exit of a shutdown
  // comes after the registry's final flush
  RtwLog ("D0 exit (WDF_POWER_DEVICE_STATE %d)", (int)TargetState);
  RtwWifiD0Exit (RtwGetDevice (Device));
  return STATUS_SUCCESS;
}

VOID
RtwEvtDeviceCleanup (
  _In_ WDFOBJECT  Object
  )
{
  PRTW_DEVICE  Dev;
  WDFQUEUE     Queue;

  Dev = RtwGetDevice ((WDFDEVICE)Object);

  // the control device keeps the driver loaded and points at this context: let running IOCTLs finish, then
  // delete it (KMDF does not delete control devices by itself)
  if (Dev->Cdo != NULL) {
    Queue = WdfDeviceGetDefaultQueue (Dev->Cdo);
    if (Queue != NULL) {
      WdfIoQueuePurgeSynchronously (Queue);
    }

    RtwGetCdo (Dev->Cdo)->Dev = NULL;
    WdfObjectDelete (Dev->Cdo);
    Dev->Cdo = NULL;
  }

  if (Dev->BusValid) {
    Dev->Bus.InterfaceDereference (Dev->Bus.Context);
    Dev->BusValid = FALSE;
  }
}
