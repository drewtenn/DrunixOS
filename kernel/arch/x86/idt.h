/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IDT_H
#define IDT_H

/*
 * idt_init_early: build the IDT and load it with LIDT while interrupts are
 * still disabled. Safe to call as soon as the final GDT/TSS layout is live.
 */
void idt_init_early(void);

/*
 * x86-private final interrupt-enable hook. arch.c calls this after the shared
 * boundary has installed the timer callback and early device handlers.
 */
void interrupts_enable(void);

#endif
