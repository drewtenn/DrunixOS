/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ARM64_TIMER_H
#define ARM64_TIMER_H

#include <stdint.h>

void arm64_timer_start(uint32_t hz);
void arm64_timer_init(uint32_t hz);
void arm64_timer_irq(void);
uint64_t arm64_timer_ticks(void);

/*
 * Wall-clock primitives for polled-loop bounded timeouts. Reads
 * CNTPCT_EL0 (current 64-bit physical count) and exposes the
 * counter's frequency via CNTFRQ_EL0.
 *
 * Use pattern:
 *   uint64_t freq = arm64_timer_cntfrq_hz();
 *   uint64_t deadline = arm64_timer_now_cycles() + ns * freq / 1000000000u;
 *   while (arm64_timer_now_cycles() < deadline) { ... if (done) break; }
 */
uint64_t arm64_timer_now_cycles(void);
uint64_t arm64_timer_cntfrq_hz(void);

#endif
