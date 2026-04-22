/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * irq.c — BCM2836 local interrupt routing for the AArch64 bring-up path.
 */

#include <stdint.h>

#define CORE0_TIMER_IRQCNTL (*(volatile uint32_t *)0x40000040u)
#define CNTPNSIRQ_BIT (1u << 1)

void arm64_irq_enable_timer(void)
{
	CORE0_TIMER_IRQCNTL = CNTPNSIRQ_BIT;
}
