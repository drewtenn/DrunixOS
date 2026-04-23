/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/*
 * x86-private IRQ dispatch registry.
 *
 * Hardware IRQ lines 0–15 are remapped by the 8259A PIC to vectors 32–47.
 * arch.c wraps this interface for the shared architecture boundary.
 */

#define IRQ_COUNT 16

typedef void (*irq_handler_fn)(void);

void irq_dispatch_init(void);
void irq_unmask(uint8_t irq_num);
void irq_mask(uint8_t irq_num);
void irq_set_pic_masks(uint8_t master_mask, uint8_t slave_mask);
void irq_apply_pic_masks(void);
void irq_register(uint8_t irq_num, irq_handler_fn fn);
void irq_dispatch(uint32_t vector, uint32_t error_code);

#endif
