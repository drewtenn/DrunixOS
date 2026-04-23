/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PMM_H
#define PMM_H

#include "../../../mm/pmm_core.h"
#include <stdint.h>

void pmm_init(void);
void pmm_mark_used(uint32_t base, uint32_t length);
void pmm_mark_free(uint32_t base, uint32_t length);
uint32_t pmm_alloc_page(void);
void pmm_incref(uint32_t phys_addr);
void pmm_decref(uint32_t phys_addr);
void pmm_free_page(uint32_t phys_addr);
uint8_t pmm_refcount(uint32_t phys_addr);
uint32_t pmm_free_page_count(void);

#endif
