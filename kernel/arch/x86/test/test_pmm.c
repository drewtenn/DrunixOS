/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_pmm.c — in-kernel physical memory manager unit tests.
 */

#include "ktest.h"
#include "pmm.h"
#include "paging.h"
#include "kstring.h"

/*
 * PMM unit tests.
 *
 * pmm_init() has already run by the time these execute, so the allocator
 * holds the real physical memory map.  Each test allocates pages and frees
 * them again, leaving the allocator in the same state it was found in.
 */

static void test_alloc_returns_nonzero(ktest_case_t *tc)
{
	uint32_t p = pmm_alloc_page();
	KTEST_EXPECT_NE(tc, p, 0u);
	if (p)
		pmm_free_page(p);
}

static void test_alloc_page_aligned(ktest_case_t *tc)
{
	uint32_t p = pmm_alloc_page();
	KTEST_ASSERT_NOT_NULL(tc, p);
	KTEST_EXPECT_EQ(tc, p % PAGE_SIZE, 0u);
	pmm_free_page(p);
}

static void test_two_allocs_differ(ktest_case_t *tc)
{
	uint32_t p1 = pmm_alloc_page();
	uint32_t p2 = pmm_alloc_page();
	KTEST_ASSERT_TRUE(tc, p1 && p2);
	KTEST_EXPECT_NE(tc, p1, p2);
	pmm_free_page(p1);
	pmm_free_page(p2);
}

static void test_free_count_decrements(ktest_case_t *tc)
{
	uint32_t before = pmm_free_page_count();
	uint32_t p = pmm_alloc_page();
	KTEST_ASSERT_NOT_NULL(tc, p);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before - 1u);
	pmm_free_page(p);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before);
}

static void test_free_restores_allocability(ktest_case_t *tc)
{
	uint32_t before = pmm_free_page_count();
	uint32_t p = pmm_alloc_page();
	KTEST_ASSERT_NOT_NULL(tc, p);
	pmm_free_page(p);
	/* Free count must be restored; we can allocate again without OOM. */
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before);
	uint32_t p2 = pmm_alloc_page();
	KTEST_EXPECT_NE(tc, p2, 0u);
	if (p2)
		pmm_free_page(p2);
}

static void test_multiple_alloc_and_free(ktest_case_t *tc)
{
	uint32_t pages[8];
	uint32_t before = pmm_free_page_count();

	for (int i = 0; i < 8; i++) {
		pages[i] = pmm_alloc_page();
		KTEST_ASSERT_NOT_NULL(tc, pages[i]);
	}
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before - 8u);

	for (int i = 0; i < 8; i++)
		pmm_free_page(pages[i]);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before);
}

static void test_refcount_tracks_shared_page(ktest_case_t *tc)
{
	uint32_t before = pmm_free_page_count();
	uint32_t p = pmm_alloc_page();
	KTEST_ASSERT_NOT_NULL(tc, p);
	KTEST_EXPECT_EQ(tc, pmm_refcount(p), 1u);

	pmm_incref(p);
	pmm_incref(p);
	KTEST_EXPECT_EQ(tc, pmm_refcount(p), 3u);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before - 1u);

	pmm_decref(p);
	KTEST_EXPECT_EQ(tc, pmm_refcount(p), 2u);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before - 1u);

	pmm_decref(p);
	pmm_decref(p);
	KTEST_EXPECT_EQ(tc, pmm_refcount(p), 0u);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before);
}

static void test_refcount_saturates_at_255(ktest_case_t *tc)
{
	uint32_t p = PAGE_DIR_ADDR;

	KTEST_EXPECT_EQ(tc, pmm_refcount(p), 255u);
	pmm_incref(p);
	pmm_incref(p);
	KTEST_EXPECT_EQ(tc, pmm_refcount(p), 255u);
	pmm_decref(p);
	KTEST_EXPECT_EQ(tc, pmm_refcount(p), 255u);
}

static void test_reserved_pages_are_pinned(ktest_case_t *tc)
{
	uint32_t before = pmm_free_page_count();

	KTEST_EXPECT_EQ(tc, pmm_refcount(0), 255u);
	KTEST_EXPECT_EQ(tc, pmm_refcount(0x3000u), 255u);
	KTEST_EXPECT_EQ(tc, pmm_refcount(PAGE_DIR_ADDR), 255u);

	pmm_decref(0);
	pmm_decref(0x3000u);
	pmm_decref(PAGE_DIR_ADDR);

	KTEST_EXPECT_EQ(tc, pmm_refcount(0), 255u);
	KTEST_EXPECT_EQ(tc, pmm_refcount(0x3000u), 255u);
	KTEST_EXPECT_EQ(tc, pmm_refcount(PAGE_DIR_ADDR), 255u);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before);
}

