/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "cursor_sprite.h"

static void test_cursor_sprite_has_stable_arrow_shape(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(tc, DRUNIX_CURSOR_W, 13);
	KTEST_EXPECT_EQ(tc, DRUNIX_CURSOR_H, 20);
	KTEST_EXPECT_EQ(
	    tc, drunix_cursor_pixel_at(0, 0), DRUNIX_CURSOR_PIXEL_FG);
	KTEST_EXPECT_EQ(
	    tc, drunix_cursor_pixel_at(1, 2), DRUNIX_CURSOR_PIXEL_FG);
	KTEST_EXPECT_EQ(
	    tc, drunix_cursor_pixel_at(2, 2), DRUNIX_CURSOR_PIXEL_SHADOW);
	KTEST_EXPECT_EQ(
	    tc, drunix_cursor_pixel_at(10, 10), DRUNIX_CURSOR_PIXEL_SHADOW);
	KTEST_EXPECT_EQ(
	    tc, drunix_cursor_pixel_at(5, 17), DRUNIX_CURSOR_PIXEL_SHADOW);
	KTEST_EXPECT_EQ(
	    tc, drunix_cursor_pixel_at(12, 19), DRUNIX_CURSOR_PIXEL_TRANSPARENT);
}

static void test_cursor_sprite_clips_out_of_bounds(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(
	    tc, drunix_cursor_pixel_at(-1, 0), DRUNIX_CURSOR_PIXEL_TRANSPARENT);
	KTEST_EXPECT_EQ(
	    tc, drunix_cursor_pixel_at(0, -1), DRUNIX_CURSOR_PIXEL_TRANSPARENT);
	KTEST_EXPECT_EQ(tc,
	                drunix_cursor_pixel_at(DRUNIX_CURSOR_W, 0),
	                DRUNIX_CURSOR_PIXEL_TRANSPARENT);
	KTEST_EXPECT_EQ(tc,
	                drunix_cursor_pixel_at(0, DRUNIX_CURSOR_H),
	                DRUNIX_CURSOR_PIXEL_TRANSPARENT);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_cursor_sprite_has_stable_arrow_shape),
    KTEST_CASE(test_cursor_sprite_clips_out_of_bounds),
};

static ktest_suite_t suite = KTEST_SUITE("cursor_sprite", cases);

ktest_suite_t *ktest_suite_cursor_sprite(void)
{
	return &suite;
}
