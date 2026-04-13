/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * paging.c — page directory and page table management for kernel and user mappings.
 */

#include "paging.h"
#include "pmm.h"
#include "kstring.h"
#include <stdint.h>

static void paging_invlpg(uint32_t virt)
{
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void paging_init(void) {
    uint32_t *pd = (uint32_t *)PAGE_DIR_ADDR;

    /* Clear the entire page directory */
    for (int i = 0; i < 1024; i++)
        pd[i] = 0;

    /*
     * Identity-map 0 – 128 MB using 32 page tables (each covers 4 MB).
     * Virtual address == physical address throughout this range, so the
     * kernel, VGA buffer, stack, and all our structures remain accessible
     * at the same addresses after paging is enabled.
     */
    for (int i = 0; i < 32; i++) {
        uint32_t  pt_phys = PAGE_TAB_BASE + (uint32_t)i * 0x1000;
        uint32_t *pt      = (uint32_t *)pt_phys;

        for (int j = 0; j < 1024; j++) {
            uint32_t phys = (uint32_t)i * 0x400000 + (uint32_t)j * 0x1000;
            pt[j] = phys | PG_PRESENT | PG_WRITABLE;
        }

        pd[i] = pt_phys | PG_PRESENT | PG_WRITABLE;
    }

    /* Load CR3 first, then set CR0.PG — order is critical */
    paging_load_cr3(PAGE_DIR_ADDR);
    paging_enable();
}

uint32_t paging_create_user_space(void)
{
    uint32_t pd_phys = pmm_alloc_page();
    if (!pd_phys) return 0;

    uint32_t *new_pd    = (uint32_t *)pd_phys;      /* identity-mapped: virt == phys */
    uint32_t *kernel_pd = (uint32_t *)PAGE_DIR_ADDR;

    /* Zero all 1024 PDEs */
    for (int i = 0; i < 1024; i++)
        new_pd[i] = 0;

    /*
     * Copy kernel PDEs 0–31 (covers the 128 MB identity map).
     * These entries do NOT have PG_USER set, so ring-3 code cannot read or
     * write kernel memory even though the mappings exist in the PD.
     * Supervisor (ring-0) accesses are unaffected by PG_USER.
     */
    for (int i = 0; i < 32; i++)
        new_pd[i] = kernel_pd[i];

    return pd_phys;
}

int paging_map_page(uint32_t pd_phys, uint32_t virt,
                    uint32_t phys, uint32_t flags)
{
    uint32_t *pd  = (uint32_t *)pd_phys;
    uint32_t  pdi = virt >> 22;            /* bits 31–22: PD index */
    uint32_t  pti = (virt >> 12) & 0x3FF; /* bits 21–12: PT index */

    uint32_t *pt;

    if (!(pd[pdi] & PG_PRESENT)) {
        /* No page table yet for this PDE — allocate one */
        uint32_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;

        pt = (uint32_t *)pt_phys;
        for (int i = 0; i < 1024; i++)
            pt[i] = 0;

        /* PDE must have PG_USER so the CPU will walk into it for ring-3 */
        pd[pdi] = paging_entry_build(pt_phys, PG_PRESENT | PG_WRITABLE | PG_USER);
    } else if ((flags & PG_USER) && !(pd[pdi] & PG_USER)) {
        /*
         * The PDE points to a kernel-inherited page table (no PG_USER set).
         * Writing user PTEs into the shared kernel page table would make
         * every process's mappings visible to every other process — each
         * subsequent process_create overwrites the previous one's entries.
         *
         * Instead, allocate a private page table for this process, copy the
         * existing kernel entries so ring-0 code still reaches kernel memory
         * in this address space, then replace the PDE.
         */
        uint32_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;

        uint32_t *old_pt = (uint32_t *)paging_entry_addr(pd[pdi]);
        pt = (uint32_t *)pt_phys;
        for (int i = 0; i < 1024; i++)
            pt[i] = old_pt[i];

        pd[pdi] = paging_entry_build(pt_phys, PG_PRESENT | PG_WRITABLE | PG_USER);
    } else {
        pt = (uint32_t *)paging_entry_addr(pd[pdi]);
        if (flags & PG_USER)
            pd[pdi] |= PG_USER;
    }

    pt[pti] = paging_entry_build(phys, flags);
    paging_invlpg(virt);
    return 0;
}

int paging_walk(uint32_t pd_phys, uint32_t virt, uint32_t **pte_out)
{
    uint32_t *pd  = (uint32_t *)pd_phys;
    uint32_t  pdi = virt >> 22;
    uint32_t  pti = (virt >> 12) & 0x3FF;

    if (!(pd[pdi] & PG_PRESENT))
        return -1;

    uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFFu);
    if (!(pt[pti] & PG_PRESENT))
        return -1;

    *pte_out = &pt[pti];
    return 0;
}

void paging_switch_directory(uint32_t pd_phys)
{
    paging_load_cr3(pd_phys);
}

