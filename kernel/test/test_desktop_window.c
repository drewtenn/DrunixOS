/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "desktop_window.h"

static void test_terminal_window_hit_tests_title_and_body(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(tc,
	                drunix_window_hit_test(64, 64, 656, 420, 20, 96, 72),
	                DRUNIX_WINDOW_HIT_TITLE);
	KTEST_EXPECT_EQ(tc,
	                drunix_window_hit_test(64, 64, 656, 420, 20, 96, 120),
	                DRUNIX_WINDOW_HIT_BODY);
	KTEST_EXPECT_EQ(tc,
	                drunix_window_hit_test(64, 64, 656, 420, 20, 20, 72),
	                DRUNIX_WINDOW_HIT_NONE);
}

static void test_terminal_window_hit_tests_controls(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(tc,
	                drunix_window_hit_test(64, 64, 656, 420, 20, 705, 74),
	                DRUNIX_WINDOW_HIT_CLOSE);
	KTEST_EXPECT_EQ(tc,
	                drunix_window_hit_test(64, 64, 656, 420, 20, 685, 74),
	                DRUNIX_WINDOW_HIT_MINIMIZE);
	KTEST_EXPECT_EQ(tc,
	                drunix_window_hit_test(64, 64, 656, 420, 20, 705, 92),
	                DRUNIX_WINDOW_HIT_BODY);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_terminal_window_hit_tests_title_and_body),
    KTEST_CASE(test_terminal_window_hit_tests_controls),
};

static ktest_suite_t suite = KTEST_SUITE("desktop_window", cases);

ktest_suite_t *ktest_suite_desktop_window(void)
{
	return &suite;
}
