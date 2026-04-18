/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lib/syscall.h"

#define THREADS 3
#define STACK_SIZE 4096

static volatile int ready[THREADS];
static volatile int done[THREADS];
static volatile int shared_counter;
static int parent_tid_slots[THREADS];
static int child_tid_slots[THREADS];
static unsigned char stacks[THREADS][STACK_SIZE];
static int log_fd = -1;

static int strlen_local(const char *s)
{
    int n = 0;

    while (s[n])
        n++;
    return n;
}

static void write_all(int fd, const char *s)
{
    int len = strlen_local(s);
    int off = 0;

    while (off < len) {
        int n = sys_fwrite(fd, s + off, len - off);
        if (n <= 0)
            return;
        off += n;
    }
}

static void puts_line(const char *s)
{
    sys_write(s);
    sys_write("\n");
    if (log_fd >= 0) {
        write_all(log_fd, s);
        write_all(log_fd, "\n");
    }
}

static void fail(const char *s)
{
    sys_write("THREADTEST FAIL ");
    if (log_fd >= 0)
        write_all(log_fd, "THREADTEST FAIL ");
    puts_line(s);
    sys_exit_group(1);
}

static int worker_index_from_tid_slot(void)
{
    int tid = sys_gettid();

    for (int i = 0; i < THREADS; i++) {
        if (child_tid_slots[i] == tid)
            return i;
    }
    return -1;
}

static void worker_main(void)
{
    int idx = worker_index_from_tid_slot();
    int pid = sys_getpid();
    int tid = sys_gettid();

    if (idx < 0)
        fail("child tid slot missing");
    if (pid <= 0 || tid <= 0)
        fail("bad identity");
    if (tid == pid)
        fail("worker tid equals tgid");

    ready[idx] = 1;
    shared_counter += 10 + idx;
    done[idx] = 1;
    sys_exit(0);
}

static void wait_done(int idx)
{
    int spins = 0;

    while (!done[idx] && spins < 100000) {
        sys_yield();
        spins++;
    }
    if (!done[idx])
        fail("worker timeout");
}

int main(void)
{
    int parent_pid = sys_getpid();
    int parent_tid = sys_gettid();
    unsigned int flags = CLONE_VM | CLONE_FS | CLONE_FILES |
                         CLONE_SIGHAND | CLONE_THREAD |
                         CLONE_PARENT_SETTID | CLONE_CHILD_SETTID |
                         CLONE_CHILD_CLEARTID | SIGCHLD;

    sys_unlink("/dufs/threadtest.log");
    log_fd = sys_create("/dufs/threadtest.log");

    if (parent_pid <= 0 || parent_tid <= 0)
        fail("parent identity");
    if (parent_pid != parent_tid)
        fail("single-thread parent pid/tid mismatch");

    for (int i = 0; i < THREADS; i++) {
        void *stack_top = stacks[i] + STACK_SIZE - 16;
        int tid = sys_clone(flags, stack_top,
                            &parent_tid_slots[i], 0,
                            &child_tid_slots[i]);
        if (tid < 0)
            fail("clone failed");
        if (tid == 0)
            worker_main();
        if (parent_tid_slots[i] != tid)
            fail("parent tid write");
    }

    for (int i = 0; i < THREADS; i++)
        wait_done(i);

    if (shared_counter != 33)
        fail("shared counter");
    for (int i = 0; i < THREADS; i++) {
        if (ready[i] != 1)
            fail("ready flag");
        if (child_tid_slots[i] != 0)
            fail("child tid clear");
    }

    puts_line("THREADTEST PASS");
    if (log_fd >= 0)
        sys_close(log_fd);
    return 0;
}
