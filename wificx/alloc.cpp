// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: the C++ allocation functions. The TLV library (WificxTLVGenParse.lib) and
 * ArrayOfElements (arrayofelements.hpp) allocate with operator new (size_t, ULONG_PTR AllocationContext) and free
 * with delete / delete[]; our TLV_CONTEXT.AllocationContext is always 0. Zeroed non-paged pool, like the sample's
 * memorymanagement.cpp. There is deliberately no plain operator new (size_t): the front end does not use it, and a
 * use would fail to link. On 64-bit, operator delete (void *, size_t) is also the placement delete matching
 * operator new (size_t, ULONG_PTR).
 */
#include "wifi.h"

#define RTW_TLV_POOL_TAG  'vltR'

void * __cdecl
operator new (
  size_t     Size,
  ULONG_PTR  AllocationContext
  ) throw ()
{
  if (AllocationContext != 0) {
    NT_ASSERT (AllocationContext == 0);
    return NULL;
  }

  return ExAllocatePool2 (POOL_FLAG_NON_PAGED, (Size != 0) ? Size : 1, RTW_TLV_POOL_TAG);
}

void __cdecl
operator delete (
  void  *p
  ) noexcept
{
  if (p != NULL) {
    ExFreePoolWithTag (p, RTW_TLV_POOL_TAG);
  }
}

void __cdecl
operator delete[] (
  void  *p
  ) noexcept
{
  if (p != NULL) {
    ExFreePoolWithTag (p, RTW_TLV_POOL_TAG);
  }
}

void __cdecl
operator delete (
  void    *p,
  size_t  Size
  ) noexcept
{
  UNREFERENCED_PARAMETER (Size);

  if (p != NULL) {
    ExFreePoolWithTag (p, RTW_TLV_POOL_TAG);
  }
}

void __cdecl
operator delete[] (
  void    *p,
  size_t  Size
  ) noexcept
{
  UNREFERENCED_PARAMETER (Size);

  if (p != NULL) {
    ExFreePoolWithTag (p, RTW_TLV_POOL_TAG);
  }
}
