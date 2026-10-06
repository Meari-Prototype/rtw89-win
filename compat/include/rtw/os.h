/*
 * rtw89win: the interface between the Linux-side code (rtw89, compat, mac; built with clang, no Windows headers)
 * and the Windows-side primitives (driver\os.c; MSVC, ntddk/wdf). Plain C types only, so both sides can include it.
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_OS_H
#define RTW_OS_H

#include <stddef.h>
#include <stdarg.h>
#if defined(_MSC_VER) && defined(_KERNEL_MODE)
/* the WDK kernel CRT has no stdint.h */
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#define RTWK_NORETURN   __declspec(noreturn)
#else
#define RTWK_NORETURN   __attribute__((noreturn))
#endif

/* Sizes of the opaque storage the Linux side reserves for Windows objects (checked with static_assert in os.c). */
#define RTWK_MUTEX_SIZE   64      /* KMUTEX */
#define RTWK_EVENT_SIZE   32      /* KEVENT */
#define RTWK_TIMER_SIZE   192     /* KTIMER + KDPC */

/* memory: non-paged; NULL on failure */
void *rtwk_alloc (size_t size, int zero);
void rtwk_free (void *p);
size_t rtwk_alloc_size (void *p);                     /* size passed to rtwk_alloc */

/* spinlocks: *lock is a KSPIN_LOCK. Returns the previous IRQL.
   rtwk_spin_lock_irq raises to the device interrupt level first, so it also excludes the ISR. */
uint8_t rtwk_spin_lock (uint64_t *lock);
uint8_t rtwk_spin_lock_irq (uint64_t *lock);
void rtwk_spin_unlock (uint64_t *lock, uint8_t old_irql);

/* mutex (KMUTEX; PASSIVE_LEVEL) */
void rtwk_mutex_init (void *m);
void rtwk_mutex_lock (void *m);
int rtwk_mutex_trylock (void *m);
void rtwk_mutex_unlock (void *m);
int rtwk_mutex_is_locked (void *m);

/* events (KEVENT, synchronization or notification) */
void rtwk_event_init (void *e, int notification);
void rtwk_event_set (void *e);
void rtwk_event_clear (void *e);
int rtwk_event_state (void *e);
/* returns 1 if signalled, 0 on timeout; timeout_us < 0 waits forever */
int rtwk_event_wait (void *e, int64_t timeout_us);

/* kernel threads (PASSIVE_LEVEL) */
void *rtwk_thread_start (void (*fn)(void *ctx), void *ctx);
void rtwk_thread_join (void *thread);
int rtwk_current_irql (void);

/* timer: callback runs at DISPATCH_LEVEL */
void rtwk_timer_init (void *t, void (*fn)(void *ctx), void *ctx);
void rtwk_timer_arm (void *t, uint64_t due_us);       /* replaces a pending expiry */
int rtwk_timer_cancel (void *t);                      /* 1 if it was pending */
void rtwk_timer_sync (void);                          /* waits for running timer callbacks (KeFlushQueuedDpcs) */

/* time and delays */
uint64_t rtwk_time_ns (void);                         /* monotonic, since boot */
void rtwk_udelay (uint32_t us);                       /* busy wait, any IRQL */
void rtwk_usleep (uint64_t us);                       /* sleeps at PASSIVE, busy waits above */
uint32_t rtwk_random (void);

/* log a formatted line (the driver's RtwLog ring); level 0 = error, 1 = warning, 2 = info, 3 = debug */
void rtwk_log (int level, const char *msg);
/* BUG(): stop the system with a driver-specific bug check */
RTWK_NORETURN void rtwk_bugcheck (int line);

/* DMA: everything comes from common buffers of the device's DMA adapter (32-bit addressable). Where PCIe is not
   cache coherent (RK3588) they are uncached normal memory, like Linux's coherent memory of a non-coherent device,
   so unaligned access is fine. */
void *rtwk_dma_alloc (size_t size, uint64_t *dma);    /* contiguous, zeroed, for rings */
void rtwk_dma_free (void *va);
void *rtwk_dma_heap_alloc (size_t size);              /* small DMA-able buffers (skb data); NULL if none */
void rtwk_dma_heap_free (void *va);
int rtwk_dma_lookup (const void *va, uint64_t *dma);  /* 0 if va lies in DMA memory from above */
void rtwk_dma_sync (const void *va, size_t size, int to_device);

/* PCI configuration space of the bound device; return bytes transferred */
uint32_t rtwk_pci_cfg_read (void *os_dev, uint32_t offset, void *buf, uint32_t len);
uint32_t rtwk_pci_cfg_write (void *os_dev, uint32_t offset, const void *buf, uint32_t len);

/* firmware: reads <driver directory>\<basename of name> into rtwk_alloc() memory; 0 on success */
int rtwk_firmware_load (const char *name, void **data, size_t *size);

/* interrupt: the line interrupt of the device (INTx). The ISR calls rtw_linux_isr() (rtw/glue.h) at DIRQL and,
   when it asks for it, a DPC that calls rtw_linux_irq_dpc(). */
void rtwk_irq_synchronize (void);                     /* returns when no ISR/DPC of ours is running */

/* AES-128 for management frame protection (driver\crypto.c, the kernel's CNG). PASSIVE_LEVEL only. Return 0 or a
   negative errno: -EBADMSG (74) the MIC does not match, -ENODEV not available (self-test failed, or above PASSIVE),
   -EIO. Keys are 16 bytes. */
int rtwk_crypto_ready (void);                         /* 1: the known-answer tests of the driver load passed */
/* AES-CCM with a 13-byte nonce and an 8-byte MIC (CCMP): data is encrypted in place, the MIC written to mic */
int rtwk_aes_ccm_encrypt (const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, uint32_t aad_len,
	uint8_t *data, uint32_t len, uint8_t *mic);
/* ... and back: data decrypted in place if mic matches (-EBADMSG otherwise; data is then not to be used) */
int rtwk_aes_ccm_decrypt (const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, uint32_t aad_len,
	uint8_t *data, uint32_t len, const uint8_t *mic);
/* AES-CMAC (RFC 4493) of data, the full 16 bytes */
int rtwk_aes_cmac (const uint8_t *key, const uint8_t *data, uint32_t len, uint8_t *mac);

#ifdef __cplusplus
}
#endif

#endif
