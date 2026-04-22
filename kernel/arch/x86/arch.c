/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch.c — Phase 1 shared architecture boundary adapters for x86.
 */

#include "../arch.h"
#include "clock.h"
#include "io.h"

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
