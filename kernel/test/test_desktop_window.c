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

static void test_taskbar_hit_tests_all_rendered_apps(ktest_case_t *tc)
{
	int screen_h = 600;
	int taskbar_h = 48;
	int y = screen_h - taskbar_h + 12;

	KTEST_EXPECT_EQ(tc,
	                drunix_taskbar_app_at(18, y, screen_h, taskbar_h),
	                DRUNIX_TASKBAR_APP_MENU);
	KTEST_EXPECT_EQ(tc,
	                drunix_taskbar_app_at(70, y, screen_h, taskbar_h),
	                DRUNIX_TASKBAR_APP_TERMINAL);
	KTEST_EXPECT_EQ(tc,
	                drunix_taskbar_app_at(110, y, screen_h, taskbar_h),
	                DRUNIX_TASKBAR_APP_FILES);
	KTEST_EXPECT_EQ(tc,
	                drunix_taskbar_app_at(150, y, screen_h, taskbar_h),
	                DRUNIX_TASKBAR_APP_PROCESSES);
	KTEST_EXPECT_EQ(tc,
	                drunix_taskbar_app_at(190, y, screen_h, taskbar_h),
	                DRUNIX_TASKBAR_APP_HELP);
	KTEST_EXPECT_EQ(tc,
	                drunix_taskbar_app_at(230, y, screen_h, taskbar_h),
	                DRUNIX_TASKBAR_APP_NONE);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_terminal_window_hit_tests_title_and_body),
    KTEST_CASE(test_terminal_window_hit_tests_controls),
    KTEST_CASE(test_taskbar_hit_tests_all_rendered_apps),
};

static ktest_suite_t suite = KTEST_SUITE("desktop_window", cases);

ktest_suite_t *ktest_suite_desktop_window(void)
{
	return &suite;
}
