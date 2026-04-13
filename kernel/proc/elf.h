/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* ELF magic: bytes 0–3 of e_ident, read as a little-endian uint32 */
#define ELF_MAGIC   0x464C457Fu   /* "\x7FELF" */

/* e_type values */
#define ET_REL      1   /* relocatable object (.o) */
#define ET_EXEC     2   /* executable file */
#define ET_CORE     4   /* core dump file */

/* e_machine values */
#define EM_386      3   /* Intel 80386 */

/* p_type values */
#define PT_LOAD     1   /* loadable segment */
#define PT_NOTE     4   /* auxiliary information */

/* p_flags bits */
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

/* Core note types */
#define NT_PRSTATUS 1   /* process status / integer registers */

/* sh_type values (section header types) */
#define SHT_NULL      0   /* inactive */
#define SHT_PROGBITS  1   /* program data */
#define SHT_SYMTAB    2   /* symbol table */
#define SHT_STRTAB    3   /* string table */
#define SHT_REL       9   /* relocation entries without addends */
#define SHT_NOBITS    8   /* uninitialised data (BSS) */

/* sh_flags bits */
#define SHF_ALLOC   0x2   /* section occupies memory at runtime */

/* Special section index: undefined external symbol */
#define SHN_UNDEF   0

/* i386 relocation types (r_info low byte) */
#define R_386_32    1   /* S + A */
#define R_386_PC32  2   /* S + A - P */

/* Helpers to extract symbol index and relocation type from r_info */
#define ELF32_R_SYM(i)   ((i) >> 8)
#define ELF32_R_TYPE(i)  ((uint8_t)(i))

/* Symbol binding (high nibble of st_info) */
#define STB_LOCAL   0
#define STB_GLOBAL  1
#define ELF32_ST_BIND(i)  ((i) >> 4)

/*
 * ELF32 file header — 52 bytes.
 * The first 16 bytes (e_ident) encode the magic, class (32/64-bit),
 * data encoding (little/big-endian), and ELF format version.
 */
typedef struct {
    uint8_t  e_ident[16];   /* magic + class + encoding + version + padding */
    uint16_t e_type;        /* object type (ET_EXEC = 2) */
    uint16_t e_machine;     /* target architecture (EM_386 = 3) */
    uint32_t e_version;     /* ELF format version (always 1) */
    uint32_t e_entry;       /* entry point virtual address */
    uint32_t e_phoff;       /* file offset of first program header */
    uint32_t e_shoff;       /* file offset of first section header (unused) */
    uint32_t e_flags;       /* architecture-specific flags */
    uint16_t e_ehsize;      /* size of this header (52 bytes) */
    uint16_t e_phentsize;   /* size of one program header entry */
    uint16_t e_phnum;       /* number of program header entries */
    uint16_t e_shentsize;   /* size of one section header entry */
    uint16_t e_shnum;       /* number of section header entries */
    uint16_t e_shstrndx;    /* section name string table index */
} __attribute__((packed)) Elf32_Ehdr;

/*
 * ELF32 program header — 32 bytes.
 * One entry exists for each loadable segment in the binary.
 */
typedef struct {
    uint32_t p_type;    /* segment type (PT_LOAD = 1) */
    uint32_t p_offset;  /* offset of segment data in the file */
    uint32_t p_vaddr;   /* virtual address to load the segment at */
    uint32_t p_paddr;   /* physical address (ignored on x86) */
    uint32_t p_filesz;  /* size of segment data in the file */
    uint32_t p_memsz;   /* size of segment in memory (>= p_filesz; rest is zeroed) */
    uint32_t p_flags;   /* segment permission flags (PF_R, PF_W, PF_X) */
    uint32_t p_align;   /* required alignment (power of 2) */
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
 * elf_load: parse an ELF32 executable from disk via the DUFS inode and load
 * it into a process address space.
 *
 * inode_num:       DUFS inode number of the ELF binary.
 * pd_phys:         physical address of the target process page directory.
 * entry_out:       set to the ELF entry point virtual address on success.
 * image_start_out: set to the lowest mapped PT_LOAD virtual address.
 * heap_start_out:  set to the page-rounded end of the highest PT_LOAD segment
 *                  on success; this is the natural starting address for the
 *                  process heap (initial brk).
 *
 * For each PT_LOAD segment the function:
 *   1. Allocates physical pages via pmm_alloc_page().
 *   2. Maps them into pd_phys with PG_PRESENT | PG_WRITABLE | PG_USER.
 *   3. Writes the file data into those pages via the kernel's identity map
 *      (no need to switch page directories during loading).
 *   4. Zeroes the bss portion (p_memsz > p_filesz).
 *
 * Returns 0 on success, or a negative error code:
 *   -1  disk read error
 *   -2  bad ELF magic
 *   -3  not an executable (e_type != ET_EXEC)
 *   -4  wrong architecture (e_machine != EM_386)
 *   -5  no program headers
 *   -6  could not read program header
 *   -7  out of physical memory
 *   -8  paging_map_page failed (out of pages for new page table)
 *   -9  disk read error while copying segment data
 */
/*
 * ELF32 section header — 40 bytes.
 * Used by the module loader to locate sections in a relocatable object.
 */
typedef struct {
    uint32_t sh_name;       /* byte offset into shstrtab for the section name */
    uint32_t sh_type;       /* SHT_* */
    uint32_t sh_flags;      /* SHF_* */
    uint32_t sh_addr;       /* load address (0 for relocatable objects) */
    uint32_t sh_offset;     /* file offset of section data */
    uint32_t sh_size;       /* size in bytes */
    uint32_t sh_link;       /* section index of associated section */
    uint32_t sh_info;       /* supplemental info (varies by type) */
    uint32_t sh_addralign;  /* required alignment (power of 2) */
    uint32_t sh_entsize;    /* size of each fixed-size entry (0 if none) */
} __attribute__((packed)) Elf32_Shdr;

/*
 * ELF32 symbol table entry — 16 bytes.
 */
typedef struct {
    uint32_t st_name;   /* byte offset into the associated string table */
    uint32_t st_value;  /* symbol value (section-relative offset for locals) */
    uint32_t st_size;   /* size in bytes */
    uint8_t  st_info;   /* binding (high nibble) + type (low nibble) */
    uint8_t  st_other;  /* visibility (unused here) */
    uint16_t st_shndx;  /* section index; SHN_UNDEF = 0 means external */
} __attribute__((packed)) Elf32_Sym;

/*
 * ELF32 REL relocation entry — 8 bytes.
 * No addend; the implicit addend is read from the patch site.
 */
typedef struct {
    uint32_t r_offset;  /* section-relative byte offset of the location to patch */
    uint32_t r_info;    /* ELF32_R_SYM(r_info): symbol table index
                         * ELF32_R_TYPE(r_info): R_386_* relocation type */
} __attribute__((packed)) Elf32_Rel;

int elf_load(uint32_t inode_num, uint32_t pd_phys, uint32_t *entry_out,
             uint32_t *image_start_out, uint32_t *heap_start_out);

#endif
