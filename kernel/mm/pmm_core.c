/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pmm_core.c — shared physical page allocator core.
 */

#include "pmm_core.h"

static int pmm_core_has_storage(const pmm_core_state_t *state)
{
	return state && state->bitmap && state->refcounts && state->max_pages > 0u;
}

uint32_t pmm_core_bitmap_bytes(uint32_t max_pages)
{
	return (max_pages + 7u) / 8u;
}

uint32_t pmm_core_refcount_bytes(uint32_t max_pages)
{
	return max_pages;
}

void pmm_core_bind_storage(pmm_core_state_t *state,
                           void *bitmap,
                           void *refcounts,
                           uint32_t max_pages)
{
	if (!state)
		return;

	state->bitmap = (uint8_t *)bitmap;
	state->refcounts = (uint8_t *)refcounts;
	state->max_pages = max_pages;
}

static void pmm_core_bitmap_set(pmm_core_state_t *state, uint32_t page)
{
	state->bitmap[page / 8u] |= (uint8_t)(1u << (page % 8u));
}

static void pmm_core_bitmap_clear(pmm_core_state_t *state, uint32_t page)
{
	state->bitmap[page / 8u] &= (uint8_t)~(1u << (page % 8u));
}

static int pmm_core_bitmap_test(const pmm_core_state_t *state, uint32_t page)
{
	return state->bitmap[page / 8u] & (uint8_t)(1u << (page % 8u));
}

static void
pmm_core_mark_range(pmm_core_state_t *state, uint32_t base, uint32_t length, int used)
{
	uint64_t start_page;
	uint64_t end_page;

	if (!pmm_core_has_storage(state) || length == 0u)
		return;

	start_page = (uint64_t)base / PAGE_SIZE;
	end_page = ((uint64_t)base + (uint64_t)length + PAGE_SIZE - 1u) / PAGE_SIZE;
	if (start_page >= state->max_pages)
		return;
	if (end_page > state->max_pages)
		end_page = state->max_pages;

	for (uint64_t page = start_page; page < end_page; page++) {
		uint32_t page32 = (uint32_t)page;

		if (used) {
			pmm_core_bitmap_set(state, page32);
			state->refcounts[page32] = 0xFFu;
		} else {
			pmm_core_bitmap_clear(state, page32);
			state->refcounts[page32] = 0u;
		}
	}
}

void pmm_core_init(pmm_core_state_t *state,
                   const pmm_range_t *usable,
                   uint32_t usable_count,
                   const pmm_range_t *reserved,
                   uint32_t reserved_count)
{
	uint32_t bitmap_bytes;

	if (!pmm_core_has_storage(state))
		return;

	bitmap_bytes = pmm_core_bitmap_bytes(state->max_pages);
	for (uint32_t i = 0; i < bitmap_bytes; i++)
		state->bitmap[i] = 0xFFu;
	for (uint32_t i = 0; i < state->max_pages; i++)
		state->refcounts[i] = 0xFFu;

	for (uint32_t i = 0; i < usable_count; i++)
		pmm_core_mark_range(state, usable[i].base, usable[i].length, 0);
	for (uint32_t i = 0; i < reserved_count; i++)
		pmm_core_mark_range(state, reserved[i].base, reserved[i].length, 1);
}

void pmm_core_mark_used(pmm_core_state_t *state, uint32_t base, uint32_t length)
{
	pmm_core_mark_range(state, base, length, 1);
}

void pmm_core_mark_free(pmm_core_state_t *state, uint32_t base, uint32_t length)
{
	pmm_core_mark_range(state, base, length, 0);
}

uint32_t pmm_core_alloc_page(pmm_core_state_t *state)
{
	uint32_t bitmap_bytes;

	if (!pmm_core_has_storage(state))
		return 0;

	bitmap_bytes = pmm_core_bitmap_bytes(state->max_pages);
	for (uint32_t i = 0; i < bitmap_bytes; i++) {
		if (state->bitmap[i] != 0xFFu) {
			for (uint32_t bit = 0; bit < 8u; bit++) {
				uint32_t page = i * 8u + bit;

				if (page >= state->max_pages)
					return 0;
				if ((state->bitmap[i] & (1u << bit)) == 0u) {
					pmm_core_bitmap_set(state, page);
					state->refcounts[page] = 1u;
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

	if (!pmm_core_has_storage(state))
		return;

	page = phys_addr / PAGE_SIZE;
	if (page >= state->max_pages || state->refcounts[page] == 0xFFu)
		return;
	if (state->refcounts[page] == 0u)
		pmm_core_bitmap_set(state, page);
	if (state->refcounts[page] < 0xFFu)
		state->refcounts[page]++;
}

void pmm_core_decref(pmm_core_state_t *state, uint32_t phys_addr)
{
	uint32_t page;

	if (!pmm_core_has_storage(state))
		return;

	page = phys_addr / PAGE_SIZE;
	if (page >= state->max_pages || state->refcounts[page] == 0xFFu ||
	    state->refcounts[page] == 0u)
		return;

	state->refcounts[page]--;
	if (state->refcounts[page] == 0u)
		pmm_core_bitmap_clear(state, page);
}

uint8_t pmm_core_refcount(const pmm_core_state_t *state, uint32_t phys_addr)
{
	uint32_t page;

	if (!pmm_core_has_storage(state))
		return 0;

	page = phys_addr / PAGE_SIZE;
	if (page >= state->max_pages)
		return 0;
	return state->refcounts[page];
}

uint32_t pmm_core_free_page_count(const pmm_core_state_t *state)
{
	uint32_t count = 0;

	if (!pmm_core_has_storage(state))
		return 0;

	for (uint32_t page = 0; page < state->max_pages; page++) {
		if (!pmm_core_bitmap_test(state, page))
			count++;
	}

	return count;
}
