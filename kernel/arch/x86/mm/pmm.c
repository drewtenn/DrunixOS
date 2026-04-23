/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pmm.c — x86 physical memory provider backed by the shared PMM core.
 */

#include "pmm.h"
#include <stdint.h>

/* Linker-provided symbols: first and last byte of the kernel image */
extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

static pmm_core_state_t g_pmm;

static int x86_pmm_managed_usable_range(uint64_t base,
                                        uint64_t length,
                                        uint32_t *base_out,
                                        uint32_t *length_out)
{
	const uint64_t managed_end = (uint64_t)PMM_MAX_PAGES * PAGE_SIZE;
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
                                          const multiboot_info_t *mbi)
{
	uint32_t kstart = (uint32_t)&_kernel_start;
	uint32_t kend = (uint32_t)&_kernel_end;
	uint32_t kend_page = (kend + PAGE_SIZE - 1u) / PAGE_SIZE;
	uint32_t fb_base;
	uint32_t fb_len;

	pmm_core_mark_used(state, 0x00000000u, 0x00100000u);
	pmm_core_mark_used(state, kstart, kend_page * PAGE_SIZE - kstart);
	pmm_core_mark_used(state, 0x00011000u, 0x00021000u);
	pmm_core_mark_used(state, 0x00032000u, 0x000CE000u);
	pmm_core_mark_used(state, 0x000A0000u, 0x00020000u);

	if (pmm_multiboot_framebuffer_range(mbi, &fb_base, &fb_len) == 0)
		pmm_core_mark_used(state, fb_base, fb_len);
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
	pmm_core_init(&g_pmm, 0, 0, 0, 0);
	x86_pmm_apply_usable_ranges(&g_pmm, mbi);
	x86_pmm_apply_reserved_ranges(&g_pmm, mbi);
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
