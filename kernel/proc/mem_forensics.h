/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef MEM_FORENSICS_H
#define MEM_FORENSICS_H

#include "vma.h"
#include <stdint.h>

struct process;

/*
 * Region report capacity must cover every VMA plus the synthetic image
 * fallback used when a process has image_start/image_end but no image VMA.
 */
#define MEM_FORENSICS_MAX_REGIONS  (PROCESS_MAX_VMAS + 1u)

typedef enum {
    MEM_FORENSICS_REGION_IMAGE = 1,
    MEM_FORENSICS_REGION_HEAP  = 2,
    MEM_FORENSICS_REGION_STACK = 3,
    MEM_FORENSICS_REGION_MMAP  = 4,
} mem_forensics_region_kind_t;

typedef enum {
    MEM_FORENSICS_FAULT_NONE = 0,
    MEM_FORENSICS_FAULT_UNMAPPED,
    MEM_FORENSICS_FAULT_PROTECTION,
    MEM_FORENSICS_FAULT_COW_WRITE,
    MEM_FORENSICS_FAULT_STACK_LIMIT,
    MEM_FORENSICS_FAULT_LAZY_MISS,
    MEM_FORENSICS_FAULT_UNKNOWN,
} mem_forensics_fault_kind_t;

typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t kind;
    uint32_t prot_flags;
    uint32_t reserved_bytes;
    uint32_t mapped_bytes;
    char     label[16];
} mem_forensics_region_t;

typedef struct {
    uint32_t valid;
    uint32_t signum;
    uint32_t cr2;
    uint32_t eip;
    uint32_t vector;
    uint32_t error_code;
    uint32_t in_region;
    uint32_t classification;
} mem_forensics_fault_t;

typedef struct {
    mem_forensics_region_t regions[MEM_FORENSICS_MAX_REGIONS];
    uint32_t region_count;
    uint32_t total_reserved_bytes;
    uint32_t total_mapped_bytes;
    uint32_t image_reserved_bytes;
    uint32_t image_mapped_bytes;
    uint32_t heap_reserved_bytes;
    uint32_t heap_mapped_bytes;
    uint32_t stack_reserved_bytes;
    uint32_t stack_mapped_bytes;
    uint32_t mmap_reserved_bytes;
    uint32_t mmap_mapped_bytes;
    mem_forensics_fault_t fault;
} mem_forensics_report_t;

int mem_forensics_collect(const struct process *proc,
                          mem_forensics_report_t *out);
int mem_forensics_render_vmstat(const struct process *proc,
                                char *buf, uint32_t cap, uint32_t *size_out);
int mem_forensics_render_fault(const struct process *proc,
                               char *buf, uint32_t cap, uint32_t *size_out);
int mem_forensics_render_maps(const struct process *proc,
                              char *buf, uint32_t cap, uint32_t *size_out);

uint32_t mem_forensics_vmstat_note_size(void);
uint32_t mem_forensics_fault_note_size(void);

#endif
