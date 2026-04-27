/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * fault.c — recoverable user page-fault handling for lazy allocation paths.
 */

#include "fault.h"
#include "arch.h"
#include "pmm.h"
#include "kstring.h"
#include "vfs.h"

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

static int vma_is_private_file_backed(const vm_area_t *vma)
{
	if (!vma)
		return 0;

	return vma->kind == VMA_KIND_GENERIC &&
	       (vma->flags & VMA_FLAG_ANON) == 0 &&
	       (vma->flags & VMA_FLAG_PRIVATE) != 0 && vma->file_ref.mount_id != 0;
}

static int fault_break_cow(arch_aspace_t aspace, uint32_t fault_page)
{
	arch_mm_mapping_t mapping;
	uint32_t old_phys;

	if (arch_mm_query(aspace, fault_page, &mapping) != 0)
		return -1;
	if ((mapping.flags & ARCH_MM_MAP_COW) == 0)
		return -1;

	old_phys = (uint32_t)mapping.phys_addr;
	if (pmm_refcount(old_phys) <= 1) {
		return arch_mm_update(
		    aspace, fault_page, ARCH_MM_MAP_COW, ARCH_MM_MAP_WRITE);
	}

	uint32_t new_phys = pmm_alloc_page();
	if (!new_phys)
		return -1;

	{
		void *dst = arch_page_temp_map(new_phys);
		void *src = arch_page_temp_map(mapping.phys_addr);
		uint32_t map_flags = (mapping.flags | ARCH_MM_MAP_WRITE) &
		                     ~(uint32_t)ARCH_MM_MAP_COW;

		if (!dst || !src) {
			if (src)
				arch_page_temp_unmap(src);
			if (dst)
				arch_page_temp_unmap(dst);
			pmm_free_page(new_phys);
			return -1;
		}

		k_memcpy(dst, src, PAGE_SIZE);
		arch_page_temp_unmap(src);
		arch_page_temp_unmap(dst);

		if (arch_mm_map(aspace, fault_page, new_phys, map_flags) != 0) {
			pmm_free_page(new_phys);
			return -1;
		}
	}

	pmm_decref(old_phys);
	return 0;
}

static int fault_handle_lazy_anon_fault(arch_aspace_t aspace,
                                        uint32_t cr2,
                                        uint32_t user_esp,
                                        process_t *cur,
                                        const vm_area_t *vma)
{
	uint32_t fault_page = cr2 & ~0xFFFu;
	uint32_t map_flags =
	    ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ | ARCH_MM_MAP_USER;

	if (!cur || !vma)
		return -1;

	if ((vma->flags & (VMA_FLAG_ANON | VMA_FLAG_PRIVATE)) !=
	    (VMA_FLAG_ANON | VMA_FLAG_PRIVATE))
		return -1;
	if (vma->flags & VMA_FLAG_WRITE)
		map_flags |= ARCH_MM_MAP_WRITE;

	if (vma->kind == VMA_KIND_HEAP || vma->kind == VMA_KIND_GENERIC) {
		uint32_t phys = pmm_alloc_page();
		void *page;
		if (!phys)
			return -1;

		page = arch_page_temp_map(phys);
		if (!page) {
			pmm_free_page(phys);
			return -1;
		}
		k_memset(page, 0, PAGE_SIZE);
		arch_page_temp_unmap(page);
		if (arch_mm_map(aspace, fault_page, phys, map_flags) != 0) {
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
			void *page_ptr;
			if (!phys)
				return -1;

			page_ptr = arch_page_temp_map(phys);
			if (!page_ptr) {
				pmm_free_page(phys);
				return -1;
			}
			k_memset(page_ptr, 0, PAGE_SIZE);
			arch_page_temp_unmap(page_ptr);
			if (arch_mm_map(aspace, page, phys, map_flags) != 0) {
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

int fault_handle_lazy_file_private_fault(arch_aspace_t aspace,
                                         uint32_t fault_page,
                                         const vm_area_t *vma)
{
	uint32_t phys;
	uint32_t map_flags =
	    ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ | ARCH_MM_MAP_USER;
	void *page;
	int rc = 0;

	if (!vma || fault_page < vma->start || fault_page >= vma->end)
		return -1;
	if (!vma_is_private_file_backed(vma))
		return -1;
	if (vma->flags & VMA_FLAG_WRITE)
		map_flags |= ARCH_MM_MAP_WRITE;

	phys = pmm_alloc_page();
	if (!phys)
		return -1;

	page = arch_page_temp_map(phys);
	if (!page) {
		pmm_free_page(phys);
		return -1;
	}

	k_memset(page, 0, PAGE_SIZE);

	{
		uint32_t rel = fault_page - vma->start;
		uint32_t file_offset;

		if (rel > UINT32_MAX - vma->file_offset) {
			rc = -1;
		} else {
			file_offset = vma->file_offset + rel;
			if (file_offset < vma->file_size) {
				uint32_t read_len = vma->file_size - file_offset;
				int n;

				if (read_len > PAGE_SIZE)
					read_len = PAGE_SIZE;
				n = vfs_read(vma->file_ref,
				             file_offset,
				             (uint8_t *)page,
				             read_len);
				if (n < 0)
					rc = -1;
			}
		}
	}

	arch_page_temp_unmap(page);
	if (rc != 0) {
		pmm_free_page(phys);
		return -1;
	}

	if (arch_mm_map(aspace, fault_page, phys, map_flags) != 0) {
		pmm_free_page(phys);
		return -1;
	}

	return 0;
}

int paging_handle_fault(uint32_t pd_phys,
                        uint32_t cr2,
                        uint32_t err,
                        uint32_t user_esp,
                        process_t *cur)
{
	arch_aspace_t aspace = (arch_aspace_t)pd_phys;
	uint32_t fault_page = cr2 & ~0xFFFu;

	if ((err & PF_ERR_USER) == 0 || !cur)
		return -1;

	if ((err & PF_ERR_PRESENT) == 0) {
		const vm_area_t *vma = vma_find_const(cur, cr2);

		if (!vma)
			return -1;
		if (!vma_allows_access(vma, err))
			return -1;
		if (vma_is_private_file_backed(vma))
			return fault_handle_lazy_file_private_fault(
			    aspace, fault_page, vma);
		return fault_handle_lazy_anon_fault(aspace, cr2, user_esp, cur, vma);
	}

	{
		const vm_area_t *vma = vma_find_const(cur, cr2);
		arch_mm_mapping_t mapping;
		if (!vma || !vma_allows_access(vma, err))
			return -1;

		/*
         * User processes share the kernel's identity map, so an untouched
         * anonymous page may still appear "present" to the CPU as a
         * supervisor-only kernel mapping. Treat that shadow mapping like a
         * lazy anonymous miss and replace it with a proper user page.
         */
		if (arch_mm_query(aspace, fault_page, &mapping) == 0 &&
		    (mapping.flags & (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER)) ==
		        ARCH_MM_MAP_PRESENT &&
		    (mapping.flags & ARCH_MM_MAP_COW) == 0) {
			if (vma_is_private_file_backed(vma))
				return fault_handle_lazy_file_private_fault(
				    aspace, fault_page, vma);
			if ((vma->flags & (VMA_FLAG_ANON | VMA_FLAG_PRIVATE)) ==
			    (VMA_FLAG_ANON | VMA_FLAG_PRIVATE))
				return fault_handle_lazy_anon_fault(
				    aspace, cr2, user_esp, cur, vma);
		}
	}

	if ((err & PF_ERR_WRITE) == 0)
		return -1;

	return fault_break_cow(aspace, fault_page);
}
