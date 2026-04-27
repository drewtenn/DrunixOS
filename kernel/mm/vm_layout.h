/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef VM_LAYOUT_H
#define VM_LAYOUT_H

#include "arch_layout.h"
#include <stdint.h>

#define VM_TASK_SIZE ((uint32_t)ARCH_USER_VADDR_MAX)
#define VM_USER_BASE ((uint32_t)ARCH_USER_VADDR_MIN)
#define VM_USER_IMAGE_BASE ((uint32_t)ARCH_USER_IMAGE_BASE)
#define VM_STACK_TOP ((uint32_t)ARCH_USER_STACK_TOP)
#define VM_STACK_BASE ((uint32_t)ARCH_USER_STACK_BASE)
#define VM_HEAP_MAX ((uint32_t)ARCH_USER_HEAP_MAX)
#define VM_MMAP_MIN ((uint32_t)ARCH_USER_MMAP_MIN)

/*
 * Keep a gap between top-down mmap regions and the grow-down stack.
 * Linux has a larger configurable guard gap; Drunix starts with 1 MiB
 * because current process address spaces are small and testable.
 */
#define VM_STACK_GUARD_GAP (1024u * 1024u)

static inline uint32_t vm_page_align_down(uint32_t value)
{
	return value & ~(uint32_t)0xFFFu;
}

static inline uint32_t vm_page_align_up(uint32_t value)
{
	if (value > UINT32_MAX - 0xFFFu)
		return 0;
	return (value + 0xFFFu) & ~(uint32_t)0xFFFu;
}

#endif
