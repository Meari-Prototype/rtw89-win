// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * The Windows primitives under the Linux-side code: the rtwk_* functions of compat\include\rtw\os.h.
 * One device at a time (mDev, set by RtwOsAttach).
 *
 * Locks: rtwk_spin_lock raises to DISPATCH_LEVEL, rtwk_spin_lock_irq to the device interrupt IRQL (so it also
 * excludes our ISR); both work when the caller is already at or above that level.
 *
 * DMA: the PCIe controllers of the RK3588 are not cache coherent (ACPI _CCA 0), so all DMA memory is uncached, as
 * on Linux: common buffers with the adapter's default caching, which the HAL makes non-cached here. Observed on
 * the RK3588: that mapping takes unaligned accesses (normal non-cacheable memory, not Device memory); a
 * write-combined common buffer is refused. rtwk_dma_alloc gives one common buffer per call; the DMA heap hands out
 * small blocks (skb data) of three sizes from 1 MB common buffers. Everything is below 4 GB, because rtw89 uses
 * 32-bit DMA addresses unless the upstream bridge is an Intel or ASMedia one.
 */
#include "rtw89win.h"
#include "../compat/include/rtw/os.h"
#include "../compat/include/rtw/glue.h"

// Linux errno values (returned to the Linux side as negatives)
#define ENOENT  2
#define EIO     5
#define ENOMEM  12

C_ASSERT (sizeof (KMUTEX) <= RTWK_MUTEX_SIZE);
C_ASSERT (sizeof (KEVENT) <= RTWK_EVENT_SIZE);
C_ASSERT (sizeof (KSPIN_LOCK) == sizeof (uint64_t));

static PDRIVER_OBJECT  mDriver;
static PRTW_DEVICE     mDev;
static KIRQL           mIntIrql = DISPATCH_LEVEL;   // device IRQL while the interrupt is connected

VOID
RtwOsInit (
  _In_ PDRIVER_OBJECT  DriverObject
  )
{
  mDriver = DriverObject;
}

/* ---- memory: 64-byte aligned, with the size in front (krealloc needs it) ---- */
#define ALLOC_ALIGN  64

typedef struct {
  PVOID   Base;
  SIZE_T  Size;
} ALLOC_HDR;

void *
rtwk_alloc (
  size_t  size,
  int     zero
  )
{
  PUCHAR     Base;
  ALLOC_HDR  *Hdr;

  if (size > MAXSIZE_T - ALLOC_ALIGN - sizeof (ALLOC_HDR)) {
    return NULL;
  }

  Base = ExAllocatePool2 (
           zero ? POOL_FLAG_NON_PAGED : (POOL_FLAG_NON_PAGED | POOL_FLAG_UNINITIALIZED),
           size + ALLOC_ALIGN + sizeof (ALLOC_HDR),
           RTW_POOL_TAG
           );
  if (Base == NULL) {
    return NULL;
  }

  Hdr       = (ALLOC_HDR *)ALIGN_UP_POINTER_BY (Base + sizeof (ALLOC_HDR), ALLOC_ALIGN) - 1;
  Hdr->Base = Base;
  Hdr->Size = size;
  return Hdr + 1;
}

void
rtwk_free (
  void  *p
  )
{
  if (p != NULL) {
    ExFreePoolWithTag (((ALLOC_HDR *)p - 1)->Base, RTW_POOL_TAG);
  }
}

size_t
rtwk_alloc_size (
  void  *p
  )
{
  return (p != NULL) ? ((ALLOC_HDR *)p - 1)->Size : 0;
}

/* ---- spin locks ---- */
static uint8_t
LockAt (
  uint64_t  *lock,
  KIRQL     Level
  )
{
  KIRQL  Old;

  Old = KeGetCurrentIrql ();
  if (Old < Level) {
    KeRaiseIrql (Level, &Old);
  }

  KeAcquireSpinLockAtDpcLevel ((PKSPIN_LOCK)lock);
  return Old;
}

uint8_t
rtwk_spin_lock (
  uint64_t  *lock
  )
{
  return LockAt (lock, DISPATCH_LEVEL);
}

uint8_t
rtwk_spin_lock_irq (
  uint64_t  *lock
  )
{
  return LockAt (lock, mIntIrql);
}

void
rtwk_spin_unlock (
  uint64_t  *lock,
  uint8_t   old_irql
  )
{
  KeReleaseSpinLockFromDpcLevel ((PKSPIN_LOCK)lock);
  if (old_irql < KeGetCurrentIrql ()) {
    KeLowerIrql (old_irql);
  }
}

