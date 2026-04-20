/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * vma.c — per-process virtual memory area bookkeeping for anonymous mappings.
 */

#include "vma.h"
#include "process.h"
#include "pmm.h"
#include "kstring.h"

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

static int vma_can_merge(const vm_area_t *lhs, const vm_area_t *rhs)
{
	if (!lhs || !rhs)
		return 0;

	return lhs->end == rhs->start && lhs->flags == rhs->flags &&
	       lhs->kind == rhs->kind;
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

static int vma_append(vm_area_t *dst,
                      uint32_t *count,
                      uint32_t start,
                      uint32_t end,
                      uint32_t flags,
                      uint32_t kind)
{
	if (!dst || !count || start > end)
		return -1;

	if (*count > 0 && dst[*count - 1].end == start &&
	    dst[*count - 1].flags == flags && dst[*count - 1].kind == kind) {
		dst[*count - 1].end = end;
		return 0;
	}

	if (*count >= PROCESS_MAX_VMAS)
		return -1;

	dst[*count].start = start;
	dst[*count].end = end;
	dst[*count].flags = flags;
	dst[*count].kind = kind;
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

int vma_add(struct process *proc,
            uint32_t start,
            uint32_t end,
            uint32_t flags,
            uint32_t kind)
{
	vm_area_t *vmas;
	uint32_t *count;
	uint32_t insert_at = 0;

	if (!proc || start > end)
		return -1;

	vmas = vma_table(proc);
	count = vma_count_ptr(proc);
	if (*count >= PROCESS_MAX_VMAS)
		return -1;

	while (insert_at < *count && vmas[insert_at].start <= start) {
		if (vma_overlaps(&vmas[insert_at], start, end))
			return -1;
		insert_at++;
	}

	if (insert_at < *count && vma_overlaps(&vmas[insert_at], start, end))
		return -1;

	if (insert_at < *count) {
		k_memmove(&vmas[insert_at + 1],
		          &vmas[insert_at],
		          (*count - insert_at) * sizeof(vmas[0]));
	}

	vmas[insert_at].start = start;
	vmas[insert_at].end = end;
	vmas[insert_at].flags = flags;
	vmas[insert_at].kind = kind;
	(*count)++;
	vma_coalesce(proc);
	return 0;
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

int vma_map_anonymous(struct process *proc,
                      uint32_t hint,
                      uint32_t length,
                      uint32_t flags,
                      uint32_t *addr_out)
{
	const vm_area_t *stack_vma;
	uint32_t len;
	uint32_t low;
	uint32_t high;
	uint32_t scan_end;

	if (!proc || !addr_out || length == 0)
		return -1;

	len = (length + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
	if (len == 0)
		return -1;

	stack_vma = vma_find_kind_const(proc, VMA_KIND_STACK);
	if (!stack_vma)
		return -1;

	low = (vma_brk_const(proc) + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
	if (low < USER_MMAP_MIN)
		low = USER_MMAP_MIN;
	high = stack_vma->start;
	if (low >= high || len > high - low)
		return -1;

	if (hint != 0 && (hint & (PAGE_SIZE - 1u)) == 0 && hint >= low &&
	    hint + len > hint && hint + len <= high &&
	    vma_range_is_free(proc, hint, hint + len)) {
		if (vma_add(proc, hint, hint + len, flags, VMA_KIND_GENERIC) != 0)
			return -1;
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

			gap_start = vma->end > low ? vma->end : low;
			if (scan_end > gap_start && scan_end - gap_start >= len) {
				uint32_t start = scan_end - len;
				if (vma_add(proc, start, scan_end, flags, VMA_KIND_GENERIC) !=
				    0)
					return -1;
				*addr_out = start;
				return 0;
			}

			scan_end = vma->start;
		}
	}

	if (scan_end > low && scan_end - low >= len) {
		uint32_t start = scan_end - len;
		if (vma_add(proc, start, scan_end, flags, VMA_KIND_GENERIC) != 0)
			return -1;
		*addr_out = start;
		return 0;
	}

	return -1;
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
			if (vma_append(rebuilt,
			               &rebuilt_count,
			               vma->start,
			               vma->end,
			               vma->flags,
			               vma->kind) != 0)
				return -1;
			continue;
		}

		if (vma->kind != VMA_KIND_GENERIC)
			return -1;

		overlap_start = start > vma->start ? start : vma->start;
		overlap_end = end < vma->end ? end : vma->end;

		if (vma->start < overlap_start && vma_append(rebuilt,
		                                             &rebuilt_count,
		                                             vma->start,
		                                             overlap_start,
		                                             vma->flags,
		                                             vma->kind) != 0)
			return -1;

		if (overlap_end < vma->end && vma_append(rebuilt,
		                                         &rebuilt_count,
		                                         overlap_end,
		                                         vma->end,
		                                         vma->flags,
		                                         vma->kind) != 0)
			return -1;
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
			if (vma_append(rebuilt,
			               &rebuilt_count,
			               vma->start,
			               vma->end,
			               vma->flags,
			               vma->kind) != 0)
				return -1;
			continue;
		}

		if (vma->kind != VMA_KIND_GENERIC)
			return -1;

		overlap_start = start > vma->start ? start : vma->start;
		overlap_end = end < vma->end ? end : vma->end;
		if (overlap_start != cursor)
			return -1;

		if (vma->start < overlap_start && vma_append(rebuilt,
		                                             &rebuilt_count,
		                                             vma->start,
		                                             overlap_start,
		                                             vma->flags,
		                                             vma->kind) != 0)
			return -1;

		if (vma_append(rebuilt,
		               &rebuilt_count,
		               overlap_start,
		               overlap_end,
		               new_flags,
		               vma->kind) != 0)
			return -1;

		if (overlap_end < vma->end && vma_append(rebuilt,
		                                         &rebuilt_count,
		                                         overlap_end,
		                                         vma->end,
		                                         vma->flags,
		                                         vma->kind) != 0)
			return -1;

		cursor = overlap_end;
	}

	if (cursor != end)
		return -1;

	k_memset(vmas, 0, sizeof(proc->vmas));
	k_memcpy(vmas, rebuilt, sizeof(rebuilt));
	*count = rebuilt_count;
	return 0;
}
