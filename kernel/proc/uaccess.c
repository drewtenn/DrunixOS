/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * uaccess.c — validated copies between kernel memory and user mappings.
 */

#include "uaccess.h"
#include "arch.h"
#include "pmm.h"
#include "kstring.h"

static int
uaccess_vma_allows(const process_t *proc, uint32_t user_addr, int write_access)
{
	const vm_area_t *vma = vma_find_const(proc, user_addr);

	if (!vma)
		return 0;
	if (write_access)
		return (vma->flags & VMA_FLAG_WRITE) != 0;
	return (vma->flags & (VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC)) != 0;
}

static int uaccess_range_ok(uint32_t user_addr, uint32_t len)
{
	uint32_t end;

	if (len == 0)
		return 0;
	if (user_addr >= USER_STACK_TOP)
		return -1;

	end = user_addr + len;
	if (end < user_addr || end > USER_STACK_TOP)
		return -1;

	return 0;
}

static int uaccess_break_cow(process_t *proc, uint32_t fault_page)
{
	arch_aspace_t aspace;
	arch_mm_mapping_t mapping;
	uint32_t old_phys;

	if (!proc)
		return -1;

	aspace = (arch_aspace_t)proc->pd_phys;
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
		uint32_t map_flags =
		    (mapping.flags | ARCH_MM_MAP_WRITE) & ~(uint32_t)ARCH_MM_MAP_COW;

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

static int uaccess_map_lazy_anon(process_t *proc,
                                 uint32_t fault_page,
                                 const vm_area_t *vma)
{
	arch_aspace_t aspace;
	uint32_t phys;
	void *page;
	uint32_t map_flags =
	    ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ | ARCH_MM_MAP_USER;

	if (!proc || !vma)
		return -1;
	if ((vma->flags & (VMA_FLAG_ANON | VMA_FLAG_PRIVATE)) !=
	    (VMA_FLAG_ANON | VMA_FLAG_PRIVATE))
		return -1;
	if (vma->kind != VMA_KIND_HEAP && vma->kind != VMA_KIND_GENERIC)
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
	arch_page_temp_unmap(page);

	aspace = (arch_aspace_t)proc->pd_phys;
	if (arch_mm_map(aspace, fault_page, phys, map_flags) != 0) {
		pmm_free_page(phys);
		return -1;
	}
	return 0;
}

static int uaccess_translate(process_t *proc,
                             uint32_t user_addr,
                             int write_access,
                             uint8_t **kptr_out,
                             void **page_out)
{
	arch_aspace_t aspace;
	arch_mm_mapping_t mapping;
	const vm_area_t *vma;
	void *page_ptr;
	uint32_t page_base;

	if (!proc || user_addr >= USER_STACK_TOP)
		return -1;
	vma = vma_find_const(proc, user_addr);
	if (!vma)
		return -1;
	if (!uaccess_vma_allows(proc, user_addr, write_access))
		return -1;

	aspace = (arch_aspace_t)proc->pd_phys;
	page_base = user_addr & ~0xFFFu;
	if (arch_mm_query(aspace, page_base, &mapping) != 0) {
		if (uaccess_map_lazy_anon(proc, page_base, vma) != 0)
			return -1;
		if (arch_mm_query(aspace, page_base, &mapping) != 0)
			return -1;
	}
	if ((mapping.flags & ARCH_MM_MAP_USER) == 0 &&
	    (mapping.flags & ARCH_MM_MAP_COW) == 0 &&
	    (vma->flags & (VMA_FLAG_ANON | VMA_FLAG_PRIVATE)) ==
	        (VMA_FLAG_ANON | VMA_FLAG_PRIVATE)) {
		if (uaccess_map_lazy_anon(proc, page_base, vma) != 0)
			return -1;
		if (arch_mm_query(aspace, page_base, &mapping) != 0)
			return -1;
	}
	if ((mapping.flags & ARCH_MM_MAP_USER) == 0)
		return -1;

	if (write_access && (mapping.flags & ARCH_MM_MAP_WRITE) == 0) {
		if ((mapping.flags & ARCH_MM_MAP_COW) == 0)
			return -1;
		if (uaccess_break_cow(proc, page_base) != 0)
			return -1;
		if (arch_mm_query(aspace, page_base, &mapping) != 0)
			return -1;
	}

	page_ptr = arch_page_temp_map(mapping.phys_addr);
	if (!page_ptr)
		return -1;

	*kptr_out = (uint8_t *)page_ptr + (user_addr & 0xFFFu);
	if (page_out)
		*page_out = page_ptr;
	return 0;
}

int uaccess_prepare(process_t *proc,
                    uint32_t user_addr,
                    uint32_t len,
                    int write_access)
{
	uint32_t offset = 0;

	if (uaccess_range_ok(user_addr, len) != 0)
		return -1;

	while (offset < len) {
		uint32_t addr = user_addr + offset;
		uint32_t page_off = addr & 0xFFFu;
		uint32_t chunk = PAGE_SIZE - page_off;
		uint8_t *unused;
		void *page_ptr;

		if (chunk > len - offset)
			chunk = len - offset;

		if (uaccess_translate(proc, addr, write_access, &unused, &page_ptr) !=
		    0)
			return -1;
		arch_page_temp_unmap(page_ptr);

		offset += chunk;
	}

	return 0;
}

int uaccess_copy_from_user(process_t *proc,
                           void *dst,
                           uint32_t user_src,
                           uint32_t len)
{
	uint32_t offset = 0;

	if (uaccess_prepare(proc, user_src, len, 0) != 0)
		return -1;

	while (offset < len) {
		uint32_t addr = user_src + offset;
		uint32_t page_off = addr & 0xFFFu;
		uint32_t chunk = PAGE_SIZE - page_off;
		uint8_t *kptr;
		void *page_ptr;

		if (chunk > len - offset)
			chunk = len - offset;

		if (uaccess_translate(proc, addr, 0, &kptr, &page_ptr) != 0)
			return -1;

		k_memcpy((uint8_t *)dst + offset, kptr, chunk);
		arch_page_temp_unmap(page_ptr);
		offset += chunk;
	}

	return 0;
}

int uaccess_copy_to_user(process_t *proc,
                         uint32_t user_dst,
                         const void *src,
                         uint32_t len)
{
	uint32_t offset = 0;

	if (uaccess_prepare(proc, user_dst, len, 1) != 0)
		return -1;

	while (offset < len) {
		uint32_t addr = user_dst + offset;
		uint32_t page_off = addr & 0xFFFu;
		uint32_t chunk = PAGE_SIZE - page_off;
		uint8_t *kptr;
		void *page_ptr;

		if (chunk > len - offset)
			chunk = len - offset;

		if (uaccess_translate(proc, addr, 1, &kptr, &page_ptr) != 0)
			return -1;

		k_memcpy(kptr, (const uint8_t *)src + offset, chunk);
		arch_page_temp_unmap(page_ptr);
		offset += chunk;
	}

	return 0;
}

int uaccess_copy_string_from_user(process_t *proc,
                                  char *dst,
                                  uint32_t dstsz,
                                  uint32_t user_src)
{
	uint32_t i;

	if (!proc || !dst || dstsz == 0 || user_src == 0)
		return -1;

	for (i = 0; i < dstsz; i++) {
		uint8_t *kptr;
		void *page_ptr;

		if (uaccess_translate(proc, user_src + i, 0, &kptr, &page_ptr) != 0)
			return -1;

		dst[i] = (char)*kptr;
		arch_page_temp_unmap(page_ptr);
		if (dst[i] == '\0')
			return 0;
	}

	return -1;
}
