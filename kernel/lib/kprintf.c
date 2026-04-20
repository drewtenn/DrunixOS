/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kprintf.c — printf-style formatting helpers used by kernel logging and diagnostics.
 */

#include "kprintf.h"
#include <stdint.h>
#include <stdarg.h>

/* ── Output buffer ────────────────────────────────────────────────────── */

typedef struct {
	char *buf;
	uint32_t size; /* total capacity, including space for NUL */
	uint32_t pos;  /* next write index (always counts even past size) */
} outbuf_t;

static void ob_putc(outbuf_t *ob, char c)
{
	/* Always count; only store if there is room (leave last byte for NUL). */
	if (ob->pos + 1 < ob->size)
		ob->buf[ob->pos] = c;
	ob->pos++;
}

static void ob_pad(outbuf_t *ob, char padch, int count)
{
	while (count-- > 0)
		ob_putc(ob, padch);
}

/* ── Integer formatting ───────────────────────────────────────────────── */

#define F_LEFT (1 << 0)  /* '-' flag: left-align */
#define F_ZERO (1 << 1)  /* '0' flag: zero-pad   */
#define F_PLUS (1 << 2)  /* '+' flag: force sign  */
#define F_SPACE (1 << 3) /* ' ' flag: space before positive */

/*
 * fmt_uint: format an unsigned integer `val` in base `base`.
 * `upper` selects A-F vs a-f for hex digits.
 * `width` is the minimum field width; `flags` controls padding/alignment.
 * `sign` is the sign character to prepend ('\0' = none).
 */
static void fmt_uint(outbuf_t *ob,
                     uint32_t val,
                     int base,
                     int upper,
                     int flags,
                     int width,
                     char sign)
{
	static const char lo[] = "0123456789abcdef";
	static const char hi[] = "0123456789ABCDEF";
	const char *digits = upper ? hi : lo;

	/* Build reversed digits into a local buffer (max 10 decimal / 8 hex). */
	char tmp[12];
	int len = 0;
	if (val == 0) {
		tmp[len++] = '0';
	} else {
		uint32_t v = val;
		while (v) {
			tmp[len++] = digits[v % (uint32_t)base];
			v /= (uint32_t)base;
		}
	}

	/* Total content width: optional sign + digits. */
	int content = len + (sign ? 1 : 0);
	int pad = width - content;

	if (flags & F_LEFT) {
		/* Left-align: sign, digits, then spaces. */
		if (sign)
			ob_putc(ob, sign);
		for (int i = len - 1; i >= 0; i--)
			ob_putc(ob, tmp[i]);
		ob_pad(ob, ' ', pad);
	} else if (flags & F_ZERO) {
		/* Zero-pad: sign first, then zeros, then digits. */
		if (sign)
			ob_putc(ob, sign);
		ob_pad(ob, '0', pad);
		for (int i = len - 1; i >= 0; i--)
			ob_putc(ob, tmp[i]);
	} else {
		/* Right-align with spaces: spaces, sign, digits. */
		ob_pad(ob, ' ', pad);
		if (sign)
			ob_putc(ob, sign);
		for (int i = len - 1; i >= 0; i--)
			ob_putc(ob, tmp[i]);
	}
}

static void fmt_int(outbuf_t *ob, int32_t val, int flags, int width)
{
	char sign;
	uint32_t uval;

	if (val < 0) {
		sign = '-';
		/* Two's-complement safe negation via unsigned arithmetic. */
		uval = (uint32_t)(-(uint32_t)val);
	} else {
		sign = (flags & F_PLUS) ? '+' : (flags & F_SPACE) ? ' ' : '\0';
		uval = (uint32_t)val;
	}
	/* Account for sign in width. */
	fmt_uint(ob, uval, 10, 0, flags, sign ? width - 1 : width, sign);
}

/* ── String formatting ────────────────────────────────────────────────── */

static void fmt_str(outbuf_t *ob, const char *s, int flags, int width)
{
	if (!s)
		s = "(null)";
	uint32_t len = 0;
	const char *p = s;
	while (*p++)
		len++;

	int pad = width - (int)len;

	if (flags & F_LEFT) {
		while (*s)
			ob_putc(ob, *s++);
		ob_pad(ob, ' ', pad);
	} else {
		ob_pad(ob, ' ', pad);
		while (*s)
			ob_putc(ob, *s++);
	}
}

/* ── k_vsnprintf ──────────────────────────────────────────────────────── */

int k_vsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap)
{
	outbuf_t ob = {buf, size, 0};

	while (*fmt) {
		if (*fmt != '%') {
			ob_putc(&ob, *fmt++);
			continue;
		}
		fmt++; /* skip '%' */

		/* ── Flags ── */
		int flags = 0;
		for (;;) {
			if (*fmt == '-') {
				flags |= F_LEFT;
				fmt++;
			} else if (*fmt == '0') {
				flags |= F_ZERO;
				fmt++;
			} else if (*fmt == '+') {
				flags |= F_PLUS;
				fmt++;
			} else if (*fmt == ' ') {
				flags |= F_SPACE;
				fmt++;
			} else
				break;
		}

		/* ── Width ── */
		int width = 0;
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				flags |= F_LEFT;
				width = -width;
			}
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}

		/* ── Length modifier (l treated same as none on 32-bit) ── */
		if (*fmt == 'l')
			fmt++;

		/* ── Conversion ── */
		switch (*fmt++) {
		case 'd':
		case 'i':
			fmt_int(&ob, (int32_t)va_arg(ap, int), flags, width);
			break;
		case 'u':
			fmt_uint(&ob,
			         (uint32_t)va_arg(ap, unsigned int),
			         10,
			         0,
			         flags,
			         width,
			         '\0');
			break;
		case 'x':
			fmt_uint(&ob,
			         (uint32_t)va_arg(ap, unsigned int),
			         16,
			         0,
			         flags,
			         width,
			         '\0');
			break;
		case 'X':
			fmt_uint(&ob,
			         (uint32_t)va_arg(ap, unsigned int),
			         16,
			         1,
			         flags,
			         width,
			         '\0');
			break;
		case 'p':
			ob_putc(&ob, '0');
			ob_putc(&ob, 'x');
			fmt_uint(&ob, (uint32_t)va_arg(ap, void *), 16, 0, F_ZERO, 8, '\0');
			break;
		case 's':
			fmt_str(&ob, va_arg(ap, const char *), flags, width);
			break;
		case 'c': {
			char ch = (char)va_arg(ap, int);
			int pad = width - 1;
			if (flags & F_LEFT) {
				ob_putc(&ob, ch);
				ob_pad(&ob, ' ', pad);
			} else {
				ob_pad(&ob, ' ', pad);
				ob_putc(&ob, ch);
			}
		} break;
		case '%':
			ob_putc(&ob, '%');
			break;
		default:
			ob_putc(&ob, '?');
			break;
		}
	}

	/* NUL-terminate within the buffer. */
	if (size > 0)
		ob.buf[ob.pos < size ? ob.pos : size - 1] = '\0';

	return (int)ob.pos; /* chars that would be written (excluding NUL) */
}

/* ── k_snprintf ───────────────────────────────────────────────────────── */

int k_snprintf(char *buf, uint32_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = k_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return r;
}
