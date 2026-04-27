/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "console/runtime.h"
#include <stdint.h>

static void test_display_claim_suppresses_legacy_console_paths(ktest_case_t *tc)
{
	console_runtime_reset_for_test();

	KTEST_EXPECT_FALSE(tc, console_runtime_display_claimed());
	KTEST_EXPECT_TRUE(tc, console_runtime_legacy_input_enabled());

	KTEST_EXPECT_EQ(tc, console_runtime_claim_display(7u), 0);
	KTEST_EXPECT_TRUE(tc, console_runtime_display_claimed());
	KTEST_EXPECT_FALSE(tc, console_runtime_legacy_input_enabled());
	KTEST_EXPECT_EQ(tc, console_runtime_write_feedback("abc", 3u), 3u);
	KTEST_EXPECT_EQ(tc, console_runtime_write_process_output(0, "abc", 3u), 3u);
	KTEST_EXPECT_EQ(tc, console_runtime_write_kernel_output("abc", 3u), 3u);
}

static void test_display_claim_tracks_owner(ktest_case_t *tc)
{
	console_runtime_reset_for_test();

	KTEST_EXPECT_EQ(tc, console_runtime_claim_display(7u), 0);
	KTEST_EXPECT_NE(tc, console_runtime_claim_display(8u), 0);
	KTEST_EXPECT_NE(tc, console_runtime_release_display(8u), 0);
	KTEST_EXPECT_TRUE(tc, console_runtime_display_claimed());
	KTEST_EXPECT_EQ(tc, console_runtime_release_display(7u), 0);
	KTEST_EXPECT_FALSE(tc, console_runtime_display_claimed());
	KTEST_EXPECT_TRUE(tc, console_runtime_legacy_input_enabled());
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_display_claim_suppresses_legacy_console_paths),
    KTEST_CASE(test_display_claim_tracks_owner),
};

static ktest_suite_t suite = KTEST_SUITE("console_runtime", cases);

ktest_suite_t *ktest_suite_console_runtime(void)
{
	return &suite;
}
