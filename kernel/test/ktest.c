/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ktest.c — in-kernel test runner and assertion/reporting support.
 *
 * Test output is routed through klog_silent* so that a KTEST build's boot
 * screen stays visually identical to a non-KTEST build. Pass/fail evidence
 * lives in the klog ring (hence /proc/kmsg) and on the QEMU debugcon port,
 * which is where `make test` scrapes for the summary line.
 */

#include "ktest.h"
#include "klog.h"
#include "kprintf.h"

/* ── Internal failure reporter ──────────────────────────────────────────── */

static void report_fail(ktest_case_t *tc, const char *expr, int line)
{
	klog_silent("KTEST FAIL", tc->name);
	klog_silent("          ", expr);
	klog_silent_uint("          ", "line", (uint32_t)line);
	tc->failed = 1;
}

/* ── Expectation helpers ─────────────────────────────────────────────────── */

void ktest_expect_eq(ktest_case_t *tc,
                     uint32_t a,
                     uint32_t b,
                     const char *as,
                     const char *bs,
                     int line)
{
	(void)bs;
	if (a != b) {
		report_fail(tc, as, line);
		klog_silent_hex("          ", "  got     ", a);
		klog_silent_hex("          ", "  expected", b);
	}
}

void ktest_expect_ne(ktest_case_t *tc,
                     uint32_t a,
                     uint32_t b,
                     const char *as,
                     const char *bs,
                     int line)
{
	(void)bs;
	if (a == b) {
		report_fail(tc, as, line);
		klog_silent_hex("          ", "  both", a);
	}
}

void ktest_expect_true(ktest_case_t *tc, int cond, const char *expr, int line)
{
	if (!cond)
		report_fail(tc, expr, line);
}

void ktest_expect_ge(ktest_case_t *tc,
                     uint32_t a,
                     uint32_t b,
                     const char *as,
                     const char *bs,
                     int line)
{
	(void)bs;
	if (a < b) {
		report_fail(tc, as, line);
		klog_silent_hex("          ", "  got     ", a);
		klog_silent_hex("          ", "  want >= ", b);
	}
}

/* ── Suite runner ───────────────────────────────────────────────────────── */

void ktest_run_suite(ktest_suite_t *suite)
{
	ktest_run_suite_counts(suite, 0, 0);
}

void ktest_run_suite_counts(ktest_suite_t *suite,
                            int *passed_out,
                            int *failed_out)
{
	int passed = 0, failed = 0;
	klog_silent("KTEST", suite->name);

	for (int i = 0; i < suite->num_cases; i++) {
		ktest_case_t *tc = &suite->cases[i];
		tc->failed = 0;
		tc->fn(tc);
		if (tc->failed)
			failed++;
		else
			passed++;
	}

	klog_silent_uint("KTEST", "  passed", (uint32_t)passed);
	if (failed)
		klog_silent_uint("KTEST", "  FAILED", (uint32_t)failed);

	if (passed_out)
		*passed_out = passed;
	if (failed_out)
		*failed_out = failed;
}

/* ── Top-level runner ───────────────────────────────────────────────────── */

static void
run_and_tally(ktest_suite_t *suite, int *total_pass, int *total_fail)
{
	int p = 0, f = 0;
	ktest_run_suite_counts(suite, &p, &f);
	*total_pass += p;
	*total_fail += f;
}

void ktest_run_all(void)
{
	int total_pass = 0, total_fail = 0;
	char summary[96];

	klog_silent("KTEST", "=== kernel unit tests begin ===");

	run_and_tally(ktest_suite_pmm(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_console_terminal(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_pmm_core(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_kheap(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_vfs(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_process(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_sched(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_fs(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_uaccess(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_desktop(), &total_pass, &total_fail);
	run_and_tally(ktest_suite_blkdev(), &total_pass, &total_fail);

	klog_silent("KTEST", "=== kernel unit tests end ===");

	/*
     * Deterministic marker the Makefile greps for. The exact format
     * "KTEST: SUMMARY pass=N fail=M" is load-bearing — don't change it
     * without updating the `test` target.
     */
	k_snprintf(summary,
	           sizeof(summary),
	           "SUMMARY pass=%d fail=%d",
	           total_pass,
	           total_fail);
	klog_silent("KTEST", summary);
}
