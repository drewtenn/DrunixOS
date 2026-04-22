/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/*
 * Wall-clock time, stored as seconds since the Unix epoch (UTC).
 *
 * clock_init() seeds the clock from the CMOS RTC during boot.  The PIT IRQ
 * path calls clock_tick() at SCHED_HZ so the value advances while the kernel
 * runs.
 */
void clock_init(void);
void clock_tick(void);
uint32_t clock_unix_time(void);
uint32_t clock_uptime_ticks(void);

#endif