static void test_alloc_never_returns_low_conventional_memory(ktest_case_t *tc)
{
	uint32_t p = pmm_alloc_page();
	KTEST_ASSERT_NOT_NULL(tc, p);
	KTEST_EXPECT_GE(tc, p, 0x00100000u);
	pmm_free_page(p);
}

static void
test_multiboot_framebuffer_reservation_covers_visible_rows(ktest_case_t *tc)
{
	multiboot_info_t mbi;
	uint32_t base = 0;
	uint32_t length = 0;

	k_memset(&mbi, 0, sizeof(mbi));
	mbi.flags = MULTIBOOT_FLAG_FRAMEBUFFER;
	mbi.framebuffer_addr = 0x0046B000u;
	mbi.framebuffer_width = 1024;
	mbi.framebuffer_height = 768;
	mbi.framebuffer_pitch = 4096;
	mbi.framebuffer_bpp = 32;

	KTEST_EXPECT_EQ(
	    tc, pmm_multiboot_framebuffer_range_for_test(&mbi, &base, &length), 0);
	KTEST_EXPECT_EQ(tc, base, 0x0046B000u);
	KTEST_EXPECT_EQ(tc, length, 1024u * 768u * 4u);
}

static void
test_multiboot_framebuffer_reservation_rejects_wrap(ktest_case_t *tc)
{
	multiboot_info_t mbi;
	uint32_t base = 0;
	uint32_t length = 0;

	k_memset(&mbi, 0, sizeof(mbi));
	mbi.flags = MULTIBOOT_FLAG_FRAMEBUFFER;
	mbi.framebuffer_addr = 0xFFFFF000u;
	mbi.framebuffer_width = 1024;
	mbi.framebuffer_height = 2;
	mbi.framebuffer_pitch = 4096;
	mbi.framebuffer_bpp = 32;

	KTEST_EXPECT_EQ(
	    tc, pmm_multiboot_framebuffer_range_for_test(&mbi, &base, &length), -1);
}

static void
test_multiboot_usable_ranges_ignore_high_addr_wrap(ktest_case_t *tc)
{
	static pmm_core_state_t state;
	static multiboot_mmap_entry_t map[1];
	multiboot_info_t mbi;

	k_memset(&state, 0, sizeof(state));
	k_memset(&mbi, 0, sizeof(mbi));
	k_memset(map, 0, sizeof(map));

	pmm_core_init(&state, 0, 0, 0, 0);
	map[0].size = sizeof(multiboot_mmap_entry_t) - sizeof(uint32_t);
	map[0].addr = 0x100002000ull;
	map[0].len = PAGE_SIZE;
	map[0].type = 1u;

	mbi.flags = MULTIBOOT_FLAG_MMAP;
	mbi.mmap_addr = (uint32_t)(uintptr_t)map;
	mbi.mmap_length = sizeof(map);

	pmm_multiboot_apply_usable_ranges_for_test(&state, &mbi);

	KTEST_EXPECT_EQ(tc, pmm_core_free_page_count(&state), 0u);
	KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00002000u), 255u);
	KTEST_EXPECT_EQ(tc, pmm_core_alloc_page(&state), 0u);
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

static ktest_case_t cases[] = {
    KTEST_CASE(test_alloc_returns_nonzero),
    KTEST_CASE(test_alloc_page_aligned),
    KTEST_CASE(test_two_allocs_differ),
    KTEST_CASE(test_free_count_decrements),
    KTEST_CASE(test_free_restores_allocability),
    KTEST_CASE(test_multiple_alloc_and_free),
    KTEST_CASE(test_refcount_tracks_shared_page),
    KTEST_CASE(test_refcount_saturates_at_255),
    KTEST_CASE(test_reserved_pages_are_pinned),
    KTEST_CASE(test_alloc_never_returns_low_conventional_memory),
    KTEST_CASE(test_multiboot_framebuffer_reservation_covers_visible_rows),
    KTEST_CASE(test_multiboot_framebuffer_reservation_rejects_wrap),
    KTEST_CASE(test_multiboot_usable_ranges_ignore_high_addr_wrap),
};

static ktest_suite_t suite = KTEST_SUITE("pmm", cases);

ktest_suite_t *ktest_suite_pmm(void)
{
	return &suite;
}
