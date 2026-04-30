/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * timer.c — ARM Generic Timer support for Milestone 1 bring-up.
 */

#include "platform/platform.h"
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
	platform_irq_init();
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

/*
 * Wall-clock primitives backed by ARM's CNTPCT_EL0 / CNTFRQ_EL0
 * (introduced for M3.x to replace iteration-count polled-loop
 * timeouts in the virtio drivers — codex's M3.0 delivery review #5
 * flagged that iteration counts are host-speed-dependent and
 * should be wall-clock based once the timer subsystem stabilizes).
 *
 * arm64_timer_now_cycles() returns the current 64-bit physical
 * counter value. Monotonic; rolls over after ~9300 years at
 * 62.5 MHz so wraparound is not a concern at scale.
 *
 * arm64_timer_cntfrq_hz() returns the counter frequency in Hz.
 * QEMU virt reports 62.5 MHz; real hardware varies.
 *
 * Callers compute a deadline as
 *   deadline = arm64_timer_now_cycles() + ns * cntfrq / 1e9
 * and poll-loop on `arm64_timer_now_cycles() < deadline`.
 */
uint64_t arm64_timer_now_cycles(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));
	return value;
}

uint64_t arm64_timer_cntfrq_hz(void)
{
	return cntfrq();
}
