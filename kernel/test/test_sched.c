/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_sched.c — in-kernel scheduler unit tests built around synthetic processes.
 */

#include "ktest.h"
#include "sched.h"
#include "process.h"
#include "kstring.h"

/*
 * Scheduler unit tests.
 *
 * Each test calls sched_init() itself to reset the process table to a clean
 * state.  Because kernel.c also calls sched_init() after the test suite
 * finishes, any dummy entries added here are wiped before the shell process
 * is created.
 *
 * No real ELF loading or page-table setup is performed; the tests only
 * exercise the scheduler's table-management logic with synthetic process
 * descriptors.  PROC_UNUSED == 0, so a zero-filled process_t is correctly
 * treated as an empty slot by sched_add().
 *
 * sched_add() now synthesises an initial kernel-stack frame for any process
 * whose saved_esp is 0. These tests do not allocate real kernel stacks, so
 * the dummy descriptors carry a non-zero saved_esp sentinel to skip that
 * path.
 */

static void init_dummy_proc(process_t *proc) {
    k_memset(proc, 0, sizeof(*proc));
    proc->saved_esp = 1;
}

static void queue_blocked_proc(wait_queue_t *queue, process_t *proc)
{
    proc->state = PROC_BLOCKED;
    proc->wait_queue = queue;
    proc->wait_next = 0;

    if (queue->tail)
        queue->tail->wait_next = proc;
    else
        queue->head = proc;
    queue->tail = proc;
}

static void test_no_running_process_after_init(ktest_case_t *tc) {
    sched_init();
    KTEST_EXPECT_NULL(tc, sched_current());
}

static void test_add_returns_valid_pid(ktest_case_t *tc) {
    sched_init();
    static process_t proc;
    init_dummy_proc(&proc);
    int pid = sched_add(&proc);
    KTEST_EXPECT_GE(tc, (uint32_t)pid, 1u);
}

static void test_add_two_pids_are_unique(ktest_case_t *tc) {
    sched_init();
    static process_t p1, p2;
    init_dummy_proc(&p1);
    init_dummy_proc(&p2);
    int pid1 = sched_add(&p1);
    int pid2 = sched_add(&p2);
    KTEST_EXPECT_GE(tc, (uint32_t)pid1, 1u);
    KTEST_EXPECT_GE(tc, (uint32_t)pid2, 1u);
    KTEST_EXPECT_NE(tc, (uint32_t)pid1, (uint32_t)pid2);
}

static void test_table_full_returns_error(ktest_case_t *tc) {
    sched_init();
    /* Fill every slot. */
    static process_t procs[MAX_PROCS];
    for (int i = 0; i < MAX_PROCS; i++) {
        init_dummy_proc(&procs[i]);
        int pid = sched_add(&procs[i]);
        KTEST_ASSERT_TRUE(tc, pid >= 1);
    }
    /* One more must fail. */
    static process_t extra;
    init_dummy_proc(&extra);
    int rc = sched_add(&extra);
    KTEST_EXPECT_TRUE(tc, rc < 0);
}

static void test_current_pid_zero_after_init(ktest_case_t *tc) {
    sched_init();
    KTEST_EXPECT_EQ(tc, sched_current_pid(), 0u);
}

static void test_sched_bootstrap_null_when_empty(ktest_case_t *tc) {
    sched_init();
    /* With no processes added, bootstrap must return NULL rather than crash. */
    process_t *p = sched_bootstrap();
    KTEST_EXPECT_NULL(tc, p);
}

static void test_sched_bootstrap_finds_ready(ktest_case_t *tc) {
    sched_init();
    static process_t proc;
    init_dummy_proc(&proc);
    int pid = sched_add(&proc);
    KTEST_ASSERT_TRUE(tc, pid >= 1);

    /* bootstrap should find the READY process and return a non-NULL pointer. */
    process_t *running = sched_bootstrap();
    KTEST_EXPECT_NOT_NULL(tc, running);
}

static void test_sched_add_unique_pids_three(ktest_case_t *tc) {
    sched_init();
    static process_t p1, p2, p3;
    init_dummy_proc(&p1);
    init_dummy_proc(&p2);
    init_dummy_proc(&p3);
    int pid1 = sched_add(&p1);
    int pid2 = sched_add(&p2);
    int pid3 = sched_add(&p3);
    KTEST_EXPECT_GE(tc, (uint32_t)pid1, 1u);
    KTEST_EXPECT_GE(tc, (uint32_t)pid2, 1u);
    KTEST_EXPECT_GE(tc, (uint32_t)pid3, 1u);
    KTEST_EXPECT_NE(tc, (uint32_t)pid1, (uint32_t)pid2);
    KTEST_EXPECT_NE(tc, (uint32_t)pid2, (uint32_t)pid3);
    KTEST_EXPECT_NE(tc, (uint32_t)pid1, (uint32_t)pid3);
}

