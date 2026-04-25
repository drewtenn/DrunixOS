/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_MMU_H
#define KERNEL_ARCH_ARM64_MMU_H

#include "../../arch.h"
#include <stdint.h>

void arm64_mmu_init(void);
arch_aspace_t arm64_mmu_kernel_aspace(void);
arch_aspace_t arm64_mmu_aspace_create(void);
arch_aspace_t arm64_mmu_aspace_clone(arch_aspace_t src);
void arm64_mmu_aspace_switch(arch_aspace_t aspace);
void arm64_mmu_aspace_destroy(arch_aspace_t aspace);
void arm64_mmu_sync_current_from_identity(void);
void arm64_mmu_sync_current_to_identity(void);
int arm64_mmu_map(arch_aspace_t aspace,
                  uintptr_t virt,
                  uint64_t phys,
                  uint32_t flags);
int arm64_mmu_unmap(arch_aspace_t aspace, uintptr_t virt);
int arm64_mmu_query(arch_aspace_t aspace,
                    uintptr_t virt,
                    arch_mm_mapping_t *out);
int arm64_mmu_update(arch_aspace_t aspace,
                     uintptr_t virt,
                     uint32_t clear_flags,
                     uint32_t set_flags);
void arm64_mmu_invalidate_page(arch_aspace_t aspace, uintptr_t virt);
void *arm64_temp_map(uint64_t phys_addr);
void arm64_temp_unmap(void *ptr);

#endif
