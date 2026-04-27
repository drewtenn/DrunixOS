/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>

/*
 * string.h — minimal C string and memory primitives.
 *
 * Pure functions with no kernel dependencies.  Safe to call from every layer
 * of the user runtime above syscall.c.
 */

#ifdef __cplusplus
extern "C" {
#endif

size_t strlen(const char *s);

int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strdup(const char *s);

char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif
