/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "pmm_core.h"

#define TEST_STORAGE(name, pages)                                             \
	static uint8_t name##_bitmap[((pages) + 7u) / 8u];                    \
	static uint8_t name##_refcounts[(pages)]

static void test_pmm_core_storage_size_helpers(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(tc, pmm_core_bitmap_bytes(0u), 0u);
	KTEST_EXPECT_EQ(tc, pmm_core_bitmap_bytes(1u), 1u);
	KTEST_EXPECT_EQ(tc, pmm_core_bitmap_bytes(8u), 1u);
	KTEST_EXPECT_EQ(tc, pmm_core_bitmap_bytes(9u), 2u);
	KTEST_EXPECT_EQ(tc, pmm_core_refcount_bytes(4097u), 4097u);
}

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
	TEST_STORAGE(storage, 8192u);

	pmm_core_bind_storage(&state, storage_bitmap, storage_refcounts, 8192u);
	pmm_core_init(&state, usable, 1u, reserved, 2u);
	KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00100000u), 255u);
	KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00200000u), 255u);
	KTEST_EXPECT_TRUE(tc, pmm_core_free_page_count(&state) > 0u);
	KTEST_EXPECT_EQ(tc, pmm_core_alloc_page(&state), 0x00102000u);
}

static void test_pmm_core_mark_helpers_update_accounting(ktest_case_t *tc)
{
	static pmm_core_state_t state;
	TEST_STORAGE(storage, 2048u);

	pmm_core_bind_storage(&state, storage_bitmap, storage_refcounts, 2048u);
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

static void test_pmm_core_uses_bound_max_pages(ktest_case_t *tc)
{
	static const pmm_range_t usable[] = {
		{.base = 0x00100000u, .length = PAGE_SIZE * 16u},
	};
	static pmm_core_state_t state;
	TEST_STORAGE(storage, 260u);

	pmm_core_bind_storage(&state, storage_bitmap, storage_refcounts, 260u);
	pmm_core_init(&state, usable, 1u, 0, 0);

	KTEST_EXPECT_EQ(tc, state.max_pages, 260u);
	KTEST_EXPECT_EQ(tc, pmm_core_free_page_count(&state), 4u);
	KTEST_EXPECT_EQ(tc, pmm_core_alloc_page(&state), 0x00100000u);
}

static void test_pmm_core_unbound_storage_fails_safely(ktest_case_t *tc)
{
	static pmm_core_state_t state;
	static const pmm_range_t usable[] = {
		{.base = 0x00100000u, .length = PAGE_SIZE},
	};

	pmm_core_init(&state, usable, 1u, 0, 0);
	KTEST_EXPECT_EQ(tc, pmm_core_free_page_count(&state), 0u);
	KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00100000u), 0u);
	KTEST_EXPECT_EQ(tc, pmm_core_alloc_page(&state), 0u);
	pmm_core_mark_free(&state, 0x00100000u, PAGE_SIZE);
	pmm_core_mark_used(&state, 0x00100000u, PAGE_SIZE);
	pmm_core_free_page(&state, 0x00100000u);
	pmm_core_incref(&state, 0x00100000u);
	pmm_core_decref(&state, 0x00100000u);
}

static ktest_case_t cases[] = {
	KTEST_CASE(test_pmm_core_storage_size_helpers),
	KTEST_CASE(test_pmm_core_respects_reserved_ranges),
	KTEST_CASE(test_pmm_core_mark_helpers_update_accounting),
	KTEST_CASE(test_pmm_core_uses_bound_max_pages),
	KTEST_CASE(test_pmm_core_unbound_storage_fails_safely),
};

static ktest_suite_t suite = KTEST_SUITE("pmm_core", cases);

ktest_suite_t *ktest_suite_pmm_core(void)
{
	return &suite;
}
