/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PROC_FRAME_H
#define KERNEL_ARCH_ARM64_PROC_FRAME_H

#include <stdint.h>

typedef struct arch_trap_frame {
	uint64_t x[31];
	uint64_t sp_el0;
	uint64_t elr_el1;
	uint64_t spsr_el1;
	uint64_t esr_el1;
	uint64_t far_el1;
} arch_trap_frame_t;

#endif
