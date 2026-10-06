// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win compat: sk_buff. Linear buffers only. The data area comes from the DMA heap when it can
 * (rtwk_dma_heap_alloc), so dma_map_single() of skb data is a lookup instead of a bounce copy.
 */
#include <rtw/linux.h>

static u8 *alloc_data (unsigned int size, u8 *from_heap)
{
	u8 *p = rtwk_dma_heap_alloc(size);

	*from_heap = p != NULL;
	return p ? p : rtwk_alloc(size, 0);
}

static void free_data (u8 *p, u8 from_heap)
{
	if (from_heap)
		rtwk_dma_heap_free(p);
	else
		rtwk_free(p);
}

/*
 * Every live skb is on skb_all, so that rtw_skb_reclaim() can free what is left once rtw89 is gone. rtw89 leaks
 * C2H events: one queued while wiphy->mtx is held (e.g. during start) stays in rtwdev->c2h_queue when
 * rtw89_core_stop() cancels the C2H work, and nothing frees that queue. Its data is a DMA heap block, and a heap
 * with blocks in use is never given back (driver\os.c), so the next probe would fail.
 */
static LIST_HEAD(skb_all);
static spinlock_t skb_all_lock;         /* zero = unlocked */

struct sk_buff *__alloc_skb (unsigned int size, gfp_t gfp)
{
	struct sk_buff *skb = kzalloc(sizeof(*skb), gfp);
	unsigned long flags;

	if (!skb)
		return NULL;
	size = ALIGN(size ? size : 1, 8);
	skb->head = alloc_data(size, &skb->dma_heap);
	if (!skb->head) {
		kfree(skb);
		return NULL;
	}
	skb->data = skb->tail = skb->head;
	skb->end = skb->head + size;
	skb->truesize = size + sizeof(*skb);
	spin_lock_irqsave(&skb_all_lock, flags);
	list_add_tail(&skb->all, &skb_all);
	spin_unlock_irqrestore(&skb_all_lock, flags);
	return skb;
}

struct sk_buff *__dev_alloc_skb (unsigned int len, gfp_t gfp)
{
	struct sk_buff *skb = __alloc_skb(len + NET_SKB_PAD, gfp);

	if (skb)
		skb_reserve(skb, NET_SKB_PAD);
	return skb;
}

void kfree_skb (struct sk_buff *skb)
{
	unsigned long flags;

	if (!skb)
		return;
	spin_lock_irqsave(&skb_all_lock, flags);
	list_del(&skb->all);
	spin_unlock_irqrestore(&skb_all_lock, flags);
	free_data(skb->head, skb->dma_heap);
	kfree(skb);
}

/* After the pci_driver remove and devres release: rtwdev is freed, so no skb left is reachable. PASSIVE_LEVEL. */
void rtw_skb_reclaim (void)
{
	struct sk_buff *skb;
	unsigned long flags;
	u8 first[8];
	u32 n = 0, first_len = 0;

	for (;;) {
		spin_lock_irqsave(&skb_all_lock, flags);
		skb = list_first_entry_or_null(&skb_all, struct sk_buff, all);
		if (skb)
			list_del(&skb->all);
		spin_unlock_irqrestore(&skb_all_lock, flags);
		if (!skb)
			break;
		if (!n++) {
			first_len = skb->len;
			memcpy(first, skb->data, min(skb->len, 8u));
		}
		free_data(skb->head, skb->dma_heap);
		kfree(skb);
	}
	/* only the first 8 bytes (a C2H/H2C header tells what it was): further on, an H2C left behind could carry key
	   material (security CAM entries), a data frame user data */
	if (n)
		pr_info("rtw89: freed %u skbs it left behind (first: %u bytes, %*ph)", n, first_len,
			(int)min(first_len, 8u), first);
}

void *skb_put (struct sk_buff *skb, unsigned int len)
{
	u8 *p = skb->tail;

	if (unlikely(skb->tail + len > skb->end)) {
		rtw_printk(0, "skb_put over end: len %u + %u, room %d", skb->len, len, skb_tailroom(skb));
		BUG();
	}
	skb->tail += len;
	skb->len += len;
	return p;
}

