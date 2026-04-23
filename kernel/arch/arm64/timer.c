/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * timer.c — ARM Generic Timer support for Milestone 1 bring-up.
 */

#include "irq.h"
#include "timer.h"
#include <stdint.h>

static uint64_t g_ticks_per_interval;
static volatile uint64_t g_tick_count;

static uint64_t cntfrq(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

static void cntp_tval_write(uint64_t value)
{
	__asm__ volatile("msr cntp_tval_el0, %0" : : "r"(value));
}

static void cntp_ctl_write(uint64_t value)
{
	__asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(value));
}

void arm64_timer_start(uint32_t hz)
{
	if (hz == 0)
		return;

	g_ticks_per_interval = cntfrq() / hz;
	g_tick_count = 0;
	cntp_tval_write(g_ticks_per_interval);
	cntp_ctl_write(1u);
}

void arm64_timer_init(uint32_t hz)
{
	arm64_irq_init();
	arm64_timer_start(hz);
}

void arm64_timer_irq(void)
{
	cntp_tval_write(g_ticks_per_interval);
	g_tick_count++;
}

uint64_t arm64_timer_ticks(void)
{
	return g_tick_count;
}
