/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * runtime.c — console-first routing between processes and the text console.
 *
 * Now that the desktop runs in user space, this file no longer routes
 * shell output through the in-kernel desktop.  All process output goes
 * straight to arch_console_write(); the function names are kept for
 * call-site stability across the syscall layer.
 */

#include "runtime.h"
#include "arch.h"
#include "process.h"

int console_runtime_clear(void)
{
	return 0;
}

int console_runtime_scroll(int rows)
{
	(void)rows;
	return 0;
}

int console_runtime_write_feedback(const char *buf, uint32_t len)
{
	arch_console_write(buf, len);
	return (int)len;
}

uintptr_t console_runtime_begin_process_output(const process_t *proc)
{
	(void)proc;
	return 0;
}

void console_runtime_end_process_output(uintptr_t batch_token)
{
	(void)batch_token;
}

int console_runtime_write_process_output(const process_t *proc,
                                         const char *buf,
                                         uint32_t len)
{
	(void)proc;
	arch_console_write(buf, len);
	return (int)len;
}

void console_runtime_winsize(uint16_t *rows_out, uint16_t *cols_out)
{
	if (rows_out)
		*rows_out = 25u;
	if (cols_out)
		*cols_out = 80u;
}
