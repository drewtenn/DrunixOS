/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>
#include "tss.h"

/*
 * GDT selector values.
 * Selector = (index * 8) | RPL
 * RPL is the Requested Privilege Level in the low 2 bits.
 */
#define GDT_KERNEL_CS   0x08    /* index 1, ring 0 */
#define GDT_KERNEL_DS   0x10    /* index 2, ring 0 */
#define GDT_USER_CS     0x1B    /* index 3, ring 3 (0x18 | 3) */
#define GDT_USER_DS     0x23    /* index 4, ring 3 (0x20 | 3) */
#define GDT_TSS_SEG     0x28    /* index 5, ring 0 */
#define GDT_DF_TSS_SEG  0x30    /* index 6, ring 0 */

/* 8-byte GDT descriptor */
typedef struct {
    uint16_t limit_low;          /* bits 0–15 of segment limit */
    uint16_t base_low;           /* bits 0–15 of base address */
    uint8_t  base_mid;           /* bits 16–23 of base address */
    uint8_t  access;             /* present, DPL, S, type, accessed */
    uint8_t  limit_high_flags;   /* bits 4–7: flags; bits 0–3: limit[19:16] */
    uint8_t  base_high;          /* bits 24–31 of base address */
} __attribute__((packed)) gdt_entry_t;

/* Pointer structure loaded by LGDT */
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_register_t;

/*
 * gdt_init: build a 6-entry GDT (null, kcode, kdata, ucode, udata, tss),
 * initialize the TSS, and reload all segment registers via gdt_flush.
 * Must be called before idt_init_early() and before any ring-3 execution.
 */
void gdt_init(void);

/*
 * gdt_set_tss_esp0: update the kernel stack pointer stored in the TSS.
 * Call this before launching each user process so that INT 0x80 from
 * ring 3 switches to the correct kernel stack.
 */
void gdt_set_tss_esp0(uint32_t esp0);

/*
 * gdt_get_runtime_tss: return the main TSS currently loaded in TR. During a
 * double-fault task switch, the CPU writes the pre-fault register state into
 * this TSS, which the panic path can inspect for diagnostics.
 */
const tss_t *gdt_get_runtime_tss(void);

#endif
