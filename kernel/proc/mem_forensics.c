/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "mem_forensics.h"

#include "arch.h"
#include "process.h"
#include "kprintf.h"
#include "kstring.h"
#include <stdarg.h>

typedef struct {
	char *buf;
	uint32_t cap;
	uint32_t len;
} render_buf_t;

#define PF_ERR_PRESENT 0x1u
#define PF_ERR_WRITE 0x2u

#define MEM_FORENSICS_VMSTAT_NOTE_CAP 512u
#define MEM_FORENSICS_FAULT_NOTE_CAP 512u

static int mem_forensics_vma_allows_access(const vm_area_t *vma, uint32_t err)
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

static uint32_t count_present_user_pages(arch_aspace_t aspace,
                                         uint32_t start,
                                         uint32_t end)
{
	uint32_t count = 0;

	if (!aspace || start >= end)
		return 0;

	for (uint32_t addr = start; addr < end; addr += 0x1000u) {
		arch_mm_mapping_t mapping;

		if (arch_mm_query(aspace, addr, &mapping) == 0 &&
		    (mapping.flags & (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER)) ==
		        (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER))
			count++;
	}

	return count;
}

static void mem_forensics_emitf(render_buf_t *rb, const char *fmt, ...)
{
	va_list ap;
	uint32_t avail = (rb->cap > rb->len) ? rb->cap - rb->len : 0;
	int n;

	va_start(ap, fmt);
	n = k_vsnprintf(avail ? rb->buf + rb->len : 0, avail, fmt, ap);
	va_end(ap);

	if (n > 0)
		rb->len += (uint32_t)n;
}

static uint32_t mem_forensics_rendered_size(uint32_t len, uint32_t cap)
{
	if (cap != 0 && len >= cap)
		return cap - 1u;
	return len;
}

static int mem_forensics_vma_is_file_backed(const vm_area_t *vma)
{
	return vma && vma->kind == VMA_KIND_GENERIC &&
	       (vma->flags & VMA_FLAG_ANON) == 0;
}

static uint32_t mem_forensics_committed_pages(const struct process *proc)
{
	if (!proc || !proc->as)
		return 0;
	return proc->as->committed_pages;
}

static const char *mem_forensics_image_label(const struct process *proc)
{
	if (!proc)
		return "[image]";
	return proc->name[0] ? proc->name : "[image]";
}

static const char *mem_forensics_label_for_vma(const struct process *proc,
                                               const vm_area_t *vma)
{
	if (!vma)
		return "";

	if (vma->kind == VMA_KIND_IMAGE)
		return mem_forensics_image_label(proc);
	if (vma->kind == VMA_KIND_HEAP)
		return "[heap]";
	if (vma->kind == VMA_KIND_STACK)
		return "[stack]";
	if (mem_forensics_vma_is_file_backed(vma))
		return "file";
	return "anon";
}

static const char *mem_forensics_label_for_range(const struct process *proc,
                                                 uint32_t start,
                                                 uint32_t end)
{
	const vm_area_t *vma;

	if (!proc)
		return "";

	vma = vma_find_const(proc, start);
	if (vma && end <= vma->end)
		return mem_forensics_label_for_vma(proc, vma);

	if (proc->image_start < proc->image_end && start < proc->image_end &&
	    end > proc->image_start)
		return mem_forensics_image_label(proc);

	if (proc->heap_start < proc->brk && start < proc->brk &&
	    end > proc->heap_start)
		return "[heap]";

	if (proc->stack_low_limit < USER_STACK_TOP && start < USER_STACK_TOP &&
	    end > proc->stack_low_limit)
		return "[stack]";

	return "";
}

static void mem_forensics_emit_map_region(render_buf_t *rb,
                                          const struct process *proc,
                                          uint32_t start,
                                          uint32_t end,
                                          uint32_t flags,
                                          const char *label)
{
	const char *perm = (flags & ARCH_MM_MAP_WRITE) ? "rw-p" : "r--p";

	if (!label)
		label = mem_forensics_label_for_range(proc, start, end);

	if (label[0] != '\0')
		mem_forensics_emitf(rb, "%08x-%08x %s %s\n", start, end, perm, label);
	else
		mem_forensics_emitf(rb, "%08x-%08x %s\n", start, end, perm);
}

static const char *mem_forensics_fault_name(uint32_t classification)
{
	switch (classification) {
	case MEM_FORENSICS_FAULT_NONE:
		return "none";
	case MEM_FORENSICS_FAULT_UNMAPPED:
		return "unmapped";
	case MEM_FORENSICS_FAULT_PROTECTION:
		return "protection";
	case MEM_FORENSICS_FAULT_COW_WRITE:
		return "cow-write";
	case MEM_FORENSICS_FAULT_STACK_LIMIT:
		return "stack-limit";
	case MEM_FORENSICS_FAULT_LAZY_MISS:
		return "lazy-miss";
	default:
		return "unknown";
	}
}

