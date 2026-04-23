/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * exceptions.c — minimal AArch64 exception handlers for Milestone 1 bring-up.
 */

#include "irq.h"
#include "uart.h"
#include "kprintf.h"
#include <stdint.h>

#define CORE0_IRQ_SOURCE (*(volatile uint32_t *)0x40000060u)
#define CNTPNSIRQ_BIT (1u << 1)

static volatile uint32_t g_spurious_irq_count;

static void arm64_halt_forever(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

static void uart_put_hex64(const char *label, uint64_t value)
{
	char line[64];
	uint32_t hi = (uint32_t)(value >> 32);
	uint32_t lo = (uint32_t)value;

	k_snprintf(line, sizeof(line), "%s=0x%08X%08X\n", label, hi, lo);
	uart_puts(line);
}

void arm64_sync_handler(uint64_t *frame)
{
	uint64_t esr;
	uint64_t elr;
	uint64_t far;

	(void)frame;

	__asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
	__asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
	__asm__ volatile("mrs %0, far_el1" : "=r"(far));

	uart_puts("sync exception\n");
	uart_put_hex64("ESR_EL1", esr);
	uart_put_hex64("ELR_EL1", elr);
	uart_put_hex64("FAR_EL1", far);
	arm64_halt_forever();
}

void arm64_irq_handler(uint64_t *frame)
{
	uint32_t source;

	(void)frame;

	source = CORE0_IRQ_SOURCE;
	if (source & CNTPNSIRQ_BIT) {
		arm64_irq_dispatch(ARM64_IRQ_LOCAL_TIMER);
		return;
	}

	g_spurious_irq_count++;
}

void arm64_fiq_handler(uint64_t *frame)
{
	(void)frame;
	uart_puts("fiq exception\n");
	arm64_halt_forever();
}

void arm64_serror_handler(uint64_t *frame)
{
	(void)frame;
	uart_puts("serror exception\n");
	arm64_halt_forever();
}
