/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pmm.c — x86 physical memory provider backed by the shared PMM core.
 */

#include "pmm.h"
#include <stdint.h>

/* Linker-provided symbols: first and last byte of the kernel image */
extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

#define X86_PMM_MANAGED_LIMIT 0x40000000u
#define X86_PMM_FALLBACK_MAX_PAGES (0x10000000u / PAGE_SIZE)
#define X86_PMM_MAX_USABLE_RANGES 64u

typedef struct {
	pmm_range_t usable[X86_PMM_MAX_USABLE_RANGES];
	uint32_t usable_count;
	uint32_t max_pages;
	uint32_t fb_base;
	uint32_t fb_len;
	int has_framebuffer;
} x86_pmm_boot_info_t;

static pmm_core_state_t g_pmm;

static uint32_t x86_page_align_up(uint32_t value)
{
	return (value + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
}

static int x86_pmm_managed_usable_range(uint64_t base,
                                        uint64_t length,
                                        uint32_t managed_pages,
                                        uint32_t *base_out,
                                        uint32_t *length_out)
{
	const uint64_t managed_end = (uint64_t)managed_pages * PAGE_SIZE;
	uint64_t end;

	if (!base_out || !length_out || length == 0 || base >= managed_end)
		return -1;

	end = base + length;
	if (end < base)
		end = UINT64_MAX;
	if (end > managed_end)
		end = managed_end;
	if (end <= base)
		return -1;

	*base_out = (uint32_t)base;
	*length_out = (uint32_t)(end - base);
	return 0;
}

static void x86_pmm_snapshot_add_usable(x86_pmm_boot_info_t *boot,
                                        uint64_t base,
                                        uint64_t length,
                                        uint64_t *highest_out)
{
	uint64_t end;

	if (!boot || !highest_out || length == 0u || base >= X86_PMM_MANAGED_LIMIT)
		return;

	end = base + length;
	if (end < base || end > X86_PMM_MANAGED_LIMIT)
		end = X86_PMM_MANAGED_LIMIT;
	if (end <= base)
		return;

	if (end > *highest_out)
		*highest_out = end;
	if (boot->usable_count < X86_PMM_MAX_USABLE_RANGES) {
		boot->usable[boot->usable_count].base = (uint32_t)base;
		boot->usable[boot->usable_count].length = (uint32_t)(end - base);
		boot->usable_count++;
	}
}

static int pmm_multiboot_framebuffer_range(const multiboot_info_t *mbi,
                                           uint32_t *base_out,
                                           uint32_t *length_out)
{
	uint32_t bytes_per_pixel;
	uint64_t row_bytes;
	uint64_t last_row_offset;
	uint64_t length;
	uint64_t base;

	if (!mbi || !base_out || !length_out)
		return -1;
	if ((mbi->flags & MULTIBOOT_FLAG_FRAMEBUFFER) == 0)
		return -1;
	base = mbi->framebuffer_addr;
	if (base > UINT32_MAX)
		return -1;
	if (mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0 ||
	    mbi->framebuffer_pitch == 0 || mbi->framebuffer_bpp == 0)
		return -1;

	bytes_per_pixel = ((uint32_t)mbi->framebuffer_bpp + 7u) / 8u;
	row_bytes = (uint64_t)mbi->framebuffer_width * bytes_per_pixel;
	last_row_offset =
	    (uint64_t)(mbi->framebuffer_height - 1u) * mbi->framebuffer_pitch;
	length = last_row_offset + row_bytes;
	if (length == 0 || length > UINT32_MAX ||
	    base + length > ((uint64_t)UINT32_MAX + 1u))
		return -1;

	*base_out = (uint32_t)base;
	*length_out = (uint32_t)length;
	return 0;
}

static void x86_pmm_snapshot_boot_info(x86_pmm_boot_info_t *boot,
                                       const multiboot_info_t *mbi)
{
	uint64_t highest = 0;

	if (!boot)
		return;

	boot->usable_count = 0;
	boot->max_pages = X86_PMM_FALLBACK_MAX_PAGES;
	boot->fb_base = 0;
	boot->fb_len = 0;
	boot->has_framebuffer = 0;

	if (mbi && (mbi->flags & MULTIBOOT_FLAG_MMAP)) {
		uintptr_t entry_addr = (uintptr_t)mbi->mmap_addr;
		uintptr_t map_end = entry_addr + (uintptr_t)mbi->mmap_length;

		if (map_end >= entry_addr) {
			while (entry_addr + sizeof(uint32_t) <= map_end) {
				const multiboot_mmap_entry_t *entry =
				    (const multiboot_mmap_entry_t *)entry_addr;
				uintptr_t next =
				    entry_addr + entry->size + sizeof(entry->size);

				if (next <= entry_addr || next > map_end)
					break;

				if (entry->type == 1u)
					x86_pmm_snapshot_add_usable(boot,
					                            entry->addr,
					                            entry->len,
					                            &highest);

				entry_addr = next;
			}
		}
	} else {
		boot->usable[0].base = 0x00100000u;
		boot->usable[0].length = 0x07F00000u;
		boot->usable_count = 1u;
		highest = (uint64_t)X86_PMM_FALLBACK_MAX_PAGES * PAGE_SIZE;
	}

	if (highest > 0u)
		boot->max_pages = (uint32_t)(highest / PAGE_SIZE);
	if (pmm_multiboot_framebuffer_range(mbi, &boot->fb_base, &boot->fb_len) == 0)
		boot->has_framebuffer = 1;
}

static void x86_pmm_apply_usable_ranges(pmm_core_state_t *state,
                                        const multiboot_info_t *mbi)
{
	uint32_t have_mmap = mbi && (mbi->flags & MULTIBOOT_FLAG_MMAP);

	if (have_mmap) {
		uintptr_t entry_addr = (uintptr_t)mbi->mmap_addr;
		uintptr_t map_end = entry_addr + (uintptr_t)mbi->mmap_length;

		if (map_end < entry_addr)
			return;

		while (entry_addr + sizeof(uint32_t) <= map_end) {
			const multiboot_mmap_entry_t *entry =
			    (const multiboot_mmap_entry_t *)entry_addr;
			uintptr_t next = entry_addr + entry->size + sizeof(entry->size);

			if (next <= entry_addr || next > map_end)
				break;

			if (entry->type == 1u) {
				uint32_t range_base;
				uint32_t range_length;

				if (x86_pmm_managed_usable_range(entry->addr,
				                                 entry->len,
				                                 state ? state->max_pages : 0u,
				                                 &range_base,
				                                 &range_length) == 0) {
					pmm_core_mark_free(state, range_base, range_length);
				}
			}

			entry_addr = next;
		}
	} else {
		pmm_core_mark_free(state, 0x00100000u, 0x07F00000u);
	}
}

static void x86_pmm_apply_reserved_ranges(pmm_core_state_t *state,
                                          const x86_pmm_boot_info_t *boot,
                                          uint32_t metadata_base,
                                          uint32_t metadata_length)
{
	uint32_t kstart = (uint32_t)&_kernel_start;
	uint32_t kend = (uint32_t)&_kernel_end;
	uint32_t kend_page = (kend + PAGE_SIZE - 1u) / PAGE_SIZE;
	uint32_t fb_base;
	uint32_t fb_len;

	pmm_core_mark_used(state, 0x00000000u, 0x00100000u);
	pmm_core_mark_used(state, kstart, kend_page * PAGE_SIZE - kstart);
	pmm_core_mark_used(state, 0x00011000u, 0x00041000u);
	pmm_core_mark_used(state, 0x00032000u, 0x000CE000u);
	pmm_core_mark_used(state, 0x000A0000u, 0x00020000u);
	if (metadata_length > 0u)
		pmm_core_mark_used(state, metadata_base, metadata_length);

	if (boot && boot->has_framebuffer) {
		fb_base = boot->fb_base;
		fb_len = boot->fb_len;
		pmm_core_mark_used(state, fb_base, fb_len);
	}
}

void pmm_mark_used(uint32_t base, uint32_t length)
{
	pmm_core_mark_used(&g_pmm, base, length);
}

void pmm_mark_free(uint32_t base, uint32_t length)
{
	pmm_core_mark_free(&g_pmm, base, length);
}

void pmm_init(multiboot_info_t *mbi)
{
	x86_pmm_boot_info_t boot;
	uint32_t max_pages;
	uint32_t metadata_base = x86_page_align_up((uint32_t)&_kernel_end);
	uint32_t bitmap_bytes;
	uint32_t refcount_bytes;
	uint32_t metadata_length;
	void *bitmap;
	void *refcounts;

	x86_pmm_snapshot_boot_info(&boot, mbi);
	max_pages = boot.max_pages;
	bitmap_bytes = pmm_core_bitmap_bytes(max_pages);
	refcount_bytes = pmm_core_refcount_bytes(max_pages);
	metadata_length = x86_page_align_up(bitmap_bytes + refcount_bytes);
	bitmap = (void *)(uintptr_t)metadata_base;
	refcounts = (void *)(uintptr_t)(metadata_base + bitmap_bytes);

	pmm_core_bind_storage(&g_pmm, bitmap, refcounts, max_pages);
	pmm_core_init(&g_pmm, boot.usable, boot.usable_count, 0, 0);
	x86_pmm_apply_reserved_ranges(&g_pmm, &boot, metadata_base, metadata_length);
}

#ifdef KTEST_ENABLED
int pmm_multiboot_framebuffer_range_for_test(const multiboot_info_t *mbi,
                                             uint32_t *base_out,
                                             uint32_t *length_out)
{
	return pmm_multiboot_framebuffer_range(mbi, base_out, length_out);
}

void pmm_multiboot_apply_usable_ranges_for_test(pmm_core_state_t *state,
                                                const multiboot_info_t *mbi)
{
	x86_pmm_apply_usable_ranges(state, mbi);
}

void pmm_x86_init_state_from_multiboot_for_test(pmm_core_state_t *state,
                                                const multiboot_info_t *mbi,
                                                void *bitmap,
                                                void *refcounts)
{
	x86_pmm_boot_info_t boot;

	x86_pmm_snapshot_boot_info(&boot, mbi);
	pmm_core_bind_storage(state, bitmap, refcounts, boot.max_pages);
	pmm_core_init(state, boot.usable, boot.usable_count, 0, 0);
	x86_pmm_apply_reserved_ranges(state, &boot, 0, 0);
}
#endif

uint32_t pmm_alloc_page(void)
{
	return pmm_core_alloc_page(&g_pmm);
}

void pmm_free_page(uint32_t phys_addr)
{
	pmm_core_free_page(&g_pmm, phys_addr);
}

void pmm_incref(uint32_t phys_addr)
{
	pmm_core_incref(&g_pmm, phys_addr);
}

void pmm_decref(uint32_t phys_addr)
{
	pmm_core_decref(&g_pmm, phys_addr);
}

uint8_t pmm_refcount(uint32_t phys_addr)
{
	return pmm_core_refcount(&g_pmm, phys_addr);
}

uint32_t pmm_free_page_count(void)
{
	return pmm_core_free_page_count(&g_pmm);
}
