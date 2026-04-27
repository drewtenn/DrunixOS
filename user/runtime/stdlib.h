/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

/*
 * stdlib.h — a tiny subset of the standard general utilities.
 *
 * Pulls in malloc.h so a caller can #include one header and get the full
 * heap interface (malloc/free/realloc/sbrk) alongside exit/abort/atoi.
 */

#include "malloc.h"

#ifdef __cplusplus
extern "C" {
#endif

int atoi(const char *s);
int abs(int x);
char *getenv(const char *name);
void qsort(void *base,
           size_t nmemb,
           size_t size,
           int (*compar)(const void *, const void *));

/*
 * Terminate the calling process with `status` as its exit code.
 * Wraps sys_exit(); never returns.
 */
void exit(int status);

/*
 * Raise SIGABRT on the calling process, which terminates it under the
 * default disposition.  Like POSIX abort(), this never returns.
 */
void abort(void);

#ifdef __cplusplus
}
#endif

#endif
