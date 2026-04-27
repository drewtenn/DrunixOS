/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * vma.c — per-process virtual memory area bookkeeping for anonymous mappings.
 */

#include "vma.h"
#include "process.h"
#include "pmm.h"
#include "kstring.h"
#include "vm_layout.h"

#define VMA_PROT_FLAGS (VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC)

static vm_area_t *vma_table(struct process *proc)
{
	if (!proc)
		return 0;
	return proc && proc->as ? proc->as->vmas : proc->vmas;
}

static const vm_area_t *vma_table_const(const struct process *proc)
{
	if (!proc)
		return 0;
	return proc && proc->as ? proc->as->vmas : proc->vmas;
}

static uint32_t *vma_count_ptr(struct process *proc)
{
	if (!proc)
		return 0;
	return proc && proc->as ? &proc->as->vma_count : &proc->vma_count;
}

static uint32_t vma_count_const(const struct process *proc)
{
	if (!proc)
		return 0;
	return proc && proc->as ? proc->as->vma_count : proc->vma_count;
}

static uint32_t vma_brk_const(const struct process *proc)
{
	if (!proc)
		return 0;
	return proc && proc->as ? proc->as->brk : proc->brk;
}

void vma_init(struct process *proc)
{
	vm_area_t *vmas;
	uint32_t *count;

	if (!proc)
		return;

	vmas = vma_table(proc);
	count = vma_count_ptr(proc);
	*count = 0;
	k_memset(vmas, 0, sizeof(proc->vmas));
}

static int vma_overlaps(const vm_area_t *lhs, uint32_t start, uint32_t end)
{
	if (!lhs)
		return 0;

	return start < lhs->end && end > lhs->start;
}

static int vma_is_file_backed(const vm_area_t *vma)
{
	if (!vma)
		return 0;

	return vma->kind == VMA_KIND_GENERIC && (vma->flags & VMA_FLAG_ANON) == 0;
}

static vm_area_t vma_slice(const vm_area_t *vma, uint32_t start, uint32_t end)
{
	vm_area_t slice = *vma;

	if (vma_is_file_backed(vma) && start > vma->start)
		slice.file_offset += start - vma->start;
	slice.start = start;
	slice.end = end;
	return slice;
}

static uint32_t vma_replace_prot_flags(uint32_t old_flags, uint32_t new_flags)
{
	return (old_flags & ~(uint32_t)VMA_PROT_FLAGS) |
	       (new_flags & VMA_PROT_FLAGS);
}

static int vma_can_merge(const vm_area_t *lhs, const vm_area_t *rhs)
{
	if (!lhs || !rhs)
		return 0;
	if (vma_is_file_backed(lhs) || vma_is_file_backed(rhs))
		return 0;

	return lhs->end == rhs->start && lhs->flags == rhs->flags &&
	       lhs->kind == rhs->kind && lhs->file_offset == rhs->file_offset &&
	       lhs->file_size == rhs->file_size &&
	       lhs->file_ref.mount_id == rhs->file_ref.mount_id &&
	       lhs->file_ref.inode_num == rhs->file_ref.inode_num;
}

static void vma_coalesce(struct process *proc)
{
	vm_area_t *vmas;
	uint32_t *count;
	uint32_t out = 0;

	if (!proc)
		return;

	vmas = vma_table(proc);
	count = vma_count_ptr(proc);
	for (uint32_t i = 0; i < *count; i++) {
		if (out > 0 && vma_can_merge(&vmas[out - 1], &vmas[i])) {
			vmas[out - 1].end = vmas[i].end;
			continue;
		}

		if (out != i)
			vmas[out] = vmas[i];
		out++;
	}

	while (out < *count) {
		k_memset(&vmas[out], 0, sizeof(vmas[out]));
		out++;
	}

	*count = 0;
	while (*count < PROCESS_MAX_VMAS &&
	       (vmas[*count].start != 0 || vmas[*count].end != 0 ||
	        vmas[*count].flags != 0 || vmas[*count].kind != 0)) {
		(*count)++;
	}
}

static int vma_append_area(vm_area_t *dst,
                           uint32_t *count,
                           const vm_area_t *area)
{
	if (!dst || !count || !area || area->start > area->end)
		return -1;

	if (*count > 0 && vma_can_merge(&dst[*count - 1], area)) {
		dst[*count - 1].end = area->end;
		return 0;
	}

	if (*count >= PROCESS_MAX_VMAS)
		return -1;

	dst[*count] = *area;
	(*count)++;
	return 0;
}

static int
vma_range_is_free(const struct process *proc, uint32_t start, uint32_t end)
{
	const vm_area_t *vmas;
	uint32_t count;

	if (!proc || start >= end)
		return 0;

	vmas = vma_table_const(proc);
	count = vma_count_const(proc);
	for (uint32_t i = 0; i < count; i++) {
		if (vma_overlaps(&vmas[i], start, end))
			return 0;
	}

	return 1;
}

