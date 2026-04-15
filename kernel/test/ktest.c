/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ktest.c — in-kernel test runner and assertion/reporting support.
 */

#include "ktest.h"
#include "klog.h"

/* ── Internal failure reporter ──────────────────────────────────────────── */

static void report_fail(ktest_case_t *tc, const char *expr, int line) {
    klog("KTEST FAIL", tc->name);
    klog("          ", expr);
    klog_uint("          ", "line", (uint32_t)line);
    tc->failed = 1;
}

/* ── Expectation helpers ─────────────────────────────────────────────────── */

void ktest_expect_eq(ktest_case_t *tc, uint32_t a, uint32_t b,
                     const char *as, const char *bs, int line) {
    (void)bs;
    if (a != b) {
        report_fail(tc, as, line);
        klog_hex("          ", "  got     ", a);
        klog_hex("          ", "  expected", b);
    }
}

void ktest_expect_ne(ktest_case_t *tc, uint32_t a, uint32_t b,
                     const char *as, const char *bs, int line) {
    (void)bs;
    if (a == b) {
        report_fail(tc, as, line);
        klog_hex("          ", "  both", a);
    }
}

void ktest_expect_true(ktest_case_t *tc, int cond,
                       const char *expr, int line) {
    if (!cond)
        report_fail(tc, expr, line);
}

void ktest_expect_ge(ktest_case_t *tc, uint32_t a, uint32_t b,
                     const char *as, const char *bs, int line) {
    (void)bs;
    if (a < b) {
        report_fail(tc, as, line);
        klog_hex("          ", "  got     ", a);
        klog_hex("          ", "  want >= ", b);
    }
}

/* ── Suite runner ───────────────────────────────────────────────────────── */

void ktest_run_suite(ktest_suite_t *suite) {
    int passed = 0, failed = 0;
    klog("KTEST", suite->name);

    for (int i = 0; i < suite->num_cases; i++) {
        ktest_case_t *tc = &suite->cases[i];
        tc->failed = 0;
        tc->fn(tc);
        if (tc->failed)
            failed++;
        else
            passed++;
    }

    klog_uint("KTEST", "  passed", (uint32_t)passed);
    if (failed)
        klog_uint("KTEST", "  FAILED", (uint32_t)failed);
}

/* ── Top-level runner ───────────────────────────────────────────────────── */

void ktest_run_all(void) {
    klog("KTEST", "=== kernel unit tests begin ===");

    ktest_run_suite(ktest_suite_pmm());
    ktest_run_suite(ktest_suite_kheap());
    ktest_run_suite(ktest_suite_vfs());
    ktest_run_suite(ktest_suite_process());
    ktest_run_suite(ktest_suite_sched());
    ktest_run_suite(ktest_suite_fs());
    ktest_run_suite(ktest_suite_uaccess());
    ktest_run_suite(ktest_suite_desktop());

    klog("KTEST", "=== kernel unit tests end ===");
}
