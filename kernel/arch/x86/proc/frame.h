/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_X86_PROC_FRAME_H
#define KERNEL_ARCH_X86_PROC_FRAME_H

#include <stdint.h>

typedef struct __attribute__((packed)) arch_trap_frame {
	uint32_t gs, fs, es, ds;
	uint32_t edi, esi, ebp, esp_saved, ebx, edx, ecx, eax;
	uint32_t vector, error_code;
	uint32_t eip, cs, eflags;
	uint32_t user_esp, user_ss;
} arch_trap_frame_t;

#endif
