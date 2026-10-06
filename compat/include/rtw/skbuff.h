/*
 * rtw89win Linux compatibility: sk_buff and queues, net_device/NAPI (only what rtw89 touches), Ethernet/IP headers.
 * skb data buffers come from the DMA heap when possible (compat/skbuff.c), so dma_map_single() on skb->data needs
 * no bounce copy.
 * Copyright (c) 2026 Meari
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RTW_SKBUFF_H
#define RTW_SKBUFF_H

#include <rtw/base.h>
#include <rtw/sched.h>
#include <rtw/mem.h>

struct net_device;

struct sk_buff {
	union {
		struct {
			struct sk_buff *next;
			struct sk_buff *prev;
		};
		struct list_head list;
	};
	struct net_device *dev;
	char cb[64] __aligned(8);     /* FreeBSD LinuxKPI size; ieee80211_tx_info there is > 48 bytes */
	unsigned int len;
	unsigned int data_len;
	u16 queue_mapping;
	u16 mac_header;
	u16 network_header;
	u16 transport_header;
	__be16 protocol;
	u32 priority;
	unsigned int truesize;
	u8 *head;
	u8 *data;
	u8 *tail;
	u8 *end;
	u8 dma_heap;            /* head came from rtwk_dma_heap_alloc */
	u32 mac_tag;            /* mac layer (mac/tx.c): what a frame it sent is; rtw89 never touches it, unlike cb */
	struct list_head all;   /* every live skb (compat/skbuff.c), for rtw_skb_reclaim() */
};
void rtw_skb_reclaim (void);    /* frees the skbs left once the driver is gone */

struct sk_buff_head {
	struct sk_buff *next;
	struct sk_buff *prev;
	u32 qlen;
	spinlock_t lock;
};

#define NET_SKB_PAD             64
#define NET_IP_ALIGN            0

struct sk_buff *__alloc_skb (unsigned int size, gfp_t gfp);
#define alloc_skb(size, gfp)            __alloc_skb(size, gfp)
#define dev_alloc_skb(len)              __dev_alloc_skb(len, GFP_ATOMIC)
struct sk_buff *__dev_alloc_skb (unsigned int len, gfp_t gfp);
#define netdev_alloc_skb(dev, len)      __dev_alloc_skb(len, GFP_ATOMIC)
#define __netdev_alloc_skb(dev, len, g) __dev_alloc_skb(len, g)
void kfree_skb (struct sk_buff *skb);
#define dev_kfree_skb(skb)              kfree_skb(skb)
#define dev_kfree_skb_any(skb)          kfree_skb(skb)
#define dev_kfree_skb_irq(skb)          kfree_skb(skb)
#define consume_skb(skb)                kfree_skb(skb)
#define kfree_skb_reason(skb, r)        kfree_skb(skb)
struct sk_buff *skb_copy (const struct sk_buff *skb, gfp_t gfp);
struct sk_buff *skb_copy_expand (const struct sk_buff *skb, int headroom, int tailroom, gfp_t gfp);
struct sk_buff *skb_clone (struct sk_buff *skb, gfp_t gfp);
int pskb_expand_head (struct sk_buff *skb, int nhead, int ntail, gfp_t gfp);
void skb_trim (struct sk_buff *skb, unsigned int len);
#define __skb_trim(skb, len)            skb_trim(skb, len)

