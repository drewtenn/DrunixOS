/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_VIRT_UART_H
#define KERNEL_PLATFORM_VIRT_UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
char uart_getc(void);
int uart_try_getc(char *out);

#endif
