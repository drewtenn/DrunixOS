/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "console/terminal.h"
#include "kprintf.h"
#include "kstring.h"

static void console_terminal_write(console_terminal_t *term,
                                   const char *buf,
                                   uint32_t len)
{
	if (!term || !term->host.write || !buf || len == 0u)
		return;

	term->host.write(buf, len, term->host.ctx);
}

static void console_terminal_write_cstr(console_terminal_t *term, const char *s)
{
	console_terminal_write(term, s, (uint32_t)k_strlen(s));
}

static uint32_t console_terminal_metric(console_terminal_read_metric_fn fn,
                                        void *ctx)
{
	if (!fn)
		return 0u;
	return fn(ctx);
}

static void console_terminal_prompt(console_terminal_t *term)
{
	console_terminal_write_cstr(term, "drunix> ");
}

static int console_terminal_streq(const char *a, const char *b)
{
	uint32_t i = 0;

	while (a[i] && b[i]) {
		if (a[i] != b[i])
			return 0;
		i++;
	}
	return a[i] == b[i];
}

static int console_terminal_startswith(const char *s, const char *prefix)
{
	uint32_t i = 0;

	while (prefix[i]) {
		if (s[i] != prefix[i])
			return 0;
		i++;
	}
	return 1;
}

static void console_terminal_run_help(console_terminal_t *term)
{
	console_terminal_write_cstr(term, "commands: help clear echo ticks uptime mem\n");
}

static void console_terminal_run_ticks(console_terminal_t *term)
{
	char line[48];

	k_snprintf(line,
	           sizeof(line),
	           "ticks: %u\n",
	           (unsigned int)console_terminal_metric(term->host.read_ticks,
	                                                 term->host.ctx));
	console_terminal_write_cstr(term, line);
}

static void console_terminal_run_uptime(console_terminal_t *term)
{
	char line[48];

	k_snprintf(line,
	           sizeof(line),
	           "uptime: %us\n",
	           (unsigned int)console_terminal_metric(
	               term->host.read_uptime_seconds, term->host.ctx));
	console_terminal_write_cstr(term, line);
}

static void console_terminal_run_mem(console_terminal_t *term)
{
	char line[64];

	k_snprintf(line,
	           sizeof(line),
	           "free pages: %u\n",
	           (unsigned int)console_terminal_metric(
	               term->host.read_free_pages, term->host.ctx));
	console_terminal_write_cstr(term, line);
}

static void console_terminal_execute(console_terminal_t *term)
{
	char line[96];
	const char *arg = 0;

	term->line[term->line_len] = '\0';
	if (term->line_len == 0u)
		return;

	if (console_terminal_streq(term->line, "help")) {
		console_terminal_run_help(term);
		return;
	}

	if (console_terminal_streq(term->line, "clear")) {
		console_terminal_write_cstr(term, "\x1b[2J\x1b[H");
		return;
	}

	if (console_terminal_streq(term->line, "ticks")) {
		console_terminal_run_ticks(term);
		return;
	}

	if (console_terminal_streq(term->line, "uptime")) {
		console_terminal_run_uptime(term);
		return;
	}

	if (console_terminal_streq(term->line, "mem")) {
		console_terminal_run_mem(term);
		return;
	}

	if (console_terminal_startswith(term->line, "echo")) {
		arg = term->line + 4;
		if (*arg == ' ')
			arg++;
		console_terminal_write_cstr(term, arg);
		console_terminal_write_cstr(term, "\n");
		return;
	}

	k_snprintf(line, sizeof(line), "unknown command: %s\n", term->line);
	console_terminal_write_cstr(term, line);
}

void console_terminal_init(console_terminal_t *term,
                           const console_terminal_host_t *host)
{
	if (!term)
		return;

	k_memset(term, 0, sizeof(*term));
	if (host)
		term->host = *host;
}

void console_terminal_start(console_terminal_t *term)
{
	if (!term)
		return;

	console_terminal_write_cstr(term, "Drunix ARM64 console\n");
	console_terminal_write_cstr(term, "Type 'help' for commands.\n");
	console_terminal_prompt(term);
}

void console_terminal_handle_char(console_terminal_t *term, char ch)
{
	if (!term)
		return;

	if (term->ignore_next_lf && ch == '\n') {
		term->ignore_next_lf = 0u;
		return;
	}
	term->ignore_next_lf = 0u;

	if (ch == '\r' || ch == '\n') {
		if (ch == '\r')
			term->ignore_next_lf = 1u;
		console_terminal_write_cstr(term, "\n");
		console_terminal_execute(term);
		term->line_len = 0u;
		console_terminal_prompt(term);
		return;
	}

	if (ch == '\b' || ch == 0x7Fu) {
		if (term->line_len == 0u)
			return;
		term->line_len--;
		console_terminal_write_cstr(term, "\b \b");
		return;
	}

	if (ch < 32 || ch > 126)
		return;

	if (term->line_len + 1u >= CONSOLE_TERMINAL_LINE_MAX)
		return;

	term->line[term->line_len++] = ch;
	console_terminal_write(term, &ch, 1u);
}
