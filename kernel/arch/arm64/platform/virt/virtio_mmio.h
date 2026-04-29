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

#endif