/* ---- mutex ---- */
void
rtwk_mutex_init (
  void  *m
  )
{
  KeInitializeMutex ((PKMUTEX)m, 0);
}

void
rtwk_mutex_lock (
  void  *m
  )
{
  KeWaitForSingleObject (m, Executive, KernelMode, FALSE, NULL);
}

int
rtwk_mutex_trylock (
  void  *m
  )
{
  LARGE_INTEGER  Zero;

  Zero.QuadPart = 0;
  return KeWaitForSingleObject (m, Executive, KernelMode, FALSE, &Zero) == STATUS_SUCCESS;
}

void
rtwk_mutex_unlock (
  void  *m
  )
{
  KeReleaseMutex ((PKMUTEX)m, FALSE);
}

int
rtwk_mutex_is_locked (
  void  *m
  )
{
  return KeReadStateMutex ((PKMUTEX)m) != 1;
}

/* ---- events ---- */
void
rtwk_event_init (
  void  *e,
  int   notification
  )
{
  KeInitializeEvent ((PKEVENT)e, notification ? NotificationEvent : SynchronizationEvent, FALSE);
}

void
rtwk_event_set (
  void  *e
  )
{
  KeSetEvent ((PKEVENT)e, IO_NO_INCREMENT, FALSE);
}

void
rtwk_event_clear (
  void  *e
  )
{
  KeClearEvent ((PKEVENT)e);
}

int
rtwk_event_state (
  void  *e
  )
{
  return KeReadStateEvent ((PKEVENT)e) != 0;
}

