/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * malloc.c — user-space first-fit heap allocator backed by SYS_BRK and mmap.
 */

#include "malloc.h"
#include "syscall.h"
#include <stddef.h>
#include <stdint.h>

/*
 * malloc.c — first-fit free-list heap allocator backed by SYS_BRK.
 * Large allocations are backed directly by anonymous private mmap.
 *
 * Block layout:
 *
 *   +----------+----------+--------------------+
 *   | size:u32 | flags:u32| payload bytes ...  |
 *   +----------+----------+--------------------+
 *   ^-- block_hdr_t (8 bytes) --^
 *
 *   size:  payload length in bytes (not counting the header itself).
 *   flags: bit 0 — 1 = allocated, 0 = free.
 *          bit 1 — block is backed by mmap, not brk.
 *
 * Free blocks store a pointer to the next free block in the first 4 bytes
 * of their payload.  malloc() enforces a minimum allocation of 4 bytes so
 * every free block can hold this pointer.
 *
 * The free list is singly-linked and prepend-on-free (LIFO).  No address-
 * ordered coalescing is performed; that can be added as a follow-up.
 */

#define HDR_SIZE 8u
#define FLAG_USED 1u
#define MMAP_THRESHOLD (128u * 1024u)
#define HDR_FLAG_MMAP 0x2u
#define PAGE_SIZE 4096u
#define PAGE_MASK (PAGE_SIZE - 1u)
#define FREE_LINK_SIZE ((uint32_t)sizeof(uintptr_t))

typedef struct block_hdr {
	uint32_t size;
	uint32_t flags;
} block_hdr_t;

/* Head of the free list; NULL when the heap is empty or fully allocated. */
static block_hdr_t *g_free_list = 0;

/* ── sbrk ─────────────────────────────────────────────────────────────────── */

void *sbrk(int increment)
{
	if (increment == 0) {
		uintptr_t cur = (uintptr_t)sys_brk(0);
		return (void *)cur;
	}
	if (increment < 0)
		return (void *)(uintptr_t)-1; /* shrink not supported */

	uintptr_t old_brk = (uintptr_t)sys_brk(0);
	uintptr_t new_brk =
	    (uintptr_t)sys_brk((unsigned int)(old_brk + (uintptr_t)increment));

	if (new_brk == old_brk)
		return (void *)(uintptr_t)-1; /* kernel refused: OOM or guard hit */

	return (void *)old_brk; /* pointer to the start of the new region */
}

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static inline block_hdr_t *hdr_of(void *payload)
{
	return (block_hdr_t *)((uint8_t *)payload - HDR_SIZE);
}

static inline void *payload_of(block_hdr_t *h)
{
	return (void *)((uint8_t *)h + HDR_SIZE);
}

/* Next free block is stored in the first word of a free block's payload. */
static inline block_hdr_t *next_free(block_hdr_t *h)
{
	uintptr_t n = 0;
	uint8_t *p = (uint8_t *)payload_of(h);
	uint8_t *q = (uint8_t *)&n;
	for (uint32_t i = 0; i < FREE_LINK_SIZE; i++)
		q[i] = p[i];
	return (block_hdr_t *)n;
}

static inline void set_next_free(block_hdr_t *h, block_hdr_t *next)
{
	uint8_t *p = (uint8_t *)payload_of(h);
	uintptr_t next_addr = (uintptr_t)next;
	uint8_t *q = (uint8_t *)&next_addr;
	for (uint32_t i = 0; i < FREE_LINK_SIZE; i++)
		p[i] = q[i];
}

/* ── malloc ───────────────────────────────────────────────────────────────── */

