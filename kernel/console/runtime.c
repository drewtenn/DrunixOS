/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * runtime.c — console-first routing between processes and the text console.
 *
 * The legacy text console is still the fallback when the OS boots without the
 * user-space desktop.  When a compositor claims the display, visible console
 * writes are suppressed so kernel logs and tty echo do not corrupt the
 * framebuffer.
 */

#include "runtime.h"
#include "arch.h"
#include "process.h"

static uint32_t g_display_owner_pid;

static int console_runtime_visible_console_enabled(void)
{
	return g_display_owner_pid == 0;
}

int console_runtime_clear(void)
{
	return console_runtime_visible_console_enabled() ? 0 : 1;
}

int console_runtime_scroll(int rows)
{
	(void)rows;
	return console_runtime_visible_console_enabled() ? 0 : 1;
}

int console_runtime_claim_display(uint32_t owner_pid)
{
	if (owner_pid == 0)
		return -1;
	if (g_display_owner_pid != 0 && g_display_owner_pid != owner_pid)
		return -1;
	g_display_owner_pid = owner_pid;
	return 0;
}

int console_runtime_release_display(uint32_t owner_pid)
{
	if (owner_pid == 0 || g_display_owner_pid != owner_pid)
		return -1;
	g_display_owner_pid = 0;
	return 0;
}

void console_runtime_release_process(const process_t *proc)
{
	if (proc && proc->pid == g_display_owner_pid)
		g_display_owner_pid = 0;
}

int console_runtime_display_claimed(void)
{
	return g_display_owner_pid != 0;
}

int console_runtime_legacy_input_enabled(void)
{
	return g_display_owner_pid == 0;
}

int console_runtime_write_feedback(const char *buf, uint32_t len)
{
	if (console_runtime_visible_console_enabled())
		arch_console_write(buf, len);
	return (int)len;
}

int console_runtime_write_kernel_output(const char *buf, uint32_t len)
{
	if (console_runtime_visible_console_enabled())
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
	if (console_runtime_visible_console_enabled())
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

#ifdef KTEST_ENABLED
void console_runtime_reset_for_test(void)
{
	g_display_owner_pid = 0;
}
#endif
