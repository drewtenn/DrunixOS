/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * dma.c - DMA-safe page allocator for the QEMU virt platform.
 *
 * Phase 1 M2.3 of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md
 * (FR-012). The allocator backs a static, page-aligned BSS pool.
 * Pages are tracked with a small bitmap; allocation is first-fit
 * across contiguous runs of free pages so multi-page virtqueue
 * backings (8 KiB today) work without a separate code path.
 *
 * Concurrency: M2.x is single-CPU, MMU-off, no preemption. virtio
 * drivers register IRQ handlers only after their DMA buffers have
 * been handed to the device; the IRQ handler never allocates. No
 * locking is needed in M2.3. M2.4 keeps the API but moves backing
 * to kheap and adds the appropriate guards.
 *
 * Sizing rationale: 16 pages (64 KiB) covers M2.3 callers (one
 * 8 KiB virtq backing, one 4 KiB request header, one 4 KiB data
 * scratch) with comfortable headroom for Phase 2/3 virtio devices
 * landing later. Adjust VIRT_DMA_POOL_PAGES if a Phase 2 driver
 * needs more before M2.4's heap-backed allocator arrives.
 */

#include "dma.h"
#include <stdint.h>

#define VIRT_DMA_POOL_PAGES 16u
#define VIRT_DMA_POOL_BYTES (VIRT_DMA_POOL_PAGES * VIRT_DMA_PAGE_SIZE)

/* BSS-resident, page-aligned. boot.S zeroes BSS, so g_in_use starts
 * with every page free without an explicit init step. */
static uint8_t g_pool[VIRT_DMA_POOL_BYTES]
    __attribute__((aligned(VIRT_DMA_PAGE_SIZE)));

static uint8_t g_in_use[VIRT_DMA_POOL_PAGES];

void virt_dma_init(void)
{
	/* M2.3: nothing to do — BSS is zero so every page is free.
	 * Callable repeatedly; M2.4's implementation will use this
	 * hook to wire kheap-backed storage. */
}

static int virt_dma_pool_owns(const void *p)
{
	uintptr_t addr = (uintptr_t)p;
	uintptr_t base = (uintptr_t)g_pool;

	return addr >= base && addr < base + VIRT_DMA_POOL_BYTES;
}

static int virt_dma_run_is_free(uint32_t start, uint32_t npages)
{
	for (uint32_t i = 0; i < npages; i++) {
		if (g_in_use[start + i])
			return 0;
	}
	return 1;
}

void *virt_dma_alloc(uint32_t npages)
{
	if (npages == 0u || npages > VIRT_DMA_POOL_PAGES)
		return 0;

	for (uint32_t start = 0; start + npages <= VIRT_DMA_POOL_PAGES; start++) {
		if (!virt_dma_run_is_free(start, npages))
			continue;

		for (uint32_t i = 0; i < npages; i++)
			g_in_use[start + i] = 1u;

		return &g_pool[(uintptr_t)start * VIRT_DMA_PAGE_SIZE];
	}

	return 0;
}

void virt_dma_free(void *addr, uint32_t npages)
{
	uintptr_t offset;
	uint32_t page_index;

	if (!addr || npages == 0u || npages > VIRT_DMA_POOL_PAGES)
		return;
	if (!virt_dma_pool_owns(addr))
		return;

	offset = (uintptr_t)addr - (uintptr_t)g_pool;
	if ((offset & (VIRT_DMA_PAGE_SIZE - 1u)) != 0u)
		return;

	page_index = (uint32_t)(offset / VIRT_DMA_PAGE_SIZE);
	if (page_index + npages > VIRT_DMA_POOL_PAGES)
		return;

	for (uint32_t i = 0; i < npages; i++)
		g_in_use[page_index + i] = 0u;
}

uint64_t virt_virt_to_phys(const void *v)
{
	if (!v || !virt_dma_pool_owns(v))
		return 0u;
	return (uint64_t)(uintptr_t)v;
}

void *virt_phys_to_virt(uint64_t p)
{
	uintptr_t addr = (uintptr_t)p;

	if (!virt_dma_pool_owns((const void *)addr))
		return 0;
	return (void *)addr;
}

uint32_t virt_dma_pages_total(void)
{
	return VIRT_DMA_POOL_PAGES;
}

uint32_t virt_dma_pages_free(void)
{
	uint32_t free_count = 0;

	for (uint32_t i = 0; i < VIRT_DMA_POOL_PAGES; i++) {
		if (!g_in_use[i])
			free_count++;
	}
	return free_count;
}