static int vma_insert_area(struct process *proc, const vm_area_t *area)
{
	vm_area_t *vmas;
	uint32_t *count;
	uint32_t insert_at = 0;

	if (!proc || !area || area->start > area->end)
		return -1;

	vmas = vma_table(proc);
	count = vma_count_ptr(proc);
	if (*count >= PROCESS_MAX_VMAS)
		return -1;

	while (insert_at < *count && vmas[insert_at].start <= area->start) {
		if (vma_overlaps(&vmas[insert_at], area->start, area->end))
			return -1;
		insert_at++;
	}

	if (insert_at < *count &&
	    vma_overlaps(&vmas[insert_at], area->start, area->end))
		return -1;

	if (insert_at < *count) {
		k_memmove(&vmas[insert_at + 1],
		          &vmas[insert_at],
		          (*count - insert_at) * sizeof(vmas[0]));
	}

	vmas[insert_at] = *area;
	(*count)++;
	vma_coalesce(proc);
	return 0;
}

int vma_add(struct process *proc,
            uint32_t start,
            uint32_t end,
            uint32_t flags,
            uint32_t kind)
{
	vm_area_t area;

	k_memset(&area, 0, sizeof(area));
	area.start = start;
	area.end = end;
	area.flags = flags;
	area.kind = kind;
	return vma_insert_area(proc, &area);
}

vm_area_t *vma_find(struct process *proc, uint32_t addr)
{
	vm_area_t *vmas;
	uint32_t count;

	if (!proc)
		return 0;

	vmas = vma_table(proc);
	count = vma_count_const(proc);
	for (uint32_t i = 0; i < count; i++) {
		vm_area_t *vma = &vmas[i];
		if (addr >= vma->start && addr < vma->end)
			return vma;
	}

	return 0;
}

const vm_area_t *vma_find_const(const struct process *proc, uint32_t addr)
{
	const vm_area_t *vmas;
	uint32_t count;

	if (!proc)
		return 0;

	vmas = vma_table_const(proc);
	count = vma_count_const(proc);
	for (uint32_t i = 0; i < count; i++) {
		const vm_area_t *vma = &vmas[i];
		if (addr >= vma->start && addr < vma->end)
			return vma;
	}

	return 0;
}

vm_area_t *vma_find_kind(struct process *proc, uint32_t kind)
{
	vm_area_t *vmas;
	uint32_t count;

	if (!proc)
		return 0;

	vmas = vma_table(proc);
	count = vma_count_const(proc);
	for (uint32_t i = 0; i < count; i++) {
		if (vmas[i].kind == kind)
			return &vmas[i];
	}

	return 0;
}

const vm_area_t *vma_find_kind_const(const struct process *proc, uint32_t kind)
{
	const vm_area_t *vmas;
	uint32_t count;

	if (!proc)
		return 0;

	vmas = vma_table_const(proc);
	count = vma_count_const(proc);
	for (uint32_t i = 0; i < count; i++) {
		if (vmas[i].kind == kind)
			return &vmas[i];
	}

	return 0;
}

static int vma_find_topdown_gap(struct process *proc,
                                uint32_t hint,
                                uint32_t length,
                                uint32_t *addr_out)
{
	const vm_area_t *stack_vma;
	uint32_t len;
	uint32_t low;
	uint32_t high;
	uint32_t scan_end;

	if (!proc || !addr_out || length == 0)
		return -1;

	len = vm_page_align_up(length);
	if (len == 0)
		return -1;

	stack_vma = vma_find_kind_const(proc, VMA_KIND_STACK);
	if (!stack_vma)
		return -1;

	low = vm_page_align_up(vma_brk_const(proc));
	if (low < VM_MMAP_MIN)
		low = VM_MMAP_MIN;
	high = stack_vma->start;
	if (high > VM_STACK_GUARD_GAP)
		high -= VM_STACK_GUARD_GAP;
	if (low >= high || len > high - low)
		return -1;

	if (hint != 0 && (hint & (PAGE_SIZE - 1u)) == 0 && hint >= low &&
	    hint + len > hint && hint + len <= high &&
	    vma_range_is_free(proc, hint, hint + len)) {
		*addr_out = hint;
		return 0;
	}

	scan_end = high;
	{
		const vm_area_t *vmas = vma_table_const(proc);
		uint32_t count = vma_count_const(proc);
		for (uint32_t i = count; i > 0; i--) {
			const vm_area_t *vma = &vmas[i - 1];
			uint32_t gap_start;

			if (vma->start >= scan_end)
				continue;

			gap_start = vma->end > low ? vm_page_align_up(vma->end) : low;
			if (scan_end > gap_start && scan_end - gap_start >= len) {
				uint32_t start = scan_end - len;
				*addr_out = start;
				return 0;
			}

			scan_end = vm_page_align_down(vma->start);
		}
	}

	if (scan_end > low && scan_end - low >= len) {
		uint32_t start = scan_end - len;
		*addr_out = start;
		return 0;
	}

	return -1;
}

