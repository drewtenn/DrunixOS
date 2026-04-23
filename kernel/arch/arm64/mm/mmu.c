/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * mmu.c - buildable arm64 address-space scaffold for the Phase 3 MM boundary.
 *
 * This is intentionally a software-managed page-table model. It mirrors the
 * shared arch/MM contract well enough for callers to build against arm64
 * without attempting to drive EL1 translation hardware yet.
 */

#include "mmu.h"
#include "pmm.h"
#include "kstring.h"

#define ARM64_MMU_DIR_ENTRIES 1024u
#define ARM64_MMU_TABLE_ENTRIES 1024u
#define ARM64_MMU_ENTRY_ADDR_MASK 0xFFFFF000u
#define ARM64_MMU_ENTRY_FLAG_MASK 0x00000FFFu
#define ARM64_MMU_ENTRY_TABLE 0x0100u

static uint32_t g_kernel_page_dir[ARM64_MMU_DIR_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));
static arch_aspace_t g_current_aspace;
static int g_arm64_mmu_initialized;

static uint32_t arm64_mmu_pde_index(uintptr_t virt)
{
	return ((uint32_t)virt >> 22) & 0x3FFu;
}

static uint32_t arm64_mmu_pte_index(uintptr_t virt)
{
	return ((uint32_t)virt >> 12) & 0x3FFu;
}

static uint32_t *arm64_mmu_page_dir(arch_aspace_t aspace)
{
	return (uint32_t *)(uintptr_t)aspace;
}

static uint32_t arm64_mmu_entry_addr(uint32_t entry)
{
	return entry & ARM64_MMU_ENTRY_ADDR_MASK;
}

static uint32_t arm64_mmu_entry_flags(uint32_t entry)
{
	return entry & ARM64_MMU_ENTRY_FLAG_MASK;
}

static uint32_t arm64_mmu_entry_build(uint32_t addr, uint32_t flags)
{
	return arm64_mmu_entry_addr(addr) | arm64_mmu_entry_flags(flags);
}

static uint32_t arm64_mmu_leaf_flags(uint32_t flags)
{
	uint32_t leaf = flags | ARCH_MM_MAP_PRESENT;

	if (leaf & ARCH_MM_MAP_PRESENT)
		leaf |= ARCH_MM_MAP_READ;
	if (leaf & ARCH_MM_MAP_COW)
		leaf &= ~(uint32_t)ARCH_MM_MAP_WRITE;
	return leaf & ~ARM64_MMU_ENTRY_TABLE;
}

static uint32_t *arm64_mmu_page_table(uint32_t entry)
{
	return (uint32_t *)(uintptr_t)arm64_mmu_entry_addr(entry);
}

static int arm64_mmu_lookup_slot(arch_aspace_t aspace,
                                 uintptr_t virt,
                                 uint32_t **pte_out,
                                 uint32_t **pde_out)
{
	uint32_t *pd;
	uint32_t *pt;
	uint32_t pdi;

	if (!aspace || !pte_out)
		return -1;

	pd = arm64_mmu_page_dir(aspace);
	pdi = arm64_mmu_pde_index(virt);
	if ((pd[pdi] & (ARCH_MM_MAP_PRESENT | ARM64_MMU_ENTRY_TABLE)) !=
	    (ARCH_MM_MAP_PRESENT | ARM64_MMU_ENTRY_TABLE))
		return -1;

	pt = arm64_mmu_page_table(pd[pdi]);
	*pte_out = &pt[arm64_mmu_pte_index(virt)];
	if (pde_out)
		*pde_out = &pd[pdi];
	return 0;
}

static int arm64_mmu_table_has_user_page(const uint32_t *pt)
{
	for (uint32_t i = 0; i < ARM64_MMU_TABLE_ENTRIES; i++) {
		if ((pt[i] & (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER)) ==
		    (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER))
			return 1;
	}

	return 0;
}

static int arm64_mmu_ensure_table(arch_aspace_t aspace,
                                  uintptr_t virt,
                                  uint32_t flags,
                                  uint32_t **pte_out)
{
	uint32_t *pd;
	uint32_t pdi;
	uint32_t *pt;

	if (!aspace || !pte_out)
		return -1;

	pd = arm64_mmu_page_dir(aspace);
	pdi = arm64_mmu_pde_index(virt);

	if ((pd[pdi] & ARCH_MM_MAP_PRESENT) == 0) {
		uint32_t pt_phys = pmm_alloc_page();

		if (!pt_phys)
			return -1;

		pt = (uint32_t *)(uintptr_t)pt_phys;
		k_memset(pt, 0, PAGE_SIZE);
		pd[pdi] = arm64_mmu_entry_build(
		    pt_phys,
		    ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ | ARCH_MM_MAP_WRITE |
		        ARM64_MMU_ENTRY_TABLE |
		        ((flags & ARCH_MM_MAP_USER) ? ARCH_MM_MAP_USER : 0u));
	} else if ((flags & ARCH_MM_MAP_USER) != 0 &&
	           (pd[pdi] & ARCH_MM_MAP_USER) == 0) {
		uint32_t pt_phys = pmm_alloc_page();
		uint32_t *old_pt;

		if (!pt_phys)
			return -1;

		old_pt = arm64_mmu_page_table(pd[pdi]);
		pt = (uint32_t *)(uintptr_t)pt_phys;
		for (uint32_t i = 0; i < ARM64_MMU_TABLE_ENTRIES; i++)
			pt[i] = old_pt[i];

		pd[pdi] = arm64_mmu_entry_build(
		    pt_phys,
		    arm64_mmu_entry_flags(pd[pdi]) | ARCH_MM_MAP_USER |
		        ARM64_MMU_ENTRY_TABLE | ARCH_MM_MAP_PRESENT);
	} else {
		pt = arm64_mmu_page_table(pd[pdi]);
		if (flags & ARCH_MM_MAP_USER)
			pd[pdi] |= ARCH_MM_MAP_USER;
	}

	*pte_out = &pt[arm64_mmu_pte_index(virt)];
	return 0;
}

