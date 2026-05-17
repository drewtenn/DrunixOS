/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * uart.c - PL011 driver for Raspberry Pi 5.
 *
 * Pi 5 firmware (`enable_uart=1 uart_2ndstage=1` in config.txt) leaves
 * the primary debug UART programmed for 115200 8N1 before handoff, so
 * uart_init is intentionally a no-op — same pattern virt's PL011 driver
 * uses. Defensive re-init has historically wrecked diagnostics more
 * often than it helped; the cost of trusting firmware is one assumption,
 * and the benefit is that the very first banner byte appears on the
 * line whether the user is on the 40-pin GPIO header (RP1 UART0) or the
 * JST-SH header (uart10).
 *
 * The PL011 register offsets are identical between RP1 UART0 and the
 * SoC's uart10; only the MMIO base differs. PLATFORM_RASPI5_UART_BASE
 * resolves to RP1 UART0 by default and uart10 when the build sets
 * PLATFORM_RASPI5_UART_BASE=PLATFORM_RASPI5_UART10_BASE.
 */

#include "../platform.h"
#include "uart.h"
#include <stdint.h>

#define PL011_DR_OFFSET 0x00u
#define PL011_FR_OFFSET 0x18u

#define PL011_FR_RXFE (1u << 4)
#define PL011_FR_TXFF (1u << 5)

#define PL011_REG(off)                                                         \
	(*(volatile uint32_t *)(PLATFORM_RASPI5_UART_BASE + (off)))

void uart_init(void)
{
	/* Firmware-stage UART is already programmed. Leave it. */
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
	/* Order matches virt: bring UART up first so raspi5_ram_layout_init
	 * can print FDT diagnostics, then resolve the RAM layout. */
	uart_init();
	raspi5_ram_layout_init();
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
