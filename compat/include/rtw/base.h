/*
 * rtw89win Linux compatibility: basic types, compiler attributes, arithmetic and bit helpers, byte order, errno,
 * strings, printing, lists, atomics, bitmaps. Written for the subset of the kernel API that rtw89 uses; semantics
 * follow the kernel documentation. Built only by clang, target aarch64-w64-mingw32 (buildhal.ps1; GNU C extensions).
 * Windows ARM64 is LLP64: `long` is 32-bit here. rtw89 never stores pointers in `long`; bitmaps use 32-bit words.
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_BASE_H
#define RTW_BASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <rtw/os.h>

/* ---- types ---- */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u8 __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef s8 __s8;
typedef s16 __s16;
typedef s32 __s32;
typedef s64 __s64;
typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;
typedef u16 __sum16;
typedef u32 __wsum;
typedef intptr_t ssize_t;
typedef uintptr_t kernel_ulong_t;
typedef u64 dma_addr_t;
typedef u64 phys_addr_t;
typedef u64 resource_size_t;
typedef unsigned int gfp_t;
typedef s64 ktime_t;
typedef s64 time64_t;
typedef s64 loff_t;
typedef unsigned short umode_t;
typedef int pid_t;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;

/* ---- attributes ---- */
#define __packed                __attribute__((packed))
#define __aligned(x)            __attribute__((aligned(x)))
#define __always_inline         inline __attribute__((always_inline))
#define noinline                __attribute__((noinline))
#define __noinline              noinline
#define __maybe_unused          __attribute__((unused))
#define __always_unused         __attribute__((unused))
#define __unused                __attribute__((unused))
#define __used                  __attribute__((used))
#define __must_check            __attribute__((warn_unused_result))
#define __cold                  __attribute__((cold))
#define __pure                  __attribute__((pure))
#define __printf(a, b)
#define __scanf(a, b)
#define __iomem
#define __rcu
#define __force
#define __user
#define __bitwise
#define __percpu
#define __acquires(x)
#define __releases(x)
#define __must_hold(x)
#define __counted_by(x)
#define __counted_by_le(x)
#define __nonstring
#define __init
#define __exit
#define __initconst
#define __initdata
#define __read_mostly
#define __ro_after_init
#define __visible
#define __weak                  __attribute__((weak))
#define __section(s)
#define __diag_push()
#define __diag_pop()
#define fallthrough             do { } while (0)   /* -Wimplicit-fallthrough is off; FreeBSD headers put it before } */
#define likely(x)               __builtin_expect(!!(x), 1)
#define unlikely(x)             __builtin_expect(!!(x), 0)
#define __stringify_1(x...)     #x
#define __stringify(x...)       __stringify_1(x)
#define __CONCAT_(a, b)         a##b
#define __PASTE(a, b)           __CONCAT_(a, b)
#define __UNIQUE_ID(prefix)     __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)
#define __same_type(a, b)       __builtin_types_compatible_p(typeof(a), typeof(b))
#define __is_constexpr(x)       __builtin_constant_p(x)
#define typeof_member(T, m)     typeof(((T *)0)->m)

/* IS_ENABLED(CONFIG_FOO) for C expressions. Each option has a line in the table below: 1 when buildhal.ps1 defines
 * it on the clang command line, else 0. An option without a line is a compile error (undeclared __rtw_kcfg_...):
 * add it here. Nothing is built as a module, so IS_MODULE is always 0. In #if, "defined(CONFIG_FOO)" works too. */
#define IS_ENABLED(option)                  __rtw_kcfg_##option
#define IS_BUILTIN(option)                  __rtw_kcfg_##option
#define IS_REACHABLE(option)                __rtw_kcfg_##option
#define IS_MODULE(option)                   0
#ifdef CONFIG_RTW89_8852BE
#define __rtw_kcfg_CONFIG_RTW89_8852BE      1
#else
#define __rtw_kcfg_CONFIG_RTW89_8852BE      0
#endif
#ifdef CONFIG_RTW89_PCI
#define __rtw_kcfg_CONFIG_RTW89_PCI         1
#else
#define __rtw_kcfg_CONFIG_RTW89_PCI         0
#endif
#define __rtw_kcfg_CONFIG_IPV6              0

