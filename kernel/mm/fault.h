/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef FAULT_H
#define FAULT_H

#include "arch.h"
#include "process.h"
#include <stdint.h>

/*
 * paging_handle_fault: attempt to resolve a user-space page fault in-place.
 *
 * Returns 0 if the fault was handled and execution may resume at the faulting
 * instruction. Returns -1 if the fault should fall through to the existing
 * SIGSEGV or kernel-panic path.
 */
int paging_handle_fault(uint32_t pd_phys,
                        uint32_t cr2,
                        uint32_t err,
                        uint32_t user_esp,
                        process_t *cur);

int fault_handle_lazy_file_private_fault(arch_aspace_t aspace,
                                         uint32_t fault_page,
                                         const vm_area_t *vma);

#endif
