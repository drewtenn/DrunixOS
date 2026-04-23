/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ARM64_TIMER_H
#define ARM64_TIMER_H

#include <stdint.h>

void arm64_timer_start(uint32_t hz);
void arm64_timer_init(uint32_t hz);
void arm64_timer_irq(void);
uint64_t arm64_timer_ticks(void);

#endif
