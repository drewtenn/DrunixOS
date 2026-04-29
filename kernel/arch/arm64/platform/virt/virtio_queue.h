/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_VIRT_VIRTIO_QUEUE_H
#define KERNEL_PLATFORM_VIRT_VIRTIO_QUEUE_H

#include <stdint.h>

/*
 * Split-ring virtqueue layout from Virtio 1.0 §2.4. M2.1 implements
 * the legacy (v1) flavor that QEMU's `-M virt` defaults to. Modern
 * (v2) layout uses separate addresses for desc/avail/used; legacy
 * uses one contiguous PFN-addressed region with an alignment between
 * avail and used rings.
 *
 * Queue size is fixed at 16 to keep the static backing region small
 * for M2.1; the driver can request larger when M2.2 supports it.
 */

#define VIRTQ_SIZE              16u

#define VIRTQ_DESC_F_NEXT       (1u << 0)
#define VIRTQ_DESC_F_WRITE      (1u << 1)
#define VIRTQ_DESC_F_INDIRECT   (1u << 2)

#define VIRTQ_AVAIL_F_NO_INTERRUPT (1u << 0)

struct virtq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
} __attribute__((packed));

struct virtq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[VIRTQ_SIZE];
	uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
	uint32_t id;
	uint32_t len;
} __attribute__((packed));

struct virtq_used {
	uint16_t flags;
	uint16_t idx;
	struct virtq_used_elem ring[VIRTQ_SIZE];
	uint16_t avail_event;
} __attribute__((packed));

typedef struct virtq {
	struct virtq_desc *desc;
	struct virtq_avail *avail;
	struct virtq_used *used;
	uint64_t base_phys;     /* Page-aligned base; what goes in QueuePFN. */
	uint16_t next_avail;    /* Producer cursor into avail->ring. */
	uint16_t last_used;     /* Consumer cursor into used->ring. */
	uint16_t free_head;     /* Head of the free-descriptor list. */
	uint16_t num_free;
} virtq_t;

/*
 * Initialize a fresh virtqueue against a static, page-aligned backing
 * region. The region must be at least virtq_backing_size() bytes long
 * and contain three sub-regions in order: descriptor table, avail ring,
 * used ring (the latter two with VirtIO 1.0 legacy alignment of 4096
 * between avail end and used start). Returns 0 on success.
 */
int virtq_init(virtq_t *q, void *backing, uint32_t backing_len);

/*
 * Total bytes the static backing region must hold for VIRTQ_SIZE
 * descriptors under legacy-virtio (v1) layout rules.
 */
uint32_t virtq_backing_size(void);

/*
 * Allocate a chain of N descriptors out of the queue's free list.
 * Returns the head descriptor index, or 0xFFFF if the queue does not
 * have enough free descriptors. The caller fills in addr/len/flags
 * and chains them with VIRTQ_DESC_F_NEXT and the `next` field.
 */
uint16_t virtq_alloc_chain(virtq_t *q, uint16_t count);

/*
 * Submit a descriptor chain by appending its head index to the avail
 * ring. The caller must have already populated the descriptors. After
 * virtq_submit, the device sees the work once the kick is delivered
 * via virtio-mmio QueueNotify (caller's responsibility).
 */
void virtq_submit(virtq_t *q, uint16_t head);

/*
 * Drain one completion off the used ring. Returns the head descriptor
 * index of the completed chain plus the device's reported byte count
 * via *out_len. Returns 0xFFFF if no completion is pending.
 */
uint16_t virtq_drain_one(virtq_t *q, uint32_t *out_len);

/*
 * Return all descriptors in `head`'s chain to the queue's free list.
 */
void virtq_free_chain(virtq_t *q, uint16_t head);

#endif
