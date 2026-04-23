/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ARM64_IRQ_H
#define ARM64_IRQ_H

#include <stdint.h>

#define ARM64_IRQ_LOCAL_TIMER 0u
#define ARM64_IRQ_COUNT 1u

typedef void (*arm64_irq_handler_fn)(void);

void arm64_irq_init(void);
void arm64_irq_register(uint32_t irq, arm64_irq_handler_fn fn);
void arm64_irq_dispatch(uint32_t irq);
void arm64_irq_enable(void);

#endif
