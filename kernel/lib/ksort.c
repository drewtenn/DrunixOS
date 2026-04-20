/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ksort.c — small in-kernel sorting helpers.
 */

#include "ksort.h"
#include <stdint.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

static inline uint8_t *elem(uint8_t *base, uint32_t i, uint32_t size)
{
	return base + i * size;
}

static void swap(uint8_t *a, uint8_t *b, uint32_t size)
{
	while (size--) {
		uint8_t tmp = *a;
		*a++ = *b;
		*b++ = tmp;
	}
}

/*
 * sift_down: restore the max-heap property for the subtree rooted at `root`
 * in the range [root, end] (inclusive).
 */
static void sift_down(uint8_t *base,
                      uint32_t root,
                      uint32_t end,
                      uint32_t size,
                      int (*cmp)(const void *, const void *))
{
	while (1) {
		uint32_t child = 2 * root + 1; /* left child */
		if (child > end)
			break;

		/* Pick the larger child. */
		if (child + 1 <= end &&
		    cmp(elem(base, child, size), elem(base, child + 1, size)) < 0)
			child++;

		if (cmp(elem(base, root, size), elem(base, child, size)) < 0) {
			swap(elem(base, root, size), elem(base, child, size), size);
			root = child;
		} else {
			break;
		}
	}
}

/* ── k_sort ───────────────────────────────────────────────────────────── */

void k_sort(void *base,
            uint32_t n,
            uint32_t size,
            int (*cmp)(const void *a, const void *b))
{
	if (n < 2)
		return;

	uint8_t *b = (uint8_t *)base;

	/* Phase 1: build a max-heap in-place (bottom-up). */
	for (uint32_t i = (n - 2) / 2 + 1; i-- > 0;)
		sift_down(b, i, n - 1, size, cmp);

	/* Phase 2: repeatedly extract the maximum to the end of the array. */
	for (uint32_t end = n - 1; end > 0; end--) {
		swap(elem(b, 0, size), elem(b, end, size), size);
		sift_down(b, 0, end - 1, size, cmp);
	}
}
