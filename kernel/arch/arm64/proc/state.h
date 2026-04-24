/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PROC_STATE_H
#define KERNEL_ARCH_ARM64_PROC_STATE_H

#include <stdint.h>

typedef struct __attribute__((aligned(16))) arch_process_state {
	uintptr_t context;
	uint32_t user_tls_base;
	uint32_t user_tls_limit;
	uint32_t user_tls_limit_in_pages;
	uint32_t user_tls_present;
	uint8_t fpu_state[512] __attribute__((aligned(16)));
} arch_process_state_t;

#endif
