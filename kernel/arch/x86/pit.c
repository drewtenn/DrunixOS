/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pit.c — PIT timer setup and IRQ0 tick delivery into the scheduler.
 */

#include "irq.h"
#include "sched.h"
#include "clock.h"
#include "pit.h"

static void pit_irq_handler(void)
{
	clock_tick();
	sched_tick();
}

void pit_init(void)
{
	irq_register(0, pit_irq_handler);
}
