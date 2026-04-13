/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * string.c — byte and string primitives for user-space programs.
 *
 * Most functions here are pure byte/string primitives.  strdup() is the one
 * POSIX allocation helper in this layer; it depends on malloc so callers can
 * own string copies without reimplementing allocation boilerplate.
 *
 * GCC in -ffreestanding mode still emits implicit calls to memcpy/memset/
 * memmove/memcmp for struct assignments and local zero-initialisation, so
 * shipping these four symbols is not just a convenience — any user program
 * that does "struct X a = b;" will fail to link without them.
 */

#include "string.h"
#include "malloc.h"
#include <stddef.h>

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    /* Pad with NULs up to n, matching C89 semantics. */
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) { }
    return dst;
}

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (!copy)
        return 0;
    memcpy(copy, s, len);
    return copy;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;
    for (;;) {
        if (*s == ch) return (char *)s;
        if (!*s) return 0;
        s++;
    }
}

char *strrchr(const char *s, int c)
{
    char ch = (char)c;
    const char *last = 0;
    for (; *s; s++)
        if (*s == ch) last = s;
    if (ch == '\0') return (char *)s;
    return (char *)last;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    unsigned char v = (unsigned char)c;
    while (n--) *p++ = v;
    return s;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}