/* static_assert(expr) / static_assert(expr, msg) */
#define static_assert(expr, ...)            __rtw_static_assert(expr, ##__VA_ARGS__, #expr)
#define __rtw_static_assert(expr, msg, ...) _Static_assert(expr, msg)
/* like the kernel: the condition only has to fold at -O2 (a surviving call is a compile error) */
#define BUILD_BUG_ON(cond)                  __rtw_bbo(cond, __COUNTER__)
#define __rtw_bbo(cond, n)                  __rtw_bbo_(cond, n)
#define __rtw_bbo_(cond, n) \
	do { extern void __rtw_build_bug_##n (void) __attribute__((error("BUILD_BUG_ON(" #cond ")"))); \
	     if (cond) __rtw_build_bug_##n(); } while (0)
#define BUILD_BUG_ON_MSG(cond, msg)         _Static_assert(!(cond), msg)
#define BUILD_BUG_ON_ZERO(e)                ((int)(sizeof(struct { int:(-!!(e)); })))
#define BUILD_BUG()                         _Static_assert(0, "BUILD_BUG")
#define __must_be_array(a)                  0
#define typecheck(type, x)                  ({ type __dummy; typeof(x) __dummy2; (void)(&__dummy == &__dummy2); 1; })

/* ---- barriers ---- */
#define barrier()               __asm__ __volatile__("" ::: "memory")
#define mb()                    __asm__ __volatile__("dsb sy" ::: "memory")
#define rmb()                   __asm__ __volatile__("dsb ld" ::: "memory")
#define wmb()                   __asm__ __volatile__("dsb st" ::: "memory")
#define dma_rmb()               __asm__ __volatile__("dmb oshld" ::: "memory")
#define dma_wmb()               __asm__ __volatile__("dmb oshst" ::: "memory")
#define smp_mb()                __asm__ __volatile__("dmb ish" ::: "memory")
#define smp_rmb()               __asm__ __volatile__("dmb ishld" ::: "memory")
#define smp_wmb()               __asm__ __volatile__("dmb ishst" ::: "memory")
#define smp_mb__before_atomic() smp_mb()
#define smp_mb__after_atomic()  smp_mb()
#define smp_store_release(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define smp_load_acquire(p)     __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define READ_ONCE(x)            (*(const volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, v)        do { *(volatile typeof(x) *)&(x) = (v); } while (0)
#define cpu_relax()             __asm__ __volatile__("yield" ::: "memory")

/* ---- limits ---- */
/* the <stdint.h> limits, typed like the kernel's (U8_MAX is a u8, ...) */
#define U8_MAX      ((u8)UINT8_MAX)
#define S8_MAX      ((s8)INT8_MAX)
#define S8_MIN      ((s8)INT8_MIN)
#define U16_MAX     ((u16)UINT16_MAX)
#define S16_MAX     ((s16)INT16_MAX)
#define S16_MIN     ((s16)INT16_MIN)
#define U32_MAX     ((u32)UINT32_MAX)
#define S32_MAX     ((s32)INT32_MAX)
#define S32_MIN     ((s32)INT32_MIN)
#define U64_MAX     ((u64)UINT64_MAX)
#define S64_MAX     ((s64)INT64_MAX)
#define S64_MIN     ((s64)INT64_MIN)
#ifndef INT_MAX
#define INT_MAX     ((int)(~0U >> 1))
#define INT_MIN     (-INT_MAX - 1)
#define UINT_MAX    (~0U)
#define LONG_MAX    ((long)(~0UL >> 1))
#define LONG_MIN    (-LONG_MAX - 1)
#define ULONG_MAX   (~0UL)
#define LLONG_MAX   ((long long)(~0ULL >> 1))
#define LLONG_MIN   (-LLONG_MAX - 1)
#define ULLONG_MAX  (~0ULL)
#endif
#define SIZE_MAX_   (~(size_t)0)

/* ---- arithmetic ---- */
#define ARRAY_SIZE(a)           (sizeof(a) / sizeof((a)[0]))
#define sizeof_field(T, m)      sizeof(((T *)0)->m)
#define offsetofend(T, m)       (offsetof(T, m) + sizeof_field(T, m))
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define container_of_const(ptr, type, member) container_of(ptr, type, member)

/* min/max & co.: each argument evaluated once, into _rtw_x/_rtw_y of its own type */
#define min(a, b)               ({ typeof(a) _rtw_x = (a); typeof(b) _rtw_y = (b); _rtw_x < _rtw_y ? _rtw_x : _rtw_y; })
#define max(a, b)               ({ typeof(a) _rtw_x = (a); typeof(b) _rtw_y = (b); _rtw_x > _rtw_y ? _rtw_x : _rtw_y; })
#define min_t(t, a, b)          ({ t _rtw_x = (t)(a); t _rtw_y = (t)(b); _rtw_x < _rtw_y ? _rtw_x : _rtw_y; })
#define max_t(t, a, b)          ({ t _rtw_x = (t)(a); t _rtw_y = (t)(b); _rtw_x > _rtw_y ? _rtw_x : _rtw_y; })
#define umin(a, b)              min_t(u64, a, b)
#define umax(a, b)              max_t(u64, a, b)
#define min3(a, b, c)           min(min(a, b), c)
#define max3(a, b, c)           max(max(a, b), c)
#define min_not_zero(a, b)      ({ typeof(a) _rtw_p = (a); typeof(b) _rtw_q = (b); _rtw_p == 0 ? _rtw_q : (_rtw_q == 0 ? _rtw_p : min(_rtw_p, _rtw_q)); })
#define clamp(v, lo, hi)        min(max(v, lo), hi)
#define clamp_t(t, v, lo, hi)   min_t(t, max_t(t, v, lo), hi)
#define clamp_val(v, lo, hi)    clamp_t(typeof(v), v, lo, hi)
#define abs(x)                  ({ typeof(x) _rtw_v = (x); _rtw_v < 0 ? -_rtw_v : _rtw_v; })
#define abs_diff(a, b)          ({ typeof(a) _rtw_x = (a); typeof(b) _rtw_y = (b); _rtw_x > _rtw_y ? _rtw_x - _rtw_y : _rtw_y - _rtw_x; })
#define swap(a, b)              do { typeof(a) __t = (a); (a) = (b); (b) = __t; } while (0)
#define DIV_ROUND_UP(n, d)      (((n) + (d) - 1) / (d))
#define DIV_ROUND_UP_ULL(n, d)  ((u64)(((u64)(n) + (d) - 1) / (d)))
#define DIV_ROUND_DOWN_ULL(n, d) ((u64)(n) / (d))
#define DIV_ROUND_CLOSEST(x, d) ({ typeof(x) __x = (x); typeof(d) __d = (d); \
	(((typeof(x))-1) > 0 || ((typeof(d))-1) > 0 || (((__x) > 0) == ((__d) > 0))) ? \
	(((__x) + ((__d) / 2)) / (__d)) : (((__x) - ((__d) / 2)) / (__d)); })