void *skb_push (struct sk_buff *skb, unsigned int len)
{
	if (unlikely(skb->data - len < skb->head)) {
		rtw_printk(0, "skb_push under head: %u, headroom %u", len, skb_headroom(skb));
		BUG();
	}
	skb->data -= len;
	skb->len += len;
	return skb->data;
}

void *skb_pull (struct sk_buff *skb, unsigned int len)
{
	if (len > skb->len)
		return NULL;
	skb->len -= len;
	skb->data += len;
	return skb->data;
}

void skb_trim (struct sk_buff *skb, unsigned int len)
{
	if (skb->len > len) {
		skb->len = len;
		skb->tail = skb->data + len;
	}
}

static void copy_header (struct sk_buff *n, const struct sk_buff *o)
{
	memcpy(n->cb, o->cb, sizeof(n->cb));
	n->dev = o->dev;
	n->protocol = o->protocol;
	n->priority = o->priority;
	n->queue_mapping = o->queue_mapping;
	n->mac_header = o->mac_header;
	n->network_header = o->network_header;
	n->transport_header = o->transport_header;
}

struct sk_buff *skb_copy_expand (const struct sk_buff *skb, int headroom, int tailroom, gfp_t gfp)
{
	struct sk_buff *n = __alloc_skb(headroom + skb->len + tailroom, gfp);
	long shift = headroom - (long)skb_headroom(skb);

	if (!n)
		return NULL;
	skb_reserve(n, headroom);
	skb_put_data(n, skb->data, skb->len);
	copy_header(n, skb);
	n->mac_header = (u16)(n->mac_header + shift);
	n->network_header = (u16)(n->network_header + shift);
	n->transport_header = (u16)(n->transport_header + shift);
	return n;
}

struct sk_buff *skb_copy (const struct sk_buff *skb, gfp_t gfp)
{
	return skb_copy_expand(skb, skb_headroom(skb), skb_tailroom(skb), gfp);
}

/* no shared data: a clone is a copy */
struct sk_buff *skb_clone (struct sk_buff *skb, gfp_t gfp)
{
	return skb_copy(skb, gfp);
}

int pskb_expand_head (struct sk_buff *skb, int nhead, int ntail, gfp_t gfp)
{
	unsigned int size = (unsigned int)(skb->end - skb->head) + nhead + ntail;
	u8 heap;
	u8 *head = alloc_data(ALIGN(size, 8), &heap);

	(void)gfp;
	if (!head)
		return -ENOMEM;
	memcpy(head + nhead, skb->head, skb->tail - skb->head);
	skb->data = head + nhead + (skb->data - skb->head);
	skb->tail = head + nhead + (skb->tail - skb->head);
	free_data(skb->head, skb->dma_heap);
	skb->head = head;
	skb->end = head + ALIGN(size, 8);
	skb->dma_heap = heap;
	skb->mac_header = (u16)(skb->mac_header + nhead);
	skb->network_header = (u16)(skb->network_header + nhead);
	skb->transport_header = (u16)(skb->transport_header + nhead);
	return 0;
}

void skb_queue_tail (struct sk_buff_head *q, struct sk_buff *n)
{
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	__skb_queue_tail(q, n);
	spin_unlock_irqrestore(&q->lock, flags);
}

void skb_queue_head (struct sk_buff_head *q, struct sk_buff *n)
{
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	__skb_queue_head(q, n);
	spin_unlock_irqrestore(&q->lock, flags);
}

struct sk_buff *skb_dequeue (struct sk_buff_head *q)
{
	struct sk_buff *skb;
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	skb = __skb_dequeue(q);
	spin_unlock_irqrestore(&q->lock, flags);
	return skb;
}

struct sk_buff *skb_dequeue_tail (struct sk_buff_head *q)
{
	struct sk_buff *skb;
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	skb = __skb_dequeue_tail(q);
	spin_unlock_irqrestore(&q->lock, flags);
	return skb;
}

void skb_unlink (struct sk_buff *skb, struct sk_buff_head *q)
{
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	__skb_unlink(skb, q);
	spin_unlock_irqrestore(&q->lock, flags);
}

void skb_queue_purge (struct sk_buff_head *q)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(q)))
		kfree_skb(skb);
}
