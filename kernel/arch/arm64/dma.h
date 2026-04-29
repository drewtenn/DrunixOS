/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_DMA_H
#define KERNEL_ARCH_ARM64_DMA_H

#include <stdint.h>

/*
 * AArch64 DMA discipline helpers shared by all device drivers that
 * exchange in-memory descriptors or buffers with hardware. Phase 1 M2.3
 * of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md (FR-013); the
 * full rule set lives in docs/contributing/aarch64-dma.md.
 *
 * Memory-model regimes:
 *   - M2.x (today): MMU off; RAM is treated as Device-nGnRnE, so writes
 *     are ordered and uncached. The barrier helpers below are sufficient;
 *     the cache-maintenance helpers compile to nothing on purpose.
 *   - M2.4+ (planned): kernel RAM moves to Normal Inner-Shareable
 *     Cacheable. The cache-maintenance helpers flip on at that point;
 *     callers do not change.
 *
 * The split between `arm64_dma_wmb()` and `arm64_dma_rmb()` mirrors the
 * Linux kernel's `dma_wmb()` / `dma_rmb()` (which use the same `ishst` /
 * `ishld` encodings on aarch64). `arm64_dma_mb()` is a full system DMB
 * for MMIO transitions.
 */

/*
 * Producer-side ordering. Use after writing descriptors and before the
 * store that hands them to the device (avail->idx update on a virtqueue
 * or the QueueNotify MMIO write that follows).
 */
static inline void arm64_dma_wmb(void)
{
	__asm__ volatile("dmb ishst" ::: "memory");
}

/*
 * Consumer-side ordering. Use after reading the device's progress
 * counter (used->idx) and before reading the data the device wrote
 * behind it.
 */
static inline void arm64_dma_rmb(void)
{
	__asm__ volatile("dmb ishld" ::: "memory");
}

/*
 * Full system DMB. Use at MMIO boundaries that bracket DMA — for
 * example, between an MMIO write that re-arms a device and the next
 * descriptor publish. Prefer the directional helpers when you can.
 */
static inline void arm64_dma_mb(void)
{
	__asm__ volatile("dmb ish" ::: "memory");
}

/*
 * Cache-maintenance helpers.
 *
 * In M2.x these are no-ops on purpose: with the MMU off, RAM is
 * Device-nGnRnE and the data caches are bypassed for these accesses.
 * Adding `dc cvac` / `dc ivac` would either fault or do nothing
 * useful, depending on EL configuration.
 *
 * M2.4 enables Normal cacheable memory for kernel RAM. At that point
 * the bodies become `dc cvac` (clean to PoC, before a device read) and
 * `dc ivac` (invalidate, before a CPU read of device-written data).
 * Drivers must already be calling these helpers at the right points,
 * so the M2.4 flip is a one-file change.
 */
static inline void arm64_dma_cache_clean(const void *addr, uint32_t len)
{
	(void)addr;
	(void)len;
}

static inline void arm64_dma_cache_invalidate(void *addr, uint32_t len)
{
	(void)addr;
	(void)len;
}

#endif /* KERNEL_ARCH_ARM64_DMA_H */