/*
 * paging_guard_page: mark a kernel-heap page non-present in the shared
 * kernel page tables so any access to it causes a page fault.
 *
 * Used to create a no-access guard page at the bottom of each per-process
 * kernel stack.  All kernel page tables live at the fixed addresses set up
 * by paging_init(), so the PTE is reached by direct index arithmetic rather
 * than a PDE walk.  Only valid for addresses in the 0–128 MB identity map
 * (PDE index 0–31).
 */
void paging_guard_page(uint32_t virt)
{
    uint32_t pde_idx = virt >> 22;
    uint32_t pte_idx = (virt >> 12) & 0x3FF;
    uint32_t *pt = (uint32_t *)(PAGE_TAB_BASE + pde_idx * 0x1000);
    pt[pte_idx] &= ~(uint32_t)PG_PRESENT;
    paging_invlpg(virt);
}

/*
 * paging_unguard_page: restore PG_PRESENT | PG_WRITABLE on a page
 * previously marked non-present by paging_guard_page().
 *
 * Must be called before kfree()-ing the allocation that contains the
 * guard page, so the allocator can safely read and write its block header.
 */
void paging_unguard_page(uint32_t virt)
{
    uint32_t pde_idx = virt >> 22;
    uint32_t pte_idx = (virt >> 12) & 0x3FF;
    uint32_t *pt = (uint32_t *)(PAGE_TAB_BASE + pde_idx * 0x1000);
    pt[pte_idx] |= PG_PRESENT | PG_WRITABLE;
    paging_invlpg(virt);
}

uint32_t paging_clone_user_space(uint32_t src_pd_phys)
{
    uint32_t new_pd_phys = pmm_alloc_page();
    if (!new_pd_phys) return 0;

    uint32_t *new_pd = (uint32_t *)new_pd_phys;
    uint32_t *src_pd = (uint32_t *)src_pd_phys;

    for (int i = 0; i < 1024; i++) new_pd[i] = 0;

    /*
     * Two-pass clone to guarantee safe rollback on OOM.
     *
     * Pass 1 — allocate child page tables.
     * No parent PTEs are modified yet, so an allocation failure is trivially
     * rolled back: free the child page tables allocated so far plus the PD.
     *
     * Pass 2 — apply CoW flags and populate child page tables.
     * All allocations have already succeeded so this pass cannot fail.
     */

    /* ── Pass 1: allocate child page tables ─────────────────────────────── */
    for (int i = 0; i < 1024; i++) {
        if (!(src_pd[i] & PG_PRESENT)) continue;

        uint32_t *src_pt = (uint32_t *)paging_entry_addr(src_pd[i]);
        int has_user_pte = 0;
        for (int j = 0; j < 1024; j++) {
            if ((src_pt[j] & (PG_PRESENT | PG_USER)) == (PG_PRESENT | PG_USER)) {
                has_user_pte = 1;
                break;
            }
        }

        if (!has_user_pte) {
            new_pd[i] = src_pd[i];
            continue;
        }

        uint32_t new_pt_phys = pmm_alloc_page();
        if (!new_pt_phys) {
            /* Free only the child page tables we allocated (not shared ones).
             * A shared PDE has the same PT base as the source; an allocated
             * one differs. */
            for (int k = 0; k < i; k++) {
                if (!(new_pd[k] & PG_PRESENT)) continue;
                if (paging_entry_addr(new_pd[k]) != paging_entry_addr(src_pd[k]))
                    pmm_free_page(paging_entry_addr(new_pd[k]));
            }
            pmm_free_page(new_pd_phys);
            return 0;
        }

        new_pd[i] = paging_entry_build(new_pt_phys, paging_entry_flags(src_pd[i]));
    }

    /* ── Pass 2: apply CoW flags and populate child PTs ─────────────────── */
    for (int i = 0; i < 1024; i++) {
        if (!(new_pd[i] & PG_PRESENT)) continue;

        /* Skip shared kernel page tables (same PT base as source) */
        if (paging_entry_addr(new_pd[i]) == paging_entry_addr(src_pd[i])) continue;

        uint32_t *src_pt = (uint32_t *)paging_entry_addr(src_pd[i]);
        uint32_t *new_pt = (uint32_t *)paging_entry_addr(new_pd[i]);

        /* Copy all entries (kernel + user) into the child PT */
        for (int j = 0; j < 1024; j++)
            new_pt[j] = src_pt[j];

        /* Mark user entries CoW in parent and child, bump refcounts */
        for (int j = 0; j < 1024; j++) {
            if ((src_pt[j] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
                continue;

            uint32_t virt = ((uint32_t)i << 22) | ((uint32_t)j << 12);
            if (src_pt[j] & PG_WRITABLE) {
                src_pt[j] = (src_pt[j] & ~PG_WRITABLE) | PG_COW;
                paging_invlpg(virt);
            }

            new_pt[j] = src_pt[j];
            pmm_incref(paging_entry_addr(src_pt[j]));
        }
    }

    return new_pd_phys;
}
