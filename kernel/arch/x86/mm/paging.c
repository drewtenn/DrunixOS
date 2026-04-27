/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * paging.c — page directory and page table management for kernel and user mappings.
 */

#include "paging.h"
#include "arch_layout.h"
#include "pmm.h"
#include "kstring.h"
#include <stdint.h>

/* IA32 Page Attribute Table MSR. */
#define PAGING_IA32_PAT_MSR 0x277u

/* PAT memory types (3-bit fields inside the IA32_PAT MSR). */
#define PAGING_PAT_TYPE_UC 0x00u
#define PAGING_PAT_TYPE_WC 0x01u
#define PAGING_PAT_TYPE_WT 0x04u
#define PAGING_PAT_TYPE_WB 0x06u

/* Which PAT slot we reserve for write-combining. Index 4 is selected by
 * PAT=1, PCD=0, PWT=0 in a 4 KB PTE; by default it holds WB like PA0, so
 * we're free to repurpose it without disturbing the more commonly used
 * entries 0-3. */
#define PAGING_PAT_WC_SLOT 4

static int g_pat_wc_slot_ready;
static int g_paging_present_depth;
static uint32_t g_paging_present_saved_cr3;
static uint32_t g_kernel_high_page_tables[ARCH_KERNEL_DIRECT_PHYS_MAX /
                                          0x400000u][1024]
    __attribute__((aligned(PAGE_SIZE)));
extern char _kernel_end;

#define X86_KERNEL_DIRECT_MAP_PDES \
	((uint32_t)ARCH_KERNEL_DIRECT_MAP_END / 0x400000u)
#define X86_KERNEL_HIGH_MAP_PDES \
	((uint32_t)ARCH_KERNEL_DIRECT_PHYS_MAX / 0x400000u)

static void *paging_phys_ptr(uint32_t phys)
{
	if (phys >= (uint32_t)ARCH_KERNEL_DIRECT_PHYS_MAX)
		return 0;
	return (void *)ARCH_KERNEL_PHYS_TO_VIRT(phys);
}

static int paging_identity_virtual_range_allowed(uint32_t start, uint32_t end)
{
	/* The higher-half window is reserved for ARCH_KERNEL_PHYS_TO_VIRT(). */
	return start < (uint32_t)ARCH_KERNEL_VIRT_BASE &&
	       end <= (uint32_t)ARCH_KERNEL_VIRT_BASE;
}

static uint32_t paging_read_cr3(void)
{
	uint32_t cr3;

	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	return cr3;
}

static int paging_user_page_allowed(uint32_t virt)
{
	if (virt < (uint32_t)ARCH_USER_VADDR_MIN)
		return 0;
	if (virt > (uint32_t)ARCH_USER_VADDR_MAX - 0x1000u)
		return 0;
	return 1;
}

