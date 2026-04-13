/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE     4096u
#define PMM_MAX_PAGES 32768u   /* 128 MB / 4 KB */

/* ── Multiboot1 types ─────────────────────────────────────────────────────── */

/* Multiboot memory-map entry (Multiboot1 spec §3.3) */
typedef struct {
    uint32_t size;   /* size of this entry, NOT counting this field */
    uint64_t addr;
    uint64_t len;
    uint32_t type;   /* 1 = usable RAM */
} __attribute__((packed)) multiboot_mmap_entry_t;

/* Partial Multiboot1 info structure — only the fields we use */
typedef struct {
    uint32_t flags;         /* offset  0 */
    uint32_t mem_lower;     /* offset  4 — KB below 1 MB  (flags bit 0) */
    uint32_t mem_upper;     /* offset  8 — KB above 1 MB  (flags bit 0) */
    uint32_t boot_device;   /* offset 12                  (flags bit 1) */
    uint32_t cmdline;       /* offset 16                  (flags bit 2) */
    uint32_t mods_count;    /* offset 20                  (flags bit 3) */
    uint32_t mods_addr;     /* offset 24                  (flags bit 3) */
    uint8_t  syms[16];      /* offset 28 — ELF/aout info  (flags bit 4/5): 4 fields × 4 B = 16 B */
    uint32_t mmap_length;   /* offset 44                  (flags bit 6) */
    uint32_t mmap_addr;     /* offset 48                  (flags bit 6) */
} __attribute__((packed)) multiboot_info_t;

#define MULTIBOOT_FLAG_MMAP  (1u << 6)

/* ── PMM API ──────────────────────────────────────────────────────────────── */

void     pmm_init(multiboot_info_t *mbi);
void     pmm_mark_used(uint32_t base, uint32_t length);
void     pmm_mark_free(uint32_t base, uint32_t length);
uint32_t pmm_alloc_page(void);   /* returns physical address, 0 on failure */
void     pmm_incref(uint32_t phys_addr);
void     pmm_decref(uint32_t phys_addr);
void     pmm_free_page(uint32_t phys_addr);
uint8_t  pmm_refcount(uint32_t phys_addr);
uint32_t pmm_free_page_count(void);

#endif