static const vm_area_t *mem_forensics_vma_table(const struct process *proc)
{
	if (!proc)
		return 0;
	return proc->as ? proc->as->vmas : proc->vmas;
}

static uint32_t mem_forensics_vma_count(const struct process *proc)
{
	if (!proc)
		return 0;
	if (proc->as)
		return proc->as->vmas ? proc->as->vma_count : 0;
	return proc->vma_count;
}

static uint32_t mem_forensics_fault_vector(const arch_trap_frame_t *frame)
{
	return arch_trap_frame_fault_vector(frame);
}

static uint32_t mem_forensics_fault_error_code(const arch_trap_frame_t *frame)
{
	return arch_trap_frame_fault_error_code(frame);
}

static uint32_t mem_forensics_fault_stack_pointer(const arch_trap_frame_t *frame)
{
	return arch_trap_frame_stack_pointer(frame);
}

static void mem_forensics_classify_fault(const struct process *proc,
                                         mem_forensics_report_t *out)
{
	const vm_area_t *vma;
	uint32_t err;
	uint32_t fault_page;
	uint32_t fault_addr32;
	uint32_t stack_ptr;
	arch_mm_mapping_t mapping;
	int mapped;

	if (!proc->crash.valid) {
		out->fault.valid = 0;
		out->fault.classification = MEM_FORENSICS_FAULT_NONE;
		return;
	}

	out->fault.valid = 1;
	out->fault.signum = proc->crash.signum;
	out->fault.cr2 = proc->crash.fault_addr;
	out->fault.eip = arch_trap_frame_ip(&proc->crash.frame);
	out->fault.vector = mem_forensics_fault_vector(&proc->crash.frame);
	out->fault.error_code = mem_forensics_fault_error_code(&proc->crash.frame);
	err = out->fault.error_code;
	out->fault.in_region = 0u;
	stack_ptr = mem_forensics_fault_stack_pointer(&proc->crash.frame);

	if (out->fault.vector != 14u) {
		out->fault.classification = MEM_FORENSICS_FAULT_UNKNOWN;
		return;
	}

	if ((proc->crash.fault_addr >> 32) != 0) {
		out->fault.classification = MEM_FORENSICS_FAULT_UNKNOWN;
		return;
	}

	fault_addr32 = (uint32_t)proc->crash.fault_addr;
	fault_page = fault_addr32 & ~0xFFFu;
	vma = vma_find_const(proc, fault_addr32);
	out->fault.in_region = vma ? 1u : 0u;

	if (!vma) {
		out->fault.classification = MEM_FORENSICS_FAULT_UNMAPPED;
		return;
	}

	if (!mem_forensics_vma_allows_access(vma, err)) {
		out->fault.classification = MEM_FORENSICS_FAULT_PROTECTION;
		return;
	}

	if (vma->kind == VMA_KIND_STACK) {
		uint32_t stack_slack = stack_ptr > 32u ? stack_ptr - 32u : 0u;

		if ((vma->flags & VMA_FLAG_GROWSDOWN) == 0 ||
		    fault_page < vma->start ||
		    fault_page >= proc->stack_low_limit ||
		    fault_addr32 < stack_slack || fault_addr32 >= vma->end) {
			out->fault.classification = MEM_FORENSICS_FAULT_STACK_LIMIT;
			return;
		}
	}

	mapped = arch_mm_query((arch_aspace_t)proc->pd_phys, fault_page, &mapping) ==
	             0 &&
	         (mapping.flags & (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER)) ==
	             (ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER);
	if (!mapped) {
		out->fault.classification = MEM_FORENSICS_FAULT_LAZY_MISS;
		return;
	}

	if ((err & PF_ERR_WRITE) != 0u && (mapping.flags & ARCH_MM_MAP_COW) != 0) {
		out->fault.classification = MEM_FORENSICS_FAULT_COW_WRITE;
		return;
	}

	out->fault.classification = MEM_FORENSICS_FAULT_PROTECTION;
}