int
rtwk_event_wait (
  void     *e,
  int64_t  timeout_us
  )
{
  LARGE_INTEGER  Timeout;

  if (KeGetCurrentIrql () > PASSIVE_LEVEL) {
    timeout_us = 0;               // cannot block here: just test the state
  }

  if (timeout_us < 0) {
    return KeWaitForSingleObject (e, Executive, KernelMode, FALSE, NULL) == STATUS_SUCCESS;
  }

  Timeout.QuadPart = -timeout_us * 10;
  return KeWaitForSingleObject (e, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS;
}

/* ---- threads ---- */
typedef struct {
  void      (*Fn)(void *ctx);
  void      *Ctx;
  PKTHREAD  Thread;
} OS_THREAD;

static KSTART_ROUTINE  ThreadStart;

static VOID
ThreadStart (
  _In_ PVOID  Context
  )
{
  OS_THREAD  *T;

  T = Context;
  T->Fn (T->Ctx);
  PsTerminateSystemThread (STATUS_SUCCESS);
}

void *
rtwk_thread_start (
  void  (*fn)(void *ctx),
  void  *ctx
  )
{
  OBJECT_ATTRIBUTES  Oa;
  OS_THREAD          *T;
  HANDLE             Handle;
  NTSTATUS           Status;

  T = ExAllocatePool2 (POOL_FLAG_NON_PAGED, sizeof (*T), RTW_POOL_TAG);
  if (T == NULL) {
    return NULL;
  }

  T->Fn  = fn;
  T->Ctx = ctx;
  InitializeObjectAttributes (&Oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
  Status = PsCreateSystemThread (&Handle, THREAD_ALL_ACCESS, &Oa, NULL, NULL, ThreadStart, T);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("PsCreateSystemThread failed 0x%08x", Status);
    ExFreePoolWithTag (T, RTW_POOL_TAG);
    return NULL;
  }

  // the thread is running and T stays valid until rtwk_thread_join, so this cannot fail for a live handle
  Status = ObReferenceObjectByHandle (Handle, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&T->Thread, NULL);
  ZwClose (Handle);
  NT_ASSERT (NT_SUCCESS (Status));
  return T;
}

void
rtwk_thread_join (
  void  *thread
  )
{
  OS_THREAD  *T;

  T = thread;
  KeWaitForSingleObject (T->Thread, Executive, KernelMode, FALSE, NULL);
  ObDereferenceObject (T->Thread);
  ExFreePoolWithTag (T, RTW_POOL_TAG);
}

int
rtwk_current_irql (
  void
  )
{
  return KeGetCurrentIrql ();
}

/* ---- timers ---- */
typedef struct {
  KTIMER  Timer;
  KDPC    Dpc;
  void    (*Fn)(void *ctx);
  void    *Ctx;
} OS_TIMER;

C_ASSERT (sizeof (OS_TIMER) <= RTWK_TIMER_SIZE);

static KDEFERRED_ROUTINE  TimerDpc;

static VOID
TimerDpc (
  _In_ PKDPC      Dpc,
  _In_opt_ PVOID  Context,
  _In_opt_ PVOID  Arg1,
  _In_opt_ PVOID  Arg2
  )
{
  OS_TIMER  *T;

  UNREFERENCED_PARAMETER (Dpc);
  UNREFERENCED_PARAMETER (Arg1);
  UNREFERENCED_PARAMETER (Arg2);

  T = Context;
  T->Fn (T->Ctx);
}

void
rtwk_timer_init (
  void  *t,
  void  (*fn)(void *ctx),
  void  *ctx
  )
{
  OS_TIMER  *T;

  T      = t;
  T->Fn  = fn;
  T->Ctx = ctx;
  KeInitializeTimer (&T->Timer);
  KeInitializeDpc (&T->Dpc, TimerDpc, T);
}

void
rtwk_timer_arm (
  void      *t,
  uint64_t  due_us
  )
{
  OS_TIMER       *T;
  LARGE_INTEGER  Due;

  T            = t;
  Due.QuadPart = (due_us != 0) ? -(LONGLONG)due_us * 10 : -1;
  KeSetTimer (&T->Timer, Due, &T->Dpc);
}

int
rtwk_timer_cancel (
  void  *t
  )
{
  return KeCancelTimer (&((OS_TIMER *)t)->Timer);
}

void
rtwk_timer_sync (
  void
  )
{
  if (KeGetCurrentIrql () == PASSIVE_LEVEL) {
    KeFlushQueuedDpcs ();
  }
}

/* ---- time and delays ---- */
uint64_t
rtwk_time_ns (
  void
  )
{
  ULONG64  Qpc;

  return KeQueryInterruptTimePrecise (&Qpc) * 100;
}

void
rtwk_udelay (
  uint32_t  us
  )
{
  while (us > 50) {
    KeStallExecutionProcessor (50);
    us -= 50;
  }

  KeStallExecutionProcessor (us);
}

void
rtwk_usleep (
  uint64_t  us
  )
{
  PEX_TIMER      Timer;
  LARGE_INTEGER  Due;

  // short waits, and any wait above PASSIVE_LEVEL, spin like udelay (the default timer tick is 15.6 ms)
  if ((us < 200) || (KeGetCurrentIrql () > PASSIVE_LEVEL)) {
    while (us > 0) {
      ULONG  n = (us > 50000) ? 50000 : (ULONG)us;

      rtwk_udelay (n);
      us -= n;
    }

    return;
  }

  Due.QuadPart = -(LONGLONG)us * 10;
  Timer        = ExAllocateTimer (NULL, NULL, EX_TIMER_HIGH_RESOLUTION);
  if (Timer == NULL) {
    KeDelayExecutionThread (KernelMode, FALSE, &Due);
    return;
  }

  ExSetTimer (Timer, Due.QuadPart, 0, NULL);
  KeWaitForSingleObject (Timer, Executive, KernelMode, FALSE, NULL);
  ExDeleteTimer (Timer, TRUE, FALSE, NULL);
}

uint32_t
rtwk_random (
  void
  )
{
  static volatile LONG64  State;
  ULONG64                 Old;
  ULONG64                 New;

  // xorshift64*, seeded from the performance counter
  do {
    Old = (ULONG64)State;
    New = (Old != 0) ? Old : ((ULONG64)KeQueryPerformanceCounter (NULL).QuadPart | 1);
    New ^= New >> 12;
    New ^= New << 25;
    New ^= New >> 27;
  } while ((ULONG64)InterlockedCompareExchange64 (&State, (LONG64)New, (LONG64)Old) != Old);

  return (uint32_t)((New * 0x2545F4914F6CDD1DULL) >> 32);
}

/* ---- log and BUG() ---- */
void
rtwk_log (
  int         level,
  const char  *msg
  )
{
  static const PCSTR  Prefix[] = { "error: ", "warning: ", "", "" };

  RtwLog ("%s%s", Prefix[(level >= 0 && level < 4) ? level : 3], msg);
}

void
rtwk_bugcheck (
  int  line
  )
{
  // MANUALLY_INITIATED_CRASH1; parameter 1 tags it as ours, parameter 2 is the line of the BUG() in rtw89
  KeBugCheckEx (0xDEADDEAD, 'rtw8', (ULONG_PTR)line, 0, 0);
}

/* ---- PCI configuration space ---- */
uint32_t
rtwk_pci_cfg_read (
  void      *os_dev,
  uint32_t  offset,
  void      *buf,
  uint32_t  len
  )
{
  PRTW_DEVICE  Dev;

  Dev = os_dev;
  if (!Dev->BusValid) {
    return 0;
  }

  return Dev->Bus.GetBusData (Dev->Bus.Context, PCI_WHICHSPACE_CONFIG, buf, offset, len);
}

uint32_t
rtwk_pci_cfg_write (
  void        *os_dev,
  uint32_t    offset,
  const void  *buf,
  uint32_t    len
  )
{
  PRTW_DEVICE  Dev;

  Dev = os_dev;
  if (!Dev->BusValid) {
    return 0;
  }

  return Dev->Bus.SetBusData (Dev->Bus.Context, PCI_WHICHSPACE_CONFIG, (PVOID)buf, offset, len);
}

/* ---- firmware: <driver directory>\<last path component of name> ---- */
int
rtwk_firmware_load (
  const char  *name,
  void        **data,
  size_t      *size
  )
{
  const char                 *Base;
  ANSI_STRING                Ansi;
  UNICODE_STRING             Path;
  OBJECT_ATTRIBUTES          Oa;
  IO_STATUS_BLOCK            Iosb;
  FILE_STANDARD_INFORMATION  Info;
  HANDLE                     Dir;
  HANDLE                     File;
  PVOID                      Buf;
  NTSTATUS                   Status;
  int                        Ret;

  *data = NULL;
  *size = 0;
  Base  = name;
  for (const char *p = name; *p != '\0'; p++) {
    if ((*p == '/') || (*p == '\\')) {
      Base = p + 1;
    }
  }

  Status = IoGetDriverDirectory (mDriver, DriverDirectoryImage, 0, &Dir);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("firmware %s: IoGetDriverDirectory failed 0x%08x", name, Status);
    return -ENOENT;
  }

  RtlInitAnsiString (&Ansi, Base);
  Status = RtlAnsiStringToUnicodeString (&Path, &Ansi, TRUE);
  if (!NT_SUCCESS (Status)) {
    ZwClose (Dir);
    return -ENOMEM;
  }

  InitializeObjectAttributes (&Oa, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, Dir, NULL);
  Status = ZwCreateFile (
             &File,
             GENERIC_READ | SYNCHRONIZE,
             &Oa,
             &Iosb,
             NULL,
             FILE_ATTRIBUTE_NORMAL,
             FILE_SHARE_READ,
             FILE_OPEN,
             FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
             NULL,
             0
             );
  RtlFreeUnicodeString (&Path);
  ZwClose (Dir);
  if (!NT_SUCCESS (Status)) {
    // rtw89 tries several firmware names; a missing one is not an error by itself
    RtwLog ("firmware %s: not found (0x%08x)", Base, Status);
    return -ENOENT;
  }

  Ret    = -EIO;
  Status = ZwQueryInformationFile (File, &Iosb, &Info, sizeof (Info), FileStandardInformation);
  if (NT_SUCCESS (Status) && (Info.EndOfFile.QuadPart > 0) && (Info.EndOfFile.QuadPart < 0x1000000)) {
    Buf = rtwk_alloc ((size_t)Info.EndOfFile.QuadPart, 0);
    if (Buf == NULL) {
      Ret = -ENOMEM;
    } else {
      Status = ZwReadFile (File, NULL, NULL, NULL, &Iosb, Buf, Info.EndOfFile.LowPart, NULL, NULL);
      if (NT_SUCCESS (Status) && (Iosb.Information == Info.EndOfFile.LowPart)) {
        *data = Buf;
        *size = Info.EndOfFile.LowPart;
        Ret   = 0;
      } else {
        rtwk_free (Buf);
      }
    }
  }

  ZwClose (File);
  if (Ret != 0) {
    RtwLog ("firmware %s: read failed (0x%08x, %d)", Base, Status, Ret);
  }

  return Ret;
}

