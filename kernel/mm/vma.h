/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef VMA_H
#define VMA_H

#include "vfs.h"
#include <stdint.h>

struct process;

/* Stack-local process_t fallback capacity for tests without proc resources. */
#define PROCESS_MAX_VMAS 16u

#define VMA_FLAG_READ 0x01u
#define VMA_FLAG_WRITE 0x02u
#define VMA_FLAG_EXEC 0x04u
#define VMA_FLAG_ANON 0x08u
#define VMA_FLAG_PRIVATE 0x10u
#define VMA_FLAG_GROWSDOWN 0x20u

typedef enum {
	VMA_KIND_GENERIC = 0,
	VMA_KIND_HEAP = 1,
	VMA_KIND_STACK = 2,
	VMA_KIND_IMAGE = 3,
} vma_kind_t;

typedef struct {
	uint32_t start; /* inclusive */
	uint32_t end;   /* exclusive */
	uint32_t flags; /* VMA_FLAG_* */
	uint32_t kind;  /* vma_kind_t */
	uint32_t file_offset;
	uint32_t file_size;
	vfs_file_ref_t file_ref;
} vm_area_t;

void vma_init(struct process *proc);
int vma_add(struct process *proc,
            uint32_t start,
            uint32_t end,
            uint32_t flags,
            uint32_t kind);
vm_area_t *vma_find(struct process *proc, uint32_t addr);
const vm_area_t *vma_find_const(const struct process *proc, uint32_t addr);
vm_area_t *vma_find_kind(struct process *proc, uint32_t kind);
const vm_area_t *vma_find_kind_const(const struct process *proc, uint32_t kind);
int vma_map_anonymous(struct process *proc,
                      uint32_t hint,
                      uint32_t length,
                      uint32_t flags,
                      uint32_t *addr_out);
int vma_map_file_private(struct process *proc,
                         uint32_t hint,
                         uint32_t length,
                         uint32_t flags,
                         vfs_file_ref_t file_ref,
                         uint32_t file_offset,
                         uint32_t file_size,
                         uint32_t *addr_out);
int vma_unmap_range(struct process *proc, uint32_t start, uint32_t end);
int vma_protect_range(struct process *proc,
                      uint32_t start,
                      uint32_t end,
                      uint32_t new_flags);

#endif
