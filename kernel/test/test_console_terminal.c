/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "console/terminal.h"
#include <stdint.h>

typedef struct {
	char buf[512];
	uint32_t len;
	uint32_t ticks;
	uint32_t uptime;
	uint32_t free_pages;
} console_terminal_test_host_t;

static void test_write(const char *buf, uint32_t len, void *ctx)
{
	console_terminal_test_host_t *host =
	    (console_terminal_test_host_t *)ctx;

	for (uint32_t i = 0; i < len && host->len + 1u < sizeof(host->buf); i++)
		host->buf[host->len++] = buf[i];
	host->buf[host->len] = '\0';
}

static uint32_t test_ticks(void *ctx)
{
	return ((console_terminal_test_host_t *)ctx)->ticks;
}

static uint32_t test_uptime(void *ctx)
{
	return ((console_terminal_test_host_t *)ctx)->uptime;
}

static uint32_t test_free_pages(void *ctx)
{
	return ((console_terminal_test_host_t *)ctx)->free_pages;
}

static int contains_text(const char *haystack, const char *needle)
{
	uint32_t i;
	uint32_t j;

	if (!needle[0])
		return 1;

	for (i = 0; haystack[i]; i++) {
		for (j = 0; needle[j] && haystack[i + j] == needle[j]; j++)
			;
		if (!needle[j])
			return 1;
	}
	return 0;
}

static void test_console_terminal_prints_banner_and_help(ktest_case_t *tc)
{
	console_terminal_test_host_t host = {.ticks = 42u,
	                                     .uptime = 7u,
	                                     .free_pages = 99u};
	console_terminal_host_t io = {
		.write = test_write,
		.read_ticks = test_ticks,
		.read_uptime_seconds = test_uptime,
		.read_free_pages = test_free_pages,
		.ctx = &host,
	};
	console_terminal_t term;

	console_terminal_init(&term, &io);
	console_terminal_start(&term);
	console_terminal_handle_char(&term, 'h');
	console_terminal_handle_char(&term, 'e');
	console_terminal_handle_char(&term, 'l');
	console_terminal_handle_char(&term, 'p');
	console_terminal_handle_char(&term, '\r');

	KTEST_EXPECT_TRUE(tc, contains_text(host.buf, "Drunix ARM64 console"));
	KTEST_EXPECT_TRUE(tc, contains_text(host.buf, "drunix> "));
	KTEST_EXPECT_TRUE(tc, contains_text(host.buf, "help"));
	KTEST_EXPECT_TRUE(tc, contains_text(host.buf, "ticks"));
	KTEST_EXPECT_TRUE(tc, contains_text(host.buf, "mem"));
}

static void test_console_terminal_echo_and_backspace(ktest_case_t *tc)
{
	console_terminal_test_host_t host = {.ticks = 5u,
	                                     .uptime = 3u,
	                                     .free_pages = 11u};
	console_terminal_host_t io = {
		.write = test_write,
		.read_ticks = test_ticks,
		.read_uptime_seconds = test_uptime,
		.read_free_pages = test_free_pages,
		.ctx = &host,
	};
	console_terminal_t term;

	console_terminal_init(&term, &io);
	console_terminal_start(&term);
	console_terminal_handle_char(&term, 'e');
	console_terminal_handle_char(&term, 'c');
	console_terminal_handle_char(&term, 'h');
	console_terminal_handle_char(&term, 'x');
	console_terminal_handle_char(&term, '\b');
	console_terminal_handle_char(&term, 'o');
	console_terminal_handle_char(&term, ' ');
	console_terminal_handle_char(&term, 'o');
	console_terminal_handle_char(&term, 'k');
	console_terminal_handle_char(&term, '\r');

	KTEST_EXPECT_TRUE(tc, contains_text(host.buf, "ok"));
	KTEST_EXPECT_TRUE(tc, contains_text(host.buf, "drunix> "));
}

static ktest_case_t cases[] = {
	KTEST_CASE(test_console_terminal_prints_banner_and_help),
	KTEST_CASE(test_console_terminal_echo_and_backspace),
};

static ktest_suite_t suite = KTEST_SUITE("console_terminal", cases);

ktest_suite_t *ktest_suite_console_terminal(void)
{
	return &suite;
}