/* ---- DMA ---- */
#define DMA_MAX_REGIONS   128
#define DMA_CHUNK         0x100000
#define DMA_HEAP_CHUNKS   48            // at most 48 MB of heap
#define DMA_CLASS_NONE    0xFF          // a region from rtwk_dma_alloc

typedef struct {
  PUCHAR   Va;
  ULONG64  La;
  ULONG    Len;
  UCHAR    Class;
} DMA_REGION;

typedef struct {
  ULONG  Size;
  PVOID  Free;                          // free blocks, linked through their first pointer
  ULONG  FreeCount;
  ULONG  Used;
} DMA_CLASS;

// skb data: H2C and management frames, data frames up to 2 KB, RX buffers (11.5 KB + headroom)
static DMA_CLASS          mClass[] = { { 512 }, { 2048 }, { 16384 } };
static DMA_REGION         mRegion[DMA_MAX_REGIONS];
static ULONG              mRegions;
static ULONG              mHeapChunks;
static KSPIN_LOCK         mDmaLock;
static FAST_MUTEX         mGrowLock;    // one grower at a time (PASSIVE_LEVEL)
static PALLOCATE_COMMON_BUFFER_WITH_BOUNDS  mAllocWithBounds;

static PVOID
AllocCommon (
  _In_ ULONG     Len,
  _Out_ ULONG64  *La
  )
{
  PHYSICAL_ADDRESS  Max;
  PHYSICAL_ADDRESS  Logical;
  PVOID             Va;

  *La          = 0;
  Max.QuadPart = 0xFFFFFFFF;
  // default caching: decided by the HAL from the device's coherency (non-cached here)
  Va = mAllocWithBounds (mDev->Dma, NULL, &Max, Len, 0, NULL, MM_ANY_NODE_OK, &Logical);
  if (Va == NULL) {
    RtwLog ("AllocateCommonBufferWithBounds(%u, < 4 GB) failed", Len);
    return NULL;
  }

  RtlZeroMemory (Va, Len);
  *La = (ULONG64)Logical.QuadPart;
  return Va;
}