void arm64_mmu_init(void)
{
	if (g_arm64_mmu_initialized)
		return;

	k_memset(g_kernel_page_dir, 0, sizeof(g_kernel_page_dir));
	g_current_aspace = (arch_aspace_t)(uintptr_t)g_kernel_page_dir;
	g_arm64_mmu_initialized = 1;
}

arch_aspace_t arm64_mmu_kernel_aspace(void)
{
	if (!g_arm64_mmu_initialized)
		arm64_mmu_init();

	return (arch_aspace_t)(uintptr_t)g_kernel_page_dir;
}

arch_aspace_t arm64_mmu_aspace_create(void)
{
	uint32_t pd_phys;
	uint32_t *pd;

	if (!g_arm64_mmu_initialized)
		arm64_mmu_init();

	pd_phys = pmm_alloc_page();
	if (!pd_phys)
		return 0;

	pd = (uint32_t *)(uintptr_t)pd_phys;
	for (uint32_t i = 0; i < ARM64_MMU_DIR_ENTRIES; i++)
		pd[i] = g_kernel_page_dir[i];
	return (arch_aspace_t)pd_phys;
}

arch_aspace_t arm64_mmu_aspace_clone(arch_aspace_t src)
{
	uint32_t *src_pd;
	uint32_t new_pd_phys;
	uint32_t *new_pd;

	if (!src)
		return 0;

	src_pd = arm64_mmu_page_dir(src);
	new_pd_phys = pmm_alloc_page();
	if (!new_pd_phys)
		return 0;

	new_pd = (uint32_t *)(uintptr_t)new_pd_phys;
	for (uint32_t i = 0; i < ARM64_MMU_DIR_ENTRIES; i++)
		new_pd[i] = 0;

	for (uint32_t i = 0; i < ARM64_MMU_DIR_ENTRIES; i++) {
		uint32_t *src_pt;
		uint32_t new_pt_phys;

		if ((src_pd[i] & ARCH_MM_MAP_PRESENT) == 0)
			continue;

		src_pt = arm64_mmu_page_table(src_pd[i]);
		if (!arm64_mmu_table_has_user_page(src_pt)) {
			new_pd[i] = src_pd[i];
			continue;
		}

		new_pt_phys = pmm_alloc_page();
		if (!new_pt_phys) {
			for (uint32_t k = 0; k < i; k++) {
				if ((new_pd[k] & ARCH_MM_MAP_PRESENT) == 0)
					continue;
				if (arm64_mmu_entry_addr(new_pd[k]) !=
				    arm64_mmu_entry_addr(src_pd[k]))
					pmm_free_page(arm64_mmu_entry_addr(new_pd[k]));
			}
			pmm_free_page(new_pd_phys);
			return 0;
		}

		new_pd[i] = arm64_mmu_entry_build(
		    new_pt_phys,
		    arm64_mmu_entry_flags(src_pd[i]) | ARCH_MM_MAP_PRESENT |
		        ARM64_MMU_ENTRY_TABLE);
	}

	for (uint32_t i = 0; i < ARM64_MMU_DIR_ENTRIES; i++) {
		uint32_t *src_pt;
		uint32_t *new_pt;

		if ((new_pd[i] & ARCH_MM_MAP_PRESENT) == 0)
			continue;
		if (arm64_mmu_entry_addr(new_pd[i]) == arm64_mmu_entry_addr(src_pd[i]))
			continue;

		src_pt = arm64_mmu_page_table(src_pd[i]);
		new_pt = arm64_mmu_page_table(new_pd[i]);
		for (uint32_t j = 0; j < ARM64_MMU_TABLE_ENTRIES; j++)
			new_pt[j] = src_pt[j];

		for (uint32_t j = 0; j < ARM64_MMU_TABLE_ENTRIES; j++) {
			if ((src_pt[j] & (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER)) !=
			    (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER))
				continue;

			if (src_pt[j] & ARCH_MM_MAP_WRITE) {
				src_pt[j] =
				    (src_pt[j] & ~(uint32_t)ARCH_MM_MAP_WRITE) |
				    ARCH_MM_MAP_COW;
			}

			new_pt[j] = src_pt[j];
			pmm_incref(arm64_mmu_entry_addr(src_pt[j]));
		}
	}

	return (arch_aspace_t)new_pd_phys;
}

