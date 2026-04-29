/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_VIRT_VIRTIO_BLK_H
#define KERNEL_PLATFORM_VIRT_VIRTIO_BLK_H

#include <stdint.h>

/*
 * Bring up the first virtio-blk device on the virt machine and read
 * one 512-byte sector. Prints the device's reported capacity and the
 * first 16 bytes of sector 0. Returns 0 on success.
 *
 * M2.1 contract: single-shot read, polling for completion (no IRQ),
 * legacy (v1) virtio-mmio transport. M2.2 wires the device into
 * Drunix's blkdev registry and adds write + IRQ-driven completion.
 */
int virtio_blk_smoke(void);

#endif
