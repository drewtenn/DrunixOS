/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_console.c - Drunix console control syscalls and scheduler yield.
 *
 * Contains Drunix-specific console control operations and the simple yield
 * syscall. General tty/ioctl behavior belongs in the tty syscall module.
 */

#include "syscall_internal.h"
#include "desktop.h"
#include "sched.h"
#include <limits.h>
#include <stdint.h>

extern void clear_screen(void);
extern void scroll_up(int n);
extern void scroll_down(int n);

static int syscall_scroll_count(uint32_t count)
{
	if (count > (uint32_t)INT_MAX)
		return INT_MAX;
	return (int)count;
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_clear(void)
{

	if (desktop_is_active() && desktop_clear_console(desktop_global()))
		return 0;
	clear_screen();
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_scroll_up(uint32_t ebx)
{

	if (desktop_is_active() &&
	    desktop_scroll_console(desktop_global(), syscall_scroll_count(ebx)))
		return 0;
	scroll_up(syscall_scroll_count(ebx));
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_scroll_down(uint32_t ebx)
{

	if (desktop_is_active() &&
	    desktop_scroll_console(desktop_global(), -syscall_scroll_count(ebx)))
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
