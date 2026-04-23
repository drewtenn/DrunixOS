/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PIT_H
#define PIT_H

#include <stdint.h>

typedef void (*pit_handler_fn)(void);

void pit_init(void);
void pit_set_periodic_handler(pit_handler_fn fn);
void pit_start(uint32_t hz);
void pit_handle_irq(void);

#endif
