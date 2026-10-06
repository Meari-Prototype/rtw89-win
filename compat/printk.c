// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win compat: printf family with the Linux extensions rtw89 uses (%pM, %*ph, %pV), and printk/dev_* output
 * into the driver log (rtwk_log). Kernel CRT printf is not used: it lacks these and 'z'/'t' sizes.
 */
#include <rtw/linux.h>

struct out {
	char *buf;
	size_t size;
	size_t len;                     /* would-be length, like snprintf */
};

static void put (struct out *o, char c)
{
	if (o->len + 1 < o->size)
		o->buf[o->len] = c;
	o->len++;
}

static void put_str (struct out *o, const char *s, int width, int prec, bool left)
{
	size_t n = 0;

	if (!s)
		s = "(null)";
	while ((prec < 0 || (int)n < prec) && s[n])
		n++;
	if (!left)
		for (int i = (int)n; i < width; i++)
			put(o, ' ');
	for (size_t i = 0; i < n; i++)
		put(o, s[i]);
	if (left)
		for (int i = (int)n; i < width; i++)
			put(o, ' ');
}

static void put_num (struct out *o, u64 v, bool neg, int base, bool upper, int width, int prec, char pad,
		     bool left, bool plus, bool alt)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24];
	int n = 0;
	int len;

	if (v == 0 && prec != 0)
		tmp[n++] = '0';
	while (v) {
		tmp[n++] = digits[v % base];
		v /= base;
	}
	while (n < prec)
		tmp[n++] = '0';
	len = n + (neg || plus ? 1 : 0) + (alt && base == 16 ? 2 : 0);
	if (!left && pad == ' ')
		for (int i = len; i < width; i++)
			put(o, ' ');
	if (neg)
		put(o, '-');
	else if (plus)
		put(o, '+');
	if (alt && base == 16) {
		put(o, '0');
		put(o, upper ? 'X' : 'x');
	}
	if (!left && pad == '0')
		for (int i = len; i < width; i++)
			put(o, '0');
	while (n)
		put(o, tmp[--n]);
	if (left)
		for (int i = len; i < width; i++)
			put(o, ' ');
}

static int do_vsnprintf (struct out *o, const char *fmt, va_list ap);

static const char *put_pointer (struct out *o, const char *fmt, const void *p, int width)
{
	static const char hex[] = "0123456789abcdef";

	switch (*fmt) {
	case 'M': {                     /* MAC address aa:bb:cc:dd:ee:ff */
		const u8 *a = p;
		for (int i = 0; i < 6; i++) {
			if (i)
				put(o, ':');
			put(o, hex[a[i] >> 4]);
			put(o, hex[a[i] & 15]);
		}
		return fmt + 1;
	}
	case 'h': {                     /* %*ph: hex bytes separated by spaces, width = count (max 64) */
		const u8 *a = p;
		int n = width > 0 ? min(width, 64) : 1;
		for (int i = 0; i < n; i++) {
			if (i)
				put(o, ' ');
			put(o, hex[a[i] >> 4]);
			put(o, hex[a[i] & 15]);
		}
		return fmt + 1;
	}
	case 'V': {                     /* struct va_format */
		const struct va_format *vf = p;
		va_list copy;
		va_copy(copy, *vf->va);
		do_vsnprintf(o, vf->fmt, copy);
		va_end(copy);
		return fmt + 1;
	}
	default:
		put_num(o, (uintptr_t)p, false, 16, false, 16, -1, '0', false, false, false);
		while ((*fmt >= 'A' && *fmt <= 'Z') || (*fmt >= 'a' && *fmt <= 'z'))
			fmt++;                  /* unknown extension: print the pointer, skip its letters */
		return fmt;
	}
}

static int do_vsnprintf (struct out *o, const char *fmt, va_list ap)
{
	while (*fmt) {
		bool left = false, plus = false, alt = false;
		char pad = ' ';
		int width = -1, prec = -1, size = 0;    /* size: 0 int, 1 long, 2 long long, 3 size_t/ptrdiff, -1 short, -2 char */

		if (*fmt != '%') {
			put(o, *fmt++);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				left = true;
			else if (*fmt == '+')
				plus = true;
			else if (*fmt == '#')
				alt = true;
			else if (*fmt == '0')
				pad = '0';
			else if (*fmt != ' ')
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = (width < 0 ? 0 : width * 10) + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
			}
		}
		for (;; fmt++) {
			if (*fmt == 'l')
				size = size == 1 ? 2 : 1;
			else if (*fmt == 'h')
				size = size == -1 ? -2 : -1;
			else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j')
				size = 3;
			else
				break;
		}
		if (left)
			pad = ' ';

		switch (*fmt) {
		case 'd':
		case 'i': {
			s64 v = size == 2 || size == 3 ? va_arg(ap, s64) : size == 1 ? va_arg(ap, long) : va_arg(ap, int);
			if (size == -1)
				v = (s16)v;
			else if (size == -2)
				v = (s8)v;
			put_num(o, v < 0 ? (u64)-v : (u64)v, v < 0, 10, false, width, prec, pad, left, plus, false);
			fmt++;
			break;
		}
		case 'u':
		case 'x':
		case 'X':
		case 'o': {
			u64 v = size == 2 || size == 3 ? va_arg(ap, u64) : size == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
			if (size == -1)
				v = (u16)v;
			else if (size == -2)
				v = (u8)v;
			int base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
			put_num(o, v, false, base, *fmt == 'X', width, prec, pad, left, false, alt && base == 16);
			fmt++;
			break;
		}
		case 'c':
			put(o, (char)va_arg(ap, int));
			fmt++;
			break;
		case 's':
			put_str(o, va_arg(ap, const char *), width, prec, left);
			fmt++;
			break;
		case 'p':
			fmt = put_pointer(o, fmt + 1, va_arg(ap, const void *), width);
			break;
		case '%':
			put(o, '%');
			fmt++;
			break;
		case '\0':
			break;
		default:
			put(o, '%');
			put(o, *fmt++);
			break;
		}
	}
	return (int)o->len;
}

