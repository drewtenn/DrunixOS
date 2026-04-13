/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdint.h>
#include <stdarg.h>

/*
 * kprintf — kernel formatted string output to a buffer.
 *
 * k_vsnprintf / k_snprintf write at most `size` bytes (including the
 * terminating NUL) into `buf`.  They return the number of characters that
 * would have been written if `buf` were large enough (like POSIX snprintf),
 * NOT counting the NUL terminator.
 *
 * Supported conversions:
 *   %d / %i  — signed 32-bit decimal
 *   %u       — unsigned 32-bit decimal
 *   %x / %X  — unsigned hex, lower / upper case
 *   %s       — NUL-terminated string  (NULL → "(null)")
 *   %c       — single character
 *   %p       — pointer as 0x???????? (8 hex digits, zero-padded)
 *   %%       — literal percent
 *
 * Flag / width support:
 *   -        — left-align in field
 *   0        — zero-pad (right-align only)
 *   +        — always print sign for %d/%i
 *   <space>  — space before positive %d/%i
 *   width    — minimum field width (decimal constant or * from arg)
 *   l        — length modifier (consumed but ignored; int == long on i386)
 */

int k_vsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap);
int k_snprintf(char *buf, uint32_t size, const char *fmt, ...);

#endif