/*
 * test_waitpid_reaps_zombie: add a process, manually mark it as a zombie with
 * a known exit_status, bootstrap a parent process so g_current is valid, then
 * verify sched_waitpid returns the correct status and the slot transitions to
 * PROC_UNUSED (a second waitpid returns -1).
 *
 * The dummy processes must carry a non-zero saved_esp sentinel so sched_add()
 * does not try to synthesise an initial launch frame on a non-existent kernel
 * stack. pd_phys and kstack_bottom remain 0 so sched_reap's teardown helpers
 * are safe no-ops.
 */
static void test_waitpid_reaps_zombie(ktest_case_t *tc) {
    sched_init();

    /* Add a "parent" process so sched_bootstrap sets g_current. */
    static process_t parent;
    init_dummy_proc(&parent);
    int ppid = sched_add(&parent);
    KTEST_ASSERT_TRUE(tc, ppid >= 1);
    process_t *running = sched_bootstrap();
    KTEST_ASSERT_NOT_NULL(tc, running);

    /* Add a "child" process to get a valid PID in the table. */
    static process_t child;
    init_dummy_proc(&child);
    child.pd_phys = 0;
    child.kstack_bottom = 0;
    int cpid = sched_add(&child);
    KTEST_ASSERT_TRUE(tc, cpid >= 1);

    /* Reach into the table via sched_find_pid and zombify. */
    process_t *slot = sched_find_pid((uint32_t)cpid);
    KTEST_ASSERT_NOT_NULL(tc, slot);
    slot->state = PROC_ZOMBIE;
    slot->exit_status = 0x2A00;  /* encodes exit code 42 */

    /* waitpid should return the encoded exit status. */
    int status = sched_waitpid((uint32_t)cpid, 0);
    KTEST_EXPECT_EQ(tc, (uint32_t)status, 0x2A00u);

    /* The slot should now be PROC_UNUSED; a second waitpid returns -1. */
    int again = sched_waitpid((uint32_t)cpid, 0);
    KTEST_EXPECT_EQ(tc, (uint32_t)again, (uint32_t)-1);
}

static void test_wait_queue_init_clears_links(ktest_case_t *tc)
{
    wait_queue_t queue;
    queue.head = (process_t *)1;
    queue.tail = (process_t *)1;

    sched_wait_queue_init(&queue);

    KTEST_EXPECT_NULL(tc, queue.head);
    KTEST_EXPECT_NULL(tc, queue.tail);
}

static void test_sched_wake_all_wakes_only_queued_blocked_procs(ktest_case_t *tc)
{
    wait_queue_t queue;
    static process_t sleeper1, sleeper2, unaffected;

    sched_wait_queue_init(&queue);
    init_dummy_proc(&sleeper1);
    init_dummy_proc(&sleeper2);
    init_dummy_proc(&unaffected);

    queue_blocked_proc(&queue, &sleeper1);
    queue_blocked_proc(&queue, &sleeper2);
    unaffected.state = PROC_READY;

    sched_wake_all(&queue);

    KTEST_EXPECT_EQ(tc, sleeper1.state, PROC_READY);
    KTEST_EXPECT_EQ(tc, sleeper2.state, PROC_READY);
    KTEST_EXPECT_EQ(tc, unaffected.state, PROC_READY);
    KTEST_EXPECT_NULL(tc, sleeper1.wait_queue);
    KTEST_EXPECT_NULL(tc, sleeper2.wait_queue);
    KTEST_EXPECT_NULL(tc, sleeper1.wait_next);
    KTEST_EXPECT_NULL(tc, sleeper2.wait_next);
    KTEST_EXPECT_EQ(tc, sleeper1.wait_deadline_set, 0u);
    KTEST_EXPECT_EQ(tc, sleeper2.wait_deadline_set, 0u);
    KTEST_EXPECT_NULL(tc, queue.head);
    KTEST_EXPECT_NULL(tc, queue.tail);
}