static inline unsigned int skb_headroom (const struct sk_buff *skb) { return (unsigned int)(skb->data - skb->head); }
static inline int skb_tailroom (const struct sk_buff *skb) { return (int)(skb->end - skb->tail); }
static inline u8 *skb_tail_pointer (const struct sk_buff *skb) { return skb->tail; }
static inline void skb_reset_tail_pointer (struct sk_buff *skb) { skb->tail = skb->data; }
static inline void skb_set_tail_pointer (struct sk_buff *skb, int off) { skb->tail = skb->data + off; }
static inline void skb_reserve (struct sk_buff *skb, int len) { skb->data += len; skb->tail += len; }
void *skb_put (struct sk_buff *skb, unsigned int len);
#define __skb_put(skb, len)             skb_put(skb, len)
static inline void *skb_put_data (struct sk_buff *skb, const void *d, unsigned int len) { void *p = skb_put(skb, len); memcpy(p, d, len); return p; }
static inline void *skb_put_zero (struct sk_buff *skb, unsigned int len) { void *p = skb_put(skb, len); memset(p, 0, len); return p; }
static inline void skb_put_u8 (struct sk_buff *skb, u8 v) { *(u8 *)skb_put(skb, 1) = v; }
void *skb_push (struct sk_buff *skb, unsigned int len);
#define __skb_push(skb, len)            skb_push(skb, len)
void *skb_pull (struct sk_buff *skb, unsigned int len);
#define __skb_pull(skb, len)            skb_pull(skb, len)
static inline void *skb_pull_data (struct sk_buff *skb, size_t len) { void *d = skb->data; if (skb->len < len) return NULL; skb_pull(skb, (unsigned int)len); return d; }
static inline bool skb_is_nonlinear (const struct sk_buff *skb) { return skb->data_len != 0; }
static inline int skb_linearize (struct sk_buff *skb) { (void)skb; return 0; }
static inline unsigned int skb_headlen (const struct sk_buff *skb) { return skb->len - skb->data_len; }
static inline u16 skb_get_queue_mapping (const struct sk_buff *skb) { return skb->queue_mapping; }
static inline void skb_set_queue_mapping (struct sk_buff *skb, u16 q) { skb->queue_mapping = q; }
static inline u8 *skb_mac_header (const struct sk_buff *skb) { return skb->head + skb->mac_header; }
static inline u8 *skb_network_header (const struct sk_buff *skb) { return skb->head + skb->network_header; }
static inline u8 *skb_transport_header (const struct sk_buff *skb) { return skb->head + skb->transport_header; }
static inline void skb_reset_mac_header (struct sk_buff *skb) { skb->mac_header = (u16)(skb->data - skb->head); }
static inline void skb_reset_network_header (struct sk_buff *skb) { skb->network_header = (u16)(skb->data - skb->head); }
static inline void skb_set_network_header (struct sk_buff *skb, int off) { skb->network_header = (u16)(skb->data - skb->head + off); }
static inline void skb_reset_transport_header (struct sk_buff *skb) { skb->transport_header = (u16)(skb->data - skb->head); }
static inline void skb_set_transport_header (struct sk_buff *skb, int off) { skb->transport_header = (u16)(skb->data - skb->head + off); }
static inline int skb_network_offset (const struct sk_buff *skb) { return (int)(skb_network_header(skb) - skb->data); }
static inline void skb_copy_from_linear_data (const struct sk_buff *skb, void *to, unsigned int len) { memcpy(to, skb->data, len); }
static inline int skb_copy_bits (const struct sk_buff *skb, int off, void *to, int len) { memcpy(to, skb->data + off, len); return 0; }
#define skb_get(skb)                    (skb)

/* queues */
static inline void __skb_queue_head_init (struct sk_buff_head *q) { q->prev = q->next = (struct sk_buff *)q; q->qlen = 0; }
static inline void skb_queue_head_init (struct sk_buff_head *q) { spin_lock_init(&q->lock); __skb_queue_head_init(q); }
static inline u32 skb_queue_len (const struct sk_buff_head *q) { return q->qlen; }
static inline u32 skb_queue_len_lockless (const struct sk_buff_head *q) { return READ_ONCE(q->qlen); }
static inline bool skb_queue_empty (const struct sk_buff_head *q) { return q->next == (const struct sk_buff *)q; }
static inline bool skb_queue_empty_lockless (const struct sk_buff_head *q) { return READ_ONCE(q->next) == (const struct sk_buff *)q; }
static inline struct sk_buff *skb_peek (const struct sk_buff_head *q) { struct sk_buff *s = q->next; return s == (const struct sk_buff *)q ? NULL : s; }
static inline struct sk_buff *skb_peek_tail (const struct sk_buff_head *q) { struct sk_buff *s = q->prev; return s == (const struct sk_buff *)q ? NULL : s; }
static inline void __skb_insert (struct sk_buff *n, struct sk_buff *prev, struct sk_buff *next, struct sk_buff_head *q)
{ n->next = next; n->prev = prev; next->prev = n; prev->next = n; q->qlen++; }
static inline void __skb_queue_tail (struct sk_buff_head *q, struct sk_buff *n) { __skb_insert(n, q->prev, (struct sk_buff *)q, q); }
static inline void __skb_queue_head (struct sk_buff_head *q, struct sk_buff *n) { __skb_insert(n, (struct sk_buff *)q, q->next, q); }
static inline void __skb_unlink (struct sk_buff *skb, struct sk_buff_head *q)
{ struct sk_buff *next = skb->next, *prev = skb->prev; q->qlen--; skb->next = skb->prev = NULL; next->prev = prev; prev->next = next; }
static inline struct sk_buff *__skb_dequeue (struct sk_buff_head *q) { struct sk_buff *s = skb_peek(q); if (s) __skb_unlink(s, q); return s; }
static inline struct sk_buff *__skb_dequeue_tail (struct sk_buff_head *q) { struct sk_buff *s = skb_peek_tail(q); if (s) __skb_unlink(s, q); return s; }
void skb_queue_tail (struct sk_buff_head *q, struct sk_buff *n);
void skb_queue_head (struct sk_buff_head *q, struct sk_buff *n);
struct sk_buff *skb_dequeue (struct sk_buff_head *q);
struct sk_buff *skb_dequeue_tail (struct sk_buff_head *q);
void skb_unlink (struct sk_buff *skb, struct sk_buff_head *q);
void skb_queue_purge (struct sk_buff_head *q);
static inline void __skb_queue_purge (struct sk_buff_head *q) { struct sk_buff *s; while ((s = __skb_dequeue(q))) kfree_skb(s); }
static inline void __skb_queue_splice (const struct sk_buff_head *l, struct sk_buff *prev, struct sk_buff *next)
{ struct sk_buff *f = l->next, *t = l->prev; f->prev = prev; prev->next = f; t->next = next; next->prev = t; }
static inline void skb_queue_splice (const struct sk_buff_head *l, struct sk_buff_head *h)
{ if (!skb_queue_empty(l)) { __skb_queue_splice(l, (struct sk_buff *)h, h->next); h->qlen += l->qlen; } }
static inline void skb_queue_splice_init (struct sk_buff_head *l, struct sk_buff_head *h)
{ if (!skb_queue_empty(l)) { __skb_queue_splice(l, (struct sk_buff *)h, h->next); h->qlen += l->qlen; __skb_queue_head_init(l); } }
static inline void skb_queue_splice_tail (const struct sk_buff_head *l, struct sk_buff_head *h)
{ if (!skb_queue_empty(l)) { __skb_queue_splice(l, h->prev, (struct sk_buff *)h); h->qlen += l->qlen; } }
static inline void skb_queue_splice_tail_init (struct sk_buff_head *l, struct sk_buff_head *h)
{ if (!skb_queue_empty(l)) { __skb_queue_splice(l, h->prev, (struct sk_buff *)h); h->qlen += l->qlen; __skb_queue_head_init(l); } }
#define skb_queue_walk(q, skb) \
	for (skb = (q)->next; skb != (struct sk_buff *)(q); skb = skb->next)
