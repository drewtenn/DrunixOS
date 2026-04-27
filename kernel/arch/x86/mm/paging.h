/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PAGING_H
#define PAGING_H

#include "arch.h"
#include <stdint.h>

/* Physical addresses for paging structures */
#define PAGE_DIR_ADDR 0x00011000u /* page directory: 4 KB, 1024 PDEs */
#define PAGE_TAB_BASE 0x00012000u /* first kernel direct-map page table */

/* PDE / PTE flag bits */
#define PG_PRESENT 0x1
#define PG_WRITABLE 0x2
#define PG_USER 0x4
#define PG_PWT 0x8
#define PG_PCD 0x10
#define PG_PAT_4K 0x80 /* Bit 7 on a 4 KB PTE selects the upper PAT half */
#define PG_COW (1u << 9)
/*
 * PG_IO marks an entry whose physical frame is device memory (MMIO),
 * not a PMM-managed page. paging_unmap_page() must not decref such
 * frames. Stored in one of the OS-available bits (10) of the PTE.
 */
#define PG_IO (1u << 10)

#define PG_ENTRY_ADDR_MASK 0xFFFFF000u
#define PG_ENTRY_FLAGS_MASK 0x00000FFFu

static inline uint32_t paging_entry_addr(uint32_t entry)
{
	return entry & PG_ENTRY_ADDR_MASK;
}

static inline uint32_t paging_entry_flags(uint32_t entry)
{
	return entry & PG_ENTRY_FLAGS_MASK;
}

static inline uint32_t paging_entry_build(uint32_t addr, uint32_t flags)
{
	return paging_entry_addr(addr) | paging_entry_flags(flags);
}

static inline uint32_t paging_entry_replace_addr(uint32_t entry, uint32_t addr)
{
	return paging_entry_build(addr, entry);
}

void paging_init(void);

/*
 * paging_identity_map_kernel_range: identity-map a physical range in the
 * shared kernel page directory as supervisor memory.
 *
 * This is used for device memory such as a Multiboot-provided framebuffer that
 * can live above the low 128 MB identity map created at boot.
 *
 * phys_start: physical start address; may be unaligned.
 * byte_len:   number of bytes to map.
 * flags:      PTE flags; PG_USER is intentionally stripped.
 *
 * Returns 0 on success, -1 on overflow or page-table allocation failure.
 */
int paging_identity_map_kernel_range(uint32_t phys_start,
                                     uint32_t byte_len,
                                     uint32_t flags);

/*
 * paging_mark_range_write_combining: switch an identity-mapped physical
 * range's cacheability to write-combining (WC) so successive writes — such
 * as every memcpy that flushes the back buffer to the visible
 * framebuffer — coalesce into burst transactions instead of paying a full
 * bus cycle per store.
 *
 * On first call this programs the IA32_PAT MSR's PA4 slot to WC if the
 * CPU supports PAT; subsequent calls reuse the same slot. Pages in the
 * range are rewritten with PAT=1, PCD=0, PWT=0 (which selects PA4) and
 * each page's TLB entry is invalidated.
 *
 * Returns 0 on success, -1 if the CPU doesn't support PAT, -2 if the PTE
 * for any page in the range is not present.
 */
int paging_mark_range_write_combining(uint32_t phys_start, uint32_t byte_len);

/* Implemented in paging.asm — CR3/CR0 cannot be written from plain C */
extern void paging_load_cr3(uint32_t pd_phys);
extern void paging_enable(void);

/*
 * paging_create_user_space: allocate a fresh page directory for a new process.
 *
 * Copies all present supervisor-only kernel PDEs into the new directory
 * without PG_USER, so ring-3 code cannot reach kernel memory. User pages are
 * added separately with paging_map_page().
 *
 * Returns the physical address of the new page directory, or 0 on failure.
 */
uint32_t paging_create_user_space(void);
void paging_destroy_user_space(uint32_t pd_phys);

/*
 * paging_map_page: install a single page mapping in an arbitrary page directory.
 *
 * pd_phys: physical address of the target page directory.
 * virt:    virtual address to map (must be 4 KB aligned).
 * phys:    physical address to map to (must be 4 KB aligned).
 * flags:   combination of PG_PRESENT, PG_WRITABLE, PG_USER, etc.
 *
 * PG_USER mappings are accepted only inside the x86 user virtual range.
 * Allocates a new page table via pmm_alloc_page() if the PDE is not yet present.
 * Paging-structure physical pages are accessed through the x86 kernel direct
 * map, so PMM pages above the legacy low identity window work.
 * Returns 0 on success, -1 if the address is invalid or a page table could not
 * be allocated.
 */
int paging_map_page(uint32_t pd_phys,
                    uint32_t virt,
                    uint32_t phys,
                    uint32_t flags);
int paging_unmap_page(uint32_t pd_phys, uint32_t virt);

/*
 * paging_walk: locate the live PTE slot for a virtual address.
 *
 * Returns 0 on success and writes a kernel-accessible PTE pointer to pte_out.
 * Returns -1 if the PDE or PTE is not present.
 */
int paging_walk(uint32_t pd_phys, uint32_t virt, uint32_t **pte_out);
int paging_query_page(uint32_t pd_phys, uint32_t virt, arch_mm_mapping_t *out);
/*
 * Phase 3 scaffold limitation: this helper only supports WRITE/COW changes.
 * PRESENT/USER transitions must keep using x86-specific map/unmap flows until
 * shared callers migrate and the backend grows stronger semantics.
 */
int paging_update_page(uint32_t pd_phys,
                       uint32_t virt,
                       uint32_t clear_flags,
                       uint32_t set_flags);
void paging_invalidate_page(uint32_t virt);
void *paging_temp_map(uint32_t phys_addr);
void paging_temp_unmap(void *ptr);
uint32_t paging_present_begin(void);
void paging_present_end(uint32_t flags);

/*
 * paging_clone_user_space: copy-on-write clone of a process's user space.
 *
 * Kernel-only PDEs are shared by reference.  For every PDE that contains
 * user PTEs, a private child page table is allocated and all user pages are
 * shared with CoW semantics: PG_WRITABLE is cleared and PG_COW is set in
 * both the parent and child PTEs, and the physical frame's refcount is
 * incremented.
 *
 * Allocation and PTE modification are separated into two passes so that an
 * OOM during page-table allocation can be rolled back without leaving stale
 * CoW flags in the parent's page tables.
 *
 * Returns the physical address of the new page directory, or 0 on failure.
 */
uint32_t paging_clone_user_space(uint32_t src_pd_phys);

/*
 * paging_switch_directory: activate a page directory by loading CR3.
 */
void paging_switch_directory(uint32_t pd_phys);

/*
 * paging_guard_page / paging_unguard_page: manage a non-present guard page
 * in the shared kernel identity-map page tables.
 *
 * paging_guard_page:   clear PG_PRESENT so any access faults immediately.
 * paging_unguard_page: restore PG_PRESENT | PG_WRITABLE (call before kfree).
 *
 * Only valid for virt in 0–128 MB (PDE 0–31), which covers the kernel heap.
 */
void paging_guard_page(uint32_t virt);
void paging_unguard_page(uint32_t virt);

#endif
