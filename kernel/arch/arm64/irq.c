/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * irq.c — BCM2836 local interrupt routing for the AArch64 bring-up path.
 */

#include "irq.h"
#include "timer.h"
#include <stdint.h>

#define CORE0_TIMER_IRQCNTL (*(volatile uint32_t *)0x40000040u)
#define CNTPNSIRQ_BIT (1u << 1)

static arm64_irq_handler_fn g_irq_table[ARM64_IRQ_COUNT];

void arm64_irq_init(void)
{
	for (uint32_t i = 0; i < ARM64_IRQ_COUNT; i++)
		g_irq_table[i] = 0;

	CORE0_TIMER_IRQCNTL = CNTPNSIRQ_BIT;
}

void arm64_irq_register(uint32_t irq, arm64_irq_handler_fn fn)
{
	if (irq < ARM64_IRQ_COUNT)
		g_irq_table[irq] = fn;
}

void arm64_irq_dispatch(uint32_t irq)
{
	if (irq >= ARM64_IRQ_COUNT)
		return;

	if (irq == ARM64_IRQ_LOCAL_TIMER)
		arm64_timer_irq();

	if (g_irq_table[irq])
		g_irq_table[irq]();
}

void arm64_irq_enable(void)
{
	__asm__ volatile("msr daifclr, #2");
}
