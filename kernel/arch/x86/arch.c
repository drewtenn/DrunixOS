/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch.c — Phase 1/2 shared architecture boundary adapters for x86.
 */

#include "../arch.h"
#include "clock.h"
#include "idt.h"
#include "io.h"
#include "irq.h"
#include "pit.h"

#define QEMU_DEBUG_PORT 0xE9

extern void print_bytes(const char *buf, int n);

uint32_t arch_time_unix_seconds(void)
{
	return clock_unix_time();
}

uint32_t arch_time_uptime_ticks(void)
{
	return clock_uptime_ticks();
}

void arch_console_write(const char *buf, uint32_t len)
{
	if (!buf || len == 0)
		return;

	print_bytes(buf, (int)len);
}

void arch_debug_write(const char *buf, uint32_t len)
{
	if (!buf || len == 0)
		return;

#ifdef KLOG_TO_DEBUGCON
	for (uint32_t i = 0; i < len; i++) {
		if (buf[i] == '\n')
			port_byte_out(QEMU_DEBUG_PORT, '\r');
		port_byte_out(QEMU_DEBUG_PORT, (unsigned char)buf[i]);
	}
#else
	(void)buf;
	(void)len;
#endif
}

void arch_irq_init(void)
{
	irq_dispatch_init();
	pit_init();
	irq_register(0, pit_handle_irq);
}

void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn)
{
	if (irq < IRQ_COUNT)
		irq_register((uint8_t)irq, fn);
}

void arch_irq_mask(uint32_t irq)
{
	if (irq < IRQ_COUNT)
		irq_mask((uint8_t)irq);
}

void arch_irq_unmask(uint32_t irq)
{
	if (irq < IRQ_COUNT)
		irq_unmask((uint8_t)irq);
}

void arch_timer_set_periodic_handler(arch_irq_handler_fn fn)
{
	pit_set_periodic_handler(fn);
}

void arch_timer_start(uint32_t hz)
{
	pit_start(hz);
}

void arch_interrupts_enable(void)
{
	interrupts_enable();
}
