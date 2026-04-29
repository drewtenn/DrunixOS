/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_queue.c - Split-ring virtqueue mechanics for the virt platform.
 *
 * Phase 1 M2.1 of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
 * Implements the legacy (v1) split-ring layout because QEMU's `-M virt`
 * exposes virtio devices in legacy mode by default. Modern (v2) layout
 * is a small refactor away once the driver wires up the
 * virtio-mmio.force-legacy=false transport — tracked under FR-010
 * "modern transport" in the v1.2 PRD.
 *
 * Memory ordering rules live in docs/contributing/aarch64-dma.md and
 * are accessed through the helpers in kernel/arch/arm64/dma.h. M2.x
 * (MMU off, RAM as Device-nGnRnE) needs DMB barriers only; M2.4 will
 * add cache maintenance through the same helpers without changing
 * call sites.
 */

#include "../../dma.h"
#include "virtio_queue.h"
#include "kstring.h"
#include <stdint.h>

#define VIRTQ_BACKING_ALIGN 4096u

/* Layout of the static backing region in legacy mode:
 *   offset 0                    desc[VIRTQ_SIZE]   (16 * 16 = 256 bytes)
 *   offset 256                  avail              (38 bytes)
 *   align up to 4096            used               (134 bytes)
 *   total                       4096 + 134 + pad   ≈ 4230 bytes
 *
 * Round to 8192 so callers can statically declare a single 8 KiB block.
 */

#define VIRTQ_DESC_BYTES (sizeof(struct virtq_desc) * VIRTQ_SIZE)
#define VIRTQ_AVAIL_BYTES (sizeof(struct virtq_avail))
#define VIRTQ_USED_BYTES (sizeof(struct virtq_used))
#define VIRTQ_USED_OFFSET                                                      \
	((VIRTQ_DESC_BYTES + VIRTQ_AVAIL_BYTES + VIRTQ_BACKING_ALIGN - 1u) &       \
	 ~(VIRTQ_BACKING_ALIGN - 1u))
#define VIRTQ_BACKING_LEN (VIRTQ_USED_OFFSET + VIRTQ_USED_BYTES)

/* Catch a future VIRTQ_SIZE bump that would push desc+avail past the
 * 4-KiB alignment boundary the layout assumes. */
_Static_assert(VIRTQ_DESC_BYTES + VIRTQ_AVAIL_BYTES <= VIRTQ_BACKING_ALIGN,
               "virtq desc+avail exceeds the 4 KiB legacy alignment window");

uint32_t virtq_backing_size(void)
{
	return VIRTQ_BACKING_LEN;
}

int virtq_init(virtq_t *q, void *backing, uint32_t backing_len)
{
	uint8_t *base = (uint8_t *)backing;
	uintptr_t base_addr = (uintptr_t)backing;
	uint16_t i;

	if (!q || !backing || backing_len < VIRTQ_BACKING_LEN)
		return -1;
	if ((base_addr & (VIRTQ_BACKING_ALIGN - 1u)) != 0u)
		return -1;

	q->desc = (struct virtq_desc *)base;
	q->avail = (struct virtq_avail *)(base + VIRTQ_DESC_BYTES);
	q->used = (struct virtq_used *)(base + VIRTQ_USED_OFFSET);
	q->base_phys = (uint64_t)base_addr;

	/* Zero the whole region so the avail/used flags+idx start clean,
	 * the descriptor table is in a known state, and freshly-allocated
	 * descriptors have predictable defaults. */
	k_memset(base, 0, VIRTQ_BACKING_LEN);

	/* Build a free list through the desc->next pointers. The free
	 * head is descriptor 0; descriptor i's `next` points to i+1; the
	 * last descriptor's `next` is 0 (terminator value, distinguished
	 * from descriptor 0 by checking num_free first). */
	for (i = 0; i + 1u < VIRTQ_SIZE; i++)
		q->desc[i].next = i + 1u;
	q->desc[VIRTQ_SIZE - 1u].next = 0u;

	q->free_head = 0u;
	q->num_free = VIRTQ_SIZE;
	q->next_avail = 0u;
	q->last_used = 0u;

	return 0;
}

uint16_t virtq_alloc_chain(virtq_t *q, uint16_t count)
{
	uint16_t head;
	uint16_t cur;

	if (!q || count == 0u || count > q->num_free)
		return 0xFFFFu;

	head = q->free_head;
	cur = head;
	for (uint16_t i = 1; i < count; i++)
		cur = q->desc[cur].next;

	q->free_head = q->desc[cur].next;
	q->num_free = (uint16_t)(q->num_free - count);
	q->desc[cur].next = 0u;

	return head;
}

void virtq_submit(virtq_t *q, uint16_t head)
{
	uint16_t slot;

	if (!q || head >= VIRTQ_SIZE)
		return;

	slot = (uint16_t)(q->next_avail & (VIRTQ_SIZE - 1u));
	q->avail->ring[slot] = head;

	/* Producer-side ordering: descriptor writes must retire before
	 * avail->idx becomes visible to the device. */
	arm64_dma_wmb();

	q->avail->idx = (uint16_t)(q->avail->idx + 1u);
	q->next_avail = (uint16_t)(q->next_avail + 1u);

	/* Order the avail->idx write before the caller's QueueNotify
	 * MMIO write that delivers the kick. */
	arm64_dma_wmb();
}

uint16_t virtq_drain_one(virtq_t *q, uint32_t *out_len)
{
	uint16_t used_idx;
	uint16_t slot;
	struct virtq_used_elem elem;

	if (!q)
		return 0xFFFFu;

	used_idx = q->used->idx;
	if (used_idx == q->last_used)
		return 0xFFFFu;

	/* Consumer-side ordering: the load of used->idx must complete
	 * before any read of the ring entry it gates. Without this
	 * barrier the CPU may speculate q->used->ring[slot] ahead of
	 * the idx check on M2.4's MMU+caches, returning stale data.
	 * Mirrors Linux's virt_rmb() placement in virtqueue_get_buf. */
	arm64_dma_rmb();
	slot = (uint16_t)(q->last_used & (VIRTQ_SIZE - 1u));
	elem = q->used->ring[slot];

	q->last_used = (uint16_t)(q->last_used + 1u);

	if (out_len)
		*out_len = elem.len;

	return (uint16_t)(elem.id & 0xFFFFu);
}

void virtq_free_chain(virtq_t *q, uint16_t head)
{
	uint16_t cur;
	uint16_t tail;
	uint16_t count = 0u;

	if (!q || head >= VIRTQ_SIZE)
		return;

	cur = head;
	while ((q->desc[cur].flags & VIRTQ_DESC_F_NEXT) != 0u) {
		count++;
		cur = q->desc[cur].next;
	}
	count++;
	tail = cur;

	q->desc[tail].next = q->free_head;
	q->free_head = head;
	q->num_free = (uint16_t)(q->num_free + count);
}
