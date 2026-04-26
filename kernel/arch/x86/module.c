/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * module.c — relocatable kernel module loader and runtime symbol resolver.
 */

#include "module.h"
#include "elf.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include <stdint.h>

#define R_386_32 1
#define R_386_PC32 2
#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((uint8_t)(i))

typedef struct {
	uint32_t loaded;
	char name[32];
	uint32_t base;
	uint32_t size;
} loaded_module_t;

static loaded_module_t g_loaded_modules[MODULE_MAX_LOADED];

static void module_name_copy(char *dst, uint32_t dstsz, const char *src)
{
	const char *base;

	if (!dst || dstsz == 0)
		return;

	dst[0] = '\0';
	if (!src || src[0] == '\0')
		return;

	base = k_strrchr(src, '/');
	base = base ? base + 1 : src;
	k_strncpy(dst, base, dstsz - 1);
	dst[dstsz - 1] = '\0';
}

static void
module_record_loaded(const char *module_name, uint32_t base, uint32_t size)
{
	for (uint32_t i = 0; i < MODULE_MAX_LOADED; i++) {
		if (g_loaded_modules[i].loaded)
			continue;
		g_loaded_modules[i].loaded = 1;
		module_name_copy(g_loaded_modules[i].name,
		                 sizeof(g_loaded_modules[i].name),
		                 module_name);
		g_loaded_modules[i].base = base;
		g_loaded_modules[i].size = size;
		return;
	}

	klog("MOD", "loaded module table full");
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Read 'size' bytes from a VFS file at byte offset
 * 'file_off' into 'dst'.  Returns 0 on success, -1 on I/O error.
 */
static int
mod_read(vfs_file_ref_t file_ref, uint32_t file_off, void *dst, uint32_t size)
{
	int n = vfs_read(file_ref, file_off, (uint8_t *)dst, size);
	return (n == (int)size) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * module_load
 * ---------------------------------------------------------------------- */

/*
 * Per-section load information built during the loading pass.
 * Indexed by ELF section index.  load_addr is zero for sections that are
 * not loaded (type == SHT_NULL, or SHF_ALLOC not set).
 */
#define MAX_SECTIONS 64

int module_load_file(const char *module_name,
                     vfs_file_ref_t file_ref,
                     uint32_t size)
{
	/* ---- 0. Size guard ---- */
	if (size > MODULE_MAX_SIZE) {
		klog("MOD", "module too large");
		return -5;
	}

	/* ---- 1. Read and validate the ELF header ---- */
	Elf32_Ehdr ehdr;
	if (mod_read(file_ref, 0, &ehdr, sizeof(ehdr)) != 0)
		return -1;

	if (*(uint32_t *)ehdr.e_ident != ELF_MAGIC) {
		klog("MOD", "bad ELF magic");
		return -1;
	}
	if (ehdr.e_type != ET_REL) {
		klog("MOD", "not a relocatable object");
		return -1;
	}
	if (ehdr.e_machine != EM_386) {
		klog("MOD", "wrong architecture");
		return -1;
	}
	if (ehdr.e_shnum == 0) {
		klog("MOD", "no sections");
		return -1;
	}
	if (ehdr.e_shnum > MAX_SECTIONS) {
		klog("MOD", "too many sections");
		return -1;
	}

	uint32_t shnum = ehdr.e_shnum;

	/*
     * All heap allocations live here so the single `done:` label at the end
     * can free them unconditionally.  NULL pointers are safe to kfree because
     * kfree checks for NULL.
     */
	Elf32_Shdr *shdrs = (void *)0;
	uint8_t *base = (void *)0;
	Elf32_Sym *syms = (void *)0;
	char *strtab_buf = (void *)0;
	uint32_t *sym_resolved = (void *)0;
	int rc = -1;

	/* ---- 2. Read the section header table ---- */
	shdrs = (Elf32_Shdr *)kmalloc(shnum * sizeof(Elf32_Shdr));
	if (!shdrs) {
		rc = -3;
		goto done;
	}
	if (mod_read(file_ref, ehdr.e_shoff, shdrs, shnum * sizeof(Elf32_Shdr)) !=
	    0)
		goto done;

	/* ---- 3. Compute total allocation size for SHF_ALLOC sections ---- */
	uint32_t total = 0;
	for (uint32_t i = 0; i < shnum; i++) {
		if (!(shdrs[i].sh_flags & SHF_ALLOC))
			continue;
		uint32_t align = shdrs[i].sh_addralign;
		if (align > 1)
			total = (total + align - 1) & ~(align - 1);
		total += shdrs[i].sh_size;
	}
	if (total == 0) {
		klog("MOD", "no loadable sections");
		goto done;
	}

	/* ---- 4. Allocate load buffer ---- */
	base = (uint8_t *)kmalloc(total);
	if (!base) {
		klog("MOD", "kmalloc failed");
		rc = -3;
		goto done;
	}
	k_memset(base, 0, total);

	/* ---- 5. Load SHF_ALLOC sections; record each section's load address ---- */
	uint32_t section_addr[MAX_SECTIONS]; /* 64 * 4 = 256 B — safe on stack */
	for (uint32_t i = 0; i < shnum; i++)
		section_addr[i] = 0;

	uint32_t cursor = 0;
	for (uint32_t i = 0; i < shnum; i++) {
		if (!(shdrs[i].sh_flags & SHF_ALLOC))
			continue;
		uint32_t align = shdrs[i].sh_addralign;
		if (align > 1)
			cursor = (cursor + align - 1) & ~(align - 1);

		section_addr[i] = (uint32_t)(base + cursor);

		if (shdrs[i].sh_type == SHT_PROGBITS) {
			if (mod_read(file_ref,
			             shdrs[i].sh_offset,
			             base + cursor,
			             shdrs[i].sh_size) != 0)
				goto done;
		}
		/* SHT_NOBITS (BSS) is already zero-filled from k_memset above */
		cursor += shdrs[i].sh_size;
	}

	/* ---- 6. Find symbol table and its string table ---- */
	uint32_t symtab_idx = 0, strtab_idx = 0;
	for (uint32_t i = 0; i < shnum; i++) {
		if (shdrs[i].sh_type == SHT_SYMTAB) {
			symtab_idx = i;
			strtab_idx = shdrs[i].sh_link;
			break;
		}
	}
	if (symtab_idx == 0) {
		klog("MOD", "no symbol table");
		goto done;
	}

	uint32_t sym_count = shdrs[symtab_idx].sh_size / sizeof(Elf32_Sym);
	if (sym_count > 256) {
		klog("MOD", "too many symbols");
		goto done;
	}

	/* Read symbol table into heap buffer */
	syms = (Elf32_Sym *)kmalloc(sym_count * sizeof(Elf32_Sym));
	if (!syms) {
		rc = -3;
		goto done;
	}
	if (mod_read(file_ref,
	             shdrs[symtab_idx].sh_offset,
	             syms,
	             shdrs[symtab_idx].sh_size) != 0)
		goto done;

	/* Read string table into heap buffer */
	uint32_t strtab_sz = shdrs[strtab_idx].sh_size;
	if (strtab_sz > 65536u) {
		klog("MOD", "strtab too large");
		goto done;
	}
	strtab_buf = (char *)kmalloc(strtab_sz);
	if (!strtab_buf) {
		rc = -3;
		goto done;
	}
	if (mod_read(
	        file_ref, shdrs[strtab_idx].sh_offset, strtab_buf, strtab_sz) != 0)
		goto done;

	/* ---- 7. Resolve undefined (external) symbols against kernel_exports ---- */
	sym_resolved = (uint32_t *)kmalloc(sym_count * sizeof(uint32_t));
	if (!sym_resolved) {
		rc = -3;
		goto done;
	}
	for (uint32_t i = 0; i < sym_count; i++)
		sym_resolved[i] = 0;

	for (uint32_t i = 0; i < sym_count; i++) {
		if (syms[i].st_shndx != SHN_UNDEF) {
			/* Defined symbol: its address is section base + value offset */
			if (syms[i].st_shndx < shnum && section_addr[syms[i].st_shndx] != 0)
				sym_resolved[i] =
				    section_addr[syms[i].st_shndx] + syms[i].st_value;
			else
				sym_resolved[i] = syms[i].st_value;
			continue;
		}

		/* Undefined: must be in kernel_exports */
		const char *name = strtab_buf + syms[i].st_name;
		uint32_t found = 0;
		for (uint32_t k = 0; k < kernel_exports_count; k++) {
			if (k_strcmp(kernel_exports[k].name, name) == 0) {
				sym_resolved[i] = (uint32_t)kernel_exports[k].addr;
				found = 1;
				break;
			}
		}
		if (!found) {
			klog("MOD", "undefined symbol:");
			klog("MOD", name);
			rc = -2;
			goto done;
		}
	}

	/* ---- 8. Apply relocations ---- */
	for (uint32_t i = 0; i < shnum; i++) {
		if (shdrs[i].sh_type != SHT_REL)
			continue;

		/* The section this relocation table patches */
		uint32_t target_sec = shdrs[i].sh_info;
		if (target_sec >= shnum || section_addr[target_sec] == 0)
			continue;

		uint32_t rel_count = shdrs[i].sh_size / sizeof(Elf32_Rel);
		for (uint32_t r = 0; r < rel_count; r++) {
			Elf32_Rel rel;
			if (mod_read(file_ref,
			             shdrs[i].sh_offset + r * sizeof(Elf32_Rel),
			             &rel,
			             sizeof(rel)) != 0)
				goto done;

			uint32_t sym_idx = ELF32_R_SYM(rel.r_info);
			uint8_t rel_type = ELF32_R_TYPE(rel.r_info);
			uint32_t S = sym_resolved[sym_idx];
			uint32_t P = section_addr[target_sec] + rel.r_offset;
			uint32_t *patch = (uint32_t *)P;

			switch (rel_type) {
			case R_386_32:
				/* Absolute: *patch = S + A (A is already at *patch) */
				*patch += S;
				break;
			case R_386_PC32:
				/* PC-relative: *patch = S + A - P */
				*patch += S - P;
				break;
			default:
				klog_uint("MOD", "unsupported reloc type", rel_type);
				rc = -2;
				goto done;
			}
		}
	}

	/* ---- 9. Find and call module_init ---- */
	module_init_fn init_fn = 0;
	for (uint32_t i = 0; i < sym_count; i++) {
		if (syms[i].st_shndx == SHN_UNDEF)
			continue;
		const char *name = strtab_buf + syms[i].st_name;
		if (k_strcmp(name, "module_init") == 0) {
			init_fn = (module_init_fn)sym_resolved[i];
			break;
		}
	}
	if (!init_fn) {
		klog("MOD", "no module_init symbol");
		rc = -2;
		goto done;
	}

	rc = init_fn();
	if (rc != 0) {
		klog_uint("MOD", "module_init returned error", (uint32_t)(-rc));
		rc = -4;
		goto done;
	}

	klog("MOD", "module loaded");
	module_record_loaded(module_name, (uint32_t)base, total);
	rc = 0;

done:
	kfree(sym_resolved);
	kfree(strtab_buf);
	kfree(syms);
	/* base is intentionally NOT freed: the loaded module code lives here.
     * If loading failed (rc != 0), base is freed below. */
	if (rc != 0)
		kfree(base);
	kfree(shdrs);
	return rc;
}

int module_snapshot(module_info_t *out, uint32_t max)
{
	uint32_t count = 0;

	if (!out || max == 0)
		return 0;

	for (uint32_t i = 0; i < MODULE_MAX_LOADED && count < max; i++) {
		if (!g_loaded_modules[i].loaded)
			continue;
		out[count].loaded = 1;
		k_strncpy(out[count].name,
		          g_loaded_modules[i].name,
		          sizeof(out[count].name) - 1);
		out[count].name[sizeof(out[count].name) - 1] = '\0';
		out[count].base = g_loaded_modules[i].base;
		out[count].size = g_loaded_modules[i].size;
		count++;
	}

	return (int)count;
}
