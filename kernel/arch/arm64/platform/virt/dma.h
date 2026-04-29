/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_VIRT_DMA_H
#define KERNEL_ARCH_ARM64_PLATFORM_VIRT_DMA_H

#include <stdint.h>

/*
 * DMA-safe page allocator for the QEMU virt platform. Phase 1 M2.3 of
 * docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md (FR-012).
 *
 * Implementation is a static, page-aligned, identity-mapped pool with
 * a small page-granular bitmap. Identity-mapped because M2.x runs with
 * the MMU off — guest physical equals kernel virtual. M2.4 promotes
 * this to a heap-backed allocator once kheap is on virt; the API
 * surface here is the contract for that switch and does not change.
 *
 * Caller contract:
 *   - Use `virt_dma_alloc` for any buffer that the guest will hand to
 *     a virtio device (descriptor backing, request headers, payload
 *     scratch). Stack and `kheap` allocations are NOT DMA-safe in
 *     M2.x because their cache attributes are unknown to the driver.
 *   - Pair writes with the barrier helpers in kernel/arch/arm64/dma.h.
 *     The allocator does not perform barriers for you.
 *   - Treat returned pointers as identity-mapped: a uintptr_t cast
 *     equals the device-visible physical address on this platform
 *     today. Use `virt_virt_to_phys` / `virt_phys_to_virt` anyway so
 *     M2.4's heap-backed allocator can reroute without touching call
 *     sites.
 *
 * See docs/contributing/aarch64-dma.md for the full rule set.
 */

#define VIRT_DMA_PAGE_SIZE 4096u

void virt_dma_init(void);

void *virt_dma_alloc(uint32_t npages);
void virt_dma_free(void *addr, uint32_t npages);

uint64_t virt_virt_to_phys(const void *v);
void *virt_phys_to_virt(uint64_t p);

uint32_t virt_dma_pages_total(void);
uint32_t virt_dma_pages_free(void);

#endif /* KERNEL_ARCH_ARM64_PLATFORM_VIRT_DMA_H */
