/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch.c — Phase 1 shared architecture boundary adapters for arm64.
 */

#include "../arch.h"
#include "timer.h"
#include "uart.h"

uint32_t arch_time_unix_seconds(void)
{
	return 0;
}

uint32_t arch_time_uptime_ticks(void)
{
	return (uint32_t)arm64_timer_ticks();
}

void arch_console_write(const char *buf, uint32_t len)
{
	if (!buf || len == 0)
		return;

	for (uint32_t i = 0; i < len; i++) {
		if (buf[i] == '\n')
			uart_putc('\r');
		uart_putc(buf[i]);
	}
}

void arch_debug_write(const char *buf, uint32_t len)
{
	arch_console_write(buf, len);
}