static void paging_invlpg(uint32_t virt)
{
	__asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

static int
paging_lookup_slot(uint32_t pd_phys, uint32_t virt, uint32_t **pte_out)
{
	uint32_t *pd = (uint32_t *)paging_phys_ptr(pd_phys);
	uint32_t pdi = virt >> 22;
	uint32_t pti = (virt >> 12) & 0x3FFu;
	uint32_t *pt;

	if (!pd)
		return -1;
	if (!(pd[pdi] & PG_PRESENT))
		return -1;

	pt = (uint32_t *)paging_phys_ptr(paging_entry_addr(pd[pdi]));
	if (!pt)
		return -1;
	*pte_out = &pt[pti];
	return 0;
}

static int paging_cpu_supports_pat(void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	__asm__ volatile("cpuid"
	                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
	                 : "a"(1u), "c"(0u));
	/* CPUID.01H:EDX bit 16 = PAT supported. */
	return (edx & (1u << 16)) != 0;
}

static uint64_t paging_read_msr(uint32_t msr)
{
	uint32_t lo;
	uint32_t hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static void paging_write_msr(uint32_t msr, uint64_t value)
{
	uint32_t lo = (uint32_t)value;
	uint32_t hi = (uint32_t)(value >> 32);

	__asm__ volatile("wrmsr" ::"c"(msr), "a"(lo), "d"(hi));
}

static int paging_prepare_wc_slot(void)
{
	uint64_t pat;
	unsigned int shift;
	uint64_t mask;

	if (g_pat_wc_slot_ready)
		return 0;
	if (!paging_cpu_supports_pat())
		return -1;

	pat = paging_read_msr(PAGING_IA32_PAT_MSR);
	shift = PAGING_PAT_WC_SLOT * 8u;
	mask = 0x7ull << shift;
	pat = (pat & ~mask) | ((uint64_t)PAGING_PAT_TYPE_WC << shift);
	paging_write_msr(PAGING_IA32_PAT_MSR, pat);

	g_pat_wc_slot_ready = 1;
	return 0;
}

static int paging_mark_pte_write_combining(uint32_t virt)
{
	uint32_t *pte;
	uint32_t entry;
	uint32_t flags;

	if (paging_lookup_slot(PAGE_DIR_ADDR, virt, &pte) != 0)
		return -2;
	entry = *pte;
	if (!(entry & PG_PRESENT))
		return -2;

	/*
	 * PAT=1, PCD=0, PWT=0 selects PA4, which paging_prepare_wc_slot()
	 * programs to WC. Clear PCD/PWT explicitly in case firmware gave the
	 * range UC defaults.
	 */
	flags = paging_entry_flags(entry);
	flags = (flags & ~(uint32_t)(PG_PCD | PG_PWT)) | PG_PAT_4K;
	*pte = paging_entry_build(entry, flags);
	paging_invlpg(virt);
	return 0;
}

int paging_mark_range_write_combining(uint32_t phys_start, uint32_t byte_len)
{
	uint32_t *pd = (uint32_t *)paging_phys_ptr(PAGE_DIR_ADDR);
	uint32_t start;
	uint64_t end64;
	uint32_t end;

	if (byte_len == 0)
		return 0;
	if (!pd)
		return -2;
	if (paging_prepare_wc_slot() != 0)
		return -1;
	if (phys_start > UINT32_MAX - (byte_len - 1u))
		return -1;

	start = phys_start & ~0xFFFu;
	end64 = ((uint64_t)phys_start + byte_len + 0xFFFu) & ~0xFFFull;
	if (end64 > UINT32_MAX)
		return -1;
	end = (uint32_t)end64;
	if (!paging_identity_virtual_range_allowed(start, end))
		return -2;

	for (uint32_t virt = start; virt < end; virt += 0x1000u) {
		if (paging_mark_pte_write_combining(virt) != 0)
			return -2;
		if (virt < (uint32_t)ARCH_KERNEL_DIRECT_PHYS_MAX &&
		    paging_mark_pte_write_combining(
		        (uint32_t)ARCH_KERNEL_PHYS_TO_VIRT(virt)) != 0)
			return -2;
	}
	return 0;
}

void paging_init(void)
{
	uint32_t *pd = (uint32_t *)PAGE_DIR_ADDR;

	/* Clear the entire page directory */
	for (int i = 0; i < 1024; i++)
		pd[i] = 0;

	/*
     * Identity-map the low kernel direct-map window. Virtual address equals
     * physical address throughout this range, so the kernel, VGA buffer,
     * stack, and boot structures remain accessible after paging is enabled.
     */
	for (uint32_t i = 0; i < X86_KERNEL_DIRECT_MAP_PDES; i++) {
		uint32_t pt_phys = PAGE_TAB_BASE + (uint32_t)i * 0x1000;
		uint32_t *pt = (uint32_t *)pt_phys;

		for (int j = 0; j < 1024; j++) {
			uint32_t phys = (uint32_t)i * 0x400000 + (uint32_t)j * 0x1000;
			pt[j] = phys | PG_PRESENT | PG_WRITABLE;
		}

		pd[i] = pt_phys | PG_PRESENT | PG_WRITABLE;
	}

	/*
	 * Transitional higher-half direct map. Keep the low identity map above
	 * intact for current boot/runtime code, and add supervisor-only aliases
	 * for physical 0..1 GiB at 0xC0000000..0xFFFFFFFF.
	 */
	for (uint32_t i = 0; i < X86_KERNEL_HIGH_MAP_PDES; i++) {
		uint32_t virt = (uint32_t)ARCH_KERNEL_VIRT_BASE + i * 0x400000u;
		uint32_t pdi = virt >> 22;
		uint32_t pt_phys = (uint32_t)(uintptr_t)g_kernel_high_page_tables[i];
		uint32_t *pt = g_kernel_high_page_tables[i];

		for (int j = 0; j < 1024; j++) {
			uint32_t phys = i * 0x400000u + (uint32_t)j * 0x1000u;
			pt[j] = phys | PG_PRESENT | PG_WRITABLE;
		}

		pd[pdi] = pt_phys | PG_PRESENT | PG_WRITABLE;
	}

	/* Load CR3 first, then set CR0.PG — order is critical */
	paging_load_cr3(PAGE_DIR_ADDR);
	paging_enable();
}

int paging_identity_map_kernel_range(uint32_t phys_start,
                                     uint32_t byte_len,
                                     uint32_t flags)
{
	uint32_t *pd = (uint32_t *)paging_phys_ptr(PAGE_DIR_ADDR);
	uint32_t start = phys_start & ~0xFFFu;
	uint64_t end64;
	uint32_t end;
	uint32_t page_flags;

	if (byte_len == 0)
		return 0;
	if (!pd)
		return -1;
	if (phys_start > UINT32_MAX - (byte_len - 1u))
		return -1;

	end64 = ((uint64_t)phys_start + byte_len + 0xFFFu) & ~0xFFFull;
	if (end64 > UINT32_MAX)
		return -1;
	end = (uint32_t)end64;
	if (!paging_identity_virtual_range_allowed(start, end))
		return -1;

	page_flags = (flags | PG_PRESENT) & ~(uint32_t)PG_USER;
	for (uint32_t virt = start; virt < end; virt += 0x1000u) {
		uint32_t pdi = virt >> 22;
		uint32_t pti = (virt >> 12) & 0x3FFu;
		uint32_t *pt;

		if (!(pd[pdi] & PG_PRESENT)) {
			uint32_t pt_phys = pmm_alloc_page();
			if (!pt_phys)
				return -1;

			pt = (uint32_t *)paging_phys_ptr(pt_phys);
			if (!pt) {
				pmm_free_page(pt_phys);
				return -1;
			}
			for (int i = 0; i < 1024; i++)
				pt[i] = 0;
			pd[pdi] = paging_entry_build(pt_phys, PG_PRESENT | PG_WRITABLE);
		} else {
			pt = (uint32_t *)paging_phys_ptr(paging_entry_addr(pd[pdi]));
			if (!pt)
				return -1;
		}

		pt[pti] = paging_entry_build(virt, page_flags);
		paging_invlpg(virt);
	}
	return 0;
}

uint32_t paging_create_user_space(void)
{
	uint32_t pd_phys = pmm_alloc_page();
	uint32_t low_kernel_pdes =
	    (((uint32_t)(uintptr_t)&_kernel_end) + 0x3FFFFFu) >> 22;
	uint32_t first_high_pde = (uint32_t)ARCH_KERNEL_VIRT_BASE >> 22;
	if (!pd_phys)
		return 0;

	uint32_t *new_pd = (uint32_t *)paging_phys_ptr(pd_phys);
	uint32_t *kernel_pd = (uint32_t *)paging_phys_ptr(PAGE_DIR_ADDR);
	if (!new_pd || !kernel_pd) {
		pmm_free_page(pd_phys);
		return 0;
	}

	/* Zero all 1024 PDEs */
	for (int i = 0; i < 1024; i++)
		new_pd[i] = 0;

	/*
     * Copy the low PDEs still needed by the low-linked kernel image, plus the
     * higher-half aliases. Do not inherit the rest of the low direct map:
     * those addresses are now legal user space.
     */
	for (uint32_t i = 0; i < 1024; i++) {
		if (i >= low_kernel_pdes && i < first_high_pde)
			continue;
		if ((kernel_pd[i] & (PG_PRESENT | PG_USER)) == PG_PRESENT)
			new_pd[i] = kernel_pd[i];
	}

	return pd_phys;
}

void paging_destroy_user_space(uint32_t pd_phys)
{
	uint32_t *pd;

	if (pd_phys == 0 || pd_phys == PAGE_DIR_ADDR)
		return;

	pd = (uint32_t *)paging_phys_ptr(pd_phys);
	if (!pd)
		return;
	for (uint32_t pdi = 0; pdi < 1024; pdi++) {
		uint32_t *pt;
		uint32_t pt_phys;

		if ((pd[pdi] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
			continue;

		pt_phys = paging_entry_addr(pd[pdi]);
		pt = (uint32_t *)paging_phys_ptr(pt_phys);
		if (!pt)
			continue;
		for (uint32_t pti = 0; pti < 1024; pti++) {
			if ((pt[pti] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
				continue;

			pmm_decref(paging_entry_addr(pt[pti]));
			pt[pti] = 0;
		}

		pmm_free_page(pt_phys);
		pd[pdi] = 0;
	}

	pmm_free_page(pd_phys);
}

int paging_map_page(uint32_t pd_phys,
                    uint32_t virt,
                    uint32_t phys,
                    uint32_t flags)
{
	uint32_t *pd = (uint32_t *)paging_phys_ptr(pd_phys);
	uint32_t pdi = virt >> 22;           /* bits 31–22: PD index */
	uint32_t pti = (virt >> 12) & 0x3FF; /* bits 21–12: PT index */

	uint32_t *pt;

	if (!pd)
		return -1;
	if ((flags & PG_USER) && !paging_user_page_allowed(virt))
		return -1;

	if (!(pd[pdi] & PG_PRESENT)) {
		/* No page table yet for this PDE — allocate one */
		uint32_t pt_phys = pmm_alloc_page();
		if (!pt_phys)
			return -1;

		pt = (uint32_t *)paging_phys_ptr(pt_phys);
		if (!pt) {
			pmm_free_page(pt_phys);
			return -1;
		}
		for (int i = 0; i < 1024; i++)
			pt[i] = 0;

		/* PDE must have PG_USER so the CPU will walk into it for ring-3 */
		pd[pdi] =
		    paging_entry_build(pt_phys, PG_PRESENT | PG_WRITABLE | PG_USER);
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
		if (!pt_phys)
			return -1;

		uint32_t *old_pt =
		    (uint32_t *)paging_phys_ptr(paging_entry_addr(pd[pdi]));
		pt = (uint32_t *)paging_phys_ptr(pt_phys);
		if (!old_pt || !pt) {
			pmm_free_page(pt_phys);
			return -1;
		}
		if ((old_pt[pti] & PG_PRESENT) && !(old_pt[pti] & PG_USER)) {
			pmm_free_page(pt_phys);
			return -1;
		}
		for (int i = 0; i < 1024; i++)
			pt[i] = old_pt[i];

		pd[pdi] =
		    paging_entry_build(pt_phys, PG_PRESENT | PG_WRITABLE | PG_USER);
	} else {
		pt = (uint32_t *)paging_phys_ptr(paging_entry_addr(pd[pdi]));
		if (!pt)
			return -1;
		if ((flags & PG_USER) && (pt[pti] & PG_PRESENT) &&
		    !(pt[pti] & PG_USER))
			return -1;
		if (flags & PG_USER)
			pd[pdi] |= PG_USER;
	}

	pt[pti] = paging_entry_build(phys, flags);
	paging_invlpg(virt);
	return 0;
}

int paging_unmap_page(uint32_t pd_phys, uint32_t virt)
{
	uint32_t *pd = (uint32_t *)paging_phys_ptr(pd_phys);
	uint32_t pdi = virt >> 22;
	uint32_t *pte;

	if (!pd)
		return -1;
	if (paging_lookup_slot(pd_phys, virt, &pte) != 0 ||
	    (*pte & PG_PRESENT) == 0)
		return -1;

	/*
	 * Device-mapped (PG_IO) frames are not part of the PMM free pool;
	 * decref'ing them would corrupt the PMM's per-frame refcounts.
	 */
	if ((pd[pdi] & PG_USER) && (*pte & PG_USER) && (*pte & PG_IO) == 0)
		pmm_decref(paging_entry_addr(*pte));

	*pte = 0;
	paging_invlpg(virt);
	return 0;
}

int paging_walk(uint32_t pd_phys, uint32_t virt, uint32_t **pte_out)
{
	if (paging_lookup_slot(pd_phys, virt, pte_out) != 0)
		return -1;
	if ((**pte_out & PG_PRESENT) == 0)
		return -1;
	return 0;
}

int paging_query_page(uint32_t pd_phys, uint32_t virt, arch_mm_mapping_t *out)
{
	uint32_t *pte;

	if (!out || paging_walk(pd_phys, virt, &pte) != 0)
		return -1;

	out->phys_addr = paging_entry_addr(*pte);
	out->flags = ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ;
	if (*pte & PG_WRITABLE)
		out->flags |= ARCH_MM_MAP_WRITE;
	if (*pte & PG_USER)
		out->flags |= ARCH_MM_MAP_USER;
	if (*pte & PG_COW)
		out->flags |= ARCH_MM_MAP_COW;

	return 0;
}

int paging_update_page(uint32_t pd_phys,
                       uint32_t virt,
                       uint32_t clear_flags,
                       uint32_t set_flags)
{
	uint32_t *pte;
	uint32_t entry;
	uint32_t flags;
	uint32_t unsupported;

	if (paging_walk(pd_phys, virt, &pte) != 0)
		return -1;

	unsupported = clear_flags | set_flags;
	if (unsupported & (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
	                   ARCH_MM_MAP_EXEC | ARCH_MM_MAP_USER))
		return -1;

	entry = *pte;
	flags = paging_entry_flags(entry);

	if (clear_flags & ARCH_MM_MAP_WRITE)
		flags &= ~(uint32_t)PG_WRITABLE;
	if (clear_flags & ARCH_MM_MAP_COW)
		flags &= ~(uint32_t)PG_COW;

	if (set_flags & ARCH_MM_MAP_COW) {
		flags |= PG_COW;
		flags &= ~(uint32_t)PG_WRITABLE;
	} else if (set_flags & ARCH_MM_MAP_WRITE) {
		flags |= PG_WRITABLE;
	}

	*pte = paging_entry_build(paging_entry_addr(entry), flags);
	paging_invlpg(virt);
	return 0;
}

void paging_invalidate_page(uint32_t virt)
{
	paging_invlpg(virt);
}

void *paging_temp_map(uint32_t phys_addr)
{
	return paging_phys_ptr(phys_addr);
}

void paging_temp_unmap(void *ptr)
{
	(void)ptr;
}

uint32_t paging_present_begin(void)
{
	uint32_t flags;

	__asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
	if (g_paging_present_depth == 0) {
		g_paging_present_saved_cr3 = paging_read_cr3();
		if (g_paging_present_saved_cr3 != PAGE_DIR_ADDR)
			paging_load_cr3(PAGE_DIR_ADDR);
	}
	g_paging_present_depth++;
	return flags;
}

void paging_present_end(uint32_t flags)
{
	if (g_paging_present_depth > 0)
		g_paging_present_depth--;
	if (g_paging_present_depth == 0 && g_paging_present_saved_cr3 != 0 &&
	    g_paging_present_saved_cr3 != PAGE_DIR_ADDR)
		paging_load_cr3(g_paging_present_saved_cr3);
	if (flags & (1u << 9))
		__asm__ volatile("sti" ::: "memory");
}

void paging_switch_directory(uint32_t pd_phys)
{
	paging_load_cr3(pd_phys);
}

/*
 * paging_guard_page: mark a kernel-heap page non-present in the shared
 * kernel page tables so any access through either kernel direct-map alias
 * causes a page fault.
 *
 * Used to create a no-access guard page at the bottom of each per-process
 * kernel stack. Only valid for addresses in the low identity map backed by
 * the transitional higher-half direct map.
 */
void paging_guard_page(uint32_t virt)
{
	uint32_t high_virt;
	uint32_t *pte;

	if (paging_lookup_slot(PAGE_DIR_ADDR, virt, &pte) == 0)
		*pte &= ~(uint32_t)PG_PRESENT;
	if (virt < (uint32_t)ARCH_KERNEL_DIRECT_PHYS_MAX) {
		high_virt = (uint32_t)ARCH_KERNEL_PHYS_TO_VIRT(virt);
		if (paging_lookup_slot(PAGE_DIR_ADDR, high_virt, &pte) == 0)
			*pte &= ~(uint32_t)PG_PRESENT;
		paging_invlpg(high_virt);
	}
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
	uint32_t high_virt;
	uint32_t *pte;

	if (paging_lookup_slot(PAGE_DIR_ADDR, virt, &pte) == 0)
		*pte |= PG_PRESENT | PG_WRITABLE;
	if (virt < (uint32_t)ARCH_KERNEL_DIRECT_PHYS_MAX) {
		high_virt = (uint32_t)ARCH_KERNEL_PHYS_TO_VIRT(virt);
		if (paging_lookup_slot(PAGE_DIR_ADDR, high_virt, &pte) == 0)
			*pte |= PG_PRESENT | PG_WRITABLE;
		paging_invlpg(high_virt);
	}
	paging_invlpg(virt);
}

uint32_t paging_clone_user_space(uint32_t src_pd_phys)
{
	uint32_t new_pd_phys = pmm_alloc_page();
	if (!new_pd_phys)
		return 0;

	uint32_t *new_pd = (uint32_t *)paging_phys_ptr(new_pd_phys);
	uint32_t *src_pd = (uint32_t *)paging_phys_ptr(src_pd_phys);
	if (!new_pd || !src_pd) {
		pmm_free_page(new_pd_phys);
		return 0;
	}

	for (int i = 0; i < 1024; i++)
		new_pd[i] = 0;

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
		if (!(src_pd[i] & PG_PRESENT))
			continue;

		uint32_t *src_pt =
		    (uint32_t *)paging_phys_ptr(paging_entry_addr(src_pd[i]));
		int has_user_pte = 0;
		if (!src_pt)
			continue;
		for (int j = 0; j < 1024; j++) {
			if ((src_pt[j] & (PG_PRESENT | PG_USER)) ==
			    (PG_PRESENT | PG_USER)) {
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
				if (!(new_pd[k] & PG_PRESENT))
					continue;
				if (paging_entry_addr(new_pd[k]) !=
				    paging_entry_addr(src_pd[k]))
					pmm_free_page(paging_entry_addr(new_pd[k]));
			}
			pmm_free_page(new_pd_phys);
			return 0;
		}
		if (!paging_phys_ptr(new_pt_phys)) {
			for (int k = 0; k < i; k++) {
				if (!(new_pd[k] & PG_PRESENT))
					continue;
				if (paging_entry_addr(new_pd[k]) !=
				    paging_entry_addr(src_pd[k]))
					pmm_free_page(paging_entry_addr(new_pd[k]));
			}
			pmm_free_page(new_pt_phys);
			pmm_free_page(new_pd_phys);
			return 0;
		}

		new_pd[i] =
		    paging_entry_build(new_pt_phys, paging_entry_flags(src_pd[i]));
	}

	/* ── Pass 2: apply CoW flags and populate child PTs ─────────────────── */
	for (int i = 0; i < 1024; i++) {
		if (!(new_pd[i] & PG_PRESENT))
			continue;

		/* Skip shared kernel page tables (same PT base as source) */
		if (paging_entry_addr(new_pd[i]) == paging_entry_addr(src_pd[i]))
			continue;

		uint32_t *src_pt =
		    (uint32_t *)paging_phys_ptr(paging_entry_addr(src_pd[i]));
		uint32_t *new_pt =
		    (uint32_t *)paging_phys_ptr(paging_entry_addr(new_pd[i]));
		if (!src_pt || !new_pt)
			continue;

		/* Copy all entries (kernel + user) into the child PT */
		for (int j = 0; j < 1024; j++)
			new_pt[j] = src_pt[j];

		/*
		 * Mark PMM-managed user entries CoW in parent and child, bump
		 * refcounts. Device mappings are already shared by definition:
		 * preserving PG_WRITABLE on PG_IO entries lets framebuffer and
		 * MMIO mappings keep targeting the hardware after fork().
		 */
		for (int j = 0; j < 1024; j++) {
			if ((src_pt[j] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
				continue;
			if (src_pt[j] & PG_IO) {
				new_pt[j] = src_pt[j];
				continue;
			}

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
