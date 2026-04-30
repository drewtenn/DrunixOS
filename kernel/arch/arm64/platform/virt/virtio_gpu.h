/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_GPU_H
#define KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_GPU_H

#include <stdint.h>

/*
 * virtio-gpu front-end driver for the QEMU `virt` platform — Phase 2 M3.0
 * of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
 *
 * M3.0 brings up the controlq and cursorq, sends the six-command 2D
 * sequence (GET_DISPLAY_INFO, RESOURCE_CREATE_2D, RESOURCE_ATTACH_BACKING,
 * SET_SCANOUT, TRANSFER_TO_HOST_2D, RESOURCE_FLUSH) on a 32x32 BGRA
 * scanout resource, draws a deterministic kernel-side test pattern, and
 * confirms the dirty-rect partial flush path. /dev/fb0 stays on ramfb;
 * the swap to virtio-gpu lands in M3.1.
 *
 * Completion is polled in M3.0 (no virtio-gpu IRQ wired in) per the
 * Define-phase debate-gate decision: bring up the protocol on a polled
 * loop where stalls are deterministic before adding IRQ plumbing.
 */

/*
 * Initialize the virtio-gpu device on the QEMU virt machine.
 *
 * Returns 0 on success, -1 if no device is present, -1 on transport
 * version mismatch (legacy v1 only in M3.0), or -1 on any feature /
 * queue / scanout sub-step failure (each step logs its own diagnostic
 * before returning).
 *
 * Tolerant by design: callers (start_kernel.c) treat -1 as "skip the
 * milestone" so headless KTEST builds and raspi3b builds without
 * `-device virtio-gpu-device` continue to boot.
 */
int arm64_virt_virtio_gpu_init(void);

/*
 * Predicate returning 1 when the driver successfully reached
 * DRIVER_OK and completed the M3.0 six-command sequence at least once.
 * KTEST and observability code use this to decide whether the rest of
 * the suite can run.
 */
int arm64_virt_virtio_gpu_ready(void);

/*
 * Test-only helpers. Exposed here so the KTEST suite can validate the
 * protocol without re-running init. Each helper returns 0 on success
 * and -1 on any failure; failure logs go to platform_uart.
 */

/* Send GET_DISPLAY_INFO; populate width/height of scanout 0. */
int arm64_virt_virtio_gpu_query_display(uint32_t *out_width,
                                        uint32_t *out_height);

/* Run the dirty-rect partial flush on the existing M3.0 scanout. */
int arm64_virt_virtio_gpu_partial_flush_smoke(void);

/* Compute a deterministic checksum over the kernel-side scanout buffer
 * after the M3.0 test pattern has been drawn. Used by the KTEST pixel
 * gate. Returns 0 if the buffer is not initialised. */
uint32_t arm64_virt_virtio_gpu_checksum_pattern(void);

/* Total pages the driver currently holds out of virt_dma_alloc.
 * Used by KTEST to assert the failure path leaks nothing. */
uint32_t arm64_virt_virtio_gpu_dma_pages_held(void);

#endif /* KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_GPU_H */
