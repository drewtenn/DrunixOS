/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KSORT_H
#define KSORT_H

#include <stdint.h>

/*
 * k_sort — in-place heapsort for arbitrary element arrays.
 *
 *   base  — pointer to the first element
 *   n     — number of elements
 *   size  — size of each element in bytes
 *   cmp   — comparison function; returns negative / zero / positive
 *           when a < b / a == b / a > b
 *
 * O(n log n) worst case, O(1) extra space.  Not stable.
 */
void k_sort(void *base,
            uint32_t n,
            uint32_t size,
            int (*cmp)(const void *a, const void *b));

#endif