void *malloc(size_t size)
{
	if (size == 0)
		return 0;

	/* Minimum payload: must fit the free-list next pointer. */
	if (size < FREE_LINK_SIZE)
		size = FREE_LINK_SIZE;

	/* Align to pointer width. */
	if (size > (size_t)UINT32_MAX - (FREE_LINK_SIZE - 1u))
		return 0;
	size = (size + FREE_LINK_SIZE - 1u) & ~(size_t)(FREE_LINK_SIZE - 1u);
	if (size > UINT32_MAX - HDR_SIZE)
		return 0;

	uint32_t total = HDR_SIZE + (uint32_t)size;
	if (size >= MMAP_THRESHOLD) {
		if (total > UINT32_MAX - PAGE_MASK)
			return 0;

		total = (total + PAGE_MASK) & ~PAGE_MASK;
		block_hdr_t *blk = (block_hdr_t *)sys_mmap(0,
		                                           total,
		                                           PROT_READ | PROT_WRITE,
		                                           MAP_PRIVATE | MAP_ANONYMOUS,
		                                           -1,
		                                           0);
		if (blk == MAP_FAILED)
			return 0;

		blk->size = total - HDR_SIZE;
		blk->flags = FLAG_USED | HDR_FLAG_MMAP;
		return payload_of(blk);
	}

	/* First-fit scan of the free list. */
	block_hdr_t *prev = 0;
	block_hdr_t *cur = g_free_list;

	while (cur) {
		if (cur->size >= (uint32_t)size) {
			/*
             * Found a fit.  If there is enough surplus space to carve a new
			 * free block from the tail (header + minimum link payload),
			 * split it.  Otherwise hand over the entire block.
			 */
			if (cur->size >= (uint32_t)size + HDR_SIZE + FREE_LINK_SIZE) {
				/* Split: new free block starts after the allocated payload. */
				block_hdr_t *tail =
				    (block_hdr_t *)((uint8_t *)payload_of(cur) + size);
				tail->size = cur->size - (uint32_t)size - HDR_SIZE;
				tail->flags = 0;
				set_next_free(tail, next_free(cur));

				cur->size = (uint32_t)size;

				/* Replace cur with tail in the free list. */
				if (prev)
					set_next_free(prev, tail);
				else
					g_free_list = tail;
			} else {
				/* No split: remove cur from the free list entirely. */
				if (prev)
					set_next_free(prev, next_free(cur));
				else
					g_free_list = next_free(cur);
			}

			cur->flags = FLAG_USED;
			return payload_of(cur);
		}

		prev = cur;
		cur = next_free(cur);
	}

	/* No suitable free block found — extend the heap. */
	uint32_t need = total;
	block_hdr_t *blk = (block_hdr_t *)sbrk((int)need);
	if (blk == (block_hdr_t *)(uintptr_t)-1)
		return 0; /* OOM */

	blk->size = (uint32_t)size;
	blk->flags = FLAG_USED;
	return payload_of(blk);
}

/* ── free ─────────────────────────────────────────────────────────────────── */

void free(void *ptr)
{
	if (!ptr)
		return;

	block_hdr_t *h = hdr_of(ptr);
	if ((h->flags & HDR_FLAG_MMAP) != 0) {
		sys_munmap(h, h->size + HDR_SIZE);
		return;
	}

	h->flags = 0;

	/* Prepend to the free list (LIFO). */
	set_next_free(h, g_free_list);
	g_free_list = h;
}

/* ── realloc ──────────────────────────────────────────────────────────────── */

void *realloc(void *ptr, size_t new_size)
{
	if (!ptr)
		return malloc(new_size);

	if (new_size == 0) {
		free(ptr);
		return 0;
	}

	block_hdr_t *h = hdr_of(ptr);

	/* If the current block is already large enough, reuse it in place. */
	if (h->size >= (uint32_t)new_size)
		return ptr;

	/* Otherwise allocate a new block, copy the old payload, then free. */
	void *newp = malloc(new_size);
	if (!newp)
		return 0;

	uint8_t *src = (uint8_t *)ptr;
	uint8_t *dst = (uint8_t *)newp;
	uint32_t copy_len =
	    h->size < (uint32_t)new_size ? h->size : (uint32_t)new_size;
	for (uint32_t i = 0; i < copy_len; i++)
		dst[i] = src[i];

	free(ptr);
	return newp;
}
