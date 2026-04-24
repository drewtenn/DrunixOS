/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_CONSOLE_TERMINAL_H
#define KERNEL_CONSOLE_TERMINAL_H

#include <stdint.h>

#define CONSOLE_TERMINAL_LINE_MAX 128u

typedef void (*console_terminal_write_fn)(const char *buf,
                                          uint32_t len,
                                          void *ctx);
typedef uint32_t (*console_terminal_read_metric_fn)(void *ctx);

typedef struct {
	console_terminal_write_fn write;
	console_terminal_read_metric_fn read_ticks;
	console_terminal_read_metric_fn read_uptime_seconds;
	console_terminal_read_metric_fn read_free_pages;
	void *ctx;
} console_terminal_host_t;

typedef struct {
	console_terminal_host_t host;
	char line[CONSOLE_TERMINAL_LINE_MAX];
	uint32_t line_len;
	uint32_t ignore_next_lf;
} console_terminal_t;

void console_terminal_init(console_terminal_t *term,
                           const console_terminal_host_t *host);
void console_terminal_start(console_terminal_t *term);
void console_terminal_handle_char(console_terminal_t *term, char ch);

#endif
