/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * console.c - Drunix console control syscalls and scheduler yield.
 *
 * Contains Drunix-specific console control operations and the simple yield
 * syscall. General tty/ioctl behavior belongs in the tty syscall module.
 */

#include "syscall_internal.h"
#include "console/runtime.h"
#include "sched.h"
#include <limits.h>
#include <stdint.h>

void __attribute__((weak)) clear_screen(void)
{
}

void __attribute__((weak)) scroll_up(int n)
{
	(void)n;
}

void __attribute__((weak)) scroll_down(int n)
{
	(void)n;
}

static int syscall_scroll_count(uint32_t count)
{
	if (count > (uint32_t)INT_MAX)
		return INT_MAX;
	return (int)count;
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_clear(void)
{
	if (console_runtime_clear())
		return 0;
	clear_screen();
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_scroll_up(uint32_t ebx)
{
	if (console_runtime_scroll(syscall_scroll_count(ebx)))
		return 0;
	scroll_up(syscall_scroll_count(ebx));
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_scroll_down(uint32_t ebx)
{
	if (console_runtime_scroll(-syscall_scroll_count(ebx)))
		return 0;
	scroll_down(syscall_scroll_count(ebx));
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_yield(void)
{

	/* Voluntarily give up the rest of the current timeslice. */
	schedule();
	return 0;
}
