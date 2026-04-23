/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pmm.c — x86 physical memory provider backed by the shared PMM core.
 */

#include "pmm.h"
#include <stdint.h>

#define X86_PMM_MAX_RANGES 32u

/* Linker-provided symbols: first and last byte of the kernel image */
extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

static pmm_core_state_t g_pmm;

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

static void x86_pmm_add_range(pmm_range_t *ranges,
                              uint32_t *count,
                              uint32_t max_count,
                              uint32_t base,
                              uint32_t length)
{
	if (!ranges || !count || length == 0 || *count >= max_count)
		return;

	ranges[*count].base = base;
	ranges[*count].length = length;
	(*count)++;
}

static void x86_pmm_collect_usable_ranges(const multiboot_info_t *mbi,
                                          pmm_range_t *usable,
                                          uint32_t *usable_count)
{
	uint32_t have_mmap = mbi && (mbi->flags & MULTIBOOT_FLAG_MMAP);

	*usable_count = 0;

	if (have_mmap) {
		multiboot_mmap_entry_t *entry =
		    (multiboot_mmap_entry_t *)mbi->mmap_addr;
		uint32_t map_end = mbi->mmap_addr + mbi->mmap_length;

		while ((uint32_t)entry < map_end) {
			if (entry->type == 1u) {
				x86_pmm_add_range(usable,
				                  usable_count,
				                  X86_PMM_MAX_RANGES,
				                  (uint32_t)entry->addr,
				                  (uint32_t)entry->len);
			}
			entry =
			    (multiboot_mmap_entry_t *)((uint32_t)entry + entry->size + 4u);
		}
	} else {
		x86_pmm_add_range(
		    usable, usable_count, X86_PMM_MAX_RANGES, 0x00100000u, 0x07F00000u);
	}
}

static void x86_pmm_collect_reserved_ranges(const multiboot_info_t *mbi,
                                            pmm_range_t *reserved,
                                            uint32_t *reserved_count)
{
	uint32_t kstart = (uint32_t)&_kernel_start;
	uint32_t kend = (uint32_t)&_kernel_end;
	uint32_t kend_page = (kend + PAGE_SIZE - 1u) / PAGE_SIZE;
	uint32_t fb_base;
	uint32_t fb_len;

	*reserved_count = 0;

	x86_pmm_add_range(
	    reserved, reserved_count, X86_PMM_MAX_RANGES, 0x00000000u, 0x00100000u);
	x86_pmm_add_range(reserved,
	                  reserved_count,
	                  X86_PMM_MAX_RANGES,
	                  kstart,
	                  kend_page * PAGE_SIZE - kstart);
	x86_pmm_add_range(
	    reserved, reserved_count, X86_PMM_MAX_RANGES, 0x00011000u, 0x00021000u);
	x86_pmm_add_range(
	    reserved, reserved_count, X86_PMM_MAX_RANGES, 0x00032000u, 0x000CE000u);
	x86_pmm_add_range(
	    reserved, reserved_count, X86_PMM_MAX_RANGES, 0x000A0000u, 0x00020000u);

	if (pmm_multiboot_framebuffer_range(mbi, &fb_base, &fb_len) == 0) {
		x86_pmm_add_range(
		    reserved, reserved_count, X86_PMM_MAX_RANGES, fb_base, fb_len);
	}
}

void pmm_mark_used(uint32_t base, uint32_t length)
{
	uint32_t page = base / PAGE_SIZE;
	uint32_t end = (uint32_t)(((uint64_t)base + (uint64_t)length + PAGE_SIZE - 1u) /
	                          PAGE_SIZE);

	for (; page < end && page < PMM_MAX_PAGES; page++)
		g_pmm.refcount[page] = 0xFFu;

	for (page = base / PAGE_SIZE;
	     page < end && page < PMM_MAX_PAGES;
	     page++)
		g_pmm.bitmap[page / 8] |= (uint8_t)(1u << (page % 8));
}

void pmm_mark_free(uint32_t base, uint32_t length)
{
	uint32_t page = base / PAGE_SIZE;
	uint32_t end = (uint32_t)(((uint64_t)base + (uint64_t)length + PAGE_SIZE - 1u) /
	                          PAGE_SIZE);

	for (; page < end && page < PMM_MAX_PAGES; page++) {
		g_pmm.bitmap[page / 8] &= (uint8_t)~(1u << (page % 8));
		g_pmm.refcount[page] = 0u;
	}
}

void pmm_init(multiboot_info_t *mbi)
{
	pmm_range_t usable[X86_PMM_MAX_RANGES];
	pmm_range_t reserved[X86_PMM_MAX_RANGES];
	uint32_t usable_count;
	uint32_t reserved_count;

	x86_pmm_collect_usable_ranges(mbi, usable, &usable_count);
	x86_pmm_collect_reserved_ranges(mbi, reserved, &reserved_count);
	pmm_core_init(&g_pmm, usable, usable_count, reserved, reserved_count);
}

#ifdef KTEST_ENABLED
int pmm_multiboot_framebuffer_range_for_test(const multiboot_info_t *mbi,
                                             uint32_t *base_out,
                                             uint32_t *length_out)
{
	return pmm_multiboot_framebuffer_range(mbi, base_out, length_out);
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
