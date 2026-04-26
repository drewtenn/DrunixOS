/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_PLATFORM_H
#define KERNEL_PLATFORM_PLATFORM_H

#include "framebuffer.h"
#include <stdint.h>

#include "raspi3b/platform.h"

#define PLATFORM_IRQ_TIMER 0u
#define PLATFORM_IRQ_COUNT 1u

typedef void (*platform_irq_handler_fn)(void);

void platform_init(void);
void platform_uart_putc(char c);
void platform_uart_puts(const char *s);
char platform_uart_getc(void);
int platform_uart_try_getc(char *out);
void platform_irq_init(void);
void platform_irq_register(uint32_t irq, platform_irq_handler_fn fn);
int platform_irq_dispatch(void);
void platform_irq_enable(void);
int platform_framebuffer_acquire(framebuffer_info_t **out);
void platform_framebuffer_console_write(const char *buf, uint32_t len);
int platform_block_register(void);
int platform_usb_hci_register(void);
void platform_usb_hci_poll(void);

#endif
