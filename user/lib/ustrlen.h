/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ustrlen.h — tiny strlen() used by syscall translation units that link
 * without the rest of libc (e.g. arm64init.elf links syscall.o without
 * string.o).  Keep it static-inline so each TU keeps its own copy and
 * pulls in no external symbols.
 */

#ifndef USER_LIB_USTRLEN_H
#define USER_LIB_USTRLEN_H

static inline int ustrlen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

#endif