#define skb_queue_walk_safe(q, skb, tmp) \
	for (skb = (q)->next, tmp = skb->next; skb != (struct sk_buff *)(q); skb = tmp, tmp = skb->next)
#define skb_queue_reverse_walk(q, skb) \
	for (skb = (q)->prev; skb != (struct sk_buff *)(q); skb = skb->prev)

/* ---- net_device / NAPI ---- */
#define IFNAMSIZ                16
#define NAPI_POLL_WEIGHT        64
struct napi_struct;
struct net_device {
	char name[IFNAMSIZ];
	u8 dev_addr[6];
	void *priv;
	void *ieee80211_ptr;
	struct list_head napi_list;
	unsigned long state;
	unsigned int mtu;
};
struct napi_struct {
	struct list_head dev_list;
	struct net_device *dev;
	int (*poll)(struct napi_struct *, int);
	int weight;
	unsigned long state;            /* bit 0 scheduled, bit 1 disabled, bit 2 polling */
	void *os;                       /* compat/netdev.c */
};
struct net_device *alloc_netdev_dummy (int sizeof_priv);
void free_netdev (struct net_device *dev);
static inline void *netdev_priv (const struct net_device *dev) { return dev->priv; }
#define init_dummy_netdev(dev)          do { } while (0)
void netif_napi_add (struct net_device *dev, struct napi_struct *napi, int (*poll)(struct napi_struct *, int));
#define netif_napi_add_weight(dev, napi, poll, w) netif_napi_add(dev, napi, poll)
#define netif_napi_add_tx(dev, napi, poll) netif_napi_add(dev, napi, poll)
void netif_napi_del (struct napi_struct *napi);
void napi_enable (struct napi_struct *napi);
void napi_disable (struct napi_struct *napi);
bool napi_schedule_prep (struct napi_struct *napi);
void __napi_schedule (struct napi_struct *napi);
static inline bool napi_schedule (struct napi_struct *napi) { if (napi_schedule_prep(napi)) { __napi_schedule(napi); return true; } return false; }
bool napi_complete_done (struct napi_struct *napi, int work_done);
#define napi_complete(n)                napi_complete_done(n, 0)
static inline bool napi_is_scheduled (struct napi_struct *napi) { return test_bit(0, &napi->state); }
void napi_synchronize (struct napi_struct *napi);
#define napi_gro_receive(napi, skb)     netif_receive_skb(skb)
int netif_receive_skb (struct sk_buff *skb);

