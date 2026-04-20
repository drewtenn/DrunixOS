/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef MODULE_H
#define MODULE_H

#include "vfs.h"
#include <stdint.h>

/*
 * Runtime loadable kernel modules.
 *
 * A module is an ELF32 relocatable object (ET_REL, produced by gcc -c) that
 * exports a symbol named "module_init".  module_load_file() reads the object from
 * disk, allocates kernel heap memory for its sections, applies R_386_32 and
 * R_386_PC32 relocations, resolves external symbols against the kernel export
 * table, and calls module_init().  The init function typically registers the
 * module's drivers with the blkdev, chardev, or VFS registries.
 *
 * Modules run in ring 0 with full kernel privilege.  There is no sandboxing.
 *
 * Unloading is not supported: loaded module code stays resident for the
 * lifetime of the kernel.
 */

#define MODULE_MAX_SIZE 65536u /* reject modules larger than 64 KB */
#define MODULE_MAX_LOADED 16u

/*
 * Every module must export a C function with this signature under the name
 * "module_init".  It is called once after relocation is complete.
 * Return 0 on success, negative on error.
 */
typedef int (*module_init_fn)(void);

/*
 * ksym_t: one entry in the kernel's compile-time symbol export table.
 * The table is defined in kernel/module_exports.c.
 */
typedef struct {
	const char *name;
	void *addr;
} ksym_t;

typedef struct {
	uint32_t loaded;
	char name[32];
	uint32_t base;
	uint32_t size;
} module_info_t;

extern const ksym_t kernel_exports[];
extern const uint32_t kernel_exports_count;

/*
 * module_load_file: load an ELF32 relocatable object from a VFS file
 * and execute its module_init function.
 *
 * file_ref:   mounted-file reference for the module binary.
 * size:       total size of the module binary in bytes.
 *
 * Returns 0 on success, or:
 *   -1  invalid ELF (bad magic, wrong type, wrong architecture)
 *   -2  relocation error (undefined symbol or unsupported relocation type)
 *   -3  out of memory (kmalloc failed)
 *   -4  module_init returned a non-zero error code
 *   -5  module too large (size > MODULE_MAX_SIZE)
 */
int module_load_file(const char *module_name,
                     vfs_file_ref_t file_ref,
                     uint32_t size);

/*
 * module_snapshot: copy currently loaded module metadata into `out`.
 *
 * Returns the number of entries written.
 */
int module_snapshot(module_info_t *out, uint32_t max);

#endif
