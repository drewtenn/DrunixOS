/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KTEST_H
#define KTEST_H

#include <stdint.h>

/*
 * ktest — lightweight kernel unit test framework.
 *
 * Inspired by Linux KUnit.  Tests are grouped into suites; each suite holds
 * an array of ktest_case_t descriptors.  The runner executes every case,
 * resets the failure flag before each run, and prints a PASS / FAIL summary
 * via klog.
 *
 * Usage
 * -----
 * 1. Write test functions with the signature:
 *        static void test_foo(ktest_case_t *tc) { ... }
 *
 * 2. Declare a case array and a suite in the same file:
 *        static ktest_case_t pmm_cases[] = {
 *            KTEST_CASE(test_foo),
 *            KTEST_CASE(test_bar),
 *        };
 *        static ktest_suite_t pmm_suite = KTEST_SUITE("pmm", pmm_cases);
 *
 * 3. Export the suite via a registration function declared below and
 *    called from ktest_run_all() in ktest.c.
 *
 * KTEST_EXPECT_*  — record a failure but continue the test case.
 * KTEST_ASSERT_*  — record a failure and immediately return from the test.
 */

/* ── Core types ─────────────────────────────────────────────────────────── */

typedef struct ktest_case {
	const char *name;
	void (*fn)(struct ktest_case *tc);
	int failed;
} ktest_case_t;

typedef struct {
	const char *name;
	ktest_case_t *cases;
	int num_cases;
} ktest_suite_t;

/* ── Suite / case definition macros ─────────────────────────────────────── */

#define KTEST_CASE(fn_) {#fn_, fn_, 0}
#define KTEST_SUITE(name_, cases_)                                             \
	{name_, cases_, (int)(sizeof(cases_) / sizeof((cases_)[0]))}

/* ── Expectation helpers (called by macros) ─────────────────────────────── */

void ktest_expect_eq(ktest_case_t *tc,
                     uint32_t a,
                     uint32_t b,
                     const char *as,
                     const char *bs,
                     int line);
void ktest_expect_ne(ktest_case_t *tc,
                     uint32_t a,
                     uint32_t b,
                     const char *as,
                     const char *bs,
                     int line);
void ktest_expect_true(ktest_case_t *tc, int cond, const char *expr, int line);
void ktest_expect_ge(ktest_case_t *tc,
                     uint32_t a,
                     uint32_t b,
                     const char *as,
                     const char *bs,
                     int line);

/* ── EXPECT macros — non-fatal, always continue ─────────────────────────── */

#define KTEST_EXPECT_EQ(tc, a, b)                                              \
	ktest_expect_eq((tc), (uint32_t)(a), (uint32_t)(b), #a, #b, __LINE__)

#define KTEST_EXPECT_NE(tc, a, b)                                              \
	ktest_expect_ne((tc), (uint32_t)(a), (uint32_t)(b), #a, #b, __LINE__)

#define KTEST_EXPECT_TRUE(tc, expr)                                            \
	ktest_expect_true((tc), !!(expr), #expr, __LINE__)

#define KTEST_EXPECT_FALSE(tc, expr)                                           \
	ktest_expect_true((tc), !(expr), "!(" #expr ")", __LINE__)

#define KTEST_EXPECT_NOT_NULL(tc, ptr)                                         \
	ktest_expect_true((tc), (ptr) != 0, #ptr " != NULL", __LINE__)

#define KTEST_EXPECT_NULL(tc, ptr)                                             \
	ktest_expect_true((tc), (ptr) == 0, #ptr " == NULL", __LINE__)

#define KTEST_EXPECT_GE(tc, a, b)                                              \
	ktest_expect_ge((tc), (uint32_t)(a), (uint32_t)(b), #a, #b, __LINE__)

#define KTEST_EXPECT_LE(tc, a, b)                                              \
	ktest_expect_ge((tc), (uint32_t)(b), (uint32_t)(a), #b, #a, __LINE__)

/* ── ASSERT macros — fatal, abort the current test case on failure ───────── */

#define KTEST_ASSERT_EQ(tc, a, b)                                              \
	do {                                                                       \
		KTEST_EXPECT_EQ(tc, a, b);                                             \
		if ((tc)->failed)                                                      \
			return;                                                            \
	} while (0)

#define KTEST_ASSERT_NE(tc, a, b)                                              \
	do {                                                                       \
		KTEST_EXPECT_NE(tc, a, b);                                             \
		if ((tc)->failed)                                                      \
			return;                                                            \
	} while (0)

#define KTEST_ASSERT_TRUE(tc, expr)                                            \
	do {                                                                       \
		KTEST_EXPECT_TRUE(tc, expr);                                           \
		if ((tc)->failed)                                                      \
			return;                                                            \
	} while (0)

#define KTEST_ASSERT_NOT_NULL(tc, ptr)                                         \
	do {                                                                       \
		KTEST_EXPECT_NOT_NULL(tc, ptr);                                        \
		if ((tc)->failed)                                                      \
			return;                                                            \
	} while (0)

/* ── Runner ─────────────────────────────────────────────────────────────── */

void ktest_run_suite(ktest_suite_t *suite);
void ktest_run_suite_counts(ktest_suite_t *suite,
                            int *passed_out,
                            int *failed_out);
void ktest_run_all(void);

/* ── Suite registration — one function per test file ────────────────────── */

ktest_suite_t *ktest_suite_pmm(void);
ktest_suite_t *ktest_suite_pmm_core(void);
ktest_suite_t *ktest_suite_kheap(void);
ktest_suite_t *ktest_suite_vfs(void);
ktest_suite_t *ktest_suite_process(void);
ktest_suite_t *ktest_suite_sched(void);
ktest_suite_t *ktest_suite_fs(void);
ktest_suite_t *ktest_suite_uaccess(void);
ktest_suite_t *ktest_suite_desktop(void);
ktest_suite_t *ktest_suite_blkdev(void);

#endif /* KTEST_H */
