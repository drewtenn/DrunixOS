/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * runtime.c - console-first routing between the desktop shell and text console.
 */

#include "runtime.h"
#include "desktop.h"
#include "arch.h"
#include "process.h"
#include "tty.h"

static desktop_state_t *console_runtime_desktop(void)
{
	return desktop_is_active() ? desktop_global() : 0;
}

static int console_runtime_should_route_process_output(desktop_state_t *desktop,
                                                       const process_t *proc)
{
	uint32_t shell_pid;
	tty_t *tty;

	if (!desktop || !proc)
		return 0;
	if (desktop_process_owns_shell(desktop, proc->pid, proc->pgid))
		return 1;

	shell_pid = desktop_shell_pid(desktop);
	if (shell_pid == 0)
		return 0;
	if (proc->parent_pid == shell_pid)
		return 1;

	tty = tty_get((int)proc->tty_id);
	if (tty && tty->fg_pgid != 0 && tty->fg_pgid == proc->pgid)
		return 1;

	return 0;
}

int console_runtime_clear(void)
{
	desktop_state_t *desktop = console_runtime_desktop();

	return desktop && desktop_clear_console(desktop);
}

int console_runtime_scroll(int rows)
{
	desktop_state_t *desktop = console_runtime_desktop();

	return desktop && desktop_scroll_console(desktop, rows);
}

int console_runtime_write_feedback(const char *buf, uint32_t len)
{
	desktop_state_t *desktop = console_runtime_desktop();

	if (desktop && desktop_write_console_output(desktop, buf, len) == (int)len)
		return (int)len;

	arch_console_write(buf, len);
	return (int)len;
}

uintptr_t console_runtime_begin_process_output(const process_t *proc)
{
	desktop_state_t *desktop = console_runtime_desktop();

	if (desktop && console_runtime_should_route_process_output(desktop, proc)) {
		desktop_begin_console_batch(desktop);
		return (uintptr_t)desktop;
	}

	return 0;
}

void console_runtime_end_process_output(uintptr_t batch_token)
{
	if (batch_token != 0) {
		desktop_state_t *desktop = (desktop_state_t *)batch_token;
		desktop_end_console_batch(desktop);
	}
}

int console_runtime_write_process_output(const process_t *proc,
                                         const char *buf,
                                         uint32_t len)
{
	desktop_state_t *desktop = console_runtime_desktop();

	if (desktop && console_runtime_should_route_process_output(desktop, proc) &&
	    desktop_write_console_output(desktop, buf, len) == (int)len)
		return (int)len;

	arch_console_write(buf, len);
	return (int)len;
}

void console_runtime_winsize(uint16_t *rows_out, uint16_t *cols_out)
{
	desktop_state_t *desktop = console_runtime_desktop();
	uint16_t rows = 25u;
	uint16_t cols = 80u;

	if (desktop && desktop->shell_terminal.rows > 0 &&
	    desktop->shell_terminal.cols > 0) {
		rows = (uint16_t)desktop->shell_terminal.rows;
		cols = (uint16_t)desktop->shell_terminal.cols;
	}

	if (rows_out)
		*rows_out = rows;
	if (cols_out)
		*cols_out = cols;
}
