/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * uart.c — BCM2835 mini-UART driver for the ARM64 bring-up path.
 */

#include "uart.h"
#include <stdint.h>

#define MMIO_BASE 0x3F000000UL
#define GPFSEL1 (*(volatile uint32_t *)(MMIO_BASE + 0x200004))
#define GPPUD (*(volatile uint32_t *)(MMIO_BASE + 0x200094))
#define GPPUDCLK0 (*(volatile uint32_t *)(MMIO_BASE + 0x200098))
#define AUX_ENABLE (*(volatile uint32_t *)(MMIO_BASE + 0x215004))
#define AUX_MU_IO (*(volatile uint32_t *)(MMIO_BASE + 0x215040))
#define AUX_MU_IER (*(volatile uint32_t *)(MMIO_BASE + 0x215044))
#define AUX_MU_IIR (*(volatile uint32_t *)(MMIO_BASE + 0x215048))
#define AUX_MU_LCR (*(volatile uint32_t *)(MMIO_BASE + 0x21504C))
#define AUX_MU_MCR (*(volatile uint32_t *)(MMIO_BASE + 0x215050))
#define AUX_MU_LSR (*(volatile uint32_t *)(MMIO_BASE + 0x215054))
#define AUX_MU_CNTL (*(volatile uint32_t *)(MMIO_BASE + 0x215060))
#define AUX_MU_BAUD (*(volatile uint32_t *)(MMIO_BASE + 0x215068))

static void uart_delay(void)
{
	for (volatile uint32_t i = 0; i < 150u; i++)
		__asm__ volatile("nop");
}

void uart_init(void)
{
	uint32_t selector;

	AUX_ENABLE |= 1u;
	AUX_MU_CNTL = 0u;
	AUX_MU_IER = 0u;
	AUX_MU_IIR = 0xC6u;
	AUX_MU_LCR = 3u;
	AUX_MU_MCR = 0u;
	AUX_MU_BAUD = 270u;

	selector = GPFSEL1;
	selector &= ~((7u << 12) | (7u << 15));
	selector |= (2u << 12) | (2u << 15);
	GPFSEL1 = selector;

	GPPUD = 0u;
	uart_delay();
	GPPUDCLK0 = (1u << 14) | (1u << 15);
	uart_delay();
	GPPUD = 0u;
	GPPUDCLK0 = 0u;

	AUX_MU_CNTL = 3u;
}

void uart_putc(char c)
{
	while ((AUX_MU_LSR & 0x20u) == 0u)
		;
	AUX_MU_IO = (uint32_t)(uint8_t)c;
}

void uart_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			uart_putc('\r');
		uart_putc(*s++);
	}
}

char uart_getc(void)
{
	while ((AUX_MU_LSR & 0x01u) == 0u)
		;
	return (char)(AUX_MU_IO & 0xFFu);
}
