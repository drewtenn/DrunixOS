/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * elf.c — ELF parser and loader for user-space executables.
 */

#include "elf.h"
#include "arch.h"
#include "kstring.h"
#include "pmm.h"
#include <stdint.h>

int elf_load_file(vfs_file_ref_t file_ref,
                  uint32_t pd_phys,
                  uint32_t *entry_out,
                  uint32_t *image_start_out,
                  uint32_t *heap_start_out)
{
	arch_aspace_t aspace = (arch_aspace_t)pd_phys;
	Elf32_Ehdr ehdr;

	if (vfs_read(file_ref, 0, (uint8_t *)&ehdr, sizeof(Elf32_Ehdr)) !=
	    sizeof(Elf32_Ehdr))
		return -1;

	/* Validate ELF magic ("\x7FELF") */
	if (*(uint32_t *)ehdr.e_ident != ELF_MAGIC)
		return -2;
	if (ehdr.e_type != ET_EXEC)
		return -3;
	if (ehdr.e_machine != EM_386)
		return -4;
	if (ehdr.e_phnum == 0)
		return -5;

	*entry_out = ehdr.e_entry;

	/* Track the lowest and highest virtual addresses across PT_LOAD segments. */
	uint32_t min_vaddr = 0xFFFFFFFFu;
	/* Track the highest virtual end across all PT_LOAD segments. */
	uint32_t max_vend = 0;

	for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
		Elf32_Phdr phdr;
		uint32_t phdr_off = ehdr.e_phoff + (uint32_t)i * ehdr.e_phentsize;

		if (vfs_read(
		        file_ref, phdr_off, (uint8_t *)&phdr, sizeof(Elf32_Phdr)) !=
		    (int)sizeof(Elf32_Phdr))
			return -6;

		if (phdr.p_type != PT_LOAD)
			continue;
		if (phdr.p_memsz == 0)
			continue;

		/* Align the virtual range to page boundaries. */
		uint32_t vaddr = phdr.p_vaddr & ~0xFFFu;
		uint32_t vend = (phdr.p_vaddr + phdr.p_memsz + 0xFFFu) & ~0xFFFu;
		uint32_t npages = (vend - vaddr) / 0x1000;
		if (vaddr < min_vaddr)
			min_vaddr = vaddr;
		if (vend > max_vend)
			max_vend = vend;

		/*
         * Allocate and map one physical page per virtual page.
         * Because the kernel is identity-mapped (physical == virtual for
         * 0–128 MB), we can write directly to the allocated physical address
         * while still running under the kernel page directory.
         */
		for (uint32_t p = 0; p < npages; p++) {
			uint32_t phys = pmm_alloc_page();
			void *page;
			if (!phys)
				return -7;

			uint32_t vpage = vaddr + p * 0x1000;
			if (arch_mm_map(aspace,
			                vpage,
			                phys,
			                ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
			                    ARCH_MM_MAP_WRITE | ARCH_MM_MAP_USER) != 0)
				return -8;

			/* Zero the page; file data (if any) will be copied below. */
			page = arch_page_temp_map(phys);
			if (!page)
				return -8;
			k_memset(page, 0, 0x1000);
			arch_page_temp_unmap(page);
		}

		/*
         * Copy file data into the physical pages.
         *
         * We resolve the physical address backing each virtual page by walking
         * the page directory / page table (both in the identity-mapped range).
         * We copy one page-sized chunk at a time, trimming by filesz so we
         * only read actual file data (the rest stays zeroed from above).
         */
		uint32_t file_remain = phdr.p_filesz;
		uint32_t file_done = 0;
		uint32_t first_off = phdr.p_vaddr & 0xFFFu;

		for (uint32_t p = 0; p < npages && file_remain > 0; p++) {
			arch_mm_mapping_t mapping;
			uint32_t vpage = vaddr + p * 0x1000;
			void *page;

			uint32_t write_off = (p == 0) ? first_off : 0u;
			uint32_t space = 0x1000 - write_off;
			uint32_t to_copy = (file_remain < space) ? file_remain : space;

			if (arch_mm_query(aspace, vpage, &mapping) != 0)
				return -9;
			page = arch_page_temp_map(mapping.phys_addr);
			if (!page)
				return -9;
			if (vfs_read(file_ref,
			             phdr.p_offset + file_done,
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

	*image_start_out = (min_vaddr == 0xFFFFFFFFu) ? 0 : min_vaddr;
	*heap_start_out = max_vend;
	return 0;
}
