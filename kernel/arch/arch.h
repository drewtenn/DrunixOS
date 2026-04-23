/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARCH_H
#define KERNEL_ARCH_ARCH_H

#include <stdint.h>

typedef void (*arch_irq_handler_fn)(void);

uint32_t arch_time_unix_seconds(void);
uint32_t arch_time_uptime_ticks(void);
void arch_console_write(const char *buf, uint32_t len);
void arch_debug_write(const char *buf, uint32_t len);
void arch_irq_init(void);
void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn);
void arch_irq_mask(uint32_t irq);
void arch_irq_unmask(uint32_t irq);
void arch_timer_set_periodic_handler(arch_irq_handler_fn fn);
void arch_timer_start(uint32_t hz);
void arch_interrupts_enable(void);

#endif
