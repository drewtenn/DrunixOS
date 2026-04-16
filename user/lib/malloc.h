/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef MALLOC_H
#define MALLOC_H

#include <stddef.h>

/*
 * sbrk: grow the process heap by `increment` bytes and return a pointer to
 * the start of the newly usable region.  Returns (void *)-1 on failure.
 * Passing increment == 0 returns the current break without allocating.
 * Shrinking (negative increment) is not supported and returns (void *)-1.
 */

#ifdef __cplusplus
extern "C" {
#endif

void *sbrk(int increment);

/* Standard heap allocation functions. */
void *malloc(size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t new_size);

#ifdef __cplusplus
}
#endif

#endif
