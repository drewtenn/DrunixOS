/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * fault.c — recoverable user page-fault handling for lazy allocation paths.
 */

#include "fault.h"
#include "paging.h"
#include "pmm.h"
#include "kstring.h"

#define PF_ERR_PRESENT 0x1u
#define PF_ERR_WRITE 0x2u
#define PF_ERR_USER 0x4u

static int vma_allows_access(const vm_area_t *vma, uint32_t err)
{
	uint32_t access =
	    vma ? (vma->flags & (VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC))
	        : 0;

	if (!vma || access == 0)
		return 0;

	if (err & PF_ERR_WRITE)
		return (vma->flags & VMA_FLAG_WRITE) != 0;

	return 1;
}

static void fault_invlpg(uint32_t virt)
{
	__asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

static int paging_handle_lazy_anon_fault(uint32_t pd_phys,
                                         uint32_t cr2,
                                         uint32_t user_esp,
                                         process_t *cur,
                                         const vm_area_t *vma)
{
	uint32_t fault_page = cr2 & ~0xFFFu;
	uint32_t map_flags = PG_PRESENT | PG_USER;

	if (!cur || !vma)
		return -1;

	if ((vma->flags & (VMA_FLAG_ANON | VMA_FLAG_PRIVATE)) !=
	    (VMA_FLAG_ANON | VMA_FLAG_PRIVATE))
		return -1;
	if (vma->flags & VMA_FLAG_WRITE)
		map_flags |= PG_WRITABLE;

	if (vma->kind == VMA_KIND_HEAP || vma->kind == VMA_KIND_GENERIC) {
		uint32_t phys = pmm_alloc_page();
		if (!phys)
			return -1;

		k_memset((void *)phys, 0, PAGE_SIZE);
		if (paging_map_page(pd_phys, fault_page, phys, map_flags) != 0) {
			pmm_free_page(phys);
			return -1;
		}
		return 0;
	}

	uint32_t stack_slack = user_esp > 32u ? user_esp - 32u : 0;
	if (vma->kind == VMA_KIND_STACK && (vma->flags & VMA_FLAG_GROWSDOWN) != 0 &&
	    fault_page >= vma->start && fault_page < cur->stack_low_limit &&
	    cr2 >= stack_slack && cr2 < vma->end) {
		for (uint32_t page = cur->stack_low_limit - PAGE_SIZE;
		     page >= fault_page && page >= vma->start;
		     page -= PAGE_SIZE) {
			uint32_t phys = pmm_alloc_page();
			if (!phys)
				return -1;

			k_memset((void *)phys, 0, PAGE_SIZE);
			if (paging_map_page(pd_phys, page, phys, map_flags) != 0) {
				pmm_free_page(phys);
				return -1;
			}
			cur->stack_low_limit = page;

			if (page == 0)
				break;
		}
		return 0;
	}

	return -1;
}

int paging_handle_fault(uint32_t pd_phys,
                        uint32_t cr2,
                        uint32_t err,
                        uint32_t user_esp,
                        process_t *cur)
{
	uint32_t fault_page = cr2 & ~0xFFFu;
	uint32_t *pte = 0;

	if ((err & PF_ERR_USER) == 0 || !cur)
		return -1;

	if ((err & PF_ERR_PRESENT) == 0) {
		const vm_area_t *vma = vma_find_const(cur, cr2);

		if (!vma)
			return -1;
		if (!vma_allows_access(vma, err))
			return -1;
		return paging_handle_lazy_anon_fault(pd_phys, cr2, user_esp, cur, vma);
	}

	{
		const vm_area_t *vma = vma_find_const(cur, cr2);
		if (!vma || !vma_allows_access(vma, err))
			return -1;

		/*
         * User processes share the kernel's identity map, so an untouched
         * anonymous page may still appear "present" to the CPU as a
         * supervisor-only kernel mapping. Treat that shadow mapping like a
         * lazy anonymous miss and replace it with a proper user page.
         */
		if (paging_walk(pd_phys, fault_page, &pte) == 0 &&
		    ((*pte & (PG_PRESENT | PG_USER)) == PG_PRESENT) &&
		    ((*pte & PG_COW) == 0) &&
		    (vma->flags & (VMA_FLAG_ANON | VMA_FLAG_PRIVATE)) ==
		        (VMA_FLAG_ANON | VMA_FLAG_PRIVATE))
			return paging_handle_lazy_anon_fault(
			    pd_phys, cr2, user_esp, cur, vma);
	}

	if ((err & PF_ERR_WRITE) == 0)
		return -1;

	if (paging_walk(pd_phys, fault_page, &pte) != 0 || ((*pte & PG_COW) == 0))
		return -1;

	uint32_t old_phys = paging_entry_addr(*pte);
	if (pmm_refcount(old_phys) <= 1) {
		uint32_t flags = (paging_entry_flags(*pte) | PG_WRITABLE) & ~PG_COW;
		*pte = paging_entry_build(old_phys, flags);
		fault_invlpg(fault_page);
		return 0;
	}

	uint32_t new_phys = pmm_alloc_page();
	if (!new_phys)
		return -1;

	k_memcpy((void *)new_phys, (const void *)old_phys, PAGE_SIZE);
	{
		uint32_t flags = (paging_entry_flags(*pte) | PG_WRITABLE) & ~PG_COW;
		*pte = paging_entry_build(new_phys, flags);
	}
	fault_invlpg(fault_page);
	pmm_decref(old_phys);
	return 0;
}
