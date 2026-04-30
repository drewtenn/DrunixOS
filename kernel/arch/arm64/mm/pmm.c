/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pmm.c — arm64 physical memory provider backed by the shared PMM core.
 *
 * Phase 1 M2.4b: per-platform sizing for the static bitmap and
 * refcount arrays. virt RAM at 0x40000000-0x80000000 needs page
 * indices up to 0x80000 (= 524 288); a 1 M-page cap (4 GiB
 * addressable) covers it with room for any FDT-reported variation.
 * raspi3b stays at 64 K pages (256 MiB addressable) so the smaller
 * platform does not pay for virt's larger BSS arrays.
 *
 * Bitmap = 128 KiB on virt, 8 KiB on raspi3b.
 * Refcounts = 1 MiB on virt, 64 KiB on raspi3b.
 *
 * The runtime ranges fed to pmm_core_init are derived from
 * platform_ram_layout(); raspi3b returns its hardcoded constants and
 * virt returns FDT-derived values populated at platform_init().
 */

#include "pmm.h"
#include "platform/platform.h"

#if DRUNIX_ARM64_PLATFORM_VIRT
#define ARM64_PMM_MAX_PAGES (0x100000u)
#else
#define ARM64_PMM_MAX_PAGES (0x10000000u / PAGE_SIZE)
#endif

static pmm_core_state_t g_pmm;
static uint8_t g_pmm_bitmap[(ARM64_PMM_MAX_PAGES + 7u) / 8u];
static uint8_t g_pmm_refcounts[ARM64_PMM_MAX_PAGES];

void pmm_mark_used(uint32_t base, uint32_t length)
{
	pmm_core_mark_used(&g_pmm, base, length);
}

void pmm_mark_free(uint32_t base, uint32_t length)
{
	pmm_core_mark_free(&g_pmm, base, length);
}

void pmm_init(void)
{
	const platform_ram_layout_t *l = platform_ram_layout();
	pmm_range_t usable[1];
	pmm_range_t reserved[2];
	uint32_t reserved_count;

	usable[0].base = (uint32_t)l->ram_base;
	usable[0].length = (uint32_t)l->ram_size;

	reserved_count = 0;
	if (l->ram_base == 0x00000000ull) {
		/* raspi3b: reserve low 512 KiB and the GPU/peripheral region. */
		reserved[reserved_count].base = 0x00000000u;
		reserved[reserved_count].length = 0x00080000u;
		reserved_count++;
		reserved[reserved_count].base = 0x3f000000u;
		reserved[reserved_count].length = 0x01000000u;
		reserved_count++;
	}
	/* virt: peripherals live below RAM (outside [ram_base, ram_base+size)),
	 * so PMM never sees them and no reservation is needed at this layer. */

	pmm_core_bind_storage(
	    &g_pmm, g_pmm_bitmap, g_pmm_refcounts, ARM64_PMM_MAX_PAGES);
	pmm_core_init(&g_pmm, usable, 1u, reserved, reserved_count);
}

uint32_t pmm_alloc_page(void)
{
	return pmm_core_alloc_page(&g_pmm);
}

void pmm_incref(uint32_t phys_addr)
{
	pmm_core_incref(&g_pmm, phys_addr);
}

void pmm_decref(uint32_t phys_addr)
{
	pmm_core_decref(&g_pmm, phys_addr);
}

void pmm_free_page(uint32_t phys_addr)
{
	pmm_core_free_page(&g_pmm, phys_addr);
}

uint8_t pmm_refcount(uint32_t phys_addr)
{
	return pmm_core_refcount(&g_pmm, phys_addr);
}

uint32_t pmm_free_page_count(void)
{
	return pmm_core_free_page_count(&g_pmm);
}
