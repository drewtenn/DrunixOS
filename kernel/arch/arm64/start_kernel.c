/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * start_kernel.c — Milestone 1 AArch64 kernel entry point.
 */

#include "../arch.h"
#include "../../console/terminal.h"
#include "mm/pmm.h"
#include "timer.h"
#include "uart.h"
#include "kprintf.h"
#include <stdint.h>

extern char vectors_el1[];

static volatile uint64_t g_heartbeat_ticks;
static console_terminal_t g_console_terminal;

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

static void arm64_terminal_write(const char *buf, uint32_t len, void *ctx)
{
	(void)ctx;
	arch_console_write(buf, len);
}

static uint32_t arm64_terminal_ticks(void *ctx)
{
	(void)ctx;
	return (uint32_t)g_heartbeat_ticks;
}

static uint32_t arm64_terminal_uptime_seconds(void *ctx)
{
	(void)ctx;
	return (uint32_t)(g_heartbeat_ticks / 10u);
}

static uint32_t arm64_terminal_free_pages(void *ctx)
{
	(void)ctx;
	return pmm_free_page_count();
}

void arm64_start_kernel(void)
{
	char line[64];
	console_terminal_host_t host = {
		.write = arm64_terminal_write,
		.read_ticks = arm64_terminal_ticks,
		.read_uptime_seconds = arm64_terminal_uptime_seconds,
		.read_free_pages = arm64_terminal_free_pages,
		.ctx = 0,
	};

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
	console_terminal_init(&g_console_terminal, &host);
	console_terminal_start(&g_console_terminal);

	for (;;) {
		char ch;

		__asm__ volatile("wfi");
		while (uart_try_getc(&ch))
			console_terminal_handle_char(&g_console_terminal, ch);
	}
}
