/*
 * rtw89win Linux compatibility: memory allocation. Everything is non-paged pool (rtwk_alloc); devm_* allocations
 * are released when the device is removed (compat/device.c).
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_MEM_H
#define RTW_MEM_H

#include <rtw/base.h>

#define GFP_KERNEL      0x01u
#define GFP_ATOMIC      0x02u
#define GFP_NOWAIT      0x04u
#define GFP_DMA         0x08u
#define GFP_DMA32       0x10u
#define __GFP_ZERO      0x100u
#define __GFP_NOWARN    0x200u
#define __GFP_NORETRY   0x400u
#define __GFP_HIGH      0x800u

#define PAGE_SHIFT      12
#define PAGE_SIZE       (1UL << PAGE_SHIFT)
#define PAGE_MASK       (~(PAGE_SIZE - 1))

static inline void *kmalloc (size_t n, gfp_t f) { return rtwk_alloc(n, (f & __GFP_ZERO) != 0); }
static inline void *kzalloc (size_t n, gfp_t f) { (void)f; return rtwk_alloc(n, 1); }
static inline void *kmalloc_array (size_t n, size_t s, gfp_t f) { return kmalloc(n * s, f); }
static inline void *kcalloc (size_t n, size_t s, gfp_t f) { (void)f; return rtwk_alloc(n * s, 1); }
static inline void kfree (const void *p) { if (p) rtwk_free((void *)p); }
static inline void kfree_sensitive (const void *p) { kfree(p); }
void *krealloc (const void *p, size_t n, gfp_t f);
static inline void *kmemdup (const void *s, size_t n, gfp_t f) { void *d = kmalloc(n, f); if (d) memcpy(d, s, n); return d; }
static inline char *kstrdup (const char *s, gfp_t f) { return s ? kmemdup(s, strlen(s) + 1, f) : NULL; }
#define kvmalloc(n, f)                  kmalloc(n, f)
#define kvzalloc(n, f)                  kzalloc(n, f)
#define kvcalloc(n, s, f)               kcalloc(n, s, f)
#define kvmalloc_array(n, s, f)         kmalloc_array(n, s, f)
#define kvfree(p)                       kfree(p)
#define vmalloc(n)                      kmalloc(n, GFP_KERNEL)
#define vzalloc(n)                      kzalloc(n, GFP_KERNEL)
#define vfree(p)                        kfree(p)
#define kmem_cache_alloc(c, f)          kmalloc((c)->size, f)

/* Linux 7.x typed allocation helpers (kzalloc_obj(*p) etc.); gfp defaults to GFP_KERNEL */
#define __rtw_default_gfp(...)          __rtw_default_gfp_(dummy, ##__VA_ARGS__, GFP_KERNEL)
#define __rtw_default_gfp_(d, g, ...)   g
#define __alloc_objs(KMALLOC, GFP, TYPE, COUNT) ((TYPE *)KMALLOC(size_mul(sizeof(TYPE), COUNT), GFP))
#define __alloc_flex(KMALLOC, GFP, TYPE, FAM, COUNT) ((TYPE *)KMALLOC(struct_size_t(TYPE, FAM, COUNT), GFP))
#define kmalloc_obj(P, ...)             __alloc_objs(kmalloc, __rtw_default_gfp(__VA_ARGS__), typeof(P), 1)
#define kmalloc_objs(P, N, ...)         __alloc_objs(kmalloc, __rtw_default_gfp(__VA_ARGS__), typeof(P), N)
#define kmalloc_flex(P, FAM, N, ...)    __alloc_flex(kmalloc, __rtw_default_gfp(__VA_ARGS__), typeof(P), FAM, N)
#define kzalloc_obj(P, ...)             __alloc_objs(kzalloc, __rtw_default_gfp(__VA_ARGS__), typeof(P), 1)
#define kzalloc_objs(P, N, ...)         __alloc_objs(kzalloc, __rtw_default_gfp(__VA_ARGS__), typeof(P), N)
#define kzalloc_flex(P, FAM, N, ...)    __alloc_flex(kzalloc, __rtw_default_gfp(__VA_ARGS__), typeof(P), FAM, N)
#define kvmalloc_obj                    kmalloc_obj
#define kvzalloc_obj                    kzalloc_obj
#define kvzalloc_objs                   kzalloc_objs
#define kvzalloc_flex                   kzalloc_flex

/* linux/cleanup.h: __free(kfree) */
#define __cleanup(fn)                   __attribute__((cleanup(fn)))
#define DEFINE_FREE(name, type, free)   static inline void __free_##name (void *p) { type _T = *(type *)p; free; }
#define __free(name)                    __cleanup(__free_##name)
#define no_free_ptr(p)                  ({ typeof(p) __p = (p); (p) = NULL; __p; })
#define return_ptr(p)                   return no_free_ptr(p)
DEFINE_FREE(kfree, void *, if (_T) kfree(_T))
DEFINE_FREE(kvfree, void *, if (_T) kvfree(_T))

#endif
