/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../../arch.h"
#include "../../../proc/elf.h"
#include "../../../proc/process.h"
#include "../mm/pmm.h"
#include "elf64.h"
#include "kstring.h"
#include <stdint.h>

static uint32_t arm64_elf_segment_flags(uint32_t p_flags)
{
	uint32_t flags = ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER;

	if (p_flags & PF_R)
		flags |= ARCH_MM_MAP_READ;
	if (p_flags & PF_W)
		flags |= ARCH_MM_MAP_WRITE;
	if (p_flags & PF_X)
		flags |= ARCH_MM_MAP_EXEC;
	if ((flags & (ARCH_MM_MAP_READ | ARCH_MM_MAP_WRITE | ARCH_MM_MAP_EXEC)) ==
	    0)
		flags |= ARCH_MM_MAP_READ;

	return flags;
}

int arch_elf_machine_supported(elf_class_t elf_class, uint16_t machine)
{
	return elf_class == ELF_CLASS_64 && machine == EM_AARCH64;
}

int arch_elf_load_user_image(vfs_file_ref_t file_ref,
                             arch_aspace_t aspace,
                             uintptr_t *entry_out,
                             uintptr_t *image_start_out,
                             uintptr_t *heap_start_out)
{
	Elf64_Ehdr ehdr;
	uint64_t min_vaddr = UINT64_MAX;
	uint64_t max_vend = 0u;
	int loaded_segment = 0;

	if (vfs_read(file_ref, 0, (uint8_t *)&ehdr, sizeof(ehdr)) !=
	    (int)sizeof(ehdr))
		return -1;

	if (*(const uint32_t *)ehdr.e_ident != ELF_MAGIC)
		return -2;
	if (ehdr.e_type != ET_EXEC)
		return -3;
	if (!arch_elf_machine_supported((elf_class_t)ehdr.e_ident[EI_CLASS],
	                                ehdr.e_machine))
		return -4;
	if (ehdr.e_phnum == 0)
		return -5;
	if (ehdr.e_entry >= USER_STACK_TOP)
		return -4;

	*entry_out = (uintptr_t)ehdr.e_entry;

	for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		uint64_t phdr_off =
		    ehdr.e_phoff + (uint64_t)i * (uint64_t)ehdr.e_phentsize;
		uint32_t seg_flags;

		if (phdr_off > UINT32_MAX)
			return -6;
		if (vfs_read(file_ref, (uint32_t)phdr_off, (uint8_t *)&phdr, sizeof(phdr)) !=
		    (int)sizeof(phdr))
			return -6;
		if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0)
			continue;
		if (phdr.p_filesz > phdr.p_memsz)
			return -6;
		if (phdr.p_vaddr >= USER_STACK_TOP || phdr.p_memsz > UINT32_MAX)
			return -4;
		if (phdr.p_vaddr + phdr.p_memsz < phdr.p_vaddr ||
		    phdr.p_vaddr + phdr.p_memsz > USER_STACK_TOP)
			return -4;

		{
			uint64_t vaddr = phdr.p_vaddr & ~0xFFFULL;
			uint64_t vend =
			    (phdr.p_vaddr + phdr.p_memsz + 0xFFFULL) & ~0xFFFULL;
			uint64_t npages = (vend - vaddr) / 0x1000ULL;

			if (vaddr < min_vaddr)
				min_vaddr = vaddr;
			if (vend > max_vend)
				max_vend = vend;
			loaded_segment = 1;

			seg_flags = arm64_elf_segment_flags(phdr.p_flags);
			for (uint64_t p = 0; p < npages; p++) {
				uint32_t phys = pmm_alloc_page();
				uintptr_t vpage = (uintptr_t)(vaddr + p * 0x1000ULL);
				void *page;

				if (!phys)
					return -7;
				if (arch_mm_map(aspace, vpage, phys, seg_flags) != 0) {
					pmm_free_page(phys);
					return -8;
				}

				page = arch_page_temp_map(phys);
				if (!page) {
					(void)arch_mm_unmap(aspace, vpage);
					pmm_free_page(phys);
					return -8;
				}
				k_memset(page, 0, 0x1000u);
				arch_page_temp_unmap(page);
			}

			{
				uint64_t file_remain = phdr.p_filesz;
				uint64_t file_done = 0u;
				uint32_t first_off = (uint32_t)(phdr.p_vaddr & 0xFFFULL);

				for (uint64_t p = 0; p < npages && file_remain > 0; p++) {
					arch_mm_mapping_t mapping;
					uintptr_t vpage = (uintptr_t)(vaddr + p * 0x1000ULL);
					uint32_t write_off = (p == 0) ? first_off : 0u;
					uint32_t space = 0x1000u - write_off;
					uint32_t to_copy =
					    (file_remain < space) ? (uint32_t)file_remain : space;
					void *page;

					if ((phdr.p_offset + file_done) > UINT32_MAX)
						return -9;
					if (arch_mm_query(aspace, vpage, &mapping) != 0)
						return -9;
					page = arch_page_temp_map(mapping.phys_addr);
					if (!page)
						return -9;
					if (vfs_read(file_ref,
					             (uint32_t)(phdr.p_offset + file_done),
					             (uint8_t *)page + write_off,
					             to_copy) != (int)to_copy) {
						arch_page_temp_unmap(page);
						return -9;
					}
					arch_page_temp_unmap(page);
					file_done += to_copy;
					file_remain -= to_copy;
				}
			}
		}
	}

	if (!loaded_segment)
		return -5;
	*image_start_out = (min_vaddr == UINT64_MAX) ? 0u : (uintptr_t)min_vaddr;
	*heap_start_out = (uintptr_t)max_vend;
	return 0;
}
