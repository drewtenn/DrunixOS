/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * mem.c - Linux i386 process memory syscalls.
 *
 * Owns brk, mmap/mmap2, munmap, mprotect, and the page-table helpers needed
 * to translate Linux protection flags into Drunix VMAs and PTE permissions.
 */

#include "syscall_internal.h"
#include "syscall_linux.h"
#include "arch.h"
#include "chardev.h"
#include "commit.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
#include "vfs.h"
#include "vma.h"
#include "wmdev.h"
#include <stdint.h>

static int prot_is_valid(uint32_t prot)
{
	return (prot & ~(LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC)) ==
	       0;
}

static int prot_has_user_access(uint32_t prot)
{
	return (prot & (LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC)) != 0;
}

static uint32_t prot_to_vma_flags(uint32_t prot)
{
	uint32_t flags = VMA_FLAG_ANON | VMA_FLAG_PRIVATE;

	if (prot & LINUX_PROT_READ)
		flags |= VMA_FLAG_READ;
	if (prot & LINUX_PROT_WRITE)
		flags |= VMA_FLAG_WRITE;
	if (prot & LINUX_PROT_EXEC)
		flags |= VMA_FLAG_EXEC;
	return flags;
}

static void
syscall_unmap_user_range(process_t *proc, uint32_t start, uint32_t end)
{
	arch_aspace_t aspace;

	if (!proc || start >= end)
		return;

	aspace = (arch_aspace_t)proc->pd_phys;
	for (uint32_t page = start; page < end; page += PAGE_SIZE) {
		arch_mm_mapping_t mapping;

		if (arch_mm_query(aspace, page, &mapping) != 0)
			continue;
		if ((mapping.flags & ARCH_MM_MAP_USER) == 0)
			continue;

		(void)arch_mm_unmap(aspace, page);
	}
}

static void syscall_apply_mprotect(process_t *proc,
                                   uint32_t start,
                                   uint32_t end,
                                   uint32_t prot)
{
	arch_aspace_t aspace;

	if (!proc || start >= end)
		return;

	aspace = (arch_aspace_t)proc->pd_phys;
	for (uint32_t page = start; page < end; page += PAGE_SIZE) {
		arch_mm_mapping_t mapping;
		uint32_t desired_flags;
		uint32_t user_changed;
		uint32_t write_changed;

		if (arch_mm_query(aspace, page, &mapping) != 0)
			continue;

		desired_flags = mapping.flags;
		if (prot_has_user_access(prot))
			desired_flags |= ARCH_MM_MAP_USER;
		else
			desired_flags &= ~(uint32_t)ARCH_MM_MAP_USER;

		if ((prot & LINUX_PROT_WRITE) != 0 &&
		    (mapping.flags & ARCH_MM_MAP_COW) == 0)
			desired_flags |= ARCH_MM_MAP_WRITE;
		else
			desired_flags &= ~(uint32_t)ARCH_MM_MAP_WRITE;

		if (desired_flags == mapping.flags)
			continue;

		user_changed =
		    (desired_flags ^ mapping.flags) & ARCH_MM_MAP_USER;
		write_changed =
		    (desired_flags ^ mapping.flags) & ARCH_MM_MAP_WRITE;

		if (user_changed == 0 && write_changed != 0) {
			if (arch_mm_update(aspace,
			                   page,
			                   (mapping.flags & ARCH_MM_MAP_WRITE)
			                       ? ARCH_MM_MAP_WRITE
			                       : 0u,
			                   (desired_flags & ARCH_MM_MAP_WRITE)
			                       ? ARCH_MM_MAP_WRITE
			                       : 0u) == 0)
				continue;
		}

		(void)arch_mm_map(aspace, page, mapping.phys_addr, desired_flags);
	}
}

static int syscall_vma_is_committable(const vm_area_t *vma)
{
	if (!vma)
		return 0;

	return vma->kind == VMA_KIND_GENERIC &&
	       (vma->flags & (VMA_FLAG_ANON | VMA_FLAG_PRIVATE)) ==
	           (VMA_FLAG_ANON | VMA_FLAG_PRIVATE);
}

static int syscall_vma_requires_commit(const vm_area_t *vma)
{
	return syscall_vma_is_committable(vma) &&
	       (vma->flags & VMA_FLAG_WRITE) != 0;
}

