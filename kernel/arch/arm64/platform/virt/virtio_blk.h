/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_VIRT_VIRTIO_BLK_H
#define KERNEL_PLATFORM_VIRT_VIRTIO_BLK_H

#include <stdint.h>

/*
 * virtio-blk driver for the QEMU virt platform. Phase 1 of
 * docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md (M2.1 single-shot
 * read, M2.2 IRQ-driven completion, M2.3 multi-shot read/write +
 * blkdev registration).
 *
 * Buffers (virtqueue backing, request header, data scratch) come from
 * the virt DMA pool in dma.h. Memory ordering is enforced through the
 * shared barrier helpers in kernel/arch/arm64/dma.h. See
 * docs/contributing/aarch64-dma.md for the rules.
 *
 * The driver is single-session (one virtio-blk device, one queue).
 * Read and write block until the device's IRQ fires; concurrent
 * sectors are not supported in M2.3.
 */

/*
 * Bring the first virtio-blk device on the bus through reset, feature
 * negotiation, queue setup, IRQ registration, and DRIVER_OK. Idempotent
 * — repeated calls return success without re-initializing. Returns 0
 * on success, negative on failure.
 */
int virtio_blk_init(void);

/*
 * Synchronous 512-byte sector read. Caller-provided `buf` must hold at
 * least 512 bytes. Blocks until the device IRQ delivers completion.
 * Returns 0 on success, negative on failure (init not done, device
 * status non-OK, IRQ timeout).
 */
int virtio_blk_read_sector(uint32_t lba, uint8_t *buf);

/*
 * Synchronous 512-byte sector write. Caller-provided `buf` must hold
 * at least 512 bytes. Returns 0 on success, negative on failure.
 */
int virtio_blk_write_sector(uint32_t lba, const uint8_t *buf);

/*
 * Capacity in 512-byte sectors as reported by the device's config
 * space. Valid after virtio_blk_init() returns success; zero before
 * that or after init failure.
 */
uint32_t virtio_blk_capacity_sectors(void);

/*
 * Boot-log canary: init the device, read sector 0, hex-dump the first
 * 16 bytes. Returns 0 on success. Wrapper over the API above so the
 * existing boot log line is preserved.
 */
int virtio_blk_smoke(void);

#endif