static void test_sched_send_signal_wakes_blocked_process_and_unqueues_it(ktest_case_t *tc)
{
    wait_queue_t queue;
    static process_t proc;

    sched_init();
    sched_wait_queue_init(&queue);
    init_dummy_proc(&proc);

    int pid = sched_add(&proc);
    KTEST_ASSERT_TRUE(tc, pid >= 1);

    process_t *slot = sched_find_pid((uint32_t)pid);
    KTEST_ASSERT_NOT_NULL(tc, slot);

    queue_blocked_proc(&queue, slot);
    slot->wait_deadline = 99;
    slot->wait_deadline_set = 1;

    sched_send_signal((uint32_t)pid, SIGTERM);

    KTEST_EXPECT_EQ(tc, slot->state, PROC_READY);
    KTEST_EXPECT_TRUE(tc, (slot->sig_pending & (1u << SIGTERM)) != 0);
    KTEST_EXPECT_NULL(tc, slot->wait_queue);
    KTEST_EXPECT_NULL(tc, slot->wait_next);
    KTEST_EXPECT_EQ(tc, slot->wait_deadline, 0u);
    KTEST_EXPECT_EQ(tc, slot->wait_deadline_set, 0u);
    KTEST_EXPECT_NULL(tc, queue.head);
    KTEST_EXPECT_NULL(tc, queue.tail);
}

static void test_sched_tick_wakes_timed_blocked_process(ktest_case_t *tc)
{
    static process_t proc;

    sched_init();
    init_dummy_proc(&proc);

    int pid = sched_add(&proc);
    KTEST_ASSERT_TRUE(tc, pid >= 1);

    process_t *slot = sched_find_pid((uint32_t)pid);
    KTEST_ASSERT_NOT_NULL(tc, slot);

    slot->state = PROC_BLOCKED;
    slot->wait_deadline = sched_ticks() + 1u;
    slot->wait_deadline_set = 1;

    sched_tick();

    KTEST_EXPECT_EQ(tc, slot->state, PROC_READY);
    KTEST_EXPECT_EQ(tc, slot->wait_deadline, 0u);
    KTEST_EXPECT_EQ(tc, slot->wait_deadline_set, 0u);
}

static void test_sched_mark_exit_wakes_child_state_waiters(ktest_case_t *tc)
{
    static process_t child;
    static process_t waiter;

    sched_init();
    init_dummy_proc(&child);
    init_dummy_proc(&waiter);

    int child_pid = sched_add(&child);
    KTEST_ASSERT_TRUE(tc, child_pid >= 1);

    process_t *running = sched_bootstrap();
    KTEST_ASSERT_NOT_NULL(tc, running);

    int waiter_pid = sched_add(&waiter);
    KTEST_ASSERT_TRUE(tc, waiter_pid >= 1);

    process_t *waiter_slot = sched_find_pid((uint32_t)waiter_pid);
    KTEST_ASSERT_NOT_NULL(tc, waiter_slot);

    queue_blocked_proc(&running->state_waiters, waiter_slot);

    sched_mark_exit();

    KTEST_EXPECT_EQ(tc, running->state, PROC_ZOMBIE);
    KTEST_EXPECT_EQ(tc, waiter_slot->state, PROC_READY);
    KTEST_EXPECT_NULL(tc, waiter_slot->wait_queue);
    KTEST_EXPECT_NULL(tc, waiter_slot->wait_next);
    KTEST_EXPECT_NULL(tc, running->state_waiters.head);
    KTEST_EXPECT_NULL(tc, running->state_waiters.tail);
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

static ktest_case_t cases[] = {
    KTEST_CASE(test_no_running_process_after_init),
    KTEST_CASE(test_add_returns_valid_pid),
    KTEST_CASE(test_add_two_pids_are_unique),
    KTEST_CASE(test_table_full_returns_error),
    KTEST_CASE(test_current_pid_zero_after_init),
    KTEST_CASE(test_sched_bootstrap_null_when_empty),
    KTEST_CASE(test_sched_bootstrap_finds_ready),
    KTEST_CASE(test_sched_add_unique_pids_three),
    KTEST_CASE(test_waitpid_reaps_zombie),
    KTEST_CASE(test_wait_queue_init_clears_links),
    KTEST_CASE(test_sched_wake_all_wakes_only_queued_blocked_procs),
    KTEST_CASE(test_sched_send_signal_wakes_blocked_process_and_unqueues_it),
    KTEST_CASE(test_sched_tick_wakes_timed_blocked_process),
    KTEST_CASE(test_sched_mark_exit_wakes_child_state_waiters),
};

static ktest_suite_t suite = KTEST_SUITE("sched", cases);

ktest_suite_t *ktest_suite_sched(void) { return &suite; }
