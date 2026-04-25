/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * irq_table.h — small shared helpers for per-arch IRQ dispatch tables.
 *
 * The two arches both maintain a static array of handler function
 * pointers indexed by IRQ number; init zeros it, register bounds-checks
 * and stores, dispatch bounds-checks and calls.  Everything around the
 * table — PIC mask management on x86, BCM2836 timer routing on ARM64 —
 * is genuinely arch-specific and stays where it is.
 *
 * Static inline so each arch keeps its own table and pulls in no
 * external symbols; the actual hot-path call to the registered handler
 * is still a single indirect call.
 */

#ifndef KERNEL_LIB_IRQ_TABLE_H
#define KERNEL_LIB_IRQ_TABLE_H

#include <stdint.h>

typedef void (*irq_handler_generic_fn)(void);

static inline void irq_table_clear(irq_handler_generic_fn *table, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
		table[i] = 0;
}

static inline void irq_table_set(irq_handler_generic_fn *table,
                                 uint32_t count,
                                 uint32_t irq,
                                 irq_handler_generic_fn fn)
{
	if (irq < count)
		table[irq] = fn;
}

static inline irq_handler_generic_fn
irq_table_get(const irq_handler_generic_fn *table, uint32_t count, uint32_t irq)
{
	if (irq < count)
		return table[irq];
	return 0;
}

#endif
