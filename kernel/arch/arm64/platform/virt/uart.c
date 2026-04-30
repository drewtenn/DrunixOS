/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * uart.c - PL011 driver for the QEMU "virt" machine.
 *
 * QEMU's PL011 model needs no clock/baud/line-control programming to work
 * for stdio. The data register at offset 0x00 takes byte writes; FR (offset
 * 0x18) carries TXFF (transmit FIFO full, bit 5) and RXFE (receive FIFO
 * empty, bit 4). That is sufficient for poll-mode TX and RX.
 *
 * Real hardware needs a fuller init sequence (CR, LCR_H, IBRD/FBRD). When
 * Drunix eventually runs on a board with a real PL011, that init lives here.
 */

#include "../platform.h"
#include "uart.h"
#include <stdint.h>

#define PL011_DR_OFFSET 0x00u
#define PL011_FR_OFFSET 0x18u

#define PL011_FR_RXFE (1u << 4)
#define PL011_FR_TXFF (1u << 5)

#define PL011_REG(off)                                                         \
	(*(volatile uint32_t *)(PLATFORM_VIRT_PL011_BASE + (off)))

void uart_init(void)
{
	/* QEMU's PL011 emulation works without explicit init. Real hardware
	 * needs UARTCR/UARTLCR_H/IBRD/FBRD configured here. */
}

void uart_putc(char c)
{
	while ((PL011_REG(PL011_FR_OFFSET) & PL011_FR_TXFF) != 0u)
		;
	PL011_REG(PL011_DR_OFFSET) = (uint32_t)(uint8_t)c;
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
	while ((PL011_REG(PL011_FR_OFFSET) & PL011_FR_RXFE) != 0u)
		;
	return (char)(PL011_REG(PL011_DR_OFFSET) & 0xFFu);
}

int uart_try_getc(char *out)
{
	if (!out || (PL011_REG(PL011_FR_OFFSET) & PL011_FR_RXFE) != 0u)
		return 0;

	*out = (char)(PL011_REG(PL011_DR_OFFSET) & 0xFFu);
	return 1;
}

void platform_init(void)
{
	/* Order is fixed:
	 * 1. uart_init — UART must be up first; virt_ram_layout_init may
	 *    print FDT diagnostics and fallback warnings.
	 * 2. virt_ram_layout_init — FDT walk, populate the per-platform
	 *    RAM layout. Falls back to QEMU defaults if the FDT pointer
	 *    is zero or the blob fails validation.
	 * 3. arch_mm_init runs next from arm64_start_kernel and consumes
	 *    platform_ram_layout(). */
	uart_init();
	virt_ram_layout_init();
}

void platform_uart_putc(char c)
{
	uart_putc(c);
}

void platform_uart_puts(const char *s)
{
	uart_puts(s);
}

char platform_uart_getc(void)
{
	return uart_getc();
}

int platform_uart_try_getc(char *out)
{
	return uart_try_getc(out);
}