static uint32_t syscall_mprotect_transition_bytes(process_t *proc,
                                                  uint32_t start,
                                                  uint32_t end,
                                                  uint32_t new_flags,
                                                  int reserve_transition)
{
	const vm_area_t *vmas;
	uint32_t count;
	uint32_t bytes = 0;

	if (!proc || start >= end)
		return 0;

	if (proc->as) {
		vmas = proc->as->vmas;
		count = proc->as->vma_count;
	} else {
		vmas = proc->vmas;
		count = proc->vma_count;
	}
	if (!vmas)
		return 0;

	for (uint32_t i = 0; i < count; i++) {
		const vm_area_t *vma = &vmas[i];
		uint32_t overlap_start;
		uint32_t overlap_end;
		uint32_t overlap_bytes;
		int has_commit;
		int needs_commit;

		if (!syscall_vma_is_committable(vma))
			continue;
		if (start >= vma->end || end <= vma->start)
			continue;

		has_commit = (vma->flags & VMA_FLAG_WRITE) != 0;
		needs_commit = (new_flags & VMA_FLAG_WRITE) != 0;
		if (reserve_transition) {
			if (has_commit || !needs_commit)
				continue;
		} else if (!has_commit || needs_commit) {
			continue;
		}

		overlap_start = start > vma->start ? start : vma->start;
		overlap_end = end < vma->end ? end : vma->end;
		if (overlap_start >= overlap_end)
			continue;

		overlap_bytes = overlap_end - overlap_start;
		if (bytes > UINT32_MAX - overlap_bytes)
			return UINT32_MAX;
		bytes += overlap_bytes;
	}

	return bytes;
}

static uint32_t
syscall_committed_munmap_bytes(process_t *proc, uint32_t start, uint32_t end)
{
	const vm_area_t *vmas;
	uint32_t count;
	uint32_t bytes = 0;

	if (!proc || start >= end)
		return 0;

	if (proc->as) {
		vmas = proc->as->vmas;
		count = proc->as->vma_count;
	} else {
		vmas = proc->vmas;
		count = proc->vma_count;
	}
	if (!vmas)
		return 0;

	for (uint32_t i = 0; i < count; i++) {
		const vm_area_t *vma = &vmas[i];
		uint32_t overlap_start;
		uint32_t overlap_end;
		uint32_t overlap_bytes;

		if (!syscall_vma_requires_commit(vma))
			continue;
		if (start >= vma->end || end <= vma->start)
			continue;

		overlap_start = start > vma->start ? start : vma->start;
		overlap_end = end < vma->end ? end : vma->end;
		if (overlap_start >= overlap_end)
			continue;

		overlap_bytes = overlap_end - overlap_start;
		if (bytes > UINT32_MAX - overlap_bytes)
			return UINT32_MAX;
		bytes += overlap_bytes;
	}

	return bytes;
}

static int syscall_range_has_shared_mapping(process_t *proc,
                                            uint32_t start,
                                            uint32_t end)
{
	arch_aspace_t aspace;

	if (!proc || start >= end)
		return 0;

	aspace = (arch_aspace_t)proc->pd_phys;
	for (uint32_t page = start; page < end; page += PAGE_SIZE) {
		arch_mm_mapping_t mapping;

		if (arch_mm_query(aspace, page, &mapping) != 0)
			continue;
		if (mapping.flags & ARCH_MM_MAP_SHARED)
			return 1;
	}

	return 0;
}

static int syscall_mmap_anonymous_private(process_t *cur,
                                          uint32_t hint,
                                          uint32_t length,
                                          uint32_t prot,
                                          uint32_t *addr_out)
{
	uint32_t flags;
	int reserved = 0;

	if (!cur || !addr_out || length == 0)
		return -1;

	flags = prot_to_vma_flags(prot);
	if ((flags & VMA_FLAG_WRITE) != 0) {
		if (vm_commit_reserve(cur, length) != 0)
			return -1;
		reserved = 1;
	}

	if (vma_map_anonymous(cur, hint, length, flags, addr_out) != 0) {
		if (reserved)
			vm_commit_release(cur, length);
		return -1;
	}

	return 0;
}

