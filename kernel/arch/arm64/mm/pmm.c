/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pmm.c — arm64 physical memory provider backed by the shared PMM core.
 */

#include "pmm.h"

static pmm_core_state_t g_pmm;

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
	static const pmm_range_t usable[] = {
	    {.base = 0x00080000u, .length = 0x07f80000u},
	};
	static const pmm_range_t reserved[] = {
	    {.base = 0x00000000u, .length = 0x00080000u},
	    {.base = 0x3f000000u, .length = 0x01000000u},
	};

	pmm_core_init(&g_pmm, usable, 1u, reserved, 2u);
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
