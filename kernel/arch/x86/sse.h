/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SSE_H
#define SSE_H

#include <stdint.h>

/*
 * sse_init - enable x87 FPU and SSE2.
 *
 * Sets CR0.MP, clears CR0.EM, sets CR4.OSFXSR and CR4.OSXMMEXCPT,
 * issues fninit, then captures a clean FXSAVE image into
 * sse_initial_fpu_state.  Must be called before any floating-point or
 * SSE instruction is executed.
 */
void sse_init(void);

/*
 * 512-byte FXSAVE image captured during sse_init() with all registers
 * zeroed and MXCSR=0x1F80.  process_create() copies this into every new
 * process_t.fpu_state so each process starts with a clean FPU context.
 */
extern uint8_t sse_initial_fpu_state[512];

#endif
