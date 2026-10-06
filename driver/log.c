// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * Driver log: a 64 KB ring of text lines ("[seconds since boot] message\n"), also sent to DbgPrintEx.
 * Callable at IRQL <= DISPATCH_LEVEL; above that only DbgPrintEx is used.
 */
#include "rtw89win.h"
#include <ntstrsafe.h>
#include <stdarg.h>

#define RTW_LOG_SIZE  0x10000     // power of two

static CHAR        mRing[RTW_LOG_SIZE];
static ULONG64     mTotal;        // bytes ever written; the ring holds [mTotal - RTW_LOG_SIZE, mTotal)
static KSPIN_LOCK  mLock;         // zero-initialised = released (KeInitializeSpinLock only zeroes it)

VOID
RtwLog (
  _In_z_ _Printf_format_string_ PCSTR  Format,
  ...
  )
{
  CHAR     Line[320];
  size_t   Len;
  size_t   Head;
  ULONG64  Ms;
  KIRQL    Irql;
  va_list  Args;
  size_t   i;

  Ms = KeQueryInterruptTime () / 10000;
  RtlStringCbPrintfA (Line, sizeof (Line), "[%5llu.%03llu] ", Ms / 1000, Ms % 1000);
  RtlStringCbLengthA (Line, sizeof (Line), &Head);

  va_start (Args, Format);
  RtlStringCbVPrintfA (Line + Head, sizeof (Line) - Head - 1, Format, Args);   // keep room for '\n'
  va_end (Args);

  RtlStringCbLengthA (Line, sizeof (Line), &Len);
  if ((Len == 0) || (Line[Len - 1] != '\n')) {
    Line[Len++] = '\n';
    Line[Len]   = '\0';
  }

  DbgPrintEx (DPFLTR_IHVNETWORK_ID, DPFLTR_INFO_LEVEL, "rtw89win: %s", Line + Head);

  if (KeGetCurrentIrql () > DISPATCH_LEVEL) {
    return;
  }

  KeAcquireSpinLock (&mLock, &Irql);
  for (i = 0; i < Len; i++) {
    mRing[(mTotal + i) & (RTW_LOG_SIZE - 1)] = Line[i];
  }

  mTotal += Len;
  KeReleaseSpinLock (&mLock, Irql);
}

VOID
RtwLogRead (
  _Inout_ ULONG64                  *From,
  _Out_ ULONG64                    *Dropped,
  _Out_writes_bytes_to_ (OutLen, *Copied) PCHAR  Out,
  _In_ ULONG                       OutLen,
  _Out_ ULONG                      *Copied
  )
{
  ULONG64  Oldest;
  ULONG64  Avail;
  ULONG    n;
  ULONG    i;
  KIRQL    Irql;

  KeAcquireSpinLock (&mLock, &Irql);
  Oldest   = (mTotal > RTW_LOG_SIZE) ? (mTotal - RTW_LOG_SIZE) : 0;
  *Dropped = 0;
  if (*From < Oldest) {
    *Dropped = Oldest - *From;
    *From    = Oldest;
  }

  if (*From > mTotal) {
    *From = mTotal;
  }

  Avail = mTotal - *From;
  n     = (Avail < OutLen) ? (ULONG)Avail : OutLen;
  for (i = 0; i < n; i++) {
    Out[i] = mRing[(*From + i) & (RTW_LOG_SIZE - 1)];
  }

  *From  += n;
  *Copied = n;
  KeReleaseSpinLock (&mLock, Irql);
}
