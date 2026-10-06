// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtkbtflt: driver and device setup, PnP/power callbacks, the crash loop guard and the session log.
 *
 * The filter is added by an extension INF (AddFilter, FilterPosition = Lower) on USB\VID_13D3&PID_3572; the device
 * keeps Microsoft's bth.inf as its base driver. As a KMDF filter (WdfFdoInitSetFilter) it is not the power policy
 * owner; its PrepareHardware / D0Entry run while the START / D0 IRP travels back up, before BTHUSB gets it.
 *  - D0Entry from D3 (and the first D0 of a start, D3Final): one session (hci.c): version check, download if the
 *    controller runs its ROM (the first session of a start reads the patch file and the config from the driver's
 *    directory). From D1/D2 (BTHUSB's selective suspend, which keeps the patch): nothing. A D3 (radio off, restart
 *    of the device) was observed to lose the patch even without a USB re-enumeration.
 *  - D0Exit to D3, ReleaseHardware, the shutdown notification (a control device), a timer: the end of the crash loop
 *    guard's hold (below).
 *  - IRP_MJ_INTERNAL_DEVICE_CONTROL goes through a WDM preprocess routine (urb.c), not a queue.
 * D0Entry always returns STATUS_SUCCESS: whatever fails, BTHUSB still starts, on the ROM if need be.
 */
#include "rtkbtflt.h"

DRIVER_INITIALIZE                  DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD          FltEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE    FltEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE    FltEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY            FltEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT             FltEvtD0Exit;
EVT_WDF_OBJECT_CONTEXT_CLEANUP     FltEvtDeviceCleanup;
EVT_WDF_TIMER                      FltEvtGuardTimer;
EVT_WDF_DEVICE_SHUTDOWN_NOTIFICATION  FltEvtShutdown;

/*
 * The shutdown notification goes to a control device (as rtw89win's): IRP_MJ_SHUTDOWN reaches it before the registry's
 * last flush. (Registering the filter's own device object with IoRegisterShutdownNotification and taking the IRP in a
 * WDM preprocess routine was observed not to end the hold at a reboot.) One Bluetooth device: mFltDev is the device
 * whose hold the shutdown ends, under mFltLock.
 */
static WDFWAITLOCK  mFltLock;
static WDFDEVICE    mFltCdo;
static PFLT_DEVICE  mFltDev;

/*
 * The guard against crash loops (as rtw89win's ArmRunning / BootAttempts), values under Services\rtkbtflt\Parameters:
 *  SessionRunning  1 (flushed to disk) before a session sends anything to the device. A session that changed the
 *                  device (sent patch fragments, or selected our own configuration at a start) keeps it at 1 while
 *                  BTHUSB / bthport start on the result: until FLT_T_GUARD_HOLD after the session, or an orderly end
 *                  before that (D0 exit to D3, release of the hardware, the system's shutdown notification - at a
 *                  reboot the D0 exit comes after the registry's last flush, so it would not reach the disk). Any
 *                  other session clears it when it ends. Still 1 when the next session begins = the last one did not
 *                  end in order (a bugcheck or a power cut during it, or while BTHUSB started on the patch).
 *  BadSessions     sessions in a row that did not end in order; 0 again when one does. At FLT_GUARD_MAX the filter
 *                  passes everything through (no download, no hooks from the next start on) until it is cleared by
 *                  hand (rtkbtflt.ps1 clear: both values 0, then a restart of the device).
 *  PassThrough     1 (set by hand) = pass-through from the next start of the device on.
 * The hold is bounded (a minute), so that an unrelated bugcheck later on does not count against the filter.
 * Early in boot the hive cannot be flushed yet (STATUS_REGISTRY_IO_FAILED); the session then waits for it
 * (FltGuardBegin) and sends nothing until it can.
 * What a session that did not end left behind: SessionStage (its last stage) and LastSession (its log up to then) are
 * flushed at the stages that matter (FltProgress); the next session copies them to LastBadStage / LastBadSession
 * before writing its own. Pass-through starts write LastPassThrough only, so the logs of the real sessions stay.
 */
#define FLT_GUARD_MAX        2
#define FLT_GUARD_RETRY_MS   500

DECLARE_CONST_UNICODE_STRING (mSessionRunningName, L"SessionRunning");
DECLARE_CONST_UNICODE_STRING (mBadSessionsName, L"BadSessions");
DECLARE_CONST_UNICODE_STRING (mPassThroughName, L"PassThrough");
DECLARE_CONST_UNICODE_STRING (mSessionStageName, L"SessionStage");
DECLARE_CONST_UNICODE_STRING (mLastSessionName, L"LastSession");