/* ---- Ethernet / IP ---- */
#define ETH_ALEN                6
#define ETH_HLEN                14
#define ETH_FCS_LEN             4
#define ETH_DATA_LEN            1500
#define ETH_FRAME_LEN           1514
#define ETH_P_IP                0x0800
#define ETH_P_ARP               0x0806
#define ETH_P_IPV6              0x86DD
#define ETH_P_PAE               0x888E
#define ETH_P_TDLS              0x890D
#define ETH_P_802_3             0x0001
#define ETH_P_80221             0x8917
#define ETH_P_802_3_MIN         0x0600
struct ethhdr {
	u8 h_dest[ETH_ALEN];
	u8 h_source[ETH_ALEN];
	__be16 h_proto;
} __packed;
static inline struct ethhdr *eth_hdr (const struct sk_buff *skb) { return (struct ethhdr *)skb_mac_header(skb); }
static inline bool is_zero_ether_addr (const u8 *a) { return !(a[0] | a[1] | a[2] | a[3] | a[4] | a[5]); }
static inline bool is_multicast_ether_addr (const u8 *a) { return a[0] & 0x01; }
static inline bool is_unicast_ether_addr (const u8 *a) { return !is_multicast_ether_addr(a); }
static inline bool is_broadcast_ether_addr (const u8 *a) { return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xff; }
static inline bool is_valid_ether_addr (const u8 *a) { return !is_multicast_ether_addr(a) && !is_zero_ether_addr(a); }
static inline bool is_local_ether_addr (const u8 *a) { return a[0] & 0x02; }
static inline bool ether_addr_equal (const u8 *a, const u8 *b) { return !memcmp(a, b, ETH_ALEN); }
static inline bool ether_addr_equal_unaligned (const u8 *a, const u8 *b) { return ether_addr_equal(a, b); }
static inline bool ether_addr_equal_masked (const u8 *a, const u8 *b, const u8 *m)
{ for (int i = 0; i < ETH_ALEN; i++) if ((a[i] ^ b[i]) & m[i]) return false; return true; }
static inline void ether_addr_copy (u8 *d, const u8 *s) { memcpy(d, s, ETH_ALEN); }
static inline void eth_zero_addr (u8 *a) { memset(a, 0, ETH_ALEN); }
static inline void eth_broadcast_addr (u8 *a) { memset(a, 0xff, ETH_ALEN); }
static inline void eth_random_addr (u8 *a) { get_random_bytes(a, ETH_ALEN); a[0] &= 0xfe; a[0] |= 0x02; }
static inline u64 ether_addr_to_u64 (const u8 *a) { u64 u = 0; for (int i = 0; i < ETH_ALEN; i++) u = u << 8 | a[i]; return u; }
static inline void u64_to_ether_addr (u64 u, u8 *a) { for (int i = ETH_ALEN - 1; i >= 0; i--) { a[i] = (u8)u; u >>= 8; } }
static inline void eth_hw_addr_set (struct net_device *dev, const u8 *a) { memcpy(dev->dev_addr, a, ETH_ALEN); }

#define IPPROTO_ICMP            1
#define IPPROTO_TCP             6
#define IPPROTO_UDP             17
#define IPPROTO_ICMPV6          58
struct iphdr {
	u8 ihl:4, version:4;
	u8 tos;
	__be16 tot_len;
	__be16 id;
	__be16 frag_off;
	u8 ttl;
	u8 protocol;
	__sum16 check;
	__be32 saddr;
	__be32 daddr;
};
struct udphdr {
	__be16 source;
	__be16 dest;
	__be16 len;
	__sum16 check;
};
struct tcphdr {
	__be16 source;
	__be16 dest;
	__be32 seq;
	__be32 ack_seq;
	u16 res1:4, doff:4, fin:1, syn:1, rst:1, psh:1, ack:1, urg:1, ece:1, cwr:1;
	__be16 window;
	__sum16 check;
	__be16 urg_ptr;
};
static inline struct iphdr *ip_hdr (const struct sk_buff *skb) { return (struct iphdr *)skb_network_header(skb); }
static inline struct udphdr *udp_hdr (const struct sk_buff *skb) { return (struct udphdr *)skb_transport_header(skb); }
static inline struct tcphdr *tcp_hdr (const struct sk_buff *skb) { return (struct tcphdr *)skb_transport_header(skb); }
#define ARPHRD_ETHER            1
#define ARPHRD_IEEE80211        801
#define ARPOP_REQUEST           1
#define ARPOP_REPLY             2
struct arphdr {
	__be16 ar_hrd;
	__be16 ar_pro;
	u8 ar_hln;
	u8 ar_pln;
	__be16 ar_op;
};
struct in6_addr {
	union {
		u8 u6_addr8[16];
		__be16 u6_addr16[8];
		__be32 u6_addr32[4];
	} in6_u;
};
#define s6_addr                 in6_u.u6_addr8
struct in_addr {
	__be32 s_addr;
};

#endif
