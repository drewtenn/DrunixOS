/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_arch_x86.c - x86-specific kernel behavior paired with shared KTESTs.
 */

#include "ktest.h"
#include "kstring.h"
#include "process.h"
#include "sched.h"

static void init_dummy_proc(process_t *proc)
{
	k_memset(proc, 0, sizeof(*proc));
	proc->arch_state.context = 1;
}

static void test_sched_record_user_fault_preserves_full_fault_addr(ktest_case_t *tc)
{
	static process_t proc;
	arch_trap_frame_t frame;
	process_t *running;
	uint64_t fault_addr = 0x1234567887654321ull;

	sched_init();
	init_dummy_proc(&proc);
	KTEST_ASSERT_TRUE(tc, sched_add(&proc) >= 1);

	running = sched_bootstrap();
	KTEST_ASSERT_NOT_NULL(tc, running);

	k_memset(&frame, 0, sizeof(frame));
	frame.eip = 0x00401234u;
	frame.vector = 14u;
	frame.error_code = 0x5u;

	sched_record_user_fault(&frame, fault_addr, SIGSEGV);

	KTEST_EXPECT_EQ(tc, running->crash.valid, 1u);
	KTEST_EXPECT_EQ(tc, running->crash.signum, (uint32_t)SIGSEGV);
	KTEST_EXPECT_TRUE(tc, running->crash.fault_addr == fault_addr);
	KTEST_EXPECT_EQ(tc, running->crash.frame.eip, frame.eip);
	KTEST_EXPECT_EQ(tc, running->crash.frame.vector, frame.vector);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_sched_record_user_fault_preserves_full_fault_addr),
};

static ktest_suite_t suite = KTEST_SUITE("arch-x86", cases);

ktest_suite_t *ktest_suite_arch_x86(void)
{
	return &suite;
}
