/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kstring.c — kernel string and memory utility routines.
 */

#include "kstring.h"
#include <stdint.h>

/* ── Memory ───────────────────────────────────────────────────────────── */

void *k_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *k_memset(void *s, int c, uint32_t n)
{
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

void *k_memmove(void *dst, const void *src, uint32_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s || d >= s + n) {
        /* non-overlapping or dst is before src: forward copy */
        while (n--) *d++ = *s++;
    } else {
        /* dst overlaps src from behind: copy backwards */
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int k_memcmp(const void *a, const void *b, uint32_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

/*
 * Weak aliases so compiler-generated calls to the bare names (e.g. for
 * struct assignment optimised to a memcpy) resolve without libc.
 */
void *memcpy(void *dst, const void *src, uint32_t n)
    __attribute__((weak, alias("k_memcpy")));
void *memset(void *s, int c, uint32_t n)
    __attribute__((weak, alias("k_memset")));
void *memmove(void *dst, const void *src, uint32_t n)
    __attribute__((weak, alias("k_memmove")));
int  memcmp(const void *a, const void *b, uint32_t n)
    __attribute__((weak, alias("k_memcmp")));

/* ── String length ────────────────────────────────────────────────────── */

uint32_t k_strlen(const char *s)
{
    uint32_t n = 0;
    while (*s++) n++;
    return n;
}

uint32_t k_strnlen(const char *s, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && *s++) n++;
    return n;
}

/* ── String copy / concatenation ──────────────────────────────────────── */

char *k_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *k_strncpy(char *dst, const char *src, uint32_t n)
{
    char *d = dst;
    while (n > 0) {
        if ((*d = *src)) src++;  /* once src hits NUL, keep writing NUL */
        d++;
        n--;
    }
    return dst;
}

char *k_strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

char *k_strncat(char *dst, const char *src, uint32_t n)
{
    char *d = dst;
    while (*d) d++;
    while (n-- && (*d = *src++)) d++;
    *d = '\0';
    return dst;
}

/* ── String comparison ────────────────────────────────────────────────── */

int k_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int k_strncmp(const char *a, const char *b, uint32_t n)
{
    while (n > 0) {
        if (*a == '\0' || *a != *b) break;
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* ── String search ────────────────────────────────────────────────────── */

char *k_strchr(const char *s, int c)
{
    do {
        if (*s == (char)c) return (char *)s;
    } while (*s++);
    return 0;
}

char *k_strrchr(const char *s, int c)
{
    const char *last = 0;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

char *k_strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return 0;
}
