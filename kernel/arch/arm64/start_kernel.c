/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * start_kernel.c — Milestone 1 AArch64 kernel entry point.
 */

#include "../arch.h"
#include "timer.h"
#include "uart.h"
#include "kprintf.h"
#include <stdint.h>

extern char vectors_el1[];

static volatile uint64_t g_heartbeat_ticks;

static uint64_t arm64_read_currentel(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
	return value;
}

static uint64_t arm64_read_cntfrq(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

static void arm64_heartbeat_tick(void)
{
	g_heartbeat_ticks++;
}

void arm64_start_kernel(void)
{
	char line[64];
	uint64_t last = 0;

	uart_init();
	__asm__ volatile("msr vbar_el1, %0" : : "r"(vectors_el1));
	__asm__ volatile("isb");

	uart_puts("Drunix AArch64 v0 - hello from EL1\n");
	arch_mm_init();

	k_snprintf(line,
	           sizeof(line),
	           "CurrentEL=0x%X (EL%u)\n",
	           (unsigned int)arm64_read_currentel(),
	           (unsigned int)(arm64_read_currentel() >> 2));
	uart_puts(line);

	k_snprintf(line,
	           sizeof(line),
	           "CNTFRQ_EL0=%uHz\n",
	           (unsigned int)arm64_read_cntfrq());
	uart_puts(line);

	arch_irq_init();
	arch_timer_set_periodic_handler(arm64_heartbeat_tick);
	arch_timer_start(10u);
	arch_interrupts_enable();

	for (;;) {
		uint64_t now;

		__asm__ volatile("wfi");
		now = g_heartbeat_ticks;
		while (last < now) {
			last++;
			k_snprintf(line, sizeof(line), "tick %u\n", (unsigned int)last);
			uart_puts(line);
		}
	}
}