#define DIV_ROUND_CLOSEST_ULL(x, d) ({ u64 __x = (x) + (d) / 2; __x / (d); })
/* roundup/rounddown: any y; round_up/round_down: y a power of two (__rtw_low_mask: the bits below y, typed like x) */
#define roundup(x, y)           ({ typeof(y) _rtw_m = (y); (((x) + (_rtw_m - 1)) / _rtw_m) * _rtw_m; })
#define rounddown(x, y)         ({ typeof(x) _rtw_v = (x); _rtw_v - (_rtw_v % (y)); })
#define roundup_u64(x, y)       (DIV_ROUND_UP_ULL(x, y) * (y))
#define __rtw_low_mask(x, y)    ((typeof(x))((y) - 1))
#define round_up(x, y)          (((x) - 1 | __rtw_low_mask(x, y)) + 1)
#define round_down(x, y)        ((x) & ~__rtw_low_mask(x, y))
#define ALIGN(x, a)             (((x) + ((typeof(x))(a) - 1)) & ~((typeof(x))(a) - 1))
#define ALIGN_DOWN(x, a)        ((x) & ~((typeof(x))(a) - 1))
#define IS_ALIGNED(x, a)        (((x) & ((typeof(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a)         ((typeof(p))ALIGN((uintptr_t)(p), (a)))
#define upper_32_bits(n)        ((u32)((u64)(n) >> 32))
#define lower_32_bits(n)        ((u32)(n))                      /* the conversions keep the low bits */
#define upper_16_bits(n)        ((u16)((u32)(n) >> 16))
#define lower_16_bits(n)        ((u16)(n))
#define mult_frac(x, n, d)      ({ typeof(x) __q = (x) / (d); typeof(x) __r = (x) % (d); __q * (n) + __r * (n) / (d); })
#define do_div(n, base)         ({ u32 __base = (base); u32 __rem = (u32)((u64)(n) % __base); (n) = (u64)(n) / __base; __rem; })
#define size_mul(a, b)          ((size_t)(a) * (size_t)(b))
#define size_add(a, b)          ((size_t)(a) + (size_t)(b))
#define array_size(a, b)        size_mul(a, b)
#define flex_array_size(p, member, count) size_mul(count, sizeof(*(p)->member))
#define struct_size(p, member, count) size_add(sizeof(*(p)), flex_array_size(p, member, count))
#define struct_size_t(type, member, count) struct_size((type *)NULL, member, count)
#define __set_flex_counter(fam, count) do { } while (0)
#define DECLARE_FLEX_ARRAY(T, name) struct { struct { } __empty_##name; T name[]; }

static inline u64 div_u64 (u64 n, u32 d) { return n / d; }
static inline s64 div_s64 (s64 n, s32 d) { return n / d; }
static inline u64 div64_u64 (u64 n, u64 d) { return n / d; }
static inline s64 div64_s64 (s64 n, s64 d) { return n / d; }
static inline u64 div_u64_rem (u64 n, u32 d, u32 *r) { *r = (u32)(n % d); return n / d; }
static inline s32 sign_extend32 (u32 v, int index) { u8 s = 31 - index; return (s32)(v << s) >> s; }
static inline s64 sign_extend64 (u64 v, int index) { u8 s = 63 - index; return (s64)(v << s) >> s; }

/* ---- bits ---- */
#define BITS_PER_BYTE           8
#define BITS_PER_TYPE(t)        (sizeof(t) * BITS_PER_BYTE)
#define BITS_PER_LONG           32
#define BITS_PER_LONG_LONG      64
#define BIT(nr)                 (1ULL << (nr))
#define BIT_ULL(nr)             (1ULL << (nr))
#define BIT_U8(nr)              ((u8)BIT(nr))
#define BIT_U16(nr)             ((u16)BIT(nr))
#define BIT_U32(nr)             ((u32)BIT(nr))
#define BIT_U64(nr)             ((u64)BIT(nr))
#define BIT_MASK(nr)            (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)            ((nr) / BITS_PER_LONG)
#define BITS_TO_LONGS(nr)       DIV_ROUND_UP(nr, BITS_PER_LONG)
#define BITS_TO_U32(nr)         DIV_ROUND_UP(nr, 32)
#define BITS_TO_BYTES(nr)       DIV_ROUND_UP(nr, BITS_PER_BYTE)
#define GENMASK_ULL(h, l)       (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (63 - (h))))
#define GENMASK(h, l)           GENMASK_ULL(h, l)
#define GENMASK_U8(h, l)        ((u8)GENMASK_ULL(h, l))
#define GENMASK_U16(h, l)       ((u16)GENMASK_ULL(h, l))
#define GENMASK_U32(h, l)       ((u32)GENMASK_ULL(h, l))
#define GENMASK_U64(h, l)       GENMASK_ULL(h, l)

