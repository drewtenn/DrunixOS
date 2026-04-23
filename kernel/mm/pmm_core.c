/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pmm_core.c — shared physical page allocator core.
 */

#include "pmm_core.h"

static void pmm_core_bitmap_set(pmm_core_state_t *state, uint32_t page)
{
	state->bitmap[page / 8] |= (uint8_t)(1u << (page % 8));
}

static void pmm_core_bitmap_clear(pmm_core_state_t *state, uint32_t page)
{
	state->bitmap[page / 8] &= (uint8_t)~(1u << (page % 8));
}

static int pmm_core_bitmap_test(const pmm_core_state_t *state, uint32_t page)
{
	return state->bitmap[page / 8] & (uint8_t)(1u << (page % 8));
}

static void
pmm_core_mark_range(pmm_core_state_t *state, uint32_t base, uint32_t length, int used)
{
	uint64_t start_page;
	uint64_t end_page;

	if (!state || length == 0)
		return;

	start_page = (uint64_t)base / PAGE_SIZE;
	end_page = ((uint64_t)base + (uint64_t)length + PAGE_SIZE - 1u) / PAGE_SIZE;
	if (start_page >= PMM_MAX_PAGES)
		return;
	if (end_page > PMM_MAX_PAGES)
		end_page = PMM_MAX_PAGES;

	for (uint64_t page = start_page; page < end_page; page++) {
		if (used) {
			pmm_core_bitmap_set(state, (uint32_t)page);
			state->refcount[page] = 0xFFu;
		} else {
			pmm_core_bitmap_clear(state, (uint32_t)page);
			state->refcount[page] = 0u;
		}
	}
}

void pmm_core_init(pmm_core_state_t *state,
                   const pmm_range_t *usable,
                   uint32_t usable_count,
                   const pmm_range_t *reserved,
                   uint32_t reserved_count)
{
	if (!state)
		return;

	for (uint32_t i = 0; i < PMM_MAX_PAGES / 8; i++)
		state->bitmap[i] = 0xFFu;
	for (uint32_t i = 0; i < PMM_MAX_PAGES; i++)
		state->refcount[i] = 0xFFu;

	for (uint32_t i = 0; i < usable_count; i++)
		pmm_core_mark_range(state, usable[i].base, usable[i].length, 0);
	for (uint32_t i = 0; i < reserved_count; i++)
		pmm_core_mark_range(state, reserved[i].base, reserved[i].length, 1);
}

uint32_t pmm_core_alloc_page(pmm_core_state_t *state)
{
	if (!state)
		return 0;

	for (uint32_t i = 0; i < PMM_MAX_PAGES / 8; i++) {
		if (state->bitmap[i] != 0xFFu) {
			for (uint32_t bit = 0; bit < 8; bit++) {
				if ((state->bitmap[i] & (1u << bit)) == 0) {
					uint32_t page = i * 8u + bit;

					pmm_core_bitmap_set(state, page);
					state->refcount[page] = 1u;
					return page * PAGE_SIZE;
				}
			}
		}
	}

	return 0;
}

void pmm_core_free_page(pmm_core_state_t *state, uint32_t phys_addr)
{
	pmm_core_decref(state, phys_addr);
}

void pmm_core_incref(pmm_core_state_t *state, uint32_t phys_addr)
{
	uint32_t page;

	if (!state)
		return;

	page = phys_addr / PAGE_SIZE;
	if (page >= PMM_MAX_PAGES || state->refcount[page] == 0xFFu)
		return;
	if (state->refcount[page] == 0u)
		pmm_core_bitmap_set(state, page);
	if (state->refcount[page] < 0xFFu)
		state->refcount[page]++;
}

void pmm_core_decref(pmm_core_state_t *state, uint32_t phys_addr)
{
	uint32_t page;

	if (!state)
		return;

	page = phys_addr / PAGE_SIZE;
	if (page >= PMM_MAX_PAGES || state->refcount[page] == 0xFFu ||
	    state->refcount[page] == 0u)
		return;

	state->refcount[page]--;
	if (state->refcount[page] == 0u)
		pmm_core_bitmap_clear(state, page);
}

uint8_t pmm_core_refcount(const pmm_core_state_t *state, uint32_t phys_addr)
{
	uint32_t page;

	if (!state)
		return 0;

	page = phys_addr / PAGE_SIZE;
	if (page >= PMM_MAX_PAGES)
		return 0;
	return state->refcount[page];
}

uint32_t pmm_core_free_page_count(const pmm_core_state_t *state)
{
	uint32_t count = 0;

	if (!state)
		return 0;

	for (uint32_t page = 0; page < PMM_MAX_PAGES; page++) {
		if (!pmm_core_bitmap_test(state, page))
			count++;
	}

	return count;
}
