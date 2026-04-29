/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_FDT_H
#define KERNEL_ARCH_ARM64_FDT_H

#include <stdint.h>

/*
 * Read-only flattened-device-tree walker. Phase 1 M2.4a of
 * docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md (FR-002).
 *
 * Scope: header validation, /memory enumeration, /chosen/bootargs
 * read. No allocation, no mutation. Only the nodes M2.4 boot needs;
 * /cpus and /intc are not parsed because their attributes are
 * hardcoded by the existing GICv3 / generic-timer drivers.
 */

#define FDT_MAGIC 0xD00DFEEDu

/*
 * Maximum number of /memory ranges fdt_get_memory will report. The
 * QEMU virt machine reports one range; the API caps at four so a
 * future board with multiple memory banks does not need an API bump.
 */
#define FDT_MAX_MEMORY_RANGES 4u

/*
 * Set by boot.S before any C code runs. Zero if no FDT was passed.
 * The pointer is a guest-physical address; under MMU-off (the M2.4a
 * regime) it is also the kernel virtual.
 */
extern uint64_t g_fdt_blob_phys;

typedef struct fdt_memory_range {
	uint64_t base;
	uint64_t size;
} fdt_memory_range_t;

/*
 * Validate the FDT header at `fdt`. Returns 0 if magic, totalsize,
 * and version are sensible; -1 otherwise. Does not walk nodes.
 */
int fdt_validate(const void *fdt);

/*
 * Walk the root node looking for memory entries. Emits up to `max`
 * ranges into `out` and writes the count to `*count`. Returns 0 on
 * success, -1 on parse error or if no memory node is found.
 */
int fdt_get_memory(const void *fdt,
                   fdt_memory_range_t *out,
                   uint32_t max,
                   uint32_t *count);

/*
 * Return /chosen/bootargs as a pointer into the DTB string section,
 * or NULL if absent. The returned string is valid for the lifetime
 * of the FDT blob; callers must not mutate it.
 */
const char *fdt_get_chosen_bootargs(const void *fdt);

#endif /* KERNEL_ARCH_ARM64_FDT_H */
