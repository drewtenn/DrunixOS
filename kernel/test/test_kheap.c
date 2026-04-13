/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_kheap.c — in-kernel kernel-heap allocator unit tests.
 */

#include "ktest.h"
#include "kheap.h"

/*
 * Kernel heap unit tests.
 *
 * kheap_init() has already run.  Every allocation made here is freed before
 * the test returns, leaving the heap in the same state it was found in.
 */

static void test_kmalloc_returns_nonnull(ktest_case_t *tc) {
    void *p = kmalloc(16);
    KTEST_EXPECT_NOT_NULL(tc, p);
    if (p) kfree(p);
}

static void test_kmalloc_4byte_aligned(ktest_case_t *tc) {
    void *p = kmalloc(1);
    KTEST_ASSERT_NOT_NULL(tc, p);
    KTEST_EXPECT_EQ(tc, (uint32_t)p % 4u, 0u);
    kfree(p);
}

static void test_kmalloc_within_heap_bounds(ktest_case_t *tc) {
    void *p = kmalloc(64);
    KTEST_ASSERT_NOT_NULL(tc, p);
    KTEST_EXPECT_GE(tc, (uint32_t)p, HEAP_START);
    KTEST_EXPECT_LE(tc, (uint32_t)p, HEAP_END - 64u);
    kfree(p);
}

static void test_two_allocs_no_overlap(ktest_case_t *tc) {
    void *a = kmalloc(64);
    void *b = kmalloc(64);
    KTEST_ASSERT_NOT_NULL(tc, a);
    KTEST_ASSERT_NOT_NULL(tc, b);
    uint32_t lo = (uint32_t)a, hi = (uint32_t)b;
    if (lo > hi) { uint32_t tmp = lo; lo = hi; hi = tmp; }
    /* The lower allocation must end before the upper one begins. */
    KTEST_EXPECT_GE(tc, hi, lo + 64u);
    kfree(a);
    kfree(b);
}

static void test_free_restores_bytes(ktest_case_t *tc) {
    uint32_t before = kheap_free_bytes();
    void *p = kmalloc(256);
    KTEST_ASSERT_NOT_NULL(tc, p);
    kfree(p);
    /* Free byte count must be at least as large as before. */
    KTEST_EXPECT_GE(tc, kheap_free_bytes(), before);
}

static void test_kmalloc_large(ktest_case_t *tc) {
    void *p = kmalloc(4096);
    KTEST_EXPECT_NOT_NULL(tc, p);
    if (p) kfree(p);
}

static void test_kmalloc_many(ktest_case_t *tc) {
    void *ptrs[16];
    for (int i = 0; i < 16; i++) {
        ptrs[i] = kmalloc(32);
        KTEST_ASSERT_NOT_NULL(tc, ptrs[i]);
    }
    for (int i = 0; i < 16; i++)
        kfree(ptrs[i]);
    /* After freeing all, we should be able to allocate again. */
    void *p = kmalloc(32);
    KTEST_EXPECT_NOT_NULL(tc, p);
    if (p) kfree(p);
}

static void test_kmalloc_writable_and_readable(ktest_case_t *tc) {
    uint8_t *p = (uint8_t *)kmalloc(64);
    KTEST_ASSERT_NOT_NULL(tc, p);
    for (int i = 0; i < 64; i++)
        p[i] = 0xAB;
    int ok = 1;
    for (int i = 0; i < 64; i++)
        if (p[i] != 0xAB) { ok = 0; break; }
    KTEST_EXPECT_TRUE(tc, ok);
    kfree(p);
}

static void test_kfree_no_neighbor_corruption(ktest_case_t *tc) {
    /* Allocate two adjacent blocks, fill both, free one, allocate a third,
     * then verify the surviving block's contents are intact. */
    uint8_t *p1 = (uint8_t *)kmalloc(64);
    uint8_t *p2 = (uint8_t *)kmalloc(64);
    KTEST_ASSERT_NOT_NULL(tc, p1);
    KTEST_ASSERT_NOT_NULL(tc, p2);

    for (int i = 0; i < 64; i++) p1[i] = 0x55;
    for (int i = 0; i < 64; i++) p2[i] = 0xAA;

    kfree(p1);

    void *p3 = kmalloc(32);   /* may reuse p1's slot */
    KTEST_EXPECT_NOT_NULL(tc, p3);

    /* p2 must be undisturbed. */
    int ok = 1;
    for (int i = 0; i < 64; i++)
        if (p2[i] != 0xAA) { ok = 0; break; }
    KTEST_EXPECT_TRUE(tc, ok);

    if (p3) kfree(p3);
    kfree(p2);
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

static ktest_case_t cases[] = {
    KTEST_CASE(test_kmalloc_returns_nonnull),
    KTEST_CASE(test_kmalloc_4byte_aligned),
    KTEST_CASE(test_kmalloc_within_heap_bounds),
    KTEST_CASE(test_two_allocs_no_overlap),
    KTEST_CASE(test_free_restores_bytes),
    KTEST_CASE(test_kmalloc_large),
    KTEST_CASE(test_kmalloc_many),
    KTEST_CASE(test_kmalloc_writable_and_readable),
    KTEST_CASE(test_kfree_no_neighbor_corruption),
};

static ktest_suite_t suite = KTEST_SUITE("kheap", cases);

ktest_suite_t *ktest_suite_kheap(void) { return &suite; }
