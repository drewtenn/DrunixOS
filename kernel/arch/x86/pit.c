/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pit.c — PIT timer programming plus periodic tick delivery.
 */

#include "clock.h"
#include "io.h"
#include "irq.h"
#include "pit.h"
#include "sched.h"
#include <stdint.h>

#define PIT_INPUT_HZ 1193182u
#define PIT_MIN_DIVISOR 1u
#define PIT_MAX_DIVISOR 65536u
#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_MODE_RATE_GENERATOR 0x34u

static pit_handler_fn g_pit_handler;

void pit_init(void)
{
	g_pit_handler = sched_tick;
	irq_register(0, pit_handle_irq);
	pit_start(SCHED_HZ);
}

void pit_set_periodic_handler(pit_handler_fn fn)
{
	g_pit_handler = fn;
}

void pit_start(uint32_t hz)
{
	uint32_t divisor;
	uint16_t reload_value;

	if (hz == 0)
		return;

	divisor = PIT_INPUT_HZ / hz;
	if (divisor < PIT_MIN_DIVISOR)
		divisor = PIT_MIN_DIVISOR;
	else if (divisor > PIT_MAX_DIVISOR)
		divisor = PIT_MAX_DIVISOR;

	reload_value = (divisor == PIT_MAX_DIVISOR) ? 0u : (uint16_t)divisor;

	port_byte_out(PIT_CMD_PORT, PIT_MODE_RATE_GENERATOR);
	port_byte_out(PIT_CH0_PORT, (uint8_t)(reload_value & 0xFFu));
	port_byte_out(PIT_CH0_PORT, (uint8_t)((reload_value >> 8) & 0xFFu));
}

void pit_handle_irq(void)
{
	clock_tick();
	if (g_pit_handler)
		g_pit_handler();
}