void arm64_mmu_aspace_switch(arch_aspace_t aspace)
{
	if (!aspace)
		aspace = arm64_mmu_kernel_aspace();

	g_current_aspace = aspace;
}

void arm64_mmu_aspace_destroy(arch_aspace_t aspace)
{
	uint32_t *pd;

	if (!aspace || aspace == arm64_mmu_kernel_aspace())
		return;

	pd = arm64_mmu_page_dir(aspace);
	for (uint32_t i = 0; i < ARM64_MMU_DIR_ENTRIES; i++) {
		uint32_t *pt;

		if ((pd[i] & ARCH_MM_MAP_PRESENT) == 0)
			continue;
		if (pd[i] == g_kernel_page_dir[i])
			continue;

		pt = arm64_mmu_page_table(pd[i]);
		for (uint32_t j = 0; j < ARM64_MMU_TABLE_ENTRIES; j++) {
			if ((pt[j] & (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER)) ==
			    (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER))
				pmm_decref(arm64_mmu_entry_addr(pt[j]));
		}

		pmm_free_page(arm64_mmu_entry_addr(pd[i]));
	}

	if (g_current_aspace == aspace)
		g_current_aspace = arm64_mmu_kernel_aspace();
	pmm_free_page((uint32_t)aspace);
}

int arm64_mmu_map(arch_aspace_t aspace,
                  uintptr_t virt,
                  uint64_t phys,
                  uint32_t flags)
{
	uint32_t *pte;

	if (!aspace || phys > UINT32_MAX)
		return -1;
	if ((virt & (PAGE_SIZE - 1u)) != 0 || (phys & (PAGE_SIZE - 1u)) != 0)
		return -1;
	if (arm64_mmu_ensure_table(aspace, virt, flags, &pte) != 0)
		return -1;

	*pte = arm64_mmu_entry_build((uint32_t)phys, arm64_mmu_leaf_flags(flags));
	return 0;
}

int arm64_mmu_unmap(arch_aspace_t aspace, uintptr_t virt)
{
	uint32_t *pte;
	uint32_t *pde;

	if (arm64_mmu_lookup_slot(aspace, virt, &pte, &pde) != 0 ||
	    (*pte & ARCH_MM_MAP_PRESENT) == 0)
		return -1;

	if (((*pde) & ARCH_MM_MAP_USER) && ((*pte) & ARCH_MM_MAP_USER))
		pmm_decref(arm64_mmu_entry_addr(*pte));

	*pte = 0;
	return 0;
}

int arm64_mmu_query(arch_aspace_t aspace,
                    uintptr_t virt,
                    arch_mm_mapping_t *out)
{
	uint32_t *pte;

	if (!out || arm64_mmu_lookup_slot(aspace, virt, &pte, 0) != 0 ||
	    (*pte & ARCH_MM_MAP_PRESENT) == 0)
		return -1;

	out->phys_addr = arm64_mmu_entry_addr(*pte);
	out->flags = arm64_mmu_entry_flags(*pte) & ~ARM64_MMU_ENTRY_TABLE;
	return 0;
}

int arm64_mmu_update(arch_aspace_t aspace,
                     uintptr_t virt,
                     uint32_t clear_flags,
                     uint32_t set_flags)
{
	uint32_t *pte;
	uint32_t entry;
	uint32_t flags;
	uint32_t unsupported;

	if (arm64_mmu_lookup_slot(aspace, virt, &pte, 0) != 0 ||
	    (*pte & ARCH_MM_MAP_PRESENT) == 0)
		return -1;

	unsupported = clear_flags | set_flags;
	if (unsupported &
	    (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ | ARCH_MM_MAP_EXEC |
	     ARCH_MM_MAP_USER))
		return -1;

	entry = *pte;
	flags = arm64_mmu_entry_flags(entry);

	if (clear_flags & ARCH_MM_MAP_WRITE)
		flags &= ~(uint32_t)ARCH_MM_MAP_WRITE;
	if (clear_flags & ARCH_MM_MAP_COW)
		flags &= ~(uint32_t)ARCH_MM_MAP_COW;

	if (set_flags & ARCH_MM_MAP_COW) {
		flags |= ARCH_MM_MAP_COW;
		flags &= ~(uint32_t)ARCH_MM_MAP_WRITE;
	} else if (set_flags & ARCH_MM_MAP_WRITE) {
		flags |= ARCH_MM_MAP_WRITE;
	}

	*pte = arm64_mmu_entry_build(arm64_mmu_entry_addr(entry), flags);
	return 0;
}

void arm64_mmu_invalidate_page(arch_aspace_t aspace, uintptr_t virt)
{
	(void)aspace;
	(void)virt;
}
