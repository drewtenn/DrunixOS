/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pmm_core.c — shared physical page allocator core.
 */

#include "pmm_core.h"

typedef struct {
	uint8_t bitmap[PMM_MAX_PAGES / 8];
	uint8_t refcount[PMM_MAX_PAGES];
} __attribute__((may_alias)) pmm_core_private_state_t;

_Static_assert(sizeof(pmm_core_private_state_t) == sizeof(pmm_core_state_t),
               "pmm_core_state_t size must match the private state layout");

static pmm_core_private_state_t *pmm_core_private(pmm_core_state_t *state)
{
	return (pmm_core_private_state_t *)state;
}

static const pmm_core_private_state_t *
pmm_core_private_const(const pmm_core_state_t *state)
{
	return (const pmm_core_private_state_t *)state;
}

static void pmm_core_bitmap_set(pmm_core_private_state_t *state, uint32_t page)
{
	state->bitmap[page / 8] |= (uint8_t)(1u << (page % 8));
}

static void pmm_core_bitmap_clear(pmm_core_private_state_t *state, uint32_t page)
{
	state->bitmap[page / 8] &= (uint8_t)~(1u << (page % 8));
}

static int pmm_core_bitmap_test(const pmm_core_private_state_t *state, uint32_t page)
{
	return state->bitmap[page / 8] & (uint8_t)(1u << (page % 8));
}

static void
pmm_core_mark_range(pmm_core_state_t *state, uint32_t base, uint32_t length, int used)
{
	pmm_core_private_state_t *private_state;
	uint64_t start_page;
	uint64_t end_page;

	if (!state || length == 0)
		return;

	private_state = pmm_core_private(state);
	start_page = (uint64_t)base / PAGE_SIZE;
	end_page = ((uint64_t)base + (uint64_t)length + PAGE_SIZE - 1u) / PAGE_SIZE;
	if (start_page >= PMM_MAX_PAGES)
		return;
	if (end_page > PMM_MAX_PAGES)
		end_page = PMM_MAX_PAGES;

	for (uint64_t page = start_page; page < end_page; page++) {
		if (used) {
			pmm_core_bitmap_set(private_state, (uint32_t)page);
			private_state->refcount[page] = 0xFFu;
		} else {
			pmm_core_bitmap_clear(private_state, (uint32_t)page);
			private_state->refcount[page] = 0u;
		}
	}
}

void pmm_core_init(pmm_core_state_t *state,
                   const pmm_range_t *usable,
                   uint32_t usable_count,
                   const pmm_range_t *reserved,
                   uint32_t reserved_count)
{
	pmm_core_private_state_t *private_state;

	if (!state)
		return;

	private_state = pmm_core_private(state);
	for (uint32_t i = 0; i < PMM_MAX_PAGES / 8; i++)
		private_state->bitmap[i] = 0xFFu;
	for (uint32_t i = 0; i < PMM_MAX_PAGES; i++)
		private_state->refcount[i] = 0xFFu;

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
	pmm_core_private_state_t *private_state;

	if (!state)
		return 0;

	private_state = pmm_core_private(state);
	for (uint32_t i = 0; i < PMM_MAX_PAGES / 8; i++) {
		if (private_state->bitmap[i] != 0xFFu) {
			for (uint32_t bit = 0; bit < 8; bit++) {
				if ((private_state->bitmap[i] & (1u << bit)) == 0) {
					uint32_t page = i * 8u + bit;

					pmm_core_bitmap_set(private_state, page);
					private_state->refcount[page] = 1u;
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
	pmm_core_private_state_t *private_state;
	uint32_t page;

	if (!state)
		return;

	private_state = pmm_core_private(state);
	page = phys_addr / PAGE_SIZE;
	if (page >= PMM_MAX_PAGES || private_state->refcount[page] == 0xFFu)
		return;
	if (private_state->refcount[page] == 0u)
		pmm_core_bitmap_set(private_state, page);
	if (private_state->refcount[page] < 0xFFu)
		private_state->refcount[page]++;
}

void pmm_core_decref(pmm_core_state_t *state, uint32_t phys_addr)
{
	pmm_core_private_state_t *private_state;
	uint32_t page;

	if (!state)
		return;

	private_state = pmm_core_private(state);
	page = phys_addr / PAGE_SIZE;
	if (page >= PMM_MAX_PAGES || private_state->refcount[page] == 0xFFu ||
	    private_state->refcount[page] == 0u)
		return;

	private_state->refcount[page]--;
	if (private_state->refcount[page] == 0u)
		pmm_core_bitmap_clear(private_state, page);
}

uint8_t pmm_core_refcount(const pmm_core_state_t *state, uint32_t phys_addr)
{
	const pmm_core_private_state_t *private_state;
	uint32_t page;

	if (!state)
		return 0;

	private_state = pmm_core_private_const(state);
	page = phys_addr / PAGE_SIZE;
	if (page >= PMM_MAX_PAGES)
		return 0;
	return private_state->refcount[page];
}

uint32_t pmm_core_free_page_count(const pmm_core_state_t *state)
{
	const pmm_core_private_state_t *private_state;
	uint32_t count = 0;

	if (!state)
		return 0;

	private_state = pmm_core_private_const(state);
	for (uint32_t page = 0; page < PMM_MAX_PAGES; page++) {
		if (!pmm_core_bitmap_test(private_state, page))
			count++;
	}

	return count;
}
