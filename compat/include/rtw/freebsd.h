/*
 * rtw89win: the few FreeBSD kernel macros used by the 802.11 headers taken from FreeBSD
 * (compat/include: linux/ieee80211.h, linux/nl80211.h, net/mac80211.h, net/cfg80211.h, net/regulatory.h, net80211/).
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_FREEBSD_H
#define RTW_FREEBSD_H

#include <rtw/base.h>

#define WANT_NET80211           1       /* net80211/ieee80211.h: the protocol structures, without _KERNEL */
#define CACHE_LINE_SIZE         64
#define nitems(x)               ARRAY_SIZE(x)
#define CTASSERT(x)             _Static_assert(x, #x)
#define __DECONST(type, var)    ((type)(uintptr_t)(const void *)(var))
#define KASSERT(exp, msg)       do { if (unlikely(!(exp))) rtw_warn(__FILE__, __LINE__, #exp); } while (0)
#define panic(fmt, ...)         BUG()
#define printf(fmt, ...)        rtw_printk(2, fmt, ##__VA_ARGS__)
#define htole16(x)              ((u16)(x))
#define htole32(x)              ((u32)(x))
#define htole64(x)              ((u64)(x))
#define le16toh(x)              ((u16)(x))
#define le32toh(x)              ((u32)(x))
#define le64toh(x)              ((u64)(x))
#define htobe16(x)              __builtin_bswap16(x)
#define be16toh(x)              __builtin_bswap16(x)
#define _ALIGN(x)               ALIGN(x, 8)

/* Linux types the FreeBSD headers expect from LinuxKPI headers we do not take */
typedef u64 netdev_features_t;
struct inet6_dev;
struct net_device_path;
struct net_device_path_ctx;
struct netdev_hw_addr_list;
#define ETHTOOL_FWVERS_LEN      32
struct ethtool_stats {
	u32 cmd;
	u32 n_stats;
	u64 data[];
};
struct ethtool_drvinfo {
	u32 cmd;
	char driver[32];
	char version[32];
	char fw_version[ETHTOOL_FWVERS_LEN];
};

#endif
