/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IDT_H
#define IDT_H

/*
 * idt_init_early: build the IDT and load it with LIDT while interrupts are
 * still disabled. Safe to call as soon as the final GDT/TSS layout is live.
 */
void idt_init_early(void);

/*
 * interrupts_enable: finish hardware interrupt bring-up by remapping the PIC,
 * programming the PIT, and executing STI.
 */
void interrupts_enable(void);

#endif
