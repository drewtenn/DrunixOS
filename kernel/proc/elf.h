/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ELF_H
#define ELF_H

#include "arch.h"
#include "vfs.h"
#include <stdint.h>

/* ELF magic: bytes 0–3 of e_ident, read as a little-endian uint32 */
#define ELF_MAGIC 0x464C457Fu /* "\x7FELF" */

/* e_ident offsets */
#define EI_CLASS 4u

typedef enum {
	ELF_CLASS_NONE = 0,
	ELF_CLASS_32 = 1,
	ELF_CLASS_64 = 2,
} elf_class_t;

/* e_type values */
#define ET_REL 1  /* relocatable object (.o) */
#define ET_EXEC 2 /* executable file */
#define ET_CORE 4 /* core dump file */

/* e_machine values */
#define EM_386 3       /* Intel 80386 */
#define EM_AARCH64 183 /* AArch64 */

/* p_type values */
#define PT_LOAD 1 /* loadable segment */
#define PT_NOTE 4 /* auxiliary information */

/* p_flags bits */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* Core note types */
#define NT_PRSTATUS 1 /* process status / integer registers */

/* sh_type values (section header types) */
#define SHT_NULL 0     /* inactive */
#define SHT_PROGBITS 1 /* program data */
#define SHT_SYMTAB 2   /* symbol table */
#define SHT_STRTAB 3   /* string table */
#define SHT_REL 9      /* relocation entries without addends */
#define SHT_NOBITS 8   /* uninitialised data (BSS) */

/* sh_flags bits */
#define SHF_ALLOC 0x2 /* section occupies memory at runtime */

/* Special section index: undefined external symbol */
#define SHN_UNDEF 0

/* i386 relocation types (r_info low byte) */
#define R_386_32 1   /* S + A */
#define R_386_PC32 2 /* S + A - P */

/* Helpers to extract symbol index and relocation type from r_info */
#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((uint8_t)(i))

/* Symbol binding (high nibble of st_info) */
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define ELF32_ST_BIND(i) ((i) >> 4)

/*
 * ELF32 file header — 52 bytes.
 * The first 16 bytes (e_ident) encode the magic, class (32/64-bit),
 * data encoding (little/big-endian), and ELF format version.
 */
typedef struct {
	uint8_t e_ident[16];  /* magic + class + encoding + version + padding */
	uint16_t e_type;      /* object type (ET_EXEC = 2) */
	uint16_t e_machine;   /* target architecture (EM_386 = 3) */
	uint32_t e_version;   /* ELF format version (always 1) */
	uint32_t e_entry;     /* entry point virtual address */
	uint32_t e_phoff;     /* file offset of first program header */
	uint32_t e_shoff;     /* file offset of first section header (unused) */
	uint32_t e_flags;     /* architecture-specific flags */
	uint16_t e_ehsize;    /* size of this header (52 bytes) */
	uint16_t e_phentsize; /* size of one program header entry */
	uint16_t e_phnum;     /* number of program header entries */
	uint16_t e_shentsize; /* size of one section header entry */
	uint16_t e_shnum;     /* number of section header entries */
	uint16_t e_shstrndx;  /* section name string table index */
} __attribute__((packed)) Elf32_Ehdr;

/*
 * ELF32 program header — 32 bytes.
 * One entry exists for each loadable segment in the binary.
 */
typedef struct {
	uint32_t p_type;   /* segment type (PT_LOAD = 1) */
	uint32_t p_offset; /* offset of segment data in the file */
	uint32_t p_vaddr;  /* virtual address to load the segment at */
	uint32_t p_paddr;  /* physical address (ignored on x86) */
	uint32_t p_filesz; /* size of segment data in the file */
	uint32_t
	    p_memsz; /* size of segment in memory (>= p_filesz; rest is zeroed) */
	uint32_t p_flags; /* segment permission flags (PF_R, PF_W, PF_X) */
	uint32_t p_align; /* required alignment (power of 2) */
} __attribute__((packed)) Elf32_Phdr;

/*
 * ELF32 note header — 12 bytes.
 * The name and descriptor payload that follow are each padded to 4 bytes.
 */
typedef struct {
	uint32_t n_namesz;
	uint32_t n_descsz;
	uint32_t n_type;
} __attribute__((packed)) Elf32_Nhdr;

/*
 * ELF32 section header — 40 bytes.
 * Used by the module loader to locate sections in a relocatable object.
 */
typedef struct {
	uint32_t sh_name;      /* byte offset into shstrtab for the section name */
	uint32_t sh_type;      /* SHT_* */
	uint32_t sh_flags;     /* SHF_* */
	uint32_t sh_addr;      /* load address (0 for relocatable objects) */
	uint32_t sh_offset;    /* file offset of section data */
	uint32_t sh_size;      /* size in bytes */
	uint32_t sh_link;      /* section index of associated section */
	uint32_t sh_info;      /* supplemental info (varies by type) */
	uint32_t sh_addralign; /* required alignment (power of 2) */
	uint32_t sh_entsize;   /* size of each fixed-size entry (0 if none) */
} __attribute__((packed)) Elf32_Shdr;

/*
 * ELF32 symbol table entry — 16 bytes.
 */
typedef struct {
	uint32_t st_name;  /* byte offset into the associated string table */
	uint32_t st_value; /* symbol value (section-relative offset for locals) */
	uint32_t st_size;  /* size in bytes */
	uint8_t st_info;   /* binding (high nibble) + type (low nibble) */
	uint8_t st_other;  /* visibility (unused here) */
	uint16_t st_shndx; /* section index; SHN_UNDEF = 0 means external */
} __attribute__((packed)) Elf32_Sym;

/*
 * ELF32 REL relocation entry — 8 bytes.
 * No addend; the implicit addend is read from the patch site.
 */
typedef struct {
	uint32_t
	    r_offset;    /* section-relative byte offset of the location to patch */
	uint32_t r_info; /* ELF32_R_SYM(r_info): symbol table index
                         * ELF32_R_TYPE(r_info): R_386_* relocation type */
} __attribute__((packed)) Elf32_Rel;

int arch_elf_machine_supported(elf_class_t elf_class, uint16_t machine);
int arch_elf_load_user_image(vfs_file_ref_t file_ref,
                             arch_aspace_t aspace,
                             uintptr_t *entry_out,
                             uintptr_t *image_start_out,
                             uintptr_t *heap_start_out);

int elf_load_file(vfs_file_ref_t file_ref,
                  arch_aspace_t aspace,
                  uintptr_t *entry_out,
                  uintptr_t *image_start_out,
                  uintptr_t *heap_start_out);

#endif
