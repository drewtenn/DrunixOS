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
 *   - Pre-M2.4b: MMU off; RAM was Device-nGnRnE; the cache-maintenance
 *     helpers were no-ops.
 *   - M2.4b+ (current): MMU on; kernel RAM is Normal Inner-Shareable
 *     Cacheable. The cache-maintenance helpers issue real `dc cvac` /
 *     `dc ivac` with `dsb ish`. Driver call sites are unchanged.
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
 * Cache-maintenance helpers (Phase 1 M2.4b: real implementations).
 *
 * Pre-M2.4b: kernel RAM ran Device-nGnRnE with the MMU off, so these
 * helpers were no-ops by design.
 *
 * M2.4b: kernel RAM moves to Normal Inner-Shareable Cacheable. These
 * helpers now issue `dc cvac` (clean to PoC, before a device read) and
 * `dc ivac` (invalidate to PoC, before a CPU read of device-written
 * data). Each iteration strides by the smallest data-cache line size on
 * any level (`CTR_EL0.DminLine`); a `dsb ish` after the loop ensures
 * the maintenance ops complete in the inner-shareable domain before
 * the next memory access.
 *
 * Both helpers tolerate misaligned addr/len: the start is rounded down
 * to a line boundary and the end is exclusive.
 */
static inline uint32_t arm64_dma_cache_line_size(void)
{
	uint64_t ctr;
	uint32_t dminline_log2;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	dminline_log2 = (uint32_t)((ctr >> 16) & 0xFu);
	return 4u << dminline_log2;
}

static inline void arm64_dma_cache_clean(const void *addr, uint32_t len)
{
	uint64_t start = (uint64_t)(uintptr_t)addr;
	uint64_t end = start + (uint64_t)len;
	uint64_t line_size = (uint64_t)arm64_dma_cache_line_size();
	uint64_t line;

	if (line_size == 0u)
		return;

	start &= ~(line_size - 1u);
	for (line = start; line < end; line += line_size)
		__asm__ volatile("dc cvac, %0" : : "r"(line) : "memory");
	__asm__ volatile("dsb ish" ::: "memory");
}

static inline void arm64_dma_cache_invalidate(void *addr, uint32_t len)
{
	uint64_t start = (uint64_t)(uintptr_t)addr;
	uint64_t end = start + (uint64_t)len;
	uint64_t line_size = (uint64_t)arm64_dma_cache_line_size();
	uint64_t line;

	if (line_size == 0u)
		return;

	start &= ~(line_size - 1u);
	for (line = start; line < end; line += line_size)
		__asm__ volatile("dc ivac, %0" : : "r"(line) : "memory");
	__asm__ volatile("dsb ish" ::: "memory");
}

#endif /* KERNEL_ARCH_ARM64_DMA_H */