int vsnprintf (char *buf, size_t size, const char *fmt, va_list ap)
{
	struct out o = { buf, size, 0 };

	do_vsnprintf(&o, fmt, ap);
	if (size)
		buf[min(o.len, size - 1)] = '\0';
	return (int)o.len;
}

int vscnprintf (char *buf, size_t size, const char *fmt, va_list ap)
{
	int n = vsnprintf(buf, size, fmt, ap);

	if (!size)
		return 0;
	return n < (int)size ? n : (int)size - 1;
}

int snprintf (char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

int scnprintf (char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vscnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

int sprintf (char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, 4096, fmt, ap);
	va_end(ap);
	return n;
}

char *kasprintf (gfp_t gfp, const char *fmt, ...)
{
	va_list ap;
	char *p;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	p = kmalloc(n + 1, gfp);
	if (!p)
		return NULL;
	va_start(ap, fmt);
	vsnprintf(p, n + 1, fmt, ap);
	va_end(ap);
	return p;
}

/*
 * Flood guard: at most LOG_PER_SEC lines a second reach the log (a 64 KB ring), the rest are counted and reported
 * when the next second starts. Generous enough for firmware dumps; it stops a per-frame message from wiping the
 * log. Lock-free, so any IRQL works.
 */
#define LOG_PER_SEC     400
static u64 log_second;
static u32 log_lines;
static u32 log_dropped;

static bool log_admit (void)
{
	u64 now = rtwk_time_ns() / NSEC_PER_SEC;
	u64 sec = __atomic_load_n(&log_second, __ATOMIC_RELAXED);

	if (now != sec && __atomic_compare_exchange_n(&log_second, &sec, now, false, __ATOMIC_SEQ_CST,
						      __ATOMIC_SEQ_CST)) {
		u32 dropped = __atomic_exchange_n(&log_dropped, 0, __ATOMIC_SEQ_CST);

		__atomic_store_n(&log_lines, 0, __ATOMIC_SEQ_CST);
		if (dropped) {
			char msg[64];

			scnprintf(msg, sizeof(msg), "log: %u lines suppressed", dropped);
			rtwk_log(1, msg);
		}
	}
	if (__atomic_add_fetch(&log_lines, 1, __ATOMIC_RELAXED) <= LOG_PER_SEC)
		return true;
	__atomic_add_fetch(&log_dropped, 1, __ATOMIC_RELAXED);
	return false;
}

/* printk("\001" "<level>" ...) prefixes (KERN_*) override the level argument */
static void vlog (int level, const char *prefix, const char *fmt, va_list ap)
{
	char line[320];
	size_t n = 0;

	if (!log_admit())
		return;
	if (fmt[0] == '\001' && fmt[1] >= '0' && fmt[1] <= '7') {
		level = fmt[1] - '0';
		fmt += 2;
	}
	if (prefix)
		n = scnprintf(line, sizeof(line), "%s", prefix);
	vscnprintf(line + n, sizeof(line) - n, fmt, ap);
	n = strlen(line);
	while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		line[--n] = '\0';
	rtwk_log(level, line);
}

void rtw_printk (int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(level, NULL, fmt, ap);
	va_end(ap);
}

void rtw_dev_printk (int level, const struct device *d, const char *fmt, ...)
{
	static const char *const tag[] = { "err: ", "warn: ", "", "dbg: " };
	va_list ap;

	(void)d;
	va_start(ap, fmt);
	vlog(level, tag[level & 3], fmt, ap);
	va_end(ap);
}

void rtw_warn (const char *file, int line, const char *cond)
{
	const char *base = strrchr(file, '\\');

	if (!base)
		base = strrchr(file, '/');
	rtw_printk(1, "WARNING at %s:%d: %s", base ? base + 1 : file, line, cond);
}

void rtw_bug (const char *file, int line)
{
	rtw_printk(0, "BUG at %s:%d", file, line);
	rtwk_bugcheck(line);
}

void print_hex_dump (const char *level, const char *prefix, int type, int rowsize, int groupsize,
		     const void *buf, size_t len, bool ascii)
{
	const u8 *p = buf;

	(void)level;
	(void)groupsize;
	(void)ascii;
	if (rowsize <= 0 || rowsize > 32)
		rowsize = 16;
	for (size_t i = 0; i < len; i += rowsize) {
		int n = (int)min(len - i, (size_t)rowsize);

		if (type == DUMP_PREFIX_OFFSET)
			rtw_printk(2, "%s%08zx: %*ph", prefix, i, n, p + i);
		else
			rtw_printk(2, "%s%*ph", prefix, n, p + i);
	}
}
