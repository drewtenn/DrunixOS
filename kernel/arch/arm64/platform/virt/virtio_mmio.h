/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_VIRT_VIRTIO_MMIO_H
#define KERNEL_PLATFORM_VIRT_VIRTIO_MMIO_H

#include <stdint.h>

/*
 * virtio-mmio device-id values used by Drunix in M2.x. The full list is
 * in the Virtio 1.3 spec §5; only the ones M2 actually uses are named.
 */
#define VIRTIO_DEV_ID_INVALID    0u
#define VIRTIO_DEV_ID_NET        1u
#define VIRTIO_DEV_ID_BLOCK      2u
#define VIRTIO_DEV_ID_CONSOLE    3u
#define VIRTIO_DEV_ID_GPU        16u
#define VIRTIO_DEV_ID_INPUT      18u
#define VIRTIO_DEV_ID_SOUND      25u

/*
 * Scan the QEMU virt machine's virtio-mmio slots and print a summary
 * to the platform UART. Returns the count of slots whose device-id is
 * non-zero (i.e. actually populated by QEMU). Safe to call before any
 * driver bind; performs only register reads.
 */
uint32_t virtio_mmio_enumerate(void);

/*
 * Locate the first slot whose DeviceID matches `device_id` and write
 * its MMIO base + transport version into out_base / out_version.
 * Returns 1 on success, 0 if no slot matches. Versions are 1 (legacy)
 * or 2 (modern); QEMU's `-M virt` defaults to legacy.
 */
int virtio_mmio_find(uint32_t device_id, uintptr_t *out_base,
                     uint32_t *out_version);

/*
 * Modern + legacy virtio-mmio register offsets (Virtio 1.0 §4.2.2).
 * Driver code drives the device through these via plain MMIO loads
 * and stores. Legacy-only fields are flagged.
 */
#define VIRTIO_MMIO_MAGIC_VALUE       0x000u  /* RO: 0x74726976 */
#define VIRTIO_MMIO_VERSION           0x004u  /* RO: 1=legacy, 2=modern */
#define VIRTIO_MMIO_DEVICE_ID         0x008u  /* RO */
#define VIRTIO_MMIO_VENDOR_ID         0x00Cu  /* RO */
#define VIRTIO_MMIO_DEVICE_FEATURES   0x010u
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014u
#define VIRTIO_MMIO_DRIVER_FEATURES   0x020u
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024u
#define VIRTIO_MMIO_GUEST_PAGE_SIZE   0x028u  /* legacy only */
#define VIRTIO_MMIO_QUEUE_SEL         0x030u
#define VIRTIO_MMIO_QUEUE_NUM_MAX     0x034u  /* RO */
#define VIRTIO_MMIO_QUEUE_NUM         0x038u
#define VIRTIO_MMIO_QUEUE_ALIGN       0x03Cu  /* legacy only */
#define VIRTIO_MMIO_QUEUE_PFN         0x040u  /* legacy only */
#define VIRTIO_MMIO_QUEUE_NOTIFY      0x050u
#define VIRTIO_MMIO_INTERRUPT_STATUS  0x060u
#define VIRTIO_MMIO_INTERRUPT_ACK     0x064u
#define VIRTIO_MMIO_STATUS            0x070u
#define VIRTIO_MMIO_CONFIG            0x100u  /* device-specific cfg base */

/* Status bits (Virtio 1.0 §2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE     (1u << 0)
#define VIRTIO_STATUS_DRIVER          (1u << 1)
#define VIRTIO_STATUS_DRIVER_OK       (1u << 2)
#define VIRTIO_STATUS_FEATURES_OK     (1u << 3)
#define VIRTIO_STATUS_FAILED          (1u << 7)

#endif
