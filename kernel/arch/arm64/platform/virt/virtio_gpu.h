/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_GPU_H
#define KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_GPU_H

#include "desktop_window.h"
#include <stdint.h>

/*
 * virtio-gpu front-end driver for the QEMU `virt` platform — Phase 2
 * of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
 *
 * M3.0 brought up the controlq and cursorq and sent the six-command
 * 2D sequence (GET_DISPLAY_INFO, RESOURCE_CREATE_2D,
 * RESOURCE_ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D,
 * RESOURCE_FLUSH) on a 32x32 BGRA scratch resource. M3.1 retires the
 * scratch resource and drives the 1024x768 BGRA framebuffer that
 * /dev/fb0 publishes — virtio-gpu now owns the chardev when its init
 * succeeds, with ramfb as the fallback.
 *
 * Completion is polled. The polled controlq wait runs in process
 * context (Commit 4 deferred-flush pump runs from arm64_sync_handler
 * on syscall return); virtio-blk-style IRQ delivery on a GICv3 SPI is
 * a later milestone.
 */

/*
 * Initialize the virtio-gpu device on the QEMU virt machine.
 *
 * Returns 0 on success, -1 if no device is present, -1 on transport
 * version mismatch (legacy v1 only), or -1 on any feature / queue /
 * scanout sub-step failure (each step logs its own diagnostic before
 * returning).
 *
 * Tolerant by design: callers (start_kernel.c) treat -1 as "skip the
 * milestone" so headless KTEST builds and raspi3b builds without
 * `-device virtio-gpu-device` continue to boot.
 */
int arm64_virt_virtio_gpu_init(void);

/*
 * Predicate returning 1 when the driver successfully reached
 * DRIVER_OK, completed the six-command 2D sequence, and registered
 * /dev/fb0. KTEST and observability code use this to decide whether
 * the rest of the suite can run.
 */
int arm64_virt_virtio_gpu_ready(void);

/*
 * Predicate returning 1 when init found a virtio-gpu device on the
 * bus (whether or not subsequent setup succeeded). Distinguishes
 * "device not advertised — ramfb-fallback path is expected" from
 * "device present, but init failed — real bug". KTEST uses this to
 * skip-pass tests on the no-virtio-gpu boot configuration.
 */
int arm64_virt_virtio_gpu_device_found(void);

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

/*
 * Deferred-flush API (M3.1 Commit 4).
 *
 * arm64_virt_virtio_gpu_request_flush() is IRQ-safe; the timer tick
 * calls it to mark the scanout dirty. It performs no submission and
 * no polling — just sets a volatile flag.
 *
 * arm64_virt_virtio_gpu_pump_flush() runs the actual TRANSFER_TO_HOST_2D
 * + RESOURCE_FLUSH if a flush has been requested. MUST run from
 * non-IRQ context — the controlq submit polls with a bounded busy
 * wait. Called from arm64_sync_handler() on syscall return.
 *
 * After a threshold of consecutive flush failures the pump
 * permanently disables itself with a one-shot log; subsequent calls
 * are no-ops, and the request side stops accepting new requests too.
 */
void arm64_virt_virtio_gpu_request_flush(void);
void arm64_virt_virtio_gpu_pump_flush(void);

/* Test-only: cumulative count of successful flushes processed by the
 * pump. Increments only when both TRANSFER_TO_HOST_2D and
 * RESOURCE_FLUSH succeeded. */
uint32_t arm64_virt_virtio_gpu_pump_runs(void);

/*
 * M3.2 dirty-rect publish hook. IRQ-safe: unions `rect` into the
 * driver's pending dirty state (a single coalesced rect) and sets
 * the flush flag so the next pump_flush picks it up. Called from
 * fbdev_ioctl on DRUNIX_FBIO_FLUSH_RECT after rect validation.
 *
 * The pump consumes the union under a brief IRQ-mask critical
 * section so an IRQ-context publish (e.g. timer-tick fallback)
 * cannot tear the consume; the actual TRANSFER+FLUSH runs with
 * IRQs enabled. See virtio_gpu.c.
 *
 * If the driver isn't ready or has been permanently disabled, this
 * call is a no-op.
 */
void arm64_virt_virtio_gpu_publish_dirty_rect(drunix_rect_t rect);

#endif /* KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_GPU_H */