#define hweight8(w)             __builtin_popcount((u32)(w) & 0xff)     /* macros: usable in static_assert */
#define hweight16(w)            __builtin_popcount((u32)(w) & 0xffff)
#define hweight32(w)            __builtin_popcount((u32)(w))
#define hweight64(w)            __builtin_popcountll((u64)(w))
static inline int hweight_long (unsigned long w) { return __builtin_popcountl(w); }
static inline unsigned int __ffs (unsigned long w) { return __builtin_ctzl(w); }
static inline unsigned int __ffs64 (u64 w) { return __builtin_ctzll(w); }
static inline unsigned int __fls (unsigned long w) { return BITS_PER_LONG - 1 - __builtin_clzl(w); }
static inline int ffs (int x) { return __builtin_ffs(x); }
static inline int fls (unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64 (u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned long ffz (unsigned long w) { return __ffs(~w); }
#define ilog2(n)                (__builtin_constant_p(n) ? (63 - __builtin_clzll((u64)(n) | 1)) : (fls64((u64)(n)) - 1))
#define order_base_2(n)         ((n) > 1 ? ilog2((n) - 1) + 1 : 0)
#define is_power_of_2(n)        ((n) != 0 && (((n) & ((n) - 1)) == 0))
static inline unsigned long roundup_pow_of_two (unsigned long n) { return 1UL << fls((unsigned int)(n - 1)); }
static inline unsigned long rounddown_pow_of_two (unsigned long n) { return 1UL << (fls((unsigned int)n) - 1); }
static inline u32 rol32 (u32 w, unsigned int s) { return (w << (s & 31)) | (w >> ((-s) & 31)); }
static inline u32 ror32 (u32 w, unsigned int s) { return (w >> (s & 31)) | (w << ((-s) & 31)); }

/* ---- bitfield ---- */
#define __bf_shf(x)             (__builtin_ffsll(x) - 1)
#define FIELD_MAX(mask)         ((typeof(mask))((mask) >> __bf_shf(mask)))
#define FIELD_FIT(mask, val)    (!((((typeof(mask))(val)) << __bf_shf(mask)) & ~(mask)))
#define FIELD_PREP(mask, val)   (((typeof(mask))(val) << __bf_shf(mask)) & (mask))
#define FIELD_GET(mask, reg)    ((typeof(mask))(((reg) & (mask)) >> __bf_shf(mask)))
#define FIELD_MODIFY(mask, reg, val) (*(reg) = (*(reg) & ~(mask)) | FIELD_PREP(mask, val))
#define FIELD_GET_SIGNED(mask, reg) \
	({ s64 __v = (s64)FIELD_GET(mask, reg); int __w = __builtin_popcountll(mask); \
	   (typeof(mask))((__v << (64 - __w)) >> (64 - __w)); })
#define field_get(mask, reg)    FIELD_GET(mask, reg)
#define field_prep(mask, val)   FIELD_PREP(mask, val)

/* <pfx>_get_bits, <pfx>_encode_bits, <pfx>_replace_bits and <pfx>p_replace_bits: FIELD_GET / FIELD_PREP / a field
 * update for one storage type T. The mask is in CPU order; for be16/be32 the stored value is big endian, so it is
 * byte swapped (swap) before a field is read and after a field is encoded. (mask & -mask) is the mask's lowest
 * set bit: dividing by it moves the field down, multiplying moves a value up. The replace keeps the bits outside
 * the mask and puts the encoded value into it. */
#define __RTW_FIELD_FNS(pfx, T, swap)                                                          \
static __always_inline T pfx##_get_bits (T stored, T mask)                                    \
{ return (swap(stored) & mask) / (mask & -mask); }                                            \
static __always_inline T pfx##_encode_bits (T v, T mask)                                      \
{ return swap((v & (T)(mask >> __bf_shf(mask))) * (T)(mask & -mask)); }                       \
static __always_inline T pfx##_replace_bits (T stored, T v, T mask)                          \
{ T keep = stored & ~swap(mask); return keep | pfx##_encode_bits(v, mask); }                  \
static __always_inline void pfx##p_replace_bits (T *p, T v, T mask)                          \
{ *p = pfx##_replace_bits(*p, v, mask); }
#define __rtw_noswap(x)         (x)
__RTW_FIELD_FNS(u8, u8, __rtw_noswap)
__RTW_FIELD_FNS(u16, u16, __rtw_noswap)
__RTW_FIELD_FNS(u32, u32, __rtw_noswap)
__RTW_FIELD_FNS(u64, u64, __rtw_noswap)
__RTW_FIELD_FNS(le16, __le16, __rtw_noswap)
__RTW_FIELD_FNS(le32, __le32, __rtw_noswap)
__RTW_FIELD_FNS(le64, __le64, __rtw_noswap)
__RTW_FIELD_FNS(be16, __be16, __builtin_bswap16)
__RTW_FIELD_FNS(be32, __be32, __builtin_bswap32)
#undef __RTW_FIELD_FNS

/* ---- byte order (little endian CPU) ---- */
#define cpu_to_le16(x)          ((__le16)(u16)(x))
#define cpu_to_le32(x)          ((__le32)(u32)(x))
#define cpu_to_le64(x)          ((__le64)(u64)(x))
#define le16_to_cpu(x)          ((u16)(__le16)(x))
#define le32_to_cpu(x)          ((u32)(__le32)(x))
#define le64_to_cpu(x)          ((u64)(__le64)(x))
#define cpu_to_be16(x)          ((__be16)__builtin_bswap16((u16)(x)))
#define cpu_to_be32(x)          ((__be32)__builtin_bswap32((u32)(x)))
#define cpu_to_be64(x)          ((__be64)__builtin_bswap64((u64)(x)))
#define be16_to_cpu(x)          __builtin_bswap16((u16)(x))
#define be32_to_cpu(x)          __builtin_bswap32((u32)(x))
#define be64_to_cpu(x)          __builtin_bswap64((u64)(x))
#define le16_to_cpup(p)         (*(const u16 *)(p))
#define le32_to_cpup(p)         (*(const u32 *)(p))
#define le32_add_cpu(p, v)      (*(p) = cpu_to_le32(le32_to_cpu(*(p)) + (v)))
#define le16_add_cpu(p, v)      (*(p) = cpu_to_le16(le16_to_cpu(*(p)) + (v)))
#define swab16(x)               __builtin_bswap16(x)
#define swab32(x)               __builtin_bswap32(x)
#define swab64(x)               __builtin_bswap64(x)
#define htons(x)                cpu_to_be16(x)
#define ntohs(x)                be16_to_cpu(x)
#define htonl(x)                cpu_to_be32(x)
#define ntohl(x)                be32_to_cpu(x)

#define __get_unaligned_t(T, p) ({ T __v; __builtin_memcpy(&__v, (p), sizeof(T)); __v; })
#define __put_unaligned_t(T, v, p) do { T __v = (v); __builtin_memcpy((p), &__v, sizeof(T)); } while (0)
#define get_unaligned(p)        __get_unaligned_t(typeof(*(p)), p)
#define put_unaligned(v, p)     __put_unaligned_t(typeof(*(p)), v, p)
#define get_unaligned_le16(p)   __get_unaligned_t(u16, p)
#define get_unaligned_le32(p)   __get_unaligned_t(u32, p)
#define get_unaligned_le64(p)   __get_unaligned_t(u64, p)
#define get_unaligned_be16(p)   be16_to_cpu(__get_unaligned_t(u16, p))
#define get_unaligned_be32(p)   be32_to_cpu(__get_unaligned_t(u32, p))
#define put_unaligned_le16(v, p) __put_unaligned_t(u16, v, p)
#define put_unaligned_le32(v, p) __put_unaligned_t(u32, v, p)
#define put_unaligned_le64(v, p) __put_unaligned_t(u64, v, p)
#define put_unaligned_be16(v, p) __put_unaligned_t(u16, cpu_to_be16(v), p)
#define put_unaligned_be32(v, p) __put_unaligned_t(u32, cpu_to_be32(v), p)
static inline u32 get_unaligned_le24 (const void *p) { const u8 *b = p; return b[0] | b[1] << 8 | b[2] << 16; }
static inline void put_unaligned_le24 (u32 v, void *p) { u8 *b = p; b[0] = (u8)v; b[1] = (u8)(v >> 8); b[2] = (u8)(v >> 16); }

/* ---- errno ---- */
#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define ENXIO           6
#define E2BIG           7
#define EBADF           9
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define ENODEV          19
#define EINVAL          22
#define ENOSPC          28
#define ERANGE          34
#define ENOSYS          38
#define ENODATA         61
#define ETIME           62
#define ENOLINK         67
#define EPROTO          71
#define EBADMSG         74
#define EOVERFLOW       75
#define EMSGSIZE        90
#define EOPNOTSUPP      95
#define ENETDOWN        100
#define ENOBUFS         105
#define ENOTCONN        107
#define ETIMEDOUT       110
#define EALREADY        114
#define EINPROGRESS     115
#define EDQUOT          122
#define ECANCELED       125
#define ENOTSUPP        524
#define MAX_ERRNO       4095
#define IS_ERR_VALUE(x) unlikely((uintptr_t)(void *)(x) >= (uintptr_t)-MAX_ERRNO)
static inline void *ERR_PTR (long error) { return (void *)(intptr_t)error; }
static inline long PTR_ERR (const void *ptr) { return (long)(intptr_t)ptr; }
static inline bool IS_ERR (const void *ptr) { return IS_ERR_VALUE((uintptr_t)ptr); }
static inline bool IS_ERR_OR_NULL (const void *ptr) { return !ptr || IS_ERR_VALUE((uintptr_t)ptr); }
static inline void *ERR_CAST (const void *ptr) { return (void *)ptr; }
static inline int PTR_ERR_OR_ZERO (const void *ptr) { return IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0; }

/* ---- strings (memcpy & co. come from ntoskrnl) ---- */
void *memcpy (void *d, const void *s, size_t n);
void *memmove (void *d, const void *s, size_t n);
void *memset (void *d, int c, size_t n);
int memcmp (const void *a, const void *b, size_t n);
void *memchr (const void *s, int c, size_t n);
size_t strlen (const char *s);
size_t strnlen (const char *s, size_t n);
int strcmp (const char *a, const char *b);
int strncmp (const char *a, const char *b, size_t n);
char *strchr (const char *s, int c);
char *strrchr (const char *s, int c);
char *strstr (const char *h, const char *n);
ssize_t strscpy (char *d, const char *s, size_t size);
#define strscpy_pad(d, s, n)    ({ memset(d, 0, n); strscpy(d, s, n); })
void *memchr_inv (const void *s, int c, size_t n);
#define memset_after(obj, v, member) \
	memset((u8 *)(obj) + offsetofend(typeof(*(obj)), member), v, \
	       sizeof(*(obj)) - offsetofend(typeof(*(obj)), member))
#define memzero_explicit(p, n)  memset(p, 0, n)
#define mem_is_zero(p, n)       (memchr_inv(p, 0, n) == NULL)
static inline const char *str_on_off (bool v) { return v ? "on" : "off"; }
static inline const char *str_enable_disable (bool v) { return v ? "enable" : "disable"; }
static inline const char *str_enabled_disabled (bool v) { return v ? "enabled" : "disabled"; }
static inline const char *str_yes_no (bool v) { return v ? "yes" : "no"; }
static inline const char *str_true_false (bool v) { return v ? "true" : "false"; }
static inline const char *str_read_write (bool v) { return v ? "read" : "write"; }

/* ---- printing: compat/printk.c has a vsnprintf with the Linux extensions rtw89 uses (%pM, %*ph, %pV; any other
 * %p extension, such as %pS, prints the pointer value) ---- */
struct va_format {
	const char *fmt;
	va_list *va;
};
/* own names, so the linker can never pick ntoskrnl's sprintf & co. (no %pM, %*ph there) */
#define vsnprintf       rtw_vsnprintf
#define snprintf        rtw_snprintf
#define scnprintf       rtw_scnprintf
#define vscnprintf      rtw_vscnprintf
#define sprintf         rtw_sprintf
#define kasprintf       rtw_kasprintf
#define sort            rtw_sort
int vsnprintf (char *buf, size_t size, const char *fmt, va_list ap);
int snprintf (char *buf, size_t size, const char *fmt, ...);
int scnprintf (char *buf, size_t size, const char *fmt, ...);
int vscnprintf (char *buf, size_t size, const char *fmt, va_list ap);
int sprintf (char *buf, const char *fmt, ...);
char *kasprintf (gfp_t gfp, const char *fmt, ...);
void rtw_printk (int level, const char *fmt, ...);
#define KERN_ERR                "\0010"
#define KERN_WARNING            "\0011"
#define KERN_INFO               "\0012"
#define KERN_DEBUG              "\0013"
#define KERN_CONT               ""
#define pr_err(fmt, ...)        rtw_printk(0, fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)       rtw_printk(1, fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)     rtw_printk(2, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)       rtw_printk(2, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)      do { } while (0)
#define pr_cont(fmt, ...)       rtw_printk(2, fmt, ##__VA_ARGS__)
#define printk(fmt, ...)        rtw_printk(2, fmt, ##__VA_ARGS__)
#define __rtw_once(stmt)        ({ static bool __done; bool __first = !__done; if (__first) { __done = true; stmt; } __first; })
#define pr_err_once(fmt, ...)   __rtw_once(pr_err(fmt, ##__VA_ARGS__))
#define pr_warn_once(fmt, ...)  __rtw_once(pr_warn(fmt, ##__VA_ARGS__))
#define pr_info_once(fmt, ...)  __rtw_once(pr_info(fmt, ##__VA_ARGS__))
#define pr_err_ratelimited(fmt, ...)  pr_err(fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(fmt, ...) pr_warn(fmt, ##__VA_ARGS__)
#define pr_info_ratelimited(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#define printk_ratelimit()      1
#define net_ratelimit()         1

void rtw_warn (const char *file, int line, const char *cond);
#define WARN_ON(cond)           ({ bool __c = !!(cond); if (unlikely(__c)) rtw_warn(__FILE__, __LINE__, #cond); unlikely(__c); })
#define WARN_ON_ONCE(cond)      ({ static bool __w; bool __c = !!(cond); if (unlikely(__c) && !__w) { __w = true; rtw_warn(__FILE__, __LINE__, #cond); } unlikely(__c); })
#define WARN(cond, fmt, ...)    ({ bool __c = !!(cond); if (unlikely(__c)) { rtw_warn(__FILE__, __LINE__, #cond); pr_warn(fmt, ##__VA_ARGS__); } unlikely(__c); })
#define WARN_ONCE(cond, fmt, ...) ({ static bool __w; bool __c = !!(cond); if (unlikely(__c) && !__w) { __w = true; WARN(1, fmt, ##__VA_ARGS__); } unlikely(__c); })
void rtw_bug (const char *file, int line) __attribute__((noreturn));
#define BUG()                   rtw_bug(__FILE__, __LINE__)
#define BUG_ON(cond)            do { if (unlikely(cond)) BUG(); } while (0)
#define might_sleep()           do { } while (0)
#define might_sleep_if(c)       do { } while (0)
#define lockdep_assert_held(l)  do { (void)(l); } while (0)
#define lockdep_assert_not_held(l) do { (void)(l); } while (0)
#define lockdep_assert(c)       do { } while (0)
#define assert_spin_locked(l)   do { (void)(l); } while (0)
#define lockdep_set_class(l, k) do { } while (0)

enum { DUMP_PREFIX_NONE, DUMP_PREFIX_ADDRESS, DUMP_PREFIX_OFFSET };
void print_hex_dump (const char *level, const char *prefix, int type, int rowsize, int groupsize,
		     const void *buf, size_t len, bool ascii);
#define print_hex_dump_debug(...) do { } while (0)
#define print_hex_dump_bytes(prefix, type, buf, len) print_hex_dump(KERN_DEBUG, prefix, type, 16, 1, buf, len, true)

/* ---- lists ---- */
struct list_head {
	struct list_head *next, *prev;
};
struct hlist_node {
	struct hlist_node *next, **pprev;
};
struct hlist_head {
	struct hlist_node *first;
};
#define LIST_HEAD_INIT(name)    { &(name), &(name) }
#define LIST_HEAD(name)         struct list_head name = LIST_HEAD_INIT(name)
static inline void INIT_LIST_HEAD (struct list_head *l) { l->next = l; l->prev = l; }
static inline void __list_add (struct list_head *n, struct list_head *prev, struct list_head *next)
{ next->prev = n; n->next = next; n->prev = prev; prev->next = n; }
static inline void list_add (struct list_head *n, struct list_head *h) { __list_add(n, h, h->next); }
static inline void list_add_tail (struct list_head *n, struct list_head *h) { __list_add(n, h->prev, h); }
static inline void __list_del (struct list_head *prev, struct list_head *next) { next->prev = prev; prev->next = next; }
static inline void list_del (struct list_head *e) { __list_del(e->prev, e->next); e->next = NULL; e->prev = NULL; }
static inline void list_del_init (struct list_head *e) { __list_del(e->prev, e->next); INIT_LIST_HEAD(e); }
static inline void list_replace (struct list_head *o, struct list_head *n)
{ n->next = o->next; n->next->prev = n; n->prev = o->prev; n->prev->next = n; }
static inline void list_move (struct list_head *e, struct list_head *h) { __list_del(e->prev, e->next); list_add(e, h); }
static inline void list_move_tail (struct list_head *e, struct list_head *h) { __list_del(e->prev, e->next); list_add_tail(e, h); }
static inline int list_empty (const struct list_head *h) { return READ_ONCE(h->next) == h; }
static inline int list_is_singular (const struct list_head *h) { return !list_empty(h) && h->next == h->prev; }
static inline int list_is_first (const struct list_head *e, const struct list_head *h) { return e->prev == h; }
static inline int list_is_last (const struct list_head *e, const struct list_head *h) { return e->next == h; }
static inline int list_is_head (const struct list_head *e, const struct list_head *h) { return e == h; }
static inline void __list_splice (const struct list_head *l, struct list_head *prev, struct list_head *next)
{ struct list_head *f = l->next, *t = l->prev; f->prev = prev; prev->next = f; t->next = next; next->prev = t; }
static inline void list_splice (const struct list_head *l, struct list_head *h) { if (!list_empty(l)) __list_splice(l, h, h->next); }
static inline void list_splice_tail (struct list_head *l, struct list_head *h) { if (!list_empty(l)) __list_splice(l, h->prev, h); }
static inline void list_splice_init (struct list_head *l, struct list_head *h)
{ if (!list_empty(l)) { __list_splice(l, h, h->next); INIT_LIST_HEAD(l); } }
static inline void list_splice_tail_init (struct list_head *l, struct list_head *h)
{ if (!list_empty(l)) { __list_splice(l, h->prev, h); INIT_LIST_HEAD(l); } }
static inline size_t list_count_nodes (struct list_head *h) { size_t n = 0; for (struct list_head *p = h->next; p != h; p = p->next) n++; return n; }
#define list_entry(node, type, field)          container_of(node, type, field)
#define list_first_entry(head, type, field)    container_of((head)->next, type, field)
#define list_last_entry(head, type, field)     container_of((head)->prev, type, field)
#define list_first_entry_or_null(ptr, type, member) \
	({ struct list_head *__lh = (ptr), *__ln = READ_ONCE(__lh->next); __ln == __lh ? NULL : list_entry(__ln, type, member); })
#define list_next_entry(e, field)              container_of((e)->field.next, typeof(*(e)), field)
#define list_prev_entry(e, field)              container_of((e)->field.prev, typeof(*(e)), field)
#define list_entry_is_head(e, head, field)     (&(e)->field == (head))
#define list_for_each(node, head)              for (node = (head)->next; node != (head); node = node->next)
#define list_for_each_safe(node, nx, head)     for (node = (head)->next, nx = node->next; node != (head); node = nx, nx = nx->next)
/* Entry iteration. __rtw_lent(node, pos, m): the entry that holds list node "node", typed like pos;
 * __rtw_lend(pos, head, m): pos is the list head itself (the walk is over). */
#define __rtw_lent(node, pos, m)               container_of(node, typeof(*(pos)), m)
#define __rtw_lend(pos, head, m)               (&(pos)->m == (head))
#define list_for_each_entry(pos, head, member) \
	for (pos = __rtw_lent((head)->next, pos, member); !__rtw_lend(pos, head, member); \
	     pos = __rtw_lent((pos)->member.next, pos, member))
#define list_for_each_entry_reverse(pos, head, member) \
	for (pos = __rtw_lent((head)->prev, pos, member); !__rtw_lend(pos, head, member); \
	     pos = __rtw_lent((pos)->member.prev, pos, member))
#define list_for_each_entry_continue(pos, head, member) \
	for (pos = __rtw_lent((pos)->member.next, pos, member); !__rtw_lend(pos, head, member); \
	     pos = __rtw_lent((pos)->member.next, pos, member))
#define list_for_each_entry_from(pos, head, member) \
	for (; !__rtw_lend(pos, head, member); pos = __rtw_lent((pos)->member.next, pos, member))
/* the _safe forms read the neighbour (n) before the body runs, so the body may unlink pos */
#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = __rtw_lent((head)->next, pos, member), n = __rtw_lent((pos)->member.next, pos, member); \
	     !__rtw_lend(pos, head, member); pos = n, n = __rtw_lent((n)->member.next, n, member))
#define list_for_each_entry_safe_reverse(pos, n, head, member) \
	for (pos = __rtw_lent((head)->prev, pos, member), n = __rtw_lent((pos)->member.prev, pos, member); \
	     !__rtw_lend(pos, head, member); pos = n, n = __rtw_lent((n)->member.prev, n, member))
#define list_for_each_entry_rcu(pos, head, member, ...) list_for_each_entry(pos, head, member)
#define list_add_rcu(n, h)          list_add(n, h)
#define list_add_tail_rcu(n, h)     list_add_tail(n, h)
#define list_del_rcu(e)             __list_del((e)->prev, (e)->next)
#define list_first_or_null_rcu(ptr, type, member) list_first_entry_or_null(ptr, type, member)
#define INIT_HLIST_HEAD(h)          ((h)->first = NULL)
#define HLIST_HEAD_INIT             { .first = NULL }
static inline void INIT_HLIST_NODE (struct hlist_node *n) { n->next = NULL; n->pprev = NULL; }
static inline int hlist_unhashed (const struct hlist_node *n) { return !n->pprev; }
static inline int hlist_empty (const struct hlist_head *h) { return !h->first; }
/* insert at the front: link the node completely, then publish it in h->first */
static inline void hlist_add_head (struct hlist_node *n, struct hlist_head *h)
{
	n->pprev = &h->first;
	n->next = h->first;
	if (n->next)
		n->next->pprev = &n->next;
	h->first = n;
}
/* unlink: the predecessor's link (*pprev) skips the node, the successor (if any) points back to that link */
static inline void hlist_del (struct hlist_node *n)
{
	if (n->next)
		n->next->pprev = n->pprev;
	*n->pprev = n->next;
	n->next = NULL;
	n->pprev = NULL;
}
static inline void hlist_del_init (struct hlist_node *n) { if (!hlist_unhashed(n)) { hlist_del(n); INIT_HLIST_NODE(n); } }
#define hlist_entry(ptr, type, member)  container_of(ptr, type, member)
#define hlist_entry_safe(ptr, type, member) ({ typeof(ptr) __hn = (ptr); __hn == NULL ? NULL : hlist_entry(__hn, type, member); })
/* __rtw_hent(node, pos, m): the entry that holds hlist node "node" (typed like pos), or NULL at the end */
#define __rtw_hent(node, pos, m)        hlist_entry_safe(node, typeof(*(pos)), m)
#define hlist_for_each_entry(pos, head, member) \
	for (pos = __rtw_hent((head)->first, pos, member); pos != NULL; pos = __rtw_hent((pos)->member.next, pos, member))
#define hlist_for_each_entry_safe(pos, n, head, member) \
	for (pos = __rtw_hent((head)->first, pos, member); pos != NULL && ((n = (pos)->member.next), 1); \
	     pos = __rtw_hent(n, pos, member))

/* ---- atomics ---- */
typedef struct { int counter; } atomic_t;
typedef struct { s64 counter; } atomic64_t;
typedef struct { atomic_t refs; } refcount_t;
#define ATOMIC_INIT(i)                  { (i) }
#define atomic_read(v)                  __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic_set(v, i)                __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)
#define atomic_add(i, v)                ((void)__atomic_fetch_add(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic_sub(i, v)                ((void)__atomic_fetch_sub(&(v)->counter, (i), __ATOMIC_RELAXED))
#define atomic_inc(v)                   atomic_add(1, v)
#define atomic_dec(v)                   atomic_sub(1, v)
#define atomic_add_return(i, v)         __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_sub_return(i, v)         __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_inc_return(v)            atomic_add_return(1, v)
#define atomic_dec_return(v)            atomic_sub_return(1, v)
#define atomic_dec_and_test(v)          (atomic_dec_return(v) == 0)
#define atomic_inc_and_test(v)          (atomic_inc_return(v) == 0)
#define atomic_fetch_add(i, v)          __atomic_fetch_add(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_xchg(v, n)               __atomic_exchange_n(&(v)->counter, (n), __ATOMIC_SEQ_CST)
#define atomic_cmpxchg(v, o, n)         ({ typeof((v)->counter) __o = (o); __atomic_compare_exchange_n(&(v)->counter, &__o, (n), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); __o; })
#define atomic64_read(v)                atomic_read(v)
#define atomic64_set(v, i)              atomic_set(v, i)
#define atomic64_inc(v)                 atomic_inc(v)
#define atomic64_add(i, v)              atomic_add(i, v)
#define atomic64_inc_return(v)          atomic_inc_return(v)
#define atomic64_add_return(i, v)       atomic_add_return(i, v)
#define cmpxchg(p, o, n)                ({ typeof(*(p)) __o = (o); __atomic_compare_exchange_n(p, &__o, (n), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); __o; })
#define xchg(p, n)                      __atomic_exchange_n(p, n, __ATOMIC_SEQ_CST)
#define refcount_set(r, n)              atomic_set(&(r)->refs, n)
#define refcount_read(r)                atomic_read(&(r)->refs)
#define refcount_inc(r)                 atomic_inc(&(r)->refs)
#define refcount_dec(r)                 atomic_dec(&(r)->refs)
#define refcount_dec_and_test(r)        atomic_dec_and_test(&(r)->refs)
static inline bool refcount_inc_not_zero (refcount_t *r)
{ int c = atomic_read(&r->refs); while (c) { if (__atomic_compare_exchange_n(&r->refs.counter, &c, c + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true; } return false; }

/* ---- bitmaps (unsigned long words = 32 bits) ---- */
#define DECLARE_BITMAP(name, bits)      unsigned long name[BITS_TO_LONGS(bits)]
static inline void set_bit (unsigned long nr, volatile unsigned long *a) { __atomic_fetch_or(&a[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_RELAXED); }
static inline void clear_bit (unsigned long nr, volatile unsigned long *a) { __atomic_fetch_and(&a[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_RELAXED); }
static inline void change_bit (unsigned long nr, volatile unsigned long *a) { __atomic_fetch_xor(&a[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_RELAXED); }
static inline bool test_bit (unsigned long nr, const volatile unsigned long *a) { return (a[BIT_WORD(nr)] >> (nr % BITS_PER_LONG)) & 1; }
static inline bool test_and_set_bit (unsigned long nr, volatile unsigned long *a)
{ return (__atomic_fetch_or(&a[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline bool test_and_clear_bit (unsigned long nr, volatile unsigned long *a)
{ return (__atomic_fetch_and(&a[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline void __set_bit (unsigned long nr, volatile unsigned long *a) { a[BIT_WORD(nr)] |= BIT_MASK(nr); }
static inline void __clear_bit (unsigned long nr, volatile unsigned long *a) { a[BIT_WORD(nr)] &= ~BIT_MASK(nr); }
static inline void __assign_bit (unsigned long nr, volatile unsigned long *a, bool v) { if (v) __set_bit(nr, a); else __clear_bit(nr, a); }
#define assign_bit(nr, a, v)            ((v) ? set_bit(nr, a) : clear_bit(nr, a))
static inline bool __test_and_set_bit (unsigned long nr, volatile unsigned long *a) { bool o = test_bit(nr, a); __set_bit(nr, a); return o; }
static inline bool __test_and_clear_bit (unsigned long nr, volatile unsigned long *a) { bool o = test_bit(nr, a); __clear_bit(nr, a); return o; }
unsigned long find_next_bit (const unsigned long *a, unsigned long size, unsigned long off);
unsigned long find_next_zero_bit (const unsigned long *a, unsigned long size, unsigned long off);
unsigned long find_last_bit (const unsigned long *a, unsigned long size);
#define find_first_bit(a, size)         find_next_bit(a, size, 0)
#define find_first_zero_bit(a, size)    find_next_zero_bit(a, size, 0)
#define for_each_set_bit(bit, a, size) \
	for ((bit) = find_next_bit((a), (size), 0); (bit) < (size); (bit) = find_next_bit((a), (size), (bit) + 1))
#define for_each_clear_bit(bit, a, size) \
	for ((bit) = find_next_zero_bit((a), (size), 0); (bit) < (size); (bit) = find_next_zero_bit((a), (size), (bit) + 1))
static inline void bitmap_zero (unsigned long *d, unsigned int n) { memset(d, 0, BITS_TO_LONGS(n) * sizeof(long)); }
static inline void bitmap_fill (unsigned long *d, unsigned int n)
{ memset(d, 0xff, BITS_TO_LONGS(n) * sizeof(long)); if (n % BITS_PER_LONG) d[BIT_WORD(n)] &= BIT_MASK(n) - 1; }
static inline void bitmap_copy (unsigned long *d, const unsigned long *s, unsigned int n) { memcpy(d, s, BITS_TO_LONGS(n) * sizeof(long)); }
int bitmap_weight (const unsigned long *a, unsigned int n);
bool bitmap_empty (const unsigned long *a, unsigned int n);
bool bitmap_full (const unsigned long *a, unsigned int n);
bool bitmap_equal (const unsigned long *a, const unsigned long *b, unsigned int n);
bool bitmap_and (unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n);
void bitmap_or (unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n);
bool bitmap_andnot (unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n);
bool bitmap_intersects (const unsigned long *a, const unsigned long *b, unsigned int n);
void bitmap_set (unsigned long *a, unsigned int start, unsigned int n);
void bitmap_clear (unsigned long *a, unsigned int start, unsigned int n);
void bitmap_from_arr32 (unsigned long *d, const u32 *s, unsigned int n);
void bitmap_to_arr32 (u32 *d, const unsigned long *s, unsigned int n);

/* ---- misc ---- */
void sort (void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
	   void (*swap_fn)(void *, void *, int));
u32 get_random_u32 (void);
static inline u8 get_random_u8 (void) { return (u8)get_random_u32(); }
static inline u16 get_random_u16 (void) { return (u16)get_random_u32(); }
static inline u32 get_random_u32_below (u32 ceil) { return ceil ? get_random_u32() % ceil : 0; }
static inline u32 prandom_u32_max (u32 ceil) { return get_random_u32_below(ceil); }
void get_random_bytes (void *buf, size_t n);
u32 crc32_le (u32 crc, const u8 *p, size_t len);
#define crc32(seed, p, len)             crc32_le(seed, p, len)

#endif
