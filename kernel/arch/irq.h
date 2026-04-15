/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/*
 * IRQ dispatch registry.
 *
 * Hardware IRQ lines 0–15 are remapped by the 8259A PIC to vectors 32–47.
 * Drivers register a handler for their IRQ line number (0-based) once during
 * init.  The common IRQ trampoline in isr.asm calls irq_dispatch() with the
 * raw vector number; irq_dispatch converts it to a line index, invokes the
 * registered handler, and sends EOI to the PIC(s).
 *
 * Only one handler per line is supported — last registration wins.
 */

#define IRQ_COUNT 16    /* vectors 32–47 */

typedef void (*irq_handler_fn)(void);

/* Must be called once before any irq_register() or interrupts_enable() calls. */
void irq_dispatch_init(void);

/*
 * PIC mask helpers.  IRQ numbers are the same 0–15 hardware lines used by
 * irq_register().  irq_unmask() also clears the master cascade bit when an
 * IRQ on the slave PIC is enabled.
 */
void irq_unmask(uint8_t irq_num);
void irq_mask(uint8_t irq_num);
void irq_set_pic_masks(uint8_t master_mask, uint8_t slave_mask);
void irq_apply_pic_masks(void);

/*
 * Register handler fn for hardware IRQ line irq_num (0 = PIT timer,
 * 1 = PS/2 keyboard, …).  Passing NULL clears the registration.
 */
void irq_register(uint8_t irq_num, irq_handler_fn fn);

/*
 * Called from irq_common in isr.asm with the raw IDT vector number (32–47)
 * and the dummy error_code (always 0 for hardware IRQs).  Dispatches to the
 * registered handler and sends EOI.
 */
void irq_dispatch(uint32_t vector, uint32_t error_code);

#endif
