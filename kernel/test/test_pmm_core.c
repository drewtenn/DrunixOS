/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "pmm_core.h"

static void test_pmm_core_respects_reserved_ranges(ktest_case_t *tc)
{
	static const pmm_range_t usable[] = {
		{.base = 0x00100000u, .length = 0x01000000u},
	};
	static const pmm_range_t reserved[] = {
		{.base = 0x00100000u, .length = 0x00002000u},
		{.base = 0x00200000u, .length = 0x00001000u},
	};
	static pmm_core_state_t state;

	pmm_core_init(&state, usable, 1u, reserved, 2u);
	KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00100000u), 255u);
	KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00200000u), 255u);
	KTEST_EXPECT_TRUE(tc, pmm_core_free_page_count(&state) > 0u);
	KTEST_EXPECT_EQ(tc, pmm_core_alloc_page(&state), 0x00102000u);
}

static void test_pmm_core_mark_helpers_update_accounting(ktest_case_t *tc)
{
	static pmm_core_state_t state;

	pmm_core_init(&state, 0, 0, 0, 0);
	KTEST_EXPECT_EQ(tc, pmm_core_free_page_count(&state), 0u);

	pmm_core_mark_free(&state, 0x00300000u, PAGE_SIZE * 2u);
	KTEST_EXPECT_EQ(tc, pmm_core_free_page_count(&state), 2u);
	KTEST_EXPECT_EQ(tc, pmm_core_alloc_page(&state), 0x00300000u);
	KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00300000u), 1u);

	pmm_core_mark_used(&state, 0x00301000u, PAGE_SIZE);
	KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00301000u), 255u);
	KTEST_EXPECT_EQ(tc, pmm_core_free_page_count(&state), 0u);
}

static ktest_case_t cases[] = {
	KTEST_CASE(test_pmm_core_respects_reserved_ranges),
	KTEST_CASE(test_pmm_core_mark_helpers_update_accounting),
};

static ktest_suite_t suite = KTEST_SUITE("pmm_core", cases);

ktest_suite_t *ktest_suite_pmm_core(void)
{
	return &suite;
}
