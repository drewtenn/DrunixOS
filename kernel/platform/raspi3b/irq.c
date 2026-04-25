/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * irq.c - BCM2836 local interrupt routing for Raspberry Pi 3.
 */

#include "../platform.h"
#include "irq.h"
#include "../../arch/arm64/timer.h"
#include "irq_table.h"
#include <stdint.h>

#define CORE0_TIMER_IRQCNTL (*(volatile uint32_t *)0x40000040u)
#define CORE0_IRQ_SOURCE (*(volatile uint32_t *)0x40000060u)
#define CNTPNSIRQ_BIT (1u << 1)

static irq_handler_generic_fn g_irq_table[ARM64_IRQ_COUNT];

void arm64_irq_init(void)
{
	irq_table_clear(g_irq_table, ARM64_IRQ_COUNT);
	CORE0_TIMER_IRQCNTL = CNTPNSIRQ_BIT;
}

void arm64_irq_register(uint32_t irq, arm64_irq_handler_fn fn)
{
	irq_table_set(g_irq_table, ARM64_IRQ_COUNT, irq, (irq_handler_generic_fn)fn);
}

void arm64_irq_dispatch(uint32_t irq)
{
	if (irq == ARM64_IRQ_LOCAL_TIMER)
		arm64_timer_irq();

	irq_handler_generic_fn fn =
	    irq_table_get(g_irq_table, ARM64_IRQ_COUNT, irq);
	if (fn)
		fn();
}

void arm64_irq_enable(void)
{
	__asm__ volatile("msr daifclr, #2");
}

void platform_irq_init(void)
{
	arm64_irq_init();
}

void platform_irq_register(uint32_t irq, platform_irq_handler_fn fn)
{
	arm64_irq_register(irq, (arm64_irq_handler_fn)fn);
}

int platform_irq_dispatch(void)
{
	uint32_t source = CORE0_IRQ_SOURCE;

	if ((source & CNTPNSIRQ_BIT) == 0u)
		return 0;

	arm64_irq_dispatch(ARM64_IRQ_LOCAL_TIMER);
	return 1;
}

void platform_irq_enable(void)
{
	arm64_irq_enable();
}