static VOID
FreeCommon (
  _In_ DMA_REGION  *R
  )
{
  PHYSICAL_ADDRESS  Logical;

  Logical.QuadPart = (LONGLONG)R->La;
  mDev->Dma->DmaOperations->FreeCommonBuffer (mDev->Dma, R->Len, Logical, R->Va, FALSE);
}

static BOOLEAN
AddRegion (
  _In_ PVOID    Va,
  _In_ ULONG64  La,
  _In_ ULONG    Len,
  _In_ UCHAR    Class
  )
{
  KIRQL    Irql;
  BOOLEAN  Ok;

  KeAcquireSpinLock (&mDmaLock, &Irql);
  Ok = mRegions < DMA_MAX_REGIONS;
  if (Ok) {
    mRegion[mRegions].Va    = Va;
    mRegion[mRegions].La    = La;
    mRegion[mRegions].Len   = Len;
    mRegion[mRegions].Class = Class;
    mRegions++;
  }

  KeReleaseSpinLock (&mDmaLock, Irql);
  return Ok;
}

/* caller holds mDmaLock */
static DMA_REGION *
FindRegion (
  _In_ const VOID  *Va
  )
{
  ULONG  i;

  for (i = 0; i < mRegions; i++) {
    if (((const UCHAR *)Va >= mRegion[i].Va) && ((const UCHAR *)Va < mRegion[i].Va + mRegion[i].Len)) {
      return &mRegion[i];
    }
  }

  return NULL;
}

void *
rtwk_dma_alloc (
  size_t    size,
  uint64_t  *dma
  )
{
  PVOID    Va;
  ULONG64  La;

  *dma = 0;
  if ((mDev == NULL) || (size == 0) || (size > MAXULONG) || (KeGetCurrentIrql () != PASSIVE_LEVEL)) {
    return NULL;
  }

  Va = AllocCommon ((ULONG)size, &La);
  if (Va == NULL) {
    return NULL;
  }

  if (!AddRegion (Va, La, (ULONG)size, DMA_CLASS_NONE)) {
    DMA_REGION  R = { Va, La, (ULONG)size, DMA_CLASS_NONE };

    RtwLog ("DMA region table full");
    FreeCommon (&R);
    return NULL;
  }

  *dma = La;
  return Va;
}

void
rtwk_dma_free (
  void  *va
  )
{
  DMA_REGION  R;
  DMA_REGION  *F;
  KIRQL       Irql;

  KeAcquireSpinLock (&mDmaLock, &Irql);
  F = FindRegion (va);
  if ((F == NULL) || (F->Va != va) || (F->Class != DMA_CLASS_NONE)) {
    KeReleaseSpinLock (&mDmaLock, Irql);
    RtwLog ("rtwk_dma_free: %p is not a DMA buffer", va);
    return;
  }

  R  = *F;
  *F = mRegion[--mRegions];
  KeReleaseSpinLock (&mDmaLock, Irql);
  FreeCommon (&R);
}

