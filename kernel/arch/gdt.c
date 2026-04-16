/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * gdt.c — installs the kernel GDT plus the runtime and double-fault TSS entries.
 */

#include "gdt.h"
#include "tss.h"
#include "kstring.h"
#include <stdint.h>

/* 7 descriptors: null, kernel/user segments, runtime TSS, double-fault TSS */
static gdt_entry_t  gdt[7];
static gdt_register_t gdtr;
static tss_t        tss;
static tss_t        df_tss;
static uint8_t      df_stack[4096] __attribute__((aligned(16)));

/* Defined in gdt_flush.asm */
extern void gdt_flush(uint32_t gdtr_ptr, uint32_t tss_selector);
extern void double_fault_task_entry(void);

static void gdt_set_entry(int idx,
                           uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t flags)
{
    gdt[idx].base_low         = (uint16_t)(base & 0xFFFF);
    gdt[idx].base_mid         = (uint8_t)((base >> 16) & 0xFF);
    gdt[idx].base_high        = (uint8_t)((base >> 24) & 0xFF);
    gdt[idx].limit_low        = (uint16_t)(limit & 0xFFFF);
    /* upper 4 bits of limit go in the lower nibble of limit_high_flags;
       upper nibble is the flags field (G, D/B, L, AVL) */
    gdt[idx].limit_high_flags = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt[idx].access           = access;
}

void gdt_init(void)
{
    /* 0: null descriptor — required by the spec */
    gdt_set_entry(0, 0, 0, 0, 0);

    /*
     * 1: kernel code — base 0, limit 4 GB, DPL=0, 32-bit, 4 KB granularity
     * access: present(0x80) | DPL=0(0x00) | S=1(0x10) | type=exec+read(0x0A) = 0x9A
     * flags: G=1(0x80) | D/B=1(0x40) = 0xC0
     */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);

    /* 2: kernel data — same range, DPL=0, data segment
     * access: present(0x80) | DPL=0(0x00) | S=1(0x10) | type=read+write(0x02) = 0x92
     */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);

    /* 3: user code — DPL=3
     * access: present(0x80) | DPL=3(0x60) | S=1(0x10) | type=exec+read(0x0A) = 0xFA
     */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);

    /* 4: user data — DPL=3
     * access: present(0x80) | DPL=3(0x60) | S=1(0x10) | type=read+write(0x02) = 0xF2
     */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);

    /*
     * 5: runtime TSS descriptor (system segment, S=0)
     * 6: dedicated double-fault TSS descriptor
     *
     * access: present(0x80) | DPL=0(0x00) | S=0 | type=available 32-bit TSS(0x09) = 0x89
     * flags: 0x00 — byte granularity (limit is in bytes, not 4 KB pages)
     */
    uint32_t tss_base  = (uint32_t)&tss;
    uint32_t tss_limit = (uint32_t)sizeof(tss_t) - 1;
    gdt_set_entry(5, tss_base, tss_limit, 0x89, 0x00);

    uint32_t df_tss_base  = (uint32_t)&df_tss;
    uint32_t df_tss_limit = (uint32_t)sizeof(tss_t) - 1;
    gdt_set_entry(6, df_tss_base, df_tss_limit, 0x89, 0x00);

    /* Initialize the runtime TSS — only the ring-3 stack-switch fields matter. */
    k_memset(&tss, 0, sizeof(tss));
    tss.ss0         = GDT_KERNEL_DS;   /* 0x10 */
    tss.esp0        = 0x90000;         /* placeholder; replaced with per-process kstack_top before any ring-3 → ring-0 transition */
    tss.iomap_base  = (uint16_t)sizeof(tss_t); /* beyond TSS end = deny all port I/O */

    /*
     * Dedicated double-fault TSS. The IDT task gate for #DF switches here
     * even if the current kernel stack is already unusable.
     */
    k_memset(&df_tss, 0, sizeof(df_tss));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(df_tss.cr3));
    df_tss.eip        = (uint32_t)double_fault_task_entry;
    df_tss.eflags     = 0x00000202u;
    df_tss.esp        = (uint32_t)(df_stack + sizeof(df_stack));
    df_tss.ebp        = df_tss.esp;
    df_tss.esp0       = df_tss.esp;
    df_tss.ss0        = GDT_KERNEL_DS;
    df_tss.cs         = GDT_KERNEL_CS;
    df_tss.ss         = GDT_KERNEL_DS;
    df_tss.ds         = GDT_KERNEL_DS;
    df_tss.es         = GDT_KERNEL_DS;
    df_tss.fs         = GDT_KERNEL_DS;
    df_tss.gs         = GDT_KERNEL_DS;
    df_tss.iomap_base = (uint16_t)sizeof(tss_t);

    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint32_t)&gdt[0];

    /* Reload segment registers and install TSS */
    gdt_flush((uint32_t)&gdtr, GDT_TSS_SEG);
}

void gdt_set_tss_esp0(uint32_t esp0)
{
    tss.esp0 = esp0;
}

const tss_t *gdt_get_runtime_tss(void)
{
    return &tss;
}
