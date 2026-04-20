/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_mem.c - Linux i386 process memory syscalls.
 *
 * Owns brk, mmap/mmap2, munmap, mprotect, and the page-table helpers needed
 * to translate Linux protection flags into Drunix VMAs and PTE permissions.
 */

#include "syscall_internal.h"
#include "syscall_linux.h"
#include "syscall.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
#include "vfs.h"
#include "vma.h"
#include <stdint.h>

void syscall_invlpg(uint32_t virt)
{
	__asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

static int prot_is_valid(uint32_t prot)
{
	return (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
}

static int prot_has_user_access(uint32_t prot)
{
	return (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) != 0;
}

static uint32_t prot_to_vma_flags(uint32_t prot)
{
	uint32_t flags = VMA_FLAG_ANON | VMA_FLAG_PRIVATE;

	if (prot & PROT_READ)
		flags |= VMA_FLAG_READ;
	if (prot & PROT_WRITE)
		flags |= VMA_FLAG_WRITE;
	if (prot & PROT_EXEC)
		flags |= VMA_FLAG_EXEC;
	return flags;
}

static void
syscall_unmap_user_range(process_t *proc, uint32_t start, uint32_t end)
{
	uint32_t *pd;

	if (!proc || start >= end)
		return;

	pd = (uint32_t *)proc->pd_phys;
	for (uint32_t page = start; page < end; page += PAGE_SIZE) {
		uint32_t pdi = page >> 22;
		uint32_t pti = (page >> 12) & 0x3FFu;
		uint32_t *pt;

		if ((pd[pdi] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
			continue;

		pt = (uint32_t *)paging_entry_addr(pd[pdi]);
		if ((pt[pti] & PG_PRESENT) == 0)
			continue;

		pmm_decref(paging_entry_addr(pt[pti]));
		pt[pti] = 0;
		syscall_invlpg(page);
	}
}

static void syscall_apply_mprotect(process_t *proc,
                                   uint32_t start,
                                   uint32_t end,
                                   uint32_t prot)
{
	if (!proc || start >= end)
		return;

	for (uint32_t page = start; page < end; page += PAGE_SIZE) {
		uint32_t *pte;
		uint32_t flags;
		uint32_t new_pte;

		if (paging_walk(proc->pd_phys, page, &pte) != 0)
			continue;

		/*
         * A PTE stores both the frame address and the low permission bits.
         * Rebuild the entry from its original address plus updated flags so
         * future permission changes cannot accidentally blend address bits.
         */
		flags = paging_entry_flags(*pte);
		if (prot_has_user_access(prot))
			flags |= PG_USER;
		else
			flags &= ~(uint32_t)PG_USER;

		if ((prot & PROT_WRITE) != 0 && (flags & PG_COW) == 0)
			flags |= PG_WRITABLE;
		else
			flags &= ~(uint32_t)PG_WRITABLE;

		new_pte = paging_entry_build(paging_entry_addr(*pte), flags);

		if (new_pte == *pte)
			continue;

		*pte = new_pte;
		syscall_invlpg(page);
	}
}

static int syscall_mmap_private_file(process_t *cur,
                                     uint32_t hint,
                                     uint32_t length,
                                     uint32_t prot,
                                     uint32_t fd,
                                     uint32_t file_offset,
                                     uint32_t *addr_out)
{
	file_handle_t *fh;
	uint32_t map_len;
	uint32_t map_addr = 0;
	uint32_t vma_flags;
	uint32_t pte_flags;

	if (!cur || !addr_out || fd >= MAX_FDS || length == 0)
		return -1;
	if (file_offset & (PAGE_SIZE - 1u))
		return -1;

	fh = &proc_fd_entries(cur)[fd];
	if ((fh->type != FD_TYPE_FILE && fh->type != FD_TYPE_SYSFILE) ||
	    fh->access_mode == LINUX_O_WRONLY)
		return -1;

	map_len = (length + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
	if (map_len == 0)
		return -1;

	vma_flags = prot_to_vma_flags(prot) & ~(uint32_t)VMA_FLAG_ANON;
	if (vma_map_anonymous(cur, hint, map_len, vma_flags, &map_addr) != 0)
		return -1;

	pte_flags = PG_PRESENT;
	if (prot_has_user_access(prot))
		pte_flags |= PG_USER;
	if (prot & PROT_WRITE)
		pte_flags |= PG_WRITABLE;

	for (uint32_t off = 0; off < map_len; off += PAGE_SIZE) {
		uint32_t phys = pmm_alloc_page();
		int n;

		if (!phys)
			goto fail;
		if (file_offset > UINT32_MAX - off) {
			pmm_free_page(phys);
			goto fail;
		}

		k_memset((void *)phys, 0, PAGE_SIZE);
		n = vfs_read(
		    fh->u.file.ref, file_offset + off, (uint8_t *)phys, PAGE_SIZE);
		if (n < 0) {
			pmm_free_page(phys);
			goto fail;
		}
		if (paging_map_page(cur->pd_phys, map_addr + off, phys, pte_flags) !=
		    0) {
			pmm_free_page(phys);
			goto fail;
		}
	}

	*addr_out = map_addr;
	return 0;

fail:
	syscall_unmap_user_range(cur, map_addr, map_addr + map_len);
	(void)vma_unmap_range(cur, map_addr, map_addr + map_len);
	return -1;
}

uint32_t SYSCALL_NOINLINE syscall_case_brk(uint32_t ebx)
{
	{
		/*
         * ebx = requested new program break, or 0 to query the current brk.
         *
         * Behaviour (matches Linux i386 brk(2) semantics):
         *   - ebx == 0: return the current brk without changing anything.
         *   - ebx < heap_start: refuse (cannot move break below the heap base).
         *   - ebx > USER_HEAP_MAX: refuse (would collide with the user stack).
         *   - Otherwise: update brk immediately. Heap pages are committed on
         *     first touch by the page-fault handler.
         *   - On shrink, any currently present heap pages in the truncated
         *     range are unmapped and their frames are decref'd immediately.
         *
         * The caller must compare the return value to its request to detect
         * failure — this is the standard Linux brk() contract.
         */
		process_t *cur = sched_current();
		proc_address_space_t *as;
		uint32_t *brk_ptr;
		uint32_t pd_phys;
		vm_area_t *heap_vma;
		if (!cur)
			return (uint32_t)-1;
		as = cur->as;
		brk_ptr = as ? &as->brk : &cur->brk;
		pd_phys = as ? as->pd_phys : cur->pd_phys;
		heap_vma = vma_find_kind(cur, VMA_KIND_HEAP);
		if (!heap_vma)
			return (uint32_t)-1;

		uint32_t new_brk = ebx;

		/* Query: return current brk without changing anything. */
		if (new_brk == 0) {
			klog_hex("BRK", "query brk", *brk_ptr);
			klog_uint("BRK", "query pid", cur->pid);
			return *brk_ptr;
		}

		/* Guard: must be above the heap base and below the stack region. */
		if (new_brk < heap_vma->start || new_brk > USER_HEAP_MAX)
			return *brk_ptr;

		if (new_brk < *brk_ptr) {
			uint32_t unmap_start = (new_brk + 0xFFFu) & ~0xFFFu;
			uint32_t old_end = (*brk_ptr + 0xFFFu) & ~0xFFFu;
			uint32_t *pd = (uint32_t *)pd_phys;

			for (uint32_t vpage = unmap_start; vpage < old_end;
			     vpage += 0x1000u) {
				uint32_t pdi = vpage >> 22;
				uint32_t pti = (vpage >> 12) & 0x3FFu;

				if (!(pd[pdi] & PG_PRESENT))
					continue;

				uint32_t *pt = (uint32_t *)paging_entry_addr(pd[pdi]);
				if (!(pt[pti] & PG_PRESENT))
					continue;

				pmm_decref(paging_entry_addr(pt[pti]));
				pt[pti] = 0;
				syscall_invlpg(vpage);
			}
		}

		*brk_ptr = new_brk;
		heap_vma->end = new_brk;
		return new_brk;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_mmap(uint32_t ebx)
{
	{
		old_mmap_args_t args;
		process_t *cur = sched_current();
		uint32_t map_addr = 0;

		if (!cur || ebx == 0)
			return (uint32_t)-1;
		if (uaccess_copy_from_user(cur, &args, ebx, sizeof(args)) != 0)
			return (uint32_t)-1;
		if (args.length == 0 || !prot_is_valid(args.prot))
			return (uint32_t)-LINUX_EINVAL;
		if ((args.flags & MAP_ANONYMOUS) != 0) {
			uint32_t required = MAP_PRIVATE | MAP_ANONYMOUS;

			if ((args.flags & required) != required ||
			    (args.flags & ~required) != 0)
				return (uint32_t)-1;
			if (args.offset != 0)
				return (uint32_t)-1;
			if (vma_map_anonymous(cur,
			                      args.addr,
			                      args.length,
			                      prot_to_vma_flags(args.prot),
			                      &map_addr) != 0)
				return (uint32_t)-1;
		} else {
			if ((args.flags & ~MAP_PRIVATE) != 0 ||
			    (args.flags & MAP_PRIVATE) == 0)
				return (uint32_t)-1;
			if (syscall_mmap_private_file(cur,
			                              args.addr,
			                              args.length,
			                              args.prot,
			                              args.fd,
			                              args.offset,
			                              &map_addr) != 0)
				return (uint32_t)-1;
		}
		return map_addr;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_mmap2(uint32_t ebx,
                                             uint32_t ecx,
                                             uint32_t edx,
                                             uint32_t esi,
                                             uint32_t edi,
                                             uint32_t ebp)
{
	{
		/*
         * Linux i386 mmap2:
         *   ebx=addr, ecx=len, edx=prot, esi=flags, edi=fd, ebp=pgoffset.
         *
         * Drunix supports private anonymous mappings and eager MAP_PRIVATE
         * file mappings without MAP_FIXED.
         */
		process_t *cur = sched_current();
		uint32_t map_addr = 0;

		if (!cur)
			return (uint32_t)-1;
		if (ecx == 0 || !prot_is_valid(edx))
			return (uint32_t)-LINUX_EINVAL;
		if (esi & MAP_ANONYMOUS) {
			uint32_t required = MAP_PRIVATE | MAP_ANONYMOUS;

			if ((esi & required) != required ||
			    (esi & ~(MAP_PRIVATE | MAP_ANONYMOUS)) != 0)
				return (uint32_t)-1;
			if (ebp != 0)
				return (uint32_t)-1;
			if (vma_map_anonymous(
			        cur, ebx, ecx, prot_to_vma_flags(edx), &map_addr) != 0)
				return (uint32_t)-1;
		} else {
			if ((esi & ~MAP_PRIVATE) != 0 || (esi & MAP_PRIVATE) == 0)
				return (uint32_t)-1;
			if (ebp > UINT32_MAX / PAGE_SIZE)
				return (uint32_t)-1;
			if (syscall_mmap_private_file(
			        cur, ebx, ecx, edx, edi, ebp * PAGE_SIZE, &map_addr) != 0)
				return (uint32_t)-1;
		}
		return map_addr;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_munmap(uint32_t ebx, uint32_t ecx)
{
	{
		process_t *cur = sched_current();
		uint32_t length;
		uint32_t end;

		if (!cur || ebx == 0 || (ebx & (PAGE_SIZE - 1u)) != 0 || ecx == 0)
			return (uint32_t)-1;

		length = (ecx + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
		end = ebx + length;
		if (length == 0 || end <= ebx || end > USER_STACK_TOP)
			return (uint32_t)-1;
		if (vma_unmap_range(cur, ebx, end) != 0)
			return (uint32_t)-1;

		syscall_unmap_user_range(cur, ebx, end);
		return 0;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_mprotect(uint32_t ebx,
                                                uint32_t ecx,
                                                uint32_t edx)
{
	{
		process_t *cur = sched_current();
		uint32_t length;
		uint32_t end;

		if (!cur || ebx == 0 || (ebx & (PAGE_SIZE - 1u)) != 0 || ecx == 0 ||
		    !prot_is_valid(edx))
			return (uint32_t)-1;

		length = (ecx + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
		end = ebx + length;
		if (length == 0 || end <= ebx || end > USER_STACK_TOP)
			return (uint32_t)-1;
		if (vma_protect_range(cur, ebx, end, prot_to_vma_flags(edx)) != 0)
			return (uint32_t)-1;

		syscall_apply_mprotect(cur, ebx, end, edx);
		return 0;
	}
}