/* Adds one 1 MB chunk to a size class. PASSIVE_LEVEL. */
static BOOLEAN
GrowClass (
  _In_ UCHAR  C
  )
{
  PUCHAR   Va;
  ULONG64  La;
  ULONG    Off;
  KIRQL    Irql;

  ExAcquireFastMutex (&mGrowLock);
  if ((mClass[C].FreeCount != 0) || (mHeapChunks >= DMA_HEAP_CHUNKS)) {
    // someone else grew it meanwhile, or the heap is at its limit
    ExReleaseFastMutex (&mGrowLock);
    return mClass[C].FreeCount != 0;
  }

  Va = AllocCommon (DMA_CHUNK, &La);
  if ((Va == NULL) || !AddRegion (Va, La, DMA_CHUNK, C)) {
    if (Va != NULL) {
      DMA_REGION  R = { Va, La, DMA_CHUNK, C };

      FreeCommon (&R);
    }

    ExReleaseFastMutex (&mGrowLock);
    return FALSE;
  }

  mHeapChunks++;
  KeAcquireSpinLock (&mDmaLock, &Irql);
  for (Off = 0; Off < DMA_CHUNK; Off += mClass[C].Size) {
    *(PVOID *)(Va + Off) = mClass[C].Free;
    mClass[C].Free       = Va + Off;
    mClass[C].FreeCount++;
  }

  KeReleaseSpinLock (&mDmaLock, Irql);
  ExReleaseFastMutex (&mGrowLock);
  return TRUE;
}

void *
rtwk_dma_heap_alloc (
  size_t  size
  )
{
  UCHAR  C;
  PVOID  P;
  KIRQL  Irql;

  if (mDev == NULL) {
    return NULL;
  }

  for (C = 0; (C < ARRAYSIZE (mClass)) && (size > mClass[C].Size); C++) {
  }

  if (C == ARRAYSIZE (mClass)) {
    return NULL;
  }

  for (;;) {
    KeAcquireSpinLock (&mDmaLock, &Irql);
    P = mClass[C].Free;
    if (P != NULL) {
      mClass[C].Free = *(PVOID *)P;
      mClass[C].FreeCount--;
      mClass[C].Used++;
    }

    KeReleaseSpinLock (&mDmaLock, Irql);
    if (P != NULL) {
      return P;
    }

    // empty: only PASSIVE_LEVEL callers can add memory; the others fall back to pool memory (and bounce buffers)
    if ((KeGetCurrentIrql () != PASSIVE_LEVEL) || !GrowClass (C)) {
      return NULL;
    }
  }
}

void
rtwk_dma_heap_free (
  void  *va
  )
{
  DMA_REGION  *R;
  KIRQL       Irql;
  BOOLEAN     Ok;

  if (va == NULL) {
    return;
  }

  KeAcquireSpinLock (&mDmaLock, &Irql);
  R = FindRegion (va);
  if ((R != NULL) && (R->Class != DMA_CLASS_NONE)) {
    *(PVOID *)va        = mClass[R->Class].Free;
    mClass[R->Class].Free = va;
    mClass[R->Class].FreeCount++;
    mClass[R->Class].Used--;
  }

  Ok = (R != NULL) && (R->Class != DMA_CLASS_NONE);
  KeReleaseSpinLock (&mDmaLock, Irql);
  if (!Ok) {
    RtwLog ("rtwk_dma_heap_free: %p is not a heap block", va);
  }
}

int
rtwk_dma_lookup (
  const void  *va,
  uint64_t    *dma
  )
{
  DMA_REGION  *R;
  KIRQL       Irql;

  KeAcquireSpinLock (&mDmaLock, &Irql);
  R = FindRegion (va);
  if (R != NULL) {
    *dma = R->La + (ULONG64)((const UCHAR *)va - R->Va);
  }

  KeReleaseSpinLock (&mDmaLock, Irql);
  return (R != NULL) ? 0 : -1;
}

void
rtwk_dma_sync (
  const void  *va,
  size_t      size,
  int         to_device
  )
{
  // uncached memory: nothing to write back or invalidate; the barriers are in readl/writel
  UNREFERENCED_PARAMETER (va);
  UNREFERENCED_PARAMETER (size);
  UNREFERENCED_PARAMETER (to_device);
}

