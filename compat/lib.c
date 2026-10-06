// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win compat: bitmap search and operations, sort, random numbers, small string helpers.
 */
#include <rtw/linux.h>

unsigned long find_next_bit (const unsigned long *a, unsigned long size, unsigned long off)
{
	while (off < size) {
		unsigned long w = a[BIT_WORD(off)] >> (off % BITS_PER_LONG);

		if (w)
			return min(off + __ffs(w), size);
		off = (BIT_WORD(off) + 1) * BITS_PER_LONG;
	}
	return size;
}

unsigned long find_next_zero_bit (const unsigned long *a, unsigned long size, unsigned long off)
{
	while (off < size) {
		unsigned long w = ~a[BIT_WORD(off)] >> (off % BITS_PER_LONG);

		if (w)
			return min(off + __ffs(w), size);
		off = (BIT_WORD(off) + 1) * BITS_PER_LONG;
	}
	return size;
}

unsigned long find_last_bit (const unsigned long *a, unsigned long size)
{
	for (unsigned long i = size; i-- > 0;)
		if (test_bit(i, a))
			return i;
	return size;
}

static unsigned long last_mask (unsigned int n)
{
	return n % BITS_PER_LONG ? BIT_MASK(n) - 1 : ~0UL;
}

int bitmap_weight (const unsigned long *a, unsigned int n)
{
	unsigned int words = BITS_TO_LONGS(n);
	int w = 0;

	for (unsigned int i = 0; i < words; i++)
		w += hweight_long(i == words - 1 ? a[i] & last_mask(n) : a[i]);
	return w;
}

bool bitmap_empty (const unsigned long *a, unsigned int n)
{
	return bitmap_weight(a, n) == 0;
}

bool bitmap_full (const unsigned long *a, unsigned int n)
{
	return bitmap_weight(a, n) == (int)n;
}

bool bitmap_equal (const unsigned long *a, const unsigned long *b, unsigned int n)
{
	unsigned int words = BITS_TO_LONGS(n);

	for (unsigned int i = 0; i < words; i++) {
		unsigned long m = i == words - 1 ? last_mask(n) : ~0UL;

		if ((a[i] ^ b[i]) & m)
			return false;
	}
	return true;
}

bool bitmap_and (unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n)
{
	unsigned long any = 0;

	for (unsigned int i = 0; i < BITS_TO_LONGS(n); i++)
		any |= d[i] = a[i] & b[i];
	return any != 0;
}

void bitmap_or (unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n)
{
	for (unsigned int i = 0; i < BITS_TO_LONGS(n); i++)
		d[i] = a[i] | b[i];
}

bool bitmap_andnot (unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n)
{
	unsigned long any = 0;

	for (unsigned int i = 0; i < BITS_TO_LONGS(n); i++)
		any |= d[i] = a[i] & ~b[i];
	return any != 0;
}

bool bitmap_intersects (const unsigned long *a, const unsigned long *b, unsigned int n)
{
	unsigned int words = BITS_TO_LONGS(n);

	for (unsigned int i = 0; i < words; i++) {
		unsigned long m = i == words - 1 ? last_mask(n) : ~0UL;

		if (a[i] & b[i] & m)
			return true;
	}
	return false;
}

void bitmap_set (unsigned long *a, unsigned int start, unsigned int n)
{
	for (unsigned int i = start; i < start + n; i++)
		__set_bit(i, a);
}

void bitmap_clear (unsigned long *a, unsigned int start, unsigned int n)
{
	for (unsigned int i = start; i < start + n; i++)
		__clear_bit(i, a);
}

/* unsigned long is 32-bit here, so the u32 array layout is the bitmap layout */
void bitmap_from_arr32 (unsigned long *d, const u32 *s, unsigned int n)
{
	memcpy(d, s, BITS_TO_U32(n) * sizeof(u32));
	if (n % 32)
		d[BIT_WORD(n)] &= last_mask(n);
}

void bitmap_to_arr32 (u32 *d, const unsigned long *s, unsigned int n)
{
	memcpy(d, s, BITS_TO_U32(n) * sizeof(u32));
	if (n % 32)
		d[n / 32] &= (u32)last_mask(n);
}

/* heap sort, like lib/sort.c (not stable, O(n log n), no recursion) */
static void swap_bytes (void *a, void *b, int size)
{
	u8 *x = a, *y = b;

	while (size--) {
		u8 t = *x;
		*x++ = *y;
		*y++ = t;
	}
}

void sort (void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
	   void (*swap_fn)(void *, void *, int))
{
	u8 *b = base;

	if (!swap_fn)
		swap_fn = swap_bytes;
	if (num < 2)
		return;
	for (size_t i = num / 2; i-- > 0;) {
		for (size_t r = i, c; (c = 2 * r + 1) < num; r = c) {
			if (c + 1 < num && cmp(b + c * size, b + (c + 1) * size) < 0)
				c++;
			if (cmp(b + r * size, b + c * size) >= 0)
				break;
			swap_fn(b + r * size, b + c * size, (int)size);
		}
	}
	for (size_t n = num - 1; n > 0; n--) {
		swap_fn(b, b + n * size, (int)size);
		for (size_t r = 0, c; (c = 2 * r + 1) < n; r = c) {
			if (c + 1 < n && cmp(b + c * size, b + (c + 1) * size) < 0)
				c++;
			if (cmp(b + r * size, b + c * size) >= 0)
				break;
			swap_fn(b + r * size, b + c * size, (int)size);
		}
	}
}

u32 get_random_u32 (void)
{
	return rtwk_random();
}

void get_random_bytes (void *buf, size_t n)
{
	u8 *p = buf;

	while (n) {
		u32 r = rtwk_random();
		size_t k = min(n, sizeof(r));

		memcpy(p, &r, k);
		p += k;
		n -= k;
	}
}

void *memchr_inv (const void *s, int c, size_t n)
{
	const u8 *p = s;

	for (size_t i = 0; i < n; i++)
		if (p[i] != (u8)c)
			return (void *)(p + i);
	return NULL;
}

ssize_t strscpy (char *d, const char *s, size_t size)
{
	size_t i;

	if (!size)
		return -E2BIG;
	for (i = 0; i + 1 < size && s[i]; i++)
		d[i] = s[i];
	d[i] = '\0';
	return s[i] ? -E2BIG : (ssize_t)i;
}

void *krealloc (const void *p, size_t n, gfp_t f)
{
	void *q;

	if (!n) {
		kfree(p);
		return NULL;
	}
	q = kmalloc(n, f);
	if (q && p) {
		memcpy(q, p, min(n, rtwk_alloc_size((void *)p)));
		kfree(p);
	}
	return q;
}