static int mem_forensics_append_region(mem_forensics_report_t *out,
                                       arch_aspace_t aspace,
                                       uint32_t start,
                                       uint32_t end,
                                       uint32_t kind,
                                       uint32_t prot_flags,
                                       const char *label)
{
	mem_forensics_region_t *r;
	uint32_t reserved_bytes;
	uint32_t mapped_bytes;

	reserved_bytes = end - start;
	mapped_bytes = count_present_user_pages(aspace, start, end) * 0x1000u;

	if (out->region_count < MEM_FORENSICS_MAX_REGIONS) {
		r = &out->regions[out->region_count++];
		r->start = start;
		r->end = end;
		r->kind = kind;
		r->prot_flags = prot_flags;
		r->reserved_bytes = reserved_bytes;
		r->mapped_bytes = mapped_bytes;
		k_strncpy(r->label, label, sizeof(r->label) - 1);
		r->label[sizeof(r->label) - 1] = '\0';
	}

	out->total_reserved_bytes += reserved_bytes;
	out->total_mapped_bytes += mapped_bytes;
	if (kind == MEM_FORENSICS_REGION_IMAGE)
		out->image_reserved_bytes += reserved_bytes;
	else if (kind == MEM_FORENSICS_REGION_HEAP)
		out->heap_reserved_bytes += reserved_bytes;
	else if (kind == MEM_FORENSICS_REGION_STACK)
		out->stack_reserved_bytes += reserved_bytes;
	else
		out->mmap_reserved_bytes += reserved_bytes;

	if (kind == MEM_FORENSICS_REGION_IMAGE)
		out->image_mapped_bytes += mapped_bytes;
	else if (kind == MEM_FORENSICS_REGION_HEAP)
		out->heap_mapped_bytes += mapped_bytes;
	else if (kind == MEM_FORENSICS_REGION_STACK)
		out->stack_mapped_bytes += mapped_bytes;
	else
		out->mmap_mapped_bytes += mapped_bytes;

	return 0;
}

int mem_forensics_collect(const struct process *proc,
                          mem_forensics_report_t *out)
{
	arch_aspace_t aspace;
	const vm_area_t *vmas;
	uint32_t vma_count;

	if (!proc || !out)
		return -1;

	k_memset(out, 0, sizeof(*out));
	aspace = (arch_aspace_t)proc->pd_phys;
	vmas = mem_forensics_vma_table(proc);
	vma_count = mem_forensics_vma_count(proc);
	out->total_committed_bytes = mem_forensics_committed_pages(proc) * 0x1000u;

	if (proc->image_start < proc->image_end) {
		int image_seen = 0;

		for (uint32_t i = 0; i < vma_count; i++) {
			if (vmas[i].kind == VMA_KIND_IMAGE) {
				image_seen = 1;
				break;
			}
		}

		if (!image_seen) {
			if (mem_forensics_append_region(out,
			                                aspace,
			                                proc->image_start,
			                                proc->image_end,
			                                MEM_FORENSICS_REGION_IMAGE,
			                                0,
			                                "[image]") != 0)
				return -1;
		}
	}

	for (uint32_t i = 0; i < vma_count; i++) {
		const vm_area_t *vma = &vmas[i];
		uint32_t kind;
		const char *label;

		if (vma->end < vma->start)
			return -1;

		if (vma->kind == VMA_KIND_IMAGE) {
			kind = MEM_FORENSICS_REGION_IMAGE;
			label = mem_forensics_image_label(proc);
		} else if (vma->kind == VMA_KIND_HEAP) {
			kind = MEM_FORENSICS_REGION_HEAP;
			label = "[heap]";
		} else if (vma->kind == VMA_KIND_STACK) {
			kind = MEM_FORENSICS_REGION_STACK;
			label = "[stack]";
		} else {
			kind = MEM_FORENSICS_REGION_MMAP;
			label = mem_forensics_vma_is_file_backed(vma) ? "file" : "anon";
		}

		if (mem_forensics_append_region(out,
		                                aspace,
		                                vma->start,
		                                vma->end,
		                                kind,
		                                vma->flags,
		                                label) != 0)
			return -1;
	}

	mem_forensics_classify_fault(proc, out);

	return 0;
}

int mem_forensics_render_vmstat(const struct process *proc,
                                char *buf,
                                uint32_t cap,
                                uint32_t *size_out)
{
	mem_forensics_report_t report;
	render_buf_t rb = {buf, cap, 0};

	if (!size_out || mem_forensics_collect(proc, &report) != 0)
		return -1;

	mem_forensics_emitf(&rb, "Reserved:\t%u\n", report.total_reserved_bytes);
	mem_forensics_emitf(&rb, "Mapped:\t%u\n", report.total_mapped_bytes);
	mem_forensics_emitf(&rb, "Committed:\t%u\n", report.total_committed_bytes);
	mem_forensics_emitf(&rb,
	                    "Image:\t%u/%u\n",
	                    report.image_mapped_bytes,
	                    report.image_reserved_bytes);
	mem_forensics_emitf(&rb,
	                    "Heap:\t%u/%u\n",
	                    report.heap_mapped_bytes,
	                    report.heap_reserved_bytes);
	mem_forensics_emitf(&rb,
	                    "Stack:\t%u/%u\n",
	                    report.stack_mapped_bytes,
	                    report.stack_reserved_bytes);
	mem_forensics_emitf(&rb,
	                    "Anon:\t%u/%u\n",
	                    report.mmap_mapped_bytes,
	                    report.mmap_reserved_bytes);
	mem_forensics_emitf(&rb, "Regions:\t%u\n", report.region_count);
	*size_out = mem_forensics_rendered_size(rb.len, cap);
	return 0;
}