static ULONG
FltRegGet (
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

static NTSTATUS
FltOpenParameters (
  _Out_ WDFKEY  *Key
  )
{
  return WdfDriverOpenParametersRegistryKey (WdfGetDriver (), KEY_READ | KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, Key);
}

/* ---- log ---- */

VOID
FltLog (
  _In_opt_ PFLT_DEVICE  Dev,
  _In_z_ _Printf_format_string_ PCSTR  Format,
  ...
  )
{
  CHAR     Line[256];
  va_list  Args;
  ULONG64  Ms;
  size_t   Left;
  PSTR     End;

  va_start (Args, Format);
  (VOID)RtlStringCbVPrintfA (Line, sizeof (Line), Format, Args);
  va_end (Args);
  DbgPrintEx (DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "rtkbtflt: %s\n", Line);
  if ((Dev == NULL) || Dev->LogFull) {
    return;
  }

  // "+<ms> <line>\r\n"; the last 32 bytes are kept for the truncation note
  Ms   = (KeQueryInterruptTime () - Dev->LogT0) / 10000;
  Left = FLT_LOG_SIZE - 32 - Dev->LogLen;
  if (!NT_SUCCESS (RtlStringCbPrintfExA (Dev->Log + Dev->LogLen, Left, &End, NULL, 0, "+%llu %s\r\n", Ms, Line))) {
    (VOID)RtlStringCbCopyA (Dev->Log + Dev->LogLen, FLT_LOG_SIZE - Dev->LogLen, "(log truncated)\r\n");
    Dev->LogLen  = (ULONG)strlen (Dev->Log);
    Dev->LogFull = TRUE;
    return;
  }

  Dev->LogLen = (ULONG)(End - Dev->Log);
}

/* Starts the text log of a session: the time (UTC) and what it is for. */
static VOID
FltLogBegin (
  _In_ PFLT_DEVICE  Dev,
  _In_ PCSTR        What
  )
{
  LARGE_INTEGER  Now;
  TIME_FIELDS    T;

  Dev->LogT0   = KeQueryInterruptTime ();
  Dev->LogLen  = 0;
  Dev->LogFull = FALSE;
  Dev->Log[0]  = '\0';
  KeQuerySystemTimePrecise (&Now);
  RtlTimeToTimeFields (&Now, &T);
  FltLog (Dev, "%04u-%02u-%02u %02u:%02u:%02u UTC: %s (rtkbtflt " FLT_VERSION ")", T.Year, T.Month, T.Day, T.Hour,
          T.Minute, T.Second, What);
}

/* Writes an ASCII string as REG_SZ. */
static VOID
FltRegSetText (
  _In_ WDFKEY            Key,
  _In_ PCUNICODE_STRING  Name,
  _In_ PCSTR             Text,
  _In_ ULONG             Len
  )
{
  PWCHAR  W;
  ULONG   i;

  W = ExAllocatePool2 (POOL_FLAG_PAGED, ((SIZE_T)Len + 1) * sizeof (WCHAR), FLT_POOL_TAG);
  if (W == NULL) {
    return;
  }

  for (i = 0; i < Len; i++) {
    W[i] = (WCHAR)(UCHAR)Text[i];
  }

  W[Len] = L'\0';
  (VOID)WdfRegistryAssignValue (Key, Name, REG_SZ, (Len + 1) * sizeof (WCHAR), W);
  ExFreePoolWithTag (W, FLT_POOL_TAG);
}

/*
 * The stage a session has reached: SessionStage, and its log so far as LastSession. Flush = commit it to the disk now
 * (before the steps a bugcheck could follow), so that the next session can tell where this one stopped.
 */
VOID
FltProgress (
  _In_ PFLT_DEVICE  Dev,
  _In_z_ PCSTR      Stage,
  _In_ BOOLEAN      Flush
  )
{
  WDFKEY  Key;

  if (!NT_SUCCESS (FltOpenParameters (&Key))) {
    return;
  }

  FltRegSetText (Key, &mSessionStageName, Stage, (ULONG)strlen (Stage));
  FltRegSetText (Key, &mLastSessionName, Dev->Log, Dev->LogLen);
  if (Flush) {
    (VOID)ZwFlushKey (WdfRegistryWdmGetHandle (Key));
  }

  WdfRegistryClose (Key);
}

/*
 * The result of a session, in Parameters: LastSession (the text log), LastDownload (the same, for a session that sent
 * patch fragments), LastResult, LastHciRev / LastLmpSubver (the controller's version at the end, 0 = unknown),
 * Downloads (patches taken, in total), SessionStage. Flushed: if BTHUSB's start on the patch goes wrong (Held), this
 * is what the next session finds.
 */
VOID
FltSaveSession (
  _In_ PFLT_DEVICE  Dev,
  _In_ FLT_RESULT   Result,
  _In_ BOOLEAN      Downloaded,
  _In_ USHORT       HciRev,
  _In_ USHORT       LmpSubver,
  _In_ BOOLEAN      Held
  )
{
  DECLARE_CONST_UNICODE_STRING (LastDownload, L"LastDownload");
  DECLARE_CONST_UNICODE_STRING (LastResult, L"LastResult");
  DECLARE_CONST_UNICODE_STRING (LastHciRev, L"LastHciRev");
  DECLARE_CONST_UNICODE_STRING (LastLmpSubver, L"LastLmpSubver");
  DECLARE_CONST_UNICODE_STRING (Downloads, L"Downloads");
  CHAR    Stage[96];
  WDFKEY  Key;

  if (Held) {
    FltLog (Dev, "guard: SessionRunning stays 1 for up to %u s while BTHUSB starts", FLT_T_GUARD_HOLD / 1000);
  }

  FltLog (Dev, "result %u", (ULONG)Result);
  if (!NT_SUCCESS (FltOpenParameters (&Key))) {
    return;
  }

  (VOID)RtlStringCbPrintfA (Stage, sizeof (Stage), "ended, result %u%s", (ULONG)Result,
                            Held ? "; guard held while BTHUSB starts" : "");
  FltRegSetText (Key, &mSessionStageName, Stage, (ULONG)strlen (Stage));
  FltRegSetText (Key, &mLastSessionName, Dev->Log, Dev->LogLen);
  if (Downloaded) {
    FltRegSetText (Key, &LastDownload, Dev->Log, Dev->LogLen);
  }

  (VOID)WdfRegistryAssignULong (Key, &LastResult, (ULONG)Result);
  (VOID)WdfRegistryAssignULong (Key, &LastHciRev, HciRev);
  (VOID)WdfRegistryAssignULong (Key, &LastLmpSubver, LmpSubver);
  if (Result == FltResultPatched) {
    (VOID)WdfRegistryAssignULong (Key, &Downloads, FltRegGet (Key, &Downloads) + 1);
  }

  (VOID)ZwFlushKey (WdfRegistryWdmGetHandle (Key));
  WdfRegistryClose (Key);
}

/* A start or D3 -> D0 in pass-through (PassThrough = 1, the guard tripped): LastPassThrough only. */
VOID
FltSavePassThrough (
  _In_ PFLT_DEVICE  Dev
  )
{
  DECLARE_CONST_UNICODE_STRING (LastPassThrough, L"LastPassThrough");
  WDFKEY  Key;

  if (NT_SUCCESS (FltOpenParameters (&Key))) {
    FltRegSetText (Key, &LastPassThrough, Dev->Log, Dev->LogLen);
    WdfRegistryClose (Key);
  }
}

/* ---- the guard ---- */

/* Writes a value and flushes the key. Every flush here follows a change, so success means it is on the disk. */
static NTSTATUS
FltRegSetFlushed (
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
 * The last session did not end in order: its stage and log go to LastBadStage / LastBadSession (in memory, flushed
 * with the guard's values), before this session overwrites them.
 */
static VOID
FltNoteBadSession (
  _In_opt_ PFLT_DEVICE  Dev,
  _In_ WDFKEY           Key
  )
{
  DECLARE_CONST_UNICODE_STRING (LastBadStage, L"LastBadStage");
  DECLARE_CONST_UNICODE_STRING (LastBadSession, L"LastBadSession");
  UNICODE_STRING  S;
  WCHAR           Stage[128];
  WDFMEMORY       Mem;
  ULONG           Type;

  RtlInitEmptyUnicodeString (&S, Stage, sizeof (Stage));
  if (NT_SUCCESS (WdfRegistryQueryUnicodeString (Key, &mSessionStageName, NULL, &S))) {
    (VOID)WdfRegistryAssignUnicodeString (Key, &LastBadStage, &S);
  } else {
    S.Length = 0;
  }

  FltLog (Dev, "guard: the last session did not end in order (a bugcheck or a power cut?); its stage: %wZ "
          "(log kept as LastBadSession)", &S);
  if (NT_SUCCESS (WdfRegistryQueryMemory (Key, &mLastSessionName, PagedPool, WDF_NO_OBJECT_ATTRIBUTES, &Mem, &Type))) {
    if (Type == REG_SZ) {
      (VOID)WdfRegistryAssignMemory (Key, &LastBadSession, REG_SZ, Mem, NULL);
    }

    WdfObjectDelete (Mem);
  }
}

/* One try: STATUS_SUCCESS (SessionRunning = 1 on the disk), STATUS_CANCELLED (tripped), or the flush's failure. */
static NTSTATUS
FltGuardTry (
  _In_  PFLT_DEVICE  Dev,
  _In_  WDFKEY       Key,
  _Out_ PULONG       Bad
  )
{
  NTSTATUS  Status;

  *Bad = FltRegGet (Key, &mBadSessionsName);
  if (FltRegGet (Key, &mSessionRunningName) != 0) {
    // both change in memory, then reach the disk together; a retry reads them consistent
    FltNoteBadSession (Dev, Key);
    (*Bad)++;
    Status = WdfRegistryAssignULong (Key, &mBadSessionsName, *Bad);
    if (NT_SUCCESS (Status)) {
      Status = WdfRegistryAssignULong (Key, &mSessionRunningName, 0);
    }

    if (NT_SUCCESS (Status)) {
      Status = ZwFlushKey (WdfRegistryWdmGetHandle (Key));
    }

    if (!NT_SUCCESS (Status)) {
      return Status;
    }

    FltLog (Dev, "guard: BadSessions = %u", *Bad);
  }

  if (*Bad >= FLT_GUARD_MAX) {
    return STATUS_CANCELLED;
  }

  Status = FltRegSetFlushed (Key, &mSessionRunningName, 1);
  if (!NT_SUCCESS (Status)) {
    // not on the disk: back to 0 in memory, so a retry does not count a session that never ran
    (VOID)WdfRegistryAssignULong (Key, &mSessionRunningName, 0);
  }

  return Status;
}

/*
 * Records the start of a session before anything is sent to the device, waiting up to WaitMs for the hive to become
 * writable (every FLT_GUARD_RETRY_MS). STATUS_SUCCESS: go on. STATUS_CANCELLED: the guard tripped. Anything else:
 * not recorded, send nothing.
 */
NTSTATUS
FltGuardBegin (
  _In_ PFLT_DEVICE  Dev,
  _In_ ULONG        WaitMs
  )
{
  LARGE_INTEGER  Delay;
  WDFKEY         Key;
  ULONG64        Start;
  ULONG          Tries;
  ULONG          Bad;
  NTSTATUS       Status;

  Status = FltOpenParameters (&Key);
  if (!NT_SUCCESS (Status)) {
    FltLog (Dev, "guard: Parameters key not opened (0x%08x)", Status);
    return Status;
  }

  Start          = KeQueryInterruptTime ();
  Delay.QuadPart = -10000LL * FLT_GUARD_RETRY_MS;
  Bad            = 0;
  for (Tries = 1; ; Tries++) {
    Status = FltGuardTry (Dev, Key, &Bad);
    if (NT_SUCCESS (Status) || (Status == STATUS_CANCELLED) ||
        (KeQueryInterruptTime () - Start >= (ULONG64)WaitMs * 10000)) {
      break;
    }

    KeDelayExecutionThread (KernelMode, FALSE, &Delay);
  }

  WdfRegistryClose (Key);
  if (NT_SUCCESS (Status)) {
    FltLog (Dev, "guard: SessionRunning = 1 (BadSessions %u; %u tries, %llu ms)", Bad, Tries,
            (KeQueryInterruptTime () - Start) / 10000);
  } else if (Status == STATUS_CANCELLED) {
    FltLog (Dev, "guard: tripped (BadSessions = %u): pass-through until cleared (rtkbtflt.ps1 clear)", Bad);
  } else {
    FltLog (Dev, "guard: the registry is not writable after %llu ms (0x%08x): nothing sent this time",
            (KeQueryInterruptTime () - Start) / 10000, Status);
  }

  return Status;
}

/* The session ended in order (whatever its result): SessionRunning and BadSessions back to 0, on the disk. */
VOID
FltGuardEnd (
  _In_opt_ PFLT_DEVICE  Dev
  )
{
  WDFKEY    Key;
  NTSTATUS  Status;

  Status = FltOpenParameters (&Key);
  if (NT_SUCCESS (Status)) {
    Status = WdfRegistryAssignULong (Key, &mBadSessionsName, 0);
    if (NT_SUCCESS (Status)) {
      Status = FltRegSetFlushed (Key, &mSessionRunningName, 0);
    }

    WdfRegistryClose (Key);
  }

  if (!NT_SUCCESS (Status)) {
    FltLog (Dev, "guard: SessionRunning not cleared (0x%08x)", Status);
  }
}

/*
 * A session changed the device (patch fragments sent, or our own configuration selected): SessionRunning stays 1
 * while BTHUSB and bthport start on it. FltGuardRelease ends the hold.
 */
VOID
FltGuardHold (
  _In_ PFLT_DEVICE  Dev
  )
{
  InterlockedExchange (&Dev->BthSelects, 0);
  InterlockedExchange (&Dev->BthEvents, 0);
  Dev->GuardHeldAt = KeQueryInterruptTime ();
  InterlockedExchange (&Dev->GuardHeld, 1);
  (VOID)WdfTimerStart (Dev->GuardTimer, WDF_REL_TIMEOUT_IN_MS (FLT_T_GUARD_HOLD));
}

/* Ends a hold, once: the guard's values back to 0 on the disk, and LastGuardRelease = why, when, what BTHUSB did. */
static VOID
FltGuardReleaseNow (
  _In_ PFLT_DEVICE  Dev,
  _In_z_ PCSTR      Why
  )
{
  DECLARE_CONST_UNICODE_STRING (LastGuardRelease, L"LastGuardRelease");
  LARGE_INTEGER  Now;
  TIME_FIELDS    T;
  CHAR           Text[192];
  WDFKEY         Key;

  if (InterlockedExchange (&Dev->GuardHeld, 0) == 0) {
    return;
  }

  KeQuerySystemTimePrecise (&Now);
  RtlTimeToTimeFields (&Now, &T);
  (VOID)RtlStringCbPrintfA (Text, sizeof (Text), "%04u-%02u-%02u %02u:%02u:%02u UTC: %s, %llu ms after the session; "
                            "BTHUSB meanwhile: %ld SELECT_CONFIGURATION, %ld event reads with data", T.Year, T.Month,
                            T.Day, T.Hour, T.Minute, T.Second, Why, (KeQueryInterruptTime () - Dev->GuardHeldAt) / 10000,
                            Dev->BthSelects, Dev->BthEvents);
  FltLog (NULL, "guard released: %s", Text);
  if (NT_SUCCESS (FltOpenParameters (&Key))) {
    FltRegSetText (Key, &LastGuardRelease, Text, (ULONG)strlen (Text));
    WdfRegistryClose (Key);
  }

  FltGuardEnd (NULL);             // flushes LastGuardRelease too (the same hive)
}

/* Ends a hold from anywhere but the timer's own callback (PASSIVE_LEVEL): the timer is stopped first. */
VOID
FltGuardRelease (
  _In_ PFLT_DEVICE  Dev,
  _In_z_ PCSTR      Why
  )
{
  if (Dev->GuardTimer != NULL) {
    (VOID)WdfTimerStop (Dev->GuardTimer, TRUE);
  }

  FltGuardReleaseNow (Dev, Why);
}

/* FLT_T_GUARD_HOLD after the session (PASSIVE_LEVEL timer): BTHUSB has had its start on the patch. */
VOID
FltEvtGuardTimer (
  _In_ WDFTIMER  Timer
  )
{
  FltGuardReleaseNow (FltGetDevice ((WDFDEVICE)WdfTimerGetParentObject (Timer)), "hold time over");
}

/*
 * The system shuts down or restarts (IRP_MJ_SHUTDOWN to the control device; PASSIVE): a hold ends in order. This comes
 * before the registry's last flush; the D0 exit of the shutdown comes after it.
 */
VOID
FltEvtShutdown (
  _In_ WDFDEVICE  Device
  )
{
  UNREFERENCED_PARAMETER (Device);

  WdfWaitLockAcquire (mFltLock, NULL);
  if (mFltDev != NULL) {
    FltGuardRelease (mFltDev, "system shutdown");
  }

  WdfWaitLockRelease (mFltLock);
}

/* The control device for the shutdown notification (administrators and SYSTEM only, no I/O). Deleted with the
   device (FltEvtDeviceCleanup). FALSE: none, a reboot during a hold then counts as a session that did not end. */
static BOOLEAN
FltCdoCreate (
  _In_ PFLT_DEVICE  Dev
  )
{
  DECLARE_CONST_UNICODE_STRING (Sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
  DECLARE_CONST_UNICODE_STRING (Name, L"\\Device\\rtkbtflt");
  PWDFDEVICE_INIT  Init;
  WDFDEVICE        Cdo;
  NTSTATUS         Status;

  if ((mFltLock == NULL) || (mFltCdo != NULL)) {
    return FALSE;                 // no lock, or a second device (not expected): the first one keeps the notification
  }

  Init = WdfControlDeviceInitAllocate (WdfGetDriver (), &Sddl);
  if (Init == NULL) {
    return FALSE;
  }

  Cdo = NULL;
  WdfDeviceInitSetCharacteristics (Init, FILE_DEVICE_SECURE_OPEN, TRUE);
  WdfControlDeviceInitSetShutdownNotification (Init, FltEvtShutdown, WdfDeviceShutdown);
  Status = WdfDeviceInitAssignName (Init, &Name);
  if (NT_SUCCESS (Status)) {
    Status = WdfDeviceCreate (&Init, WDF_NO_OBJECT_ATTRIBUTES, &Cdo);
  }

  if (!NT_SUCCESS (Status)) {
    FltLog (NULL, "control device not created (0x%08x): no shutdown notification", Status);
    WdfDeviceInitFree (Init);
    return FALSE;
  }

  WdfControlFinishInitializing (Cdo);
  WdfWaitLockAcquire (mFltLock, NULL);
  mFltCdo = Cdo;
  mFltDev = Dev;
  WdfWaitLockRelease (mFltLock);
  return TRUE;
}

/* AddDevice: the mode of this device's life (until its next start). */
static FLT_MODE
FltDecideMode (
  _Out_writes_z_ (NoteLen) PSTR  Note,
  _In_ size_t                    NoteLen
  )
{
  WDFKEY    Key;
  ULONG     Bad;
  NTSTATUS  Status;

  (VOID)RtlStringCbCopyA (Note, NoteLen, "normal");
  Status = FltOpenParameters (&Key);
  if (!NT_SUCCESS (Status)) {
    // the guard could not record anything: never download without it
    (VOID)RtlStringCbPrintfA (Note, NoteLen, "pass-through: Parameters key not opened (0x%08x)", Status);
    return FltModeBroken;
  }

  if (FltRegGet (Key, &mPassThroughName) != 0) {
    WdfRegistryClose (Key);
    (VOID)RtlStringCbCopyA (Note, NoteLen, "pass-through: Parameters\\PassThrough = 1");
    return FltModeUser;
  }

  // a tripped guard needs no writable hive to tell
  Bad = FltRegGet (Key, &mBadSessionsName) + ((FltRegGet (Key, &mSessionRunningName) != 0) ? 1 : 0);
  if (Bad >= FLT_GUARD_MAX) {
    if (FltRegGet (Key, &mSessionRunningName) != 0) {
      FltNoteBadSession (NULL, Key);
    }

    (VOID)WdfRegistryAssignULong (Key, &mBadSessionsName, Bad);
    (VOID)WdfRegistryAssignULong (Key, &mSessionRunningName, 0);
    WdfRegistryClose (Key);
    (VOID)RtlStringCbPrintfA (Note, NoteLen, "pass-through: guard tripped (BadSessions = %u), rtkbtflt.ps1 clear", Bad);
    return FltModeGuard;
  }

  WdfRegistryClose (Key);
  return FltModeNormal;
}

/* ---- driver and device ---- */

NTSTATUS
DriverEntry (
  _In_ PDRIVER_OBJECT   DriverObject,
  _In_ PUNICODE_STRING  RegistryPath
  )
{
  DECLARE_CONST_UNICODE_STRING (VersionName, L"DriverVersion");
  DECLARE_CONST_UNICODE_STRING (Version, L"" FLT_VERSION);
  WDF_DRIVER_CONFIG  Config;
  WDFDRIVER          Driver;
  WDFKEY             Key;
  NTSTATUS           Status;

  FltLog (NULL, "rtkbtflt " FLT_VERSION " loaded");
  WDF_DRIVER_CONFIG_INIT (&Config, FltEvtDeviceAdd);
  Config.DriverPoolTag = FLT_POOL_TAG;
  Status = WdfDriverCreate (DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
  if (!NT_SUCCESS (Status)) {
    FltLog (NULL, "WdfDriverCreate failed 0x%08x", Status);
    return Status;
  }

  // for the shutdown notification's control device; without it there is none (FltCdoCreate)
  if (!NT_SUCCESS (WdfWaitLockCreate (WDF_NO_OBJECT_ATTRIBUTES, &mFltLock))) {
    mFltLock = NULL;
  }

  // the image that runs (install-rtkbtflt.ps1 compares it with the package)
  if (NT_SUCCESS (WdfDriverOpenParametersRegistryKey (Driver, KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
    (VOID)WdfRegistryAssignUnicodeString (Key, &VersionName, &Version);
    WdfRegistryClose (Key);
  }

  return STATUS_SUCCESS;
}

NTSTATUS
FltEvtDeviceAdd (
  _In_    WDFDRIVER        Driver,
  _Inout_ PWDFDEVICE_INIT  DeviceInit
  )
{
  WDF_PNPPOWER_EVENT_CALLBACKS  Pnp;
  WDF_OBJECT_ATTRIBUTES         Attr;
  WDF_TIMER_CONFIG              Tc;
  WDFDEVICE                     Device;
  PFLT_DEVICE                   Dev;
  FLT_MODE                      Mode;
  CHAR                          Note[96];
  NTSTATUS                      Status;

  UNREFERENCED_PARAMETER (Driver);

  Mode = FltDecideMode (Note, sizeof (Note));
  WdfFdoInitSetFilter (DeviceInit);
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT (&Pnp);
  Pnp.EvtDevicePrepareHardware = FltEvtPrepareHardware;
  Pnp.EvtDeviceReleaseHardware = FltEvtReleaseHardware;
  Pnp.EvtDeviceD0Entry         = FltEvtD0Entry;
  Pnp.EvtDeviceD0Exit          = FltEvtD0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks (DeviceInit, &Pnp);

  // the URB hooks only where a session can run; in pass-through every IRP goes down untouched (KMDF forwards it)
  if (Mode == FltModeNormal) {
    Status = WdfDeviceInitAssignWdmIrpPreprocessCallback (DeviceInit, FltEvtWdmInternalIoctl,
                                                          IRP_MJ_INTERNAL_DEVICE_CONTROL, NULL, 0);
    if (!NT_SUCCESS (Status)) {
      (VOID)RtlStringCbPrintfA (Note, sizeof (Note), "pass-through: URB hook not set (0x%08x)", Status);
      Mode = FltModeBroken;
    }
  }

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attr, FLT_DEVICE);
  Attr.EvtCleanupCallback = FltEvtDeviceCleanup;
  Status = WdfDeviceCreate (&DeviceInit, &Attr, &Device);
  if (!NT_SUCCESS (Status)) {
    FltLog (NULL, "WdfDeviceCreate failed 0x%08x", Status);
    return Status;
  }

  Dev = FltGetDevice (Device);
  Dev->Device         = Device;
  Dev->Lower          = WdfDeviceWdmGetAttachedDevice (Device);
  Dev->Target         = WdfDeviceGetIoTarget (Device);
  Dev->EventInterface = -1;
  KeInitializeSpinLock (&Dev->Lock);
  KeInitializeEvent (&Dev->DataEvent, SynchronizationEvent, FALSE);
  KeInitializeEvent (&Dev->ReadIdle, NotificationEvent, TRUE);

  // our interrupt read: one request and the URB's memory, made once and reused by every session
  if (Mode == FltModeNormal) {
    WDF_OBJECT_ATTRIBUTES_INIT (&Attr);
    Attr.ParentObject = Device;
    Status = WdfRequestCreate (&Attr, Dev->Target, &Dev->ReadRequest);
    if (NT_SUCCESS (Status)) {
      WDF_OBJECT_ATTRIBUTES_INIT (&Attr);
      Attr.ParentObject = Device;
      Status = WdfMemoryCreatePreallocated (&Attr, &Dev->ReadUrb, sizeof (Dev->ReadUrb), &Dev->ReadUrbMemory);
    }

    // the guard's hold timer: one shot, its callback at PASSIVE_LEVEL (it writes the registry)
    if (NT_SUCCESS (Status)) {
      WDF_TIMER_CONFIG_INIT (&Tc, FltEvtGuardTimer);
      Tc.AutomaticSerialization = FALSE;
      WDF_OBJECT_ATTRIBUTES_INIT (&Attr);
      Attr.ParentObject   = Device;
      Attr.ExecutionLevel = WdfExecutionLevelPassive;
      Status = WdfTimerCreate (&Tc, &Attr, &Dev->GuardTimer);
    }

    if (!NT_SUCCESS (Status)) {
      (VOID)RtlStringCbPrintfA (Note, sizeof (Note), "pass-through: read request or timer not created (0x%08x)", Status);
      Mode = FltModeBroken;
    }
  }

  // a reboot or shutdown ends the guard's hold in order (FltEvtShutdown)
  if (Mode == FltModeNormal) {
    Dev->ShutdownHook = FltCdoCreate (Dev);
  }

  Dev->Mode = Mode;
  (VOID)RtlStringCbCopyA (Dev->ModeNote, sizeof (Dev->ModeNote), Note);
  FltLog (NULL, "device added: %s", Note);
  return STATUS_SUCCESS;
}

NTSTATUS
FltEvtPrepareHardware (
  _In_ WDFDEVICE     Device,
  _In_ WDFCMRESLIST  ResourcesRaw,
  _In_ WDFCMRESLIST  ResourcesTranslated
  )
{
  PFLT_DEVICE  Dev;

  UNREFERENCED_PARAMETER (ResourcesRaw);
  UNREFERENCED_PARAMETER (ResourcesTranslated);

  // the patch file is read and parsed by the first session of this start, under the guard (hci.c)
  Dev = FltGetDevice (Device);
  FltFreeFirmware (Dev);
  Dev->FwTried = FALSE;
  return STATUS_SUCCESS;
}

NTSTATUS
FltEvtReleaseHardware (
  _In_ WDFDEVICE     Device,
  _In_ WDFCMRESLIST  ResourcesTranslated
  )
{
  PFLT_DEVICE  Dev;

  UNREFERENCED_PARAMETER (ResourcesTranslated);

  Dev = FltGetDevice (Device);
  FltGuardRelease (Dev, "hardware released");
  FltFreeFirmware (Dev);
  Dev->FwTried = FALSE;
  // a later start begins unconfigured; whoever configures the device then tells us the pipe again
  InterlockedExchangePointer ((PVOID volatile *)&Dev->EventPipe, NULL);
  InterlockedExchange (&Dev->EventInterface, -1);
  return STATUS_SUCCESS;
}

NTSTATUS
FltEvtD0Entry (
  _In_ WDFDEVICE               Device,
  _In_ WDF_POWER_DEVICE_STATE  PreviousState
  )
{
  PFLT_DEVICE  Dev;

  Dev = FltGetDevice (Device);
  if ((PreviousState == WdfPowerDeviceD1) || (PreviousState == WdfPowerDeviceD2)) {
    // BTHUSB's selective suspend (after about a second idle): the controller keeps its patch
    Dev->D2Resumes++;
    return STATUS_SUCCESS;
  }

  FltLogBegin (Dev, (PreviousState == WdfPowerDeviceD3Final) ? "device start" : "D3 -> D0");
  if (Dev->Mode != FltModeNormal) {
    FltLog (Dev, "%s", Dev->ModeNote);
    FltSavePassThrough (Dev);
    return STATUS_SUCCESS;
  }

  FltSession (Dev, PreviousState == WdfPowerDeviceD3Final);
  return STATUS_SUCCESS;
}

/* Leaving D0 for D3 (radio off, restart, removal, sleep): BTHUSB stopped in order; D1/D2 (selective suspend): no. */
NTSTATUS
FltEvtD0Exit (
  _In_ WDFDEVICE               Device,
  _In_ WDF_POWER_DEVICE_STATE  TargetState
  )
{
  if ((TargetState != WdfPowerDeviceD1) && (TargetState != WdfPowerDeviceD2)) {
    FltGuardRelease (FltGetDevice (Device), "D0 exit to D3");
  }

  return STATUS_SUCCESS;
}

VOID
FltEvtDeviceCleanup (
  _In_ WDFOBJECT  Object
  )
{
  PFLT_DEVICE  Dev;
  WDFDEVICE    Cdo;

  Dev = FltGetDevice ((WDFDEVICE)Object);
  if (Dev->ShutdownHook) {
    // the control device goes with the device, so that the driver can unload
    WdfWaitLockAcquire (mFltLock, NULL);
    Cdo     = mFltCdo;
    mFltCdo = NULL;
    mFltDev = NULL;
    WdfWaitLockRelease (mFltLock);
    if (Cdo != NULL) {
      WdfObjectDelete (Cdo);
    }

    Dev->ShutdownHook = FALSE;
  }

  FltFreeFirmware (Dev);
}