int vma_map_anonymous(struct process *proc,
                      uint32_t hint,
                      uint32_t length,
                      uint32_t flags,
                      uint32_t *addr_out)
{
	uint32_t start;
	uint32_t len;

	if (vma_find_topdown_gap(proc, hint, length, &start) != 0)
		return -1;

	len = vm_page_align_up(length);
	if (vma_add(proc, start, start + len, flags, VMA_KIND_GENERIC) != 0)
		return -1;

	*addr_out = start;
	return 0;
}

int vma_map_file_private(struct process *proc,
                         uint32_t hint,
                         uint32_t length,
                         uint32_t flags,
                         vfs_file_ref_t file_ref,
                         uint32_t file_offset,
                         uint32_t file_size,
                         uint32_t *addr_out)
{
	vm_area_t area;
	uint32_t start;
	uint32_t len;

	if (vma_find_topdown_gap(proc, hint, length, &start) != 0)
		return -1;

	len = vm_page_align_up(length);
	k_memset(&area, 0, sizeof(area));
	area.start = start;
	area.end = start + len;
	area.flags = flags;
	area.kind = VMA_KIND_GENERIC;
	area.file_offset = file_offset;
	area.file_size = file_size;
	area.file_ref = file_ref;

	if (vma_insert_area(proc, &area) != 0)
		return -1;

	*addr_out = start;
	return 0;
}

int vma_unmap_range(struct process *proc, uint32_t start, uint32_t end)
{
	vm_area_t rebuilt[PROCESS_MAX_VMAS];
	vm_area_t *vmas;
	uint32_t *count;
	uint32_t rebuilt_count = 0;

	if (!proc || start >= end)
		return -1;

	vmas = vma_table(proc);
	count = vma_count_ptr(proc);
	k_memset(rebuilt, 0, sizeof(rebuilt));

	for (uint32_t i = 0; i < *count; i++) {
		const vm_area_t *vma = &vmas[i];
		uint32_t overlap_start;
		uint32_t overlap_end;

		if (!vma_overlaps(vma, start, end)) {
			if (vma_append_area(rebuilt, &rebuilt_count, vma) != 0)
				return -1;
			continue;
		}

		if (vma->kind != VMA_KIND_GENERIC)
			return -1;

		overlap_start = start > vma->start ? start : vma->start;
		overlap_end = end < vma->end ? end : vma->end;

		if (vma->start < overlap_start) {
			vm_area_t left = vma_slice(vma, vma->start, overlap_start);
			if (vma_append_area(rebuilt, &rebuilt_count, &left) != 0)
				return -1;
		}

		if (overlap_end < vma->end) {
			vm_area_t right = vma_slice(vma, overlap_end, vma->end);
			if (vma_append_area(rebuilt, &rebuilt_count, &right) != 0)
				return -1;
		}
	}

	k_memset(vmas, 0, sizeof(proc->vmas));
	k_memcpy(vmas, rebuilt, sizeof(rebuilt));
	*count = rebuilt_count;
	return 0;
}

int vma_protect_range(struct process *proc,
                      uint32_t start,
                      uint32_t end,
                      uint32_t new_flags)
{
	vm_area_t rebuilt[PROCESS_MAX_VMAS];
	vm_area_t *vmas;
	uint32_t *count;
	uint32_t rebuilt_count = 0;
	uint32_t cursor = start;

	if (!proc || start >= end)
		return -1;

	vmas = vma_table(proc);
	count = vma_count_ptr(proc);
	k_memset(rebuilt, 0, sizeof(rebuilt));

	for (uint32_t i = 0; i < *count; i++) {
		const vm_area_t *vma = &vmas[i];
		uint32_t overlap_start;
		uint32_t overlap_end;

		if (!vma_overlaps(vma, start, end)) {
			if (vma_append_area(rebuilt, &rebuilt_count, vma) != 0)
				return -1;
			continue;
		}

		if (vma->kind != VMA_KIND_GENERIC)
			return -1;

		overlap_start = start > vma->start ? start : vma->start;
		overlap_end = end < vma->end ? end : vma->end;
		if (overlap_start != cursor)
			return -1;

		if (vma->start < overlap_start) {
			vm_area_t left = vma_slice(vma, vma->start, overlap_start);
			if (vma_append_area(rebuilt, &rebuilt_count, &left) != 0)
				return -1;
		}

		{
			vm_area_t protected =
			    vma_slice(vma, overlap_start, overlap_end);
			protected.flags = vma_replace_prot_flags(vma->flags, new_flags);
			if (vma_append_area(rebuilt, &rebuilt_count, &protected) != 0)
				return -1;
		}

		if (overlap_end < vma->end) {
			vm_area_t right = vma_slice(vma, overlap_end, vma->end);
			if (vma_append_area(rebuilt, &rebuilt_count, &right) != 0)
				return -1;
		}

		cursor = overlap_end;
	}

	if (cursor != end)
		return -1;

	k_memset(vmas, 0, sizeof(proc->vmas));
	k_memcpy(vmas, rebuilt, sizeof(rebuilt));
	*count = rebuilt_count;
	return 0;
}
