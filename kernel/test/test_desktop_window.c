/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "desktop_window.h"
#include "desktop_wallpaper.h"

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

static void test_desktop_rect_clip_rejects_empty_and_outside(ktest_case_t *tc)
{
	drunix_rect_t bounds = drunix_rect_make(0, 0, 100, 80);
	drunix_rect_t empty = drunix_rect_make(10, 10, 0, 5);
	drunix_rect_t outside = drunix_rect_make(120, 10, 20, 20);
	drunix_rect_t clipped;

	KTEST_EXPECT_FALSE(tc, drunix_rect_valid(empty));
	KTEST_EXPECT_FALSE(tc, drunix_rect_clip(empty, bounds, &clipped));
	KTEST_EXPECT_FALSE(tc, drunix_rect_clip(outside, bounds, &clipped));
}

static void test_desktop_rect_clip_trims_to_bounds(ktest_case_t *tc)
{
	drunix_rect_t bounds = drunix_rect_make(0, 0, 100, 80);
	drunix_rect_t rect = drunix_rect_make(-10, 70, 30, 20);
	drunix_rect_t clipped;

	KTEST_EXPECT_TRUE(tc, drunix_rect_clip(rect, bounds, &clipped));
	KTEST_EXPECT_EQ(tc, clipped.x, 0);
	KTEST_EXPECT_EQ(tc, clipped.y, 70);
	KTEST_EXPECT_EQ(tc, clipped.w, 20);
	KTEST_EXPECT_EQ(tc, clipped.h, 10);
}

static void test_desktop_rect_union_covers_both_inputs(ktest_case_t *tc)
{
	drunix_rect_t a = drunix_rect_make(10, 12, 20, 30);
	drunix_rect_t b = drunix_rect_make(25, 5, 40, 10);
	drunix_rect_t merged = drunix_rect_union(a, b);

	KTEST_EXPECT_EQ(tc, merged.x, 10);
	KTEST_EXPECT_EQ(tc, merged.y, 5);
	KTEST_EXPECT_EQ(tc, merged.w, 55);
	KTEST_EXPECT_EQ(tc, merged.h, 37);
}

static void test_wallpaper_cover_crops_square_source_vertically(ktest_case_t *tc)
{
	drunix_wallpaper_sample_t top_left =
	    drunix_wallpaper_cover_sample(0, 0, 1000, 500, 6000, 6000);
	drunix_wallpaper_sample_t center =
	    drunix_wallpaper_cover_sample(500, 250, 1000, 500, 6000, 6000);

	KTEST_EXPECT_EQ(tc, top_left.x, 0u);
	KTEST_EXPECT_EQ(tc, top_left.y, 1500u);
	KTEST_EXPECT_EQ(tc, center.x, 3000u);
	KTEST_EXPECT_EQ(tc, center.y, 3000u);
}

static void test_wallpaper_cover_crops_wide_source_horizontally(ktest_case_t *tc)
{
	drunix_wallpaper_sample_t top_left =
	    drunix_wallpaper_cover_sample(0, 0, 600, 600, 1200, 600);
	drunix_wallpaper_sample_t center =
	    drunix_wallpaper_cover_sample(300, 300, 600, 600, 1200, 600);

	KTEST_EXPECT_EQ(tc, top_left.x, 300u);
	KTEST_EXPECT_EQ(tc, top_left.y, 0u);
	KTEST_EXPECT_EQ(tc, center.x, 600u);
	KTEST_EXPECT_EQ(tc, center.y, 300u);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_terminal_window_hit_tests_title_and_body),
    KTEST_CASE(test_terminal_window_hit_tests_controls),
    KTEST_CASE(test_taskbar_hit_tests_all_rendered_apps),
    KTEST_CASE(test_desktop_rect_clip_rejects_empty_and_outside),
    KTEST_CASE(test_desktop_rect_clip_trims_to_bounds),
    KTEST_CASE(test_desktop_rect_union_covers_both_inputs),
    KTEST_CASE(test_wallpaper_cover_crops_square_source_vertically),
    KTEST_CASE(test_wallpaper_cover_crops_wide_source_horizontally),
};

static ktest_suite_t suite = KTEST_SUITE("desktop_window", cases);

ktest_suite_t *ktest_suite_desktop_window(void)
{
	return &suite;
}