static int syscall_mmap_chardev(process_t *cur,
                                uint32_t hint,
                                uint32_t length,
                                uint32_t prot,
                                uint32_t fd,
                                uint32_t file_offset,
                                uint32_t *addr_out)
{
	file_handle_t *fh;
	const chardev_ops_t *dev;
	uint32_t map_len;
	uint32_t map_addr = 0;
	uint64_t base_phys = 0;
	uint32_t vma_flags;
	uint32_t map_flags;
	arch_aspace_t aspace;

	if (!cur || !addr_out || fd >= MAX_FDS || length == 0)
		return -1;
	if (file_offset & (PAGE_SIZE - 1u))
		return -1;

	fh = &proc_fd_entries(cur)[fd];
	if (fh->type != FD_TYPE_CHARDEV)
		return -1;

	dev = chardev_get(fh->u.chardev.name);
	if (!dev || !dev->mmap_phys)
		return -1;

	map_len = (length + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
	if (map_len == 0)
		return -1;

	if (dev->mmap_phys(file_offset, map_len, prot, &base_phys) != 0)
		return -1;
	if (base_phys & (PAGE_SIZE - 1u))
		return -1;

	vma_flags = prot_to_vma_flags(prot) & ~(uint32_t)VMA_FLAG_ANON;
	if (vma_map_anonymous(cur, hint, map_len, vma_flags, &map_addr) != 0)
		return -1;

	aspace = (arch_aspace_t)cur->pd_phys;
	map_flags = ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ;
	/*
	 * M2.5a: drivers may opt into a specific cache attribute for their
	 * mmap aperture. Drivers without an mmap_cache_policy op keep the
	 * historical Device-memory mapping (ARCH_MM_MAP_IO).
	 */
	if (dev->mmap_cache_policy) {
		switch (dev->mmap_cache_policy(file_offset, map_len)) {
		case CHARDEV_CACHE_DEFAULT:
			break;
		case CHARDEV_CACHE_DEVICE:
			map_flags |= ARCH_MM_MAP_IO;
			break;
		case CHARDEV_CACHE_NC:
			map_flags |= ARCH_MM_MAP_NC;
			break;
		case CHARDEV_CACHE_WB_FLUSH:
		default:
			/* Unsupported in M2.5a; fall back to Device. */
			map_flags |= ARCH_MM_MAP_IO;
			break;
		}
	} else {
		map_flags |= ARCH_MM_MAP_IO;
	}
	if (prot_has_user_access(prot))
		map_flags |= ARCH_MM_MAP_USER;
	if (prot & LINUX_PROT_WRITE)
		map_flags |= ARCH_MM_MAP_WRITE;

	for (uint32_t off = 0; off < map_len; off += PAGE_SIZE) {
		uint64_t phys = base_phys + off;

		if (phys > UINT32_MAX)
			goto fail;
		if (arch_mm_map(aspace, map_addr + off, phys, map_flags) != 0)
			goto fail;
	}

	*addr_out = map_addr;
	return 0;

fail:
	syscall_unmap_user_range(cur, map_addr, map_addr + map_len);
	(void)vma_unmap_range(cur, map_addr, map_addr + map_len);
	return -1;
}

static int syscall_mmap_wmdev(process_t *cur,
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
	uint32_t map_flags;
	arch_aspace_t aspace;

	if (!cur || !addr_out || fd >= MAX_FDS || length == 0)
		return -1;
	if (file_offset & (PAGE_SIZE - 1u))
		return -1;
	fh = &proc_fd_entries(cur)[fd];
	if (fh->type != FD_TYPE_WM)
		return -1;
	if (!prot_has_user_access(prot))
		return -1;
	map_len = (length + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
	if (map_len == 0)
		return -1;
	vma_flags = prot_to_vma_flags(prot) & ~(uint32_t)VMA_FLAG_ANON;
	if (vma_map_anonymous(cur, hint, map_len, vma_flags, &map_addr) != 0)
		return -1;
	aspace = (arch_aspace_t)cur->pd_phys;
	map_flags = ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ | ARCH_MM_MAP_USER |
	            ARCH_MM_MAP_SHARED;
	if (prot & LINUX_PROT_WRITE)
		map_flags |= ARCH_MM_MAP_WRITE;
	for (uint32_t off = 0; off < map_len; off += PAGE_SIZE) {
		uint32_t phys = 0;
		if (wmdev_mmap_page(fh->u.wm.conn_id,
		                    file_offset,
		                    off / PAGE_SIZE,
		                    &phys) != 0)
			goto fail;
		pmm_incref(phys);
		if (arch_mm_map(aspace, map_addr + off, phys, map_flags) != 0) {
			pmm_decref(phys);
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
	if (file_offset > UINT32_MAX - (map_len - PAGE_SIZE))
		return -1;

	vma_flags = prot_to_vma_flags(prot) & ~(uint32_t)VMA_FLAG_ANON;
	if (vma_map_file_private(cur,
	                         hint,
	                         map_len,
	                         vma_flags,
	                         fh->u.file.ref,
	                         file_offset,
	                         fh->u.file.size,
	                         &map_addr) != 0)
		return -1;

	*addr_out = map_addr;
	return 0;
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
		vm_area_t *heap_vma;
		if (!cur)
			return (uint32_t)-1;
		as = cur->as;
		brk_ptr = as ? &as->brk : &cur->brk;
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

		if (new_brk > *brk_ptr) {
			uint32_t old_commit_end = vm_page_align_up(*brk_ptr);
			uint32_t new_commit_end = vm_page_align_up(new_brk);

			if (as && new_brk - heap_vma->start > as->rlimit_data)
				return *brk_ptr;
			if (new_commit_end == 0)
				return *brk_ptr;
			if (new_commit_end > old_commit_end &&
			    vm_commit_reserve(cur, new_commit_end - old_commit_end) != 0)
				return *brk_ptr;
		}

		if (new_brk < *brk_ptr) {
			uint32_t unmap_start = (new_brk + 0xFFFu) & ~0xFFFu;
			uint32_t old_end = (*brk_ptr + 0xFFFu) & ~0xFFFu;

			syscall_unmap_user_range(cur, unmap_start, old_end);
			if (old_end > unmap_start)
				vm_commit_release(cur, old_end - unmap_start);
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
		if ((args.flags & LINUX_MAP_ANONYMOUS) != 0) {
			uint32_t required = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS;

			if ((args.flags & required) != required ||
			    (args.flags & ~required) != 0)
				return (uint32_t)-1;
			if (args.offset != 0)
				return (uint32_t)-1;
			if (syscall_mmap_anonymous_private(cur,
			                                   args.addr,
			                                   args.length,
			                                   args.prot,
			                                   &map_addr) != 0)
				return (uint32_t)-1;
		} else {
			file_handle_t *fh;
			int is_chardev;
			int is_wmdev;

			if (args.fd >= MAX_FDS)
				return (uint32_t)-1;
			fh = &proc_fd_entries(cur)[args.fd];
			is_chardev = (fh->type == FD_TYPE_CHARDEV);
			is_wmdev = (fh->type == FD_TYPE_WM);

			if (is_wmdev) {
				if ((args.flags & ~LINUX_MAP_SHARED) != 0 ||
				    (args.flags & LINUX_MAP_SHARED) == 0)
					return (uint32_t)-1;
				if (syscall_mmap_wmdev(cur,
				                       args.addr,
				                       args.length,
				                       args.prot,
				                       args.fd,
				                       args.offset,
				                       &map_addr) != 0)
					return (uint32_t)-1;
			} else if (is_chardev) {
				if ((args.flags & ~LINUX_MAP_SHARED) != 0 ||
				    (args.flags & LINUX_MAP_SHARED) == 0)
					return (uint32_t)-1;
				if (syscall_mmap_chardev(cur,
				                         args.addr,
				                         args.length,
				                         args.prot,
				                         args.fd,
				                         args.offset,
				                         &map_addr) != 0)
					return (uint32_t)-1;
			} else {
				if ((args.flags & ~LINUX_MAP_PRIVATE) != 0 ||
				    (args.flags & LINUX_MAP_PRIVATE) == 0)
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
         * Drunix supports private anonymous mappings and lazy MAP_PRIVATE
         * file mappings without MAP_FIXED.
         */
		process_t *cur = sched_current();
		uint32_t map_addr = 0;

		if (!cur)
			return (uint32_t)-1;
		if (ecx == 0 || !prot_is_valid(edx))
			return (uint32_t)-LINUX_EINVAL;
		if (esi & LINUX_MAP_ANONYMOUS) {
			uint32_t required = LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS;

			if ((esi & required) != required ||
			    (esi & ~(LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS)) != 0)
				return (uint32_t)-1;
			if (ebp != 0)
				return (uint32_t)-1;
			if (syscall_mmap_anonymous_private(cur,
			                                   ebx,
			                                   ecx,
			                                   edx,
			                                   &map_addr) != 0)
				return (uint32_t)-1;
		} else {
			file_handle_t *fh;
			int is_chardev;
			int is_wmdev;

			if (edi >= MAX_FDS)
				return (uint32_t)-1;
			fh = &proc_fd_entries(cur)[edi];
			is_chardev = (fh->type == FD_TYPE_CHARDEV);
			is_wmdev = (fh->type == FD_TYPE_WM);
			if (ebp > UINT32_MAX / PAGE_SIZE)
				return (uint32_t)-1;

			if (is_wmdev) {
				if ((esi & ~LINUX_MAP_SHARED) != 0 ||
				    (esi & LINUX_MAP_SHARED) == 0)
					return (uint32_t)-1;
				if (syscall_mmap_wmdev(cur,
				                       ebx,
				                       ecx,
				                       edx,
				                       edi,
				                       ebp * PAGE_SIZE,
				                       &map_addr) != 0)
					return (uint32_t)-1;
			} else if (is_chardev) {
				if ((esi & ~LINUX_MAP_SHARED) != 0 ||
				    (esi & LINUX_MAP_SHARED) == 0)
					return (uint32_t)-1;
				if (syscall_mmap_chardev(cur,
				                         ebx,
				                         ecx,
				                         edx,
				                         edi,
				                         ebp * PAGE_SIZE,
				                         &map_addr) != 0)
					return (uint32_t)-1;
			} else {
				if ((esi & ~LINUX_MAP_PRIVATE) != 0 ||
				    (esi & LINUX_MAP_PRIVATE) == 0)
					return (uint32_t)-1;
				if (syscall_mmap_private_file(cur,
				                              ebx,
				                              ecx,
				                              edx,
				                              edi,
				                              ebp * PAGE_SIZE,
				                              &map_addr) != 0)
					return (uint32_t)-1;
			}
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
		uint32_t commit_bytes;

		if (!cur || ebx == 0 || (ebx & (PAGE_SIZE - 1u)) != 0 || ecx == 0)
			return (uint32_t)-1;

		length = (ecx + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
		end = ebx + length;
		if (length == 0 || end <= ebx || end > USER_STACK_TOP)
			return (uint32_t)-1;
		commit_bytes = syscall_committed_munmap_bytes(cur, ebx, end);
		if (vma_unmap_range(cur, ebx, end) != 0)
			return (uint32_t)-1;

		syscall_unmap_user_range(cur, ebx, end);
		vm_commit_release(cur, commit_bytes);
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
		uint32_t new_flags;
		uint32_t reserve_bytes;
		uint32_t release_bytes;

		if (!cur || ebx == 0 || (ebx & (PAGE_SIZE - 1u)) != 0 || ecx == 0 ||
		    !prot_is_valid(edx))
			return (uint32_t)-1;

		length = (ecx + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
		end = ebx + length;
		if (length == 0 || end <= ebx || end > USER_STACK_TOP)
			return (uint32_t)-1;

		new_flags = prot_to_vma_flags(edx);
		if (!prot_has_user_access(edx) &&
		    syscall_range_has_shared_mapping(cur, ebx, end))
			return (uint32_t)-1;

		reserve_bytes = syscall_mprotect_transition_bytes(
		    cur, ebx, end, new_flags, 1);
		if (reserve_bytes != 0 && vm_commit_reserve(cur, reserve_bytes) != 0)
			return (uint32_t)-1;

		release_bytes = syscall_mprotect_transition_bytes(
		    cur, ebx, end, new_flags, 0);
		if (vma_protect_range(cur, ebx, end, new_flags) != 0) {
			if (reserve_bytes != 0)
				vm_commit_release(cur, reserve_bytes);
			return (uint32_t)-1;
		}

		syscall_apply_mprotect(cur, ebx, end, edx);
		if (release_bytes != 0)
			vm_commit_release(cur, release_bytes);
		return 0;
	}
}