/* Frees the heap if every block came back; otherwise leaves it (and logs), since freeing it would let a leaked
   skb point at memory the device or the next allocation owns. */
static VOID
DmaRelease (
  VOID
  )
{
  ULONG  Used;
  ULONG  i;

  Used = 0;
  for (i = 0; i < ARRAYSIZE (mClass); i++) {
    Used += mClass[i].Used;
  }

  for (i = 0; i < mRegions; i++) {
    if (mRegion[i].Class == DMA_CLASS_NONE) {
      RtwLog ("DMA buffer %p (%u bytes) was not freed", mRegion[i].Va, mRegion[i].Len);
    }
  }

  if (Used != 0) {
    RtwLog ("%u DMA heap blocks still in use; keeping %u MB of DMA memory", Used, mHeapChunks);
    return;
  }

  for (i = 0; i < mRegions; i++) {
    FreeCommon (&mRegion[i]);
  }

  RtlZeroMemory (mRegion, sizeof (mRegion));
  mRegions    = 0;
  mHeapChunks = 0;
  for (i = 0; i < ARRAYSIZE (mClass); i++) {
    mClass[i].Free      = NULL;
    mClass[i].FreeCount = 0;
  }
}

/* ---- interrupt ---- */
static KSERVICE_ROUTINE    Isr;
static KDEFERRED_ROUTINE   IsrDpc;
static KSYNCHRONIZE_ROUTINE  SyncNothing;

static BOOLEAN
Isr (
  _In_ PKINTERRUPT  Interrupt,
  _In_opt_ PVOID    Context
  )
{
  PRTW_DEVICE  Dev;
  int          Ret;

  UNREFERENCED_PARAMETER (Interrupt);

  Dev = Context;
  Ret = rtw_linux_isr ();
  if (Ret == 2) {
    KeInsertQueueDpc (&Dev->IsrDpc, NULL, NULL);
  }

  return Ret != 0;
}

static VOID
IsrDpc (
  _In_ PKDPC      Dpc,
  _In_opt_ PVOID  Context,
  _In_opt_ PVOID  Arg1,
  _In_opt_ PVOID  Arg2
  )
{
  UNREFERENCED_PARAMETER (Dpc);
  UNREFERENCED_PARAMETER (Context);
  UNREFERENCED_PARAMETER (Arg1);
  UNREFERENCED_PARAMETER (Arg2);

  rtw_linux_irq_dpc ();
}

static BOOLEAN
SyncNothing (
  _In_opt_ PVOID  Context
  )
{
  UNREFERENCED_PARAMETER (Context);
  return TRUE;
}

void
rtwk_irq_synchronize (
  void
  )
{
  if ((mDev != NULL) && (mDev->Interrupt != NULL)) {
    KeSynchronizeExecution (mDev->Interrupt, SyncNothing, NULL);
  }

  if (KeGetCurrentIrql () == PASSIVE_LEVEL) {
    KeFlushQueuedDpcs ();
  }
}

/* ---- attach / detach ---- */

