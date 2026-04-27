/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "pty.h"

static void test_pty_slave_ref_survives_original_close(ktest_case_t *tc)
{
	uint8_t in = 'x';
	uint8_t out = 0;
	int idx = pty_alloc_master();

	KTEST_ASSERT_NE(tc, (uint32_t)idx, (uint32_t)-1);
	KTEST_ASSERT_EQ(tc, pty_open_slave((uint32_t)idx), 0);

	pty_get_slave((uint32_t)idx);
	pty_release_slave((uint32_t)idx);

	KTEST_EXPECT_EQ(tc, pty_master_write((uint32_t)idx, &in, 1), 1);
	KTEST_EXPECT_EQ(tc, pty_slave_read((uint32_t)idx, &out, 1), 1);
	KTEST_EXPECT_EQ(tc, out, 'x');

	pty_release_slave((uint32_t)idx);
	pty_release_master((uint32_t)idx);
}

static void test_pty_master_ref_survives_original_close(ktest_case_t *tc)
{
	uint8_t in = 'y';
	uint8_t out = 0;
	int idx = pty_alloc_master();

	KTEST_ASSERT_NE(tc, (uint32_t)idx, (uint32_t)-1);
	KTEST_ASSERT_EQ(tc, pty_open_slave((uint32_t)idx), 0);

	pty_get_master((uint32_t)idx);
	pty_release_master((uint32_t)idx);

	KTEST_EXPECT_EQ(tc, pty_slave_write((uint32_t)idx, &in, 1), 1);
	KTEST_EXPECT_EQ(tc, pty_master_read((uint32_t)idx, &out, 1), 1);
	KTEST_EXPECT_EQ(tc, out, 'y');

	pty_release_master((uint32_t)idx);
	pty_release_slave((uint32_t)idx);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_pty_slave_ref_survives_original_close),
    KTEST_CASE(test_pty_master_ref_survives_original_close),
};

static ktest_suite_t suite = KTEST_SUITE("pty", cases);

ktest_suite_t *ktest_suite_pty(void)
{
	return &suite;
}
