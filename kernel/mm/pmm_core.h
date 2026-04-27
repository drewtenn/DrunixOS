/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PMM_CORE_H
#define PMM_CORE_H

#include <stdint.h>

#define PAGE_SIZE 4096u

typedef struct {
	uint32_t base;
	uint32_t length;
} pmm_range_t;

typedef struct {
	uint8_t *bitmap;
	uint8_t *refcounts;
	uint32_t max_pages;
} pmm_core_state_t;

void pmm_core_bind_storage(pmm_core_state_t *state,
                           void *bitmap,
                           void *refcounts,
                           uint32_t max_pages);
uint32_t pmm_core_bitmap_bytes(uint32_t max_pages);
uint32_t pmm_core_refcount_bytes(uint32_t max_pages);
void pmm_core_init(pmm_core_state_t *state,
                   const pmm_range_t *usable,
                   uint32_t usable_count,
                   const pmm_range_t *reserved,
                   uint32_t reserved_count);
void pmm_core_mark_used(pmm_core_state_t *state, uint32_t base, uint32_t length);
void pmm_core_mark_free(pmm_core_state_t *state, uint32_t base, uint32_t length);
uint32_t pmm_core_alloc_page(pmm_core_state_t *state);
void pmm_core_free_page(pmm_core_state_t *state, uint32_t phys_addr);
void pmm_core_incref(pmm_core_state_t *state, uint32_t phys_addr);
void pmm_core_decref(pmm_core_state_t *state, uint32_t phys_addr);
uint8_t pmm_core_refcount(const pmm_core_state_t *state, uint32_t phys_addr);
uint32_t pmm_core_free_page_count(const pmm_core_state_t *state);

#endif