/* Gets the DMA adapter and connects the interrupt. PASSIVE_LEVEL. */
NTSTATUS
RtwOsAttach (
  _In_ PRTW_DEVICE  Dev
  )
{
  DEVICE_DESCRIPTION                Desc;
  IO_CONNECT_INTERRUPT_PARAMETERS   Connect;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR   R;
  PDEVICE_OBJECT                    Pdo;
  ULONG                             MapRegs;
  NTSTATUS                          Status;
  UCHAR                             C;

  if (mDev != NULL) {
    return STATUS_DEVICE_BUSY;
  }

  if (!Dev->IntResValid) {
    RtwLog ("no line interrupt resource");
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  Pdo = WdfDeviceWdmGetPhysicalDevice (Dev->Device);

  RtlZeroMemory (&Desc, sizeof (Desc));
  Desc.Version           = DEVICE_DESCRIPTION_VERSION3;
  Desc.Master            = TRUE;
  Desc.ScatterGather     = TRUE;
  Desc.Dma32BitAddresses = TRUE;
  Desc.InterfaceType     = PCIBus;
  Desc.MaximumLength     = MAXULONG;
  Desc.DmaAddressWidth   = 32;
  Dev->Dma = IoGetDmaAdapter (Pdo, &Desc, &MapRegs);
  if (Dev->Dma == NULL) {
    RtwLog ("IoGetDmaAdapter failed");
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  if (Dev->Dma->DmaOperations->Size < RTL_SIZEOF_THROUGH_FIELD (DMA_OPERATIONS, AllocateCommonBufferWithBounds)) {
    RtwLog ("DMA adapter has no AllocateCommonBufferWithBounds (operations size %u)", Dev->Dma->DmaOperations->Size);
    Dev->Dma->DmaOperations->PutDmaAdapter (Dev->Dma);
    Dev->Dma = NULL;
    return STATUS_NOT_SUPPORTED;
  }

  mAllocWithBounds = Dev->Dma->DmaOperations->AllocateCommonBufferWithBounds;
  KeInitializeSpinLock (&mDmaLock);
  ExInitializeFastMutex (&mGrowLock);
  mDev = Dev;

  // one chunk per size class up front, so DISPATCH_LEVEL callers find blocks
  for (C = 0; C < ARRAYSIZE (mClass); C++) {
    if (!GrowClass (C)) {
      RtwOsDetach (Dev);
      return STATUS_INSUFFICIENT_RESOURCES;
    }
  }

  RtwLog ("DMA adapter v3, 32-bit; heap %u MB", mHeapChunks);

  R = &Dev->IntRes;
  KeInitializeDpc (&Dev->IsrDpc, IsrDpc, Dev);
  RtlZeroMemory (&Connect, sizeof (Connect));
  Connect.Version                                 = CONNECT_FULLY_SPECIFIED;
  Connect.FullySpecified.PhysicalDeviceObject     = Pdo;
  Connect.FullySpecified.InterruptObject          = &Dev->Interrupt;
  Connect.FullySpecified.ServiceRoutine           = Isr;
  Connect.FullySpecified.ServiceContext           = Dev;
  Connect.FullySpecified.SynchronizeIrql          = (KIRQL)R->u.Interrupt.Level;
  Connect.FullySpecified.FloatingSave             = FALSE;
  Connect.FullySpecified.ShareVector              = (R->ShareDisposition == CmResourceShareShared);
  Connect.FullySpecified.Vector                   = R->u.Interrupt.Vector;
  Connect.FullySpecified.Irql                     = (KIRQL)R->u.Interrupt.Level;
  Connect.FullySpecified.InterruptMode            = (R->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;
  Connect.FullySpecified.ProcessorEnableMask      = R->u.Interrupt.Affinity;   // group 0 (one group on this SoC)
  Status = IoConnectInterruptEx (&Connect);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("IoConnectInterruptEx failed 0x%08x", Status);
    Dev->Interrupt = NULL;
    RtwOsDetach (Dev);
    return Status;
  }

  mIntIrql = (KIRQL)R->u.Interrupt.Level;
  RtwLog (
    "interrupt connected: vector %u IRQL %u %s%s",
    R->u.Interrupt.Vector,
    mIntIrql,
    (Connect.FullySpecified.InterruptMode == Latched) ? "edge" : "level",
    Connect.FullySpecified.ShareVector ? " shared" : ""
    );
  return STATUS_SUCCESS;
}

/* Disconnects the interrupt and gives back the DMA memory and adapter. PASSIVE_LEVEL. */
VOID
RtwOsDetach (
  _In_ PRTW_DEVICE  Dev
  )
{
  IO_DISCONNECT_INTERRUPT_PARAMETERS  Disconnect;

  if (Dev->Interrupt != NULL) {
    RtlZeroMemory (&Disconnect, sizeof (Disconnect));
    Disconnect.Version                       = CONNECT_FULLY_SPECIFIED;
    Disconnect.ConnectionContext.InterruptObject = Dev->Interrupt;
    IoDisconnectInterruptEx (&Disconnect);
    KeFlushQueuedDpcs ();
    Dev->Interrupt = NULL;
    mIntIrql       = DISPATCH_LEVEL;
    RtwLog ("interrupt disconnected");
  }

  if (mDev == Dev) {
    DmaRelease ();
    if (mRegions == 0) {
      Dev->Dma->DmaOperations->PutDmaAdapter (Dev->Dma);
      Dev->Dma = NULL;
      mDev     = NULL;
    }
  } else if (Dev->Dma != NULL) {
    Dev->Dma->DmaOperations->PutDmaAdapter (Dev->Dma);
    Dev->Dma = NULL;
  }
}
