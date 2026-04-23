/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARCH_H
#define KERNEL_ARCH_ARCH_H

#include <stdint.h>

typedef void (*arch_irq_handler_fn)(void);
typedef uintptr_t arch_aspace_t;

typedef struct {
	uint64_t phys_addr;
	uint32_t flags;
} arch_mm_mapping_t;

#define ARCH_MM_MAP_PRESENT 0x0001u
#define ARCH_MM_MAP_READ 0x0002u
#define ARCH_MM_MAP_WRITE 0x0004u
#define ARCH_MM_MAP_EXEC 0x0008u
#define ARCH_MM_MAP_USER 0x0010u
#define ARCH_MM_MAP_COW 0x0020u

uint32_t arch_time_unix_seconds(void);
uint32_t arch_time_uptime_ticks(void);
void arch_console_write(const char *buf, uint32_t len);
void arch_debug_write(const char *buf, uint32_t len);
void arch_irq_init(void);
void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn);
void arch_irq_mask(uint32_t irq);
void arch_irq_unmask(uint32_t irq);
void arch_timer_set_periodic_handler(arch_irq_handler_fn fn);
void arch_timer_start(uint32_t hz);
void arch_interrupts_enable(void);
void arch_mm_init(void);
arch_aspace_t arch_aspace_kernel(void);
arch_aspace_t arch_aspace_create(void);
arch_aspace_t arch_aspace_clone(arch_aspace_t src);
void arch_aspace_switch(arch_aspace_t aspace);
void arch_aspace_destroy(arch_aspace_t aspace);
int arch_mm_map(arch_aspace_t aspace, uintptr_t virt, uint64_t phys, uint32_t flags);
int arch_mm_unmap(arch_aspace_t aspace, uintptr_t virt);
int arch_mm_query(arch_aspace_t aspace, uintptr_t virt, arch_mm_mapping_t *out);
/*
 * Scaffold limitation for Phase 3: arch_mm_update() only guarantees WRITE/COW
 * permission changes. PRESENT/USER transitions still require explicit
 * map/unmap paths on x86 until shared callers migrate behind the boundary.
 */
int arch_mm_update(arch_aspace_t aspace,
                   uintptr_t virt,
                   uint32_t clear_flags,
                   uint32_t set_flags);
void arch_mm_invalidate_page(arch_aspace_t aspace, uintptr_t virt);
void *arch_page_temp_map(uint64_t phys_addr);
void arch_page_temp_unmap(void *ptr);
uint32_t arch_mm_present_begin(void);
void arch_mm_present_end(uint32_t state);

#endif
