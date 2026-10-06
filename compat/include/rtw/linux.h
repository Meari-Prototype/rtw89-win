/*
 * rtw89win Linux compatibility: everything. Every linux/, asm/ and net/ header that rtw89 or the
 * FreeBSD 802.11 headers include resolves to this file, except the 802.11 ones taken from FreeBSD.
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_LINUX_H
#define RTW_LINUX_H

#include <rtw/base.h>
#include <rtw/sched.h>
#include <rtw/mem.h>
#include <rtw/skbuff.h>
#include <rtw/bus.h>
#include <rtw/freebsd.h>

/* linux/average.h: exponentially weighted moving average (the kernel's results, rtw89 reads them as RSSI/SNR/EVM).
 * The average is kept scaled by 2^precision. The first sample sets it; each later sample s gives
 *   avg' = (avg * (2^k - 1) + s) / 2^k     with 2^k = weight_rcp (a power of two), computed with shifts:
 *   avg * (2^k - 1) = (avg << k) - avg. */
#define __rtw_ewma_sample(val, prec)    ((u64)(val) << (prec))
#define DECLARE_EWMA(name, _precision, _weight_rcp)                                            \
struct ewma_##name {                                                                          \
	u64 scaled;                                                                           \
};                                                                                            \
static inline void ewma_##name##_init (struct ewma_##name *e) { e->scaled = 0; }             \
static inline unsigned long ewma_##name##_read (struct ewma_##name *e)                       \
{ return (unsigned long)(e->scaled >> (_precision)); }                                       \
static inline void ewma_##name##_add (struct ewma_##name *e, unsigned long val)              \
{                                                                                             \
	u64 k = ilog2(_weight_rcp);                                                           \
	u64 avg = READ_ONCE(e->scaled);                                                       \
	WRITE_ONCE(e->scaled, avg != 0 ? ((avg << k) - avg + __rtw_ewma_sample(val, _precision)) >> k \
				       : __rtw_ewma_sample(val, _precision));                 \
}

#endif
