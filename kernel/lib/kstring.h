/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KSTRING_H
#define KSTRING_H

#include <stdint.h>

/*
 * kstring — kernel string and memory utilities.
 *
 * All functions use uint32_t for lengths/counts; on i386 this is the same
 * width as size_t and avoids pulling in <stddef.h> throughout the kernel.
 *
 * Memory functions (k_memcpy, k_memset, k_memmove, k_memcmp) are also
 * exported under the bare names memcpy/memset/memmove/memcmp via weak
 * aliases in kstring.c so that compiler-generated calls resolve without
 * linking against libc.
 */

/* ── Memory ───────────────────────────────────────────────────────────── */

void *k_memcpy(void *dst, const void *src, uint32_t n);
void *k_memset(void *s, int c, uint32_t n);
void k_memset32(void *s, uint32_t value, uint32_t count);
void *k_memmove(void *dst, const void *src, uint32_t n);
int k_memcmp(const void *a, const void *b, uint32_t n);

/* ── String length ────────────────────────────────────────────────────── */

uint32_t k_strlen(const char *s);
uint32_t k_strnlen(const char *s, uint32_t max);

/* ── String copy / concatenation ──────────────────────────────────────── */

char *k_strcpy(char *dst, const char *src);
char *k_strncpy(char *dst, const char *src, uint32_t n);
char *k_strcat(char *dst, const char *src);
char *k_strncat(char *dst, const char *src, uint32_t n);

/* ── String comparison ────────────────────────────────────────────────── */

int k_strcmp(const char *a, const char *b);
int k_strncmp(const char *a, const char *b, uint32_t n);

/* ── String search ────────────────────────────────────────────────────── */

char *k_strchr(const char *s, int c);
char *k_strrchr(const char *s, int c);
char *k_strstr(const char *haystack, const char *needle);

#endif
