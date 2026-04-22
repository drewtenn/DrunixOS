/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARCH_H
#define KERNEL_ARCH_ARCH_H

#include <stdint.h>

uint32_t arch_time_unix_seconds(void);
uint32_t arch_time_uptime_ticks(void);
void arch_console_write(const char *buf, uint32_t len);
void arch_debug_write(const char *buf, uint32_t len);

#endif
