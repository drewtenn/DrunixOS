/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PROC_FRAME_H
#define KERNEL_ARCH_ARM64_PROC_FRAME_H

#include <stdint.h>

typedef struct arch_trap_frame {
	uint64_t regs[34];
} arch_trap_frame_t;

#endif