int mem_forensics_render_fault(const struct process *proc,
                               char *buf,
                               uint32_t cap,
                               uint32_t *size_out)
{
	mem_forensics_report_t report;
	render_buf_t rb = {buf, cap, 0};

	if (!size_out || mem_forensics_collect(proc, &report) != 0)
		return -1;

	if (!report.fault.valid) {
		mem_forensics_emitf(&rb, "State:\tnone\n");
		*size_out = mem_forensics_rendered_size(rb.len, cap);
		return 0;
	}

	mem_forensics_emitf(&rb, "Signal:\t%u\n", report.fault.signum);
	mem_forensics_emitf(&rb, "Tid:\t%u\n", proc->tid);
	mem_forensics_emitf(&rb, "Tgid:\t%u\n", proc->tgid);
	if ((report.fault.cr2 >> 32) != 0) {
		mem_forensics_emitf(&rb,
		                    "CR2:\t0x%08x%08x\n",
		                    (uint32_t)(report.fault.cr2 >> 32),
		                    (uint32_t)report.fault.cr2);
	} else {
		mem_forensics_emitf(&rb, "CR2:\t0x%08x\n", (uint32_t)report.fault.cr2);
	}
	mem_forensics_emitf(&rb, "EIP:\t0x%08x\n", report.fault.eip);
	mem_forensics_emitf(&rb, "Err:\t0x%08x\n", report.fault.error_code);
	mem_forensics_emitf(&rb,
	                    "Class:\t%s\n",
	                    mem_forensics_fault_name(report.fault.classification));
	*size_out = mem_forensics_rendered_size(rb.len, cap);
	return 0;
}

int mem_forensics_render_maps(const struct process *proc,
                              char *buf,
                              uint32_t cap,
                              uint32_t *size_out)
{
	render_buf_t rb = {buf, cap, 0};
	uint32_t region_start = 0;
	uint32_t region_end = 0;
	uint32_t region_flags = 0;
	const char *region_label = "";
	arch_aspace_t aspace;
	int have_region = 0;

	if (!proc || !size_out)
		return -1;

	if (proc->pd_phys == 0) {
		*size_out = 0;
		return 0;
	}

	aspace = (arch_aspace_t)proc->pd_phys;

	for (uint32_t vaddr = 0; vaddr < USER_STACK_TOP; vaddr += 0x1000u) {
		arch_mm_mapping_t mapping;
		uint32_t flags;
		const char *label;

		if (arch_mm_query(aspace, vaddr, &mapping) != 0 ||
		    (mapping.flags & ARCH_MM_MAP_USER) == 0) {
			if (have_region) {
				mem_forensics_emit_map_region(
				    &rb,
				    proc,
				    region_start,
				    region_end,
				    region_flags,
				    region_label);
				have_region = 0;
			}
			continue;
		}

		flags = mapping.flags & (ARCH_MM_MAP_WRITE | ARCH_MM_MAP_COW);
		label = mem_forensics_label_for_range(proc, vaddr, vaddr + 0x1000u);

		if (!have_region) {
			region_start = vaddr;
			region_end = vaddr + 0x1000u;
			region_flags = flags;
			region_label = label;
			have_region = 1;
			continue;
		}

		if (vaddr == region_end && flags == region_flags &&
		    k_strcmp(label, region_label) == 0) {
			region_end += 0x1000u;
			continue;
		}

		mem_forensics_emit_map_region(&rb,
		                              proc,
		                              region_start,
		                              region_end,
		                              region_flags,
		                              region_label);
		region_start = vaddr;
		region_end = vaddr + 0x1000u;
		region_flags = flags;
		region_label = label;
	}

	if (have_region) {
		mem_forensics_emit_map_region(&rb,
		                              proc,
		                              region_start,
		                              region_end,
		                              region_flags,
		                              region_label);
	}

	*size_out = mem_forensics_rendered_size(rb.len, cap);
	return 0;
}

uint32_t mem_forensics_vmstat_note_size(void)
{
	return MEM_FORENSICS_VMSTAT_NOTE_CAP;
}

uint32_t mem_forensics_fault_note_size(void)
{
	return MEM_FORENSICS_FAULT_NOTE_CAP;
}
