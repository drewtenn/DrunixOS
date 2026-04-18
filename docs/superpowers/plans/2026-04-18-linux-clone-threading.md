# Linux Clone Threading Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Linux i386 `clone(2)` based user-space threading with TID/TGID identity, shared resources by clone flag, thread-group signal/exit/wait behavior, and a userland `threadtest` proof program.

**Architecture:** Keep `process_t` as the scheduler's runnable task descriptor during the transition, then split process-shaped state into thread-group and refcounted resource objects. Implement `fork` and existing syscalls on top of the new task/resource model first, then add `SYS_CLONE`, thread-group semantics, and the userland test.

**Tech Stack:** Freestanding 32-bit x86 C kernel, NASM context-switch frames, Linux i386 `int 0x80` syscall ABI, in-kernel KTEST suite, Drunix user C runtime, Makefile/QEMU headless verification.

---

## File Structure

- Modify `kernel/proc/process.h`: keep `process_t` as the runnable task slot; add TID/TGID fields, thread-group/resource pointers, clone flag constants, and task-local clear-child-tid/signal state.
- Create `kernel/proc/task_group.h`: public thread-group type and lifecycle API used by scheduler, syscall, signal, wait, and procfs code.
- Create `kernel/proc/task_group.c`: static thread-group table, membership accounting, process-directed signal queue, group-exit state, and waitable group transitions.
- Create `kernel/proc/resources.h`: public refcounted resource object types and share/duplicate/release APIs for address spaces, fd tables, filesystem state, and signal actions.
- Create `kernel/proc/resources.c`: resource refcounting, copy-on-write address-space duplication wrapper, fd duplication/close helpers, cwd/umask duplication, and signal-action duplication.
- Modify `kernel/proc/process.c`: allocate resources during process creation, fork/clone child construction helpers, exec resource replacement, fd close/release migration, user-space teardown migration.
- Modify `kernel/proc/sched.h`: expose TID/TGID-aware scheduler helpers while keeping compatibility helpers for existing callers.
- Modify `kernel/proc/sched.c`: allocate task IDs, attach tasks to groups, schedule tasks, deliver task/group signals, mark task exit vs group exit, and reap thread groups.
- Modify `kernel/proc/syscall.h`: add `SYS_CLONE` and Linux clone flag constants.
- Modify `kernel/proc/syscall.c`: add clone dispatcher, update identity syscalls, `set_tid_address`, `exit`, `exit_group`, `waitpid`, `kill`, and `execve` behavior.
- Modify `kernel/proc/core.c` and `kernel/proc/mem_forensics.c`: read address-space data through the new resource object and record faulting TID/TGID where exposed.
- Modify `kernel/fs/procfs.c`: render TGID-oriented `/proc/<pid>` views from thread groups.
- Modify `kernel/gui/desktop.c` and related process-list consumers if compile errors reveal direct PID assumptions.
- Modify `kernel/test/test_sched.c`: add scheduler, TID/TGID, group membership, signal, wait, and exit tests.
- Modify `kernel/test/test_process.c`: add resource object, syscall identity, clone flag validation, TLS, TID pointer, and clone frame tests.
- Modify `kernel/test/test_uaccess.c`: keep CoW/fork tests passing through the address-space resource wrapper and add CLONE_VM sharing tests.
- Modify `Makefile`: add `kernel/proc/task_group.o`, `kernel/proc/resources.o`, `threadtest` to `USER_PROGS`, and a `test-threadtest` target.
- Modify `user/Makefile`: add `threadtest` to `PROGS` and `C_PROGS`.
- Modify `user/lib/syscall.h`: add clone flags and wrappers for `sys_clone`, `sys_gettid`, `sys_set_tid_address`, and `sys_exit_group`.
- Modify `user/lib/syscall.c`: implement the userland syscall wrappers.
- Create `user/threadtest.c`: raw clone-based userland threading smoke test.
- Modify `user/linuxabi.c`: add direct ABI probes for `clone` rejection, `gettid`, and `set_tid_address`.
- Modify docs after implementation: `docs/ch15-processes.md`, `docs/ch16-syscalls.md`, `docs/ch19-signals.md`, and `docs/linux-elf-compat.md`.

## Scope Notes

This plan intentionally keeps SMP, futexes, robust futex lists, namespaces, ptrace, and a full pthread library outside the implementation. `CLONE_CHILD_CLEARTID` clears the child TID pointer on task exit, but the first join test polls shared memory and yields instead of blocking on futex wakeups.

Use one commit per task. Do not combine tasks unless all tests for both tasks pass in the same fresh run.

## Verification Commands

Use these commands repeatedly:

```bash
make kernel
make test-headless
make test-linux-abi
make test-threadtest
```

Expected `make test-headless`: `debugcon-ktest.log` contains `KTEST: SUMMARY pass=N fail=0`.

Expected `make test-linux-abi`: `linuxabi.log` contains `LINUXABI SUMMARY passed X/X`, contains no `LINUXABI FAIL`, and `debugcon-linuxabi.log` contains no `unknown syscall` or `Unhandled syscall`.

Expected `make test-threadtest`: `threadtest.log` contains `THREADTEST PASS`, contains no `THREADTEST FAIL`, and `debugcon-threadtest.log` contains no `unknown syscall` or `Unhandled syscall`.

---

## Task 1: Add TID/TGID And Thread-Group Scaffolding

**Files:**
- Create: `kernel/proc/task_group.h`
- Create: `kernel/proc/task_group.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/sched.h`
- Modify: `kernel/proc/sched.c`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/test/test_sched.c`
- Modify: `kernel/test/test_process.c`
- Modify: `Makefile`

- [ ] **Step 1: Write failing scheduler identity tests**

Add these tests to `kernel/test/test_sched.c` after `test_add_two_pids_are_unique`:

```c
static void test_sched_add_assigns_tid_and_default_tgid(ktest_case_t *tc)
{
    sched_init();
    static process_t proc;
    init_dummy_proc(&proc);

    int tid = sched_add(&proc);
    KTEST_ASSERT_TRUE(tc, tid >= 1);

    process_t *slot = sched_find_pid((uint32_t)tid);
    KTEST_ASSERT_NOT_NULL(tc, slot);
    KTEST_EXPECT_EQ(tc, slot->tid, (uint32_t)tid);
    KTEST_EXPECT_EQ(tc, slot->tgid, (uint32_t)tid);
    KTEST_ASSERT_NOT_NULL(tc, slot->group);
    KTEST_EXPECT_EQ(tc, task_group_tgid(slot->group), (uint32_t)tid);
    KTEST_EXPECT_EQ(tc, task_group_live_count(slot->group), 1u);
}

static void test_sched_current_tid_and_tgid_split(ktest_case_t *tc)
{
    sched_init();
    static process_t leader;
    init_dummy_proc(&leader);

    int tid = sched_add(&leader);
    KTEST_ASSERT_TRUE(tc, tid >= 1);
    KTEST_ASSERT_NOT_NULL(tc, sched_bootstrap());

    KTEST_EXPECT_EQ(tc, sched_current_tid(), (uint32_t)tid);
    KTEST_EXPECT_EQ(tc, sched_current_tgid(), (uint32_t)tid);
}
```

Add both cases to the `cases[]` array immediately after `test_add_two_pids_are_unique`.

- [ ] **Step 2: Write failing syscall identity test**

In `kernel/test/test_process.c`, add this check to `test_linux_syscalls_support_busybox_identity_and_rt_sigmask` after the existing `SYS_GETTID` assertion:

```c
    KTEST_EXPECT_EQ(tc, syscall_handler(SYS_GETPID, 0, 0, 0, 0, 0, 0),
                    cur->tgid);
    KTEST_EXPECT_EQ(tc, syscall_handler(SYS_GETTID, 0, 0, 0, 0, 0, 0),
                    cur->tid);
```

The current implementation still aliases PID/TID and lacks `tid`, `tgid`, and `group`, so this must fail to compile before implementation.

- [ ] **Step 3: Run the failing tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile failure mentioning missing `tid`, `tgid`, `group`, `task_group_tgid`, `task_group_live_count`, `sched_current_tid`, or `sched_current_tgid`.

- [ ] **Step 4: Add the thread-group API**

Create `kernel/proc/task_group.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TASK_GROUP_H
#define TASK_GROUP_H

#include "wait.h"
#include <stdint.h>

typedef struct task_group {
    uint32_t used;
    uint32_t refs;
    uint32_t tgid;
    uint32_t leader_tid;
    uint32_t parent_tgid;
    uint32_t pgid;
    uint32_t sid;
    uint32_t tty_id;
    uint32_t live_tasks;
    uint32_t exit_signal;
    uint32_t group_exit;
    uint32_t exit_status;
    uint32_t sig_pending;
    wait_queue_t state_waiters;
} task_group_t;

void task_group_table_init(void);
task_group_t *task_group_create(uint32_t tgid, uint32_t leader_tid,
                                uint32_t parent_tgid, uint32_t pgid,
                                uint32_t sid, uint32_t tty_id,
                                uint32_t exit_signal);
void task_group_get(task_group_t *group);
void task_group_put(task_group_t *group);
void task_group_add_task(task_group_t *group);
void task_group_remove_task(task_group_t *group);
uint32_t task_group_tgid(const task_group_t *group);
uint32_t task_group_leader_tid(const task_group_t *group);
uint32_t task_group_live_count(const task_group_t *group);
void task_group_set_process_signal(task_group_t *group, int signum);
uint32_t task_group_take_process_signal(task_group_t *group, uint32_t blocked);

#endif
```

Create `kernel/proc/task_group.c`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "task_group.h"
#include "sched.h"
#include "kstring.h"

static task_group_t groups[MAX_PROCS];

void task_group_table_init(void)
{
    k_memset(groups, 0, sizeof(groups));
}

task_group_t *task_group_create(uint32_t tgid, uint32_t leader_tid,
                                uint32_t parent_tgid, uint32_t pgid,
                                uint32_t sid, uint32_t tty_id,
                                uint32_t exit_signal)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!groups[i].used) {
            task_group_t *g = &groups[i];
            k_memset(g, 0, sizeof(*g));
            g->used = 1;
            g->refs = 1;
            g->tgid = tgid;
            g->leader_tid = leader_tid;
            g->parent_tgid = parent_tgid;
            g->pgid = pgid;
            g->sid = sid;
            g->tty_id = tty_id;
            g->exit_signal = exit_signal;
            sched_wait_queue_init(&g->state_waiters);
            return g;
        }
    }
    return 0;
}

void task_group_get(task_group_t *group)
{
    if (group)
        group->refs++;
}

void task_group_put(task_group_t *group)
{
    if (!group || group->refs == 0)
        return;
    group->refs--;
    if (group->refs == 0)
        k_memset(group, 0, sizeof(*group));
}

void task_group_add_task(task_group_t *group)
{
    if (group)
        group->live_tasks++;
}

void task_group_remove_task(task_group_t *group)
{
    if (group && group->live_tasks > 0)
        group->live_tasks--;
}

uint32_t task_group_tgid(const task_group_t *group)
{
    return group ? group->tgid : 0;
}

uint32_t task_group_leader_tid(const task_group_t *group)
{
    return group ? group->leader_tid : 0;
}

uint32_t task_group_live_count(const task_group_t *group)
{
    return group ? group->live_tasks : 0;
}

void task_group_set_process_signal(task_group_t *group, int signum)
{
    if (!group || signum < 1 || signum >= NSIG)
        return;
    group->sig_pending |= (1u << signum);
}

uint32_t task_group_take_process_signal(task_group_t *group, uint32_t blocked)
{
    uint32_t deliverable;
    if (!group)
        return 0;
    deliverable = group->sig_pending & ~blocked;
    for (int i = 1; i < NSIG; i++) {
        uint32_t bit = 1u << i;
        if (deliverable & bit) {
            group->sig_pending &= ~bit;
            return (uint32_t)i;
        }
    }
    return 0;
}
```

- [ ] **Step 5: Add task identity fields**

In `kernel/proc/process.h`, include the thread-group header:

```c
#include "task_group.h"
```

Add these fields immediately before the existing `pid` field in `process_t`:

```c
    uint32_t     tid;           /* scheduler task ID */
    uint32_t     tgid;          /* thread-group ID returned by getpid */
    task_group_t *group;        /* owning thread group */
```

Keep the existing `pid` field for compatibility during the transition. It mirrors `tid` until later cleanup.

- [ ] **Step 6: Wire scheduler initialization and add helpers**

In `kernel/proc/sched.h`, add:

```c
uint32_t sched_current_tid(void);
uint32_t sched_current_tgid(void);
task_group_t *sched_current_group(void);
```

In `kernel/proc/sched.c`, call `task_group_table_init()` from `sched_init()` after the process table is cleared.

In `sched_add()`, assign identity like this before copying into `proc_table[i]`:

```c
            proc->tid = g_next_pid++;
            proc->pid = proc->tid;
            if (proc->tgid == 0)
                proc->tgid = proc->tid;
            if (!proc->group) {
                uint32_t parent_tgid = proc->parent_pid;
                proc->group = task_group_create(proc->tgid, proc->tid,
                                                parent_tgid,
                                                proc->pgid ? proc->pgid : proc->tgid,
                                                proc->sid ? proc->sid : proc->tgid,
                                                proc->tty_id,
                                                SIGCHLD);
                if (!proc->group)
                    return -1;
            } else {
                task_group_get(proc->group);
            }
            task_group_add_task(proc->group);
            proc->pgid = proc->group->pgid;
            proc->sid = proc->group->sid;
            proc->tty_id = proc->group->tty_id;
```

Add helper implementations:

```c
uint32_t sched_current_tid(void)
{
    return g_current ? g_current->tid : 0;
}

uint32_t sched_current_tgid(void)
{
    return g_current ? g_current->tgid : 0;
}

task_group_t *sched_current_group(void)
{
    return g_current ? g_current->group : 0;
}
```

Update `sched_current_pid()` to return `sched_current_tgid()` only after Task 6. For this task, leave it returning the legacy `pid` so existing behavior does not shift before wait/signal code is ready.

- [ ] **Step 7: Add new objects to the build**

In top-level `Makefile`, add `kernel/proc/task_group.o` to `KOBJS` next to `kernel/proc/sched.o`.

- [ ] **Step 8: Run identity tests**

Run:

```bash
make test-headless
```

Expected: all KTEST suites pass with `fail=0`.

- [ ] **Step 9: Commit**

Run:

```bash
git add Makefile kernel/proc/process.h kernel/proc/sched.h kernel/proc/sched.c kernel/proc/task_group.h kernel/proc/task_group.c kernel/test/test_sched.c kernel/test/test_process.c
git commit -m "feat: add task group identity scaffolding"
```

---

## Task 2: Introduce Refcounted Resource Objects

**Files:**
- Create: `kernel/proc/resources.h`
- Create: `kernel/proc/resources.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/proc/core.c`
- Modify: `kernel/proc/mem_forensics.c`
- Modify: `kernel/fs/procfs.c`
- Modify: `kernel/test/test_process.c`
- Modify: `kernel/test/test_uaccess.c`
- Modify: `Makefile`

- [ ] **Step 1: Write failing resource tests**

Add to `kernel/test/test_process.c` before the syscall tests:

```c
static void test_process_resources_start_with_single_refs(ktest_case_t *tc)
{
    static process_t proc;
    process_t *cur = start_syscall_test_process(&proc);
    KTEST_ASSERT_NOT_NULL(tc, cur);

    KTEST_ASSERT_NOT_NULL(tc, cur->as);
    KTEST_ASSERT_NOT_NULL(tc, cur->files);
    KTEST_ASSERT_NOT_NULL(tc, cur->fs_state);
    KTEST_ASSERT_NOT_NULL(tc, cur->sig_actions);
    KTEST_EXPECT_EQ(tc, cur->as->refs, 1u);
    KTEST_EXPECT_EQ(tc, cur->files->refs, 1u);
    KTEST_EXPECT_EQ(tc, cur->fs_state->refs, 1u);
    KTEST_EXPECT_EQ(tc, cur->sig_actions->refs, 1u);

    stop_syscall_test_process(cur);
}

static void test_process_resource_get_put_tracks_refs(ktest_case_t *tc)
{
    static process_t proc;
    process_t *cur = start_syscall_test_process(&proc);
    KTEST_ASSERT_NOT_NULL(tc, cur);

    proc_resource_get_all(cur);
    KTEST_EXPECT_EQ(tc, cur->as->refs, 2u);
    KTEST_EXPECT_EQ(tc, cur->files->refs, 2u);
    KTEST_EXPECT_EQ(tc, cur->fs_state->refs, 2u);
    KTEST_EXPECT_EQ(tc, cur->sig_actions->refs, 2u);

    proc_resource_put_all(cur);
    KTEST_EXPECT_EQ(tc, cur->as->refs, 1u);
    KTEST_EXPECT_EQ(tc, cur->files->refs, 1u);
    KTEST_EXPECT_EQ(tc, cur->fs_state->refs, 1u);
    KTEST_EXPECT_EQ(tc, cur->sig_actions->refs, 1u);

    stop_syscall_test_process(cur);
}
```

Add both cases to `cases[]` before `test_linux_syscalls_fill_uname_time_and_fstat64`.

- [ ] **Step 2: Run the failing tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile failure for missing `as`, `files`, `fs_state`, `sig_actions`, or `proc_resource_get_all`.

- [ ] **Step 3: Add resource object types**

Create `kernel/proc/resources.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PROC_RESOURCES_H
#define PROC_RESOURCES_H

#include "process.h"
#include "vma.h"
#include <stdint.h>

typedef struct proc_address_space {
    uint32_t refs;
    uint32_t pd_phys;
    uint32_t entry;
    uint32_t user_stack;
    uint32_t heap_start;
    uint32_t brk;
    uint32_t image_start;
    uint32_t image_end;
    uint32_t stack_low_limit;
    vm_area_t vmas[PROCESS_MAX_VMAS];
    uint32_t vma_count;
    char name[16];
    char psargs[80];
} proc_address_space_t;

typedef struct proc_fd_table {
    uint32_t refs;
    file_handle_t open_files[MAX_FDS];
} proc_fd_table_t;

typedef struct proc_fs_state {
    uint32_t refs;
    uint32_t umask;
    char cwd[4096];
} proc_fs_state_t;

typedef struct proc_sig_actions {
    uint32_t refs;
    uint32_t handlers[NSIG];
} proc_sig_actions_t;

int proc_resource_init_fresh(process_t *proc);
int proc_resource_clone_for_fork(process_t *child, const process_t *parent);
void proc_resource_get_all(process_t *proc);
void proc_resource_put_all(process_t *proc);
void proc_fd_table_close_all(proc_fd_table_t *files);
int proc_fd_table_dup(proc_fd_table_t **out, const proc_fd_table_t *src);
int proc_fs_state_dup(proc_fs_state_t **out, const proc_fs_state_t *src);
int proc_sig_actions_dup(proc_sig_actions_t **out, const proc_sig_actions_t *src);

#endif
```

If including `process.h` from `resources.h` creates a cycle, move resource typedefs into `process.h` and keep only function declarations in `resources.h`. The resulting public field names must stay exactly `as`, `files`, `fs_state`, and `sig_actions`.

- [ ] **Step 4: Add resource pointers to `process_t`**

In `kernel/proc/process.h`, include `resources.h` after the `file_handle_t` definition or forward declare these structs before `process_t`:

```c
typedef struct proc_address_space proc_address_space_t;
typedef struct proc_fd_table proc_fd_table_t;
typedef struct proc_fs_state proc_fs_state_t;
typedef struct proc_sig_actions proc_sig_actions_t;
```

Add to `process_t` before the legacy address-space fields:

```c
    proc_address_space_t *as;
    proc_fd_table_t      *files;
    proc_fs_state_t      *fs_state;
    proc_sig_actions_t   *sig_actions;
```

Keep legacy embedded fields for this task. Mirror data into resources first; migrate direct users in later steps.

- [ ] **Step 5: Implement resource allocation and refcounts**

Create `kernel/proc/resources.c` with allocation helpers backed by `kmalloc`/`kfree`. Implement these exact semantics:

```c
int proc_resource_init_fresh(process_t *proc)
{
    proc->as = kmalloc(sizeof(*proc->as));
    proc->files = kmalloc(sizeof(*proc->files));
    proc->fs_state = kmalloc(sizeof(*proc->fs_state));
    proc->sig_actions = kmalloc(sizeof(*proc->sig_actions));
    if (!proc->as || !proc->files || !proc->fs_state || !proc->sig_actions) {
        proc_resource_put_all(proc);
        return -1;
    }
    k_memset(proc->as, 0, sizeof(*proc->as));
    k_memset(proc->files, 0, sizeof(*proc->files));
    k_memset(proc->fs_state, 0, sizeof(*proc->fs_state));
    k_memset(proc->sig_actions, 0, sizeof(*proc->sig_actions));
    proc->as->refs = 1;
    proc->files->refs = 1;
    proc->fs_state->refs = 1;
    proc->sig_actions->refs = 1;
    return 0;
}
```

`proc_resource_get_all()` increments all non-null resource refs. `proc_resource_put_all()` decrements each non-null resource, releases address-space pages only when `as->refs` reaches zero, closes fds only when `files->refs` reaches zero, and frees objects whose refcount reaches zero. Set the process pointers to null after putting them.

- [ ] **Step 6: Mirror process creation into resource objects**

In `process_create_file()`, call `proc_resource_init_fresh(proc)` before filling process metadata. After the existing fields are assigned, mirror them:

```c
    proc->as->pd_phys = proc->pd_phys;
    proc->as->entry = proc->entry;
    proc->as->user_stack = proc->user_stack;
    proc->as->heap_start = proc->heap_start;
    proc->as->brk = proc->brk;
    proc->as->image_start = proc->image_start;
    proc->as->image_end = proc->image_end;
    proc->as->stack_low_limit = proc->stack_low_limit;
    proc->as->vma_count = proc->vma_count;
    k_memcpy(proc->as->vmas, proc->vmas, sizeof(proc->vmas));
    k_memcpy(proc->as->name, proc->name, sizeof(proc->name));
    k_memcpy(proc->as->psargs, proc->psargs, sizeof(proc->psargs));
    k_memcpy(proc->files->open_files, proc->open_files, sizeof(proc->open_files));
    proc->fs_state->umask = proc->umask;
    k_memcpy(proc->fs_state->cwd, proc->cwd, sizeof(proc->cwd));
    for (int i = 0; i < NSIG; i++)
        proc->sig_actions->handlers[i] = proc->sig_handlers[i];
```

On every existing `return -N` after resource allocation, call `proc_resource_put_all(proc)` before returning.

- [ ] **Step 7: Add objects to the build**

In `Makefile`, add `kernel/proc/resources.o` to `KOBJS` next to `kernel/proc/process.o`.

- [ ] **Step 8: Run resource tests**

Run:

```bash
make test-headless
```

Expected: all KTEST suites pass with `fail=0`.

- [ ] **Step 9: Commit**

Run:

```bash
git add Makefile kernel/proc/process.h kernel/proc/process.c kernel/proc/resources.h kernel/proc/resources.c kernel/test/test_process.c kernel/test/test_uaccess.c
git commit -m "feat: add process resource objects"
```

---

## Task 3: Migrate Address Space, FD, FS, And Signal Users To Resources

**Files:**
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/sched.c`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/proc/uaccess.c`
- Modify: `kernel/mm/fault.c`
- Modify: `kernel/mm/vma.c`
- Modify: `kernel/proc/core.c`
- Modify: `kernel/proc/mem_forensics.c`
- Modify: `kernel/fs/procfs.c`
- Modify: `kernel/drivers/tty.c`
- Modify: `kernel/gui/desktop.c`
- Modify: `kernel/test/test_process.c`
- Modify: `kernel/test/test_uaccess.c`

- [ ] **Step 1: Write failing shared-resource tests**

Add to `kernel/test/test_process.c`:

```c
static void test_fd_table_duplicate_has_independent_offset(ktest_case_t *tc)
{
    static process_t parent;
    static process_t child;
    process_t *cur = start_syscall_test_process(&parent);
    KTEST_ASSERT_NOT_NULL(tc, cur);

    cur->files->open_files[3].type = FD_TYPE_PROCFILE;
    cur->files->open_files[3].u.proc.offset = 11u;
    KTEST_ASSERT_EQ(tc, proc_fd_table_dup(&child.files, cur->files), 0u);
    child.files->open_files[3].u.proc.offset = 22u;

    KTEST_EXPECT_EQ(tc, cur->files->open_files[3].u.proc.offset, 11u);
    KTEST_EXPECT_EQ(tc, child.files->open_files[3].u.proc.offset, 22u);

    proc_resource_put_all(&child);
    stop_syscall_test_process(cur);
}

static void test_fs_state_duplicate_has_independent_cwd(ktest_case_t *tc)
{
    static process_t parent;
    static process_t child;
    process_t *cur = start_syscall_test_process(&parent);
    KTEST_ASSERT_NOT_NULL(tc, cur);

    k_strncpy(cur->fs_state->cwd, "home", sizeof(cur->fs_state->cwd) - 1u);
    KTEST_ASSERT_EQ(tc, proc_fs_state_dup(&child.fs_state, cur->fs_state), 0u);
    k_strncpy(child.fs_state->cwd, "tmp", sizeof(child.fs_state->cwd) - 1u);

    KTEST_EXPECT_TRUE(tc, k_strcmp(cur->fs_state->cwd, "home") == 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(child.fs_state->cwd, "tmp") == 0);

    proc_resource_put_all(&child);
    stop_syscall_test_process(cur);
}
```

Add both to `cases[]`.

- [ ] **Step 2: Run failing compile/test**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile failure or test failure until duplication helpers fully copy fd and fs state.

- [ ] **Step 3: Migrate address-space field reads**

Replace direct address-space users with `proc->as` values:

```c
proc->pd_phys          -> proc->as->pd_phys
proc->entry            -> proc->as->entry
proc->user_stack       -> proc->as->user_stack
proc->heap_start       -> proc->as->heap_start
proc->brk              -> proc->as->brk
proc->image_start      -> proc->as->image_start
proc->image_end        -> proc->as->image_end
proc->stack_low_limit  -> proc->as->stack_low_limit
proc->vmas             -> proc->as->vmas
proc->vma_count        -> proc->as->vma_count
proc->name             -> proc->as->name
proc->psargs           -> proc->as->psargs
```

Files that must be migrated in this step: `process.c`, `sched.c`, `syscall.c`, `uaccess.c`, `fault.c`, `vma.c`, `core.c`, `mem_forensics.c`, `procfs.c`, and tests.

When a helper accepts a `process_t *`, keep its signature and use `proc->as` internally. Do not change user-facing syscall prototypes.

- [ ] **Step 4: Migrate fd table users**

Replace fd accesses:

```c
proc->open_files[fd] -> proc->files->open_files[fd]
```

Move close logic from `process_close_all_fds()` into:

```c
void proc_fd_table_close_all(proc_fd_table_t *files);
```

Leave `process_close_all_fds(process_t *proc)` as:

```c
void process_close_all_fds(process_t *proc)
{
    if (proc && proc->files)
        proc_fd_table_close_all(proc->files);
}
```

- [ ] **Step 5: Migrate cwd and umask users**

Replace:

```c
proc->cwd   -> proc->fs_state->cwd
proc->umask -> proc->fs_state->umask
```

Files that must be migrated: `process.c`, `syscall.c`, `procfs.c`, shell-facing syscall helpers, and tests.

- [ ] **Step 6: Migrate signal action users**

Replace signal handler table accesses:

```c
proc->sig_handlers[signum] -> proc->sig_actions->handlers[signum]
```

Keep `sig_pending`, `sig_blocked`, and crash state task-local on `process_t`.

- [ ] **Step 7: Remove legacy duplicated fields after compile passes**

After all callers use resource objects, remove these fields from `process_t`:

```c
pd_phys, entry, user_stack, heap_start, brk, image_start, image_end,
stack_low_limit, vmas, vma_count, name, psargs, open_files, cwd, umask,
sig_handlers
```

Keep `kstack_top`, `kstack_bottom`, `saved_esp`, FPU state, TID/TGID, task state, wait fields, task-local signal fields, TLS fields, and crash state on `process_t`.

- [ ] **Step 8: Run migration tests**

Run:

```bash
make test-headless
make test-linux-abi
```

Expected: KTEST fail count is 0. Linux ABI probe still reports all existing checks passing.

- [ ] **Step 9: Commit**

Run:

```bash
git add kernel/proc kernel/mm kernel/fs/procfs.c kernel/drivers/tty.c kernel/gui/desktop.c kernel/test
git commit -m "refactor: route process state through shared resources"
```

---

## Task 4: Implement Clone Flag Validation And Userland Wrappers

**Files:**
- Modify: `kernel/proc/syscall.h`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/test/test_process.c`
- Modify: `user/lib/syscall.h`
- Modify: `user/lib/syscall.c`
- Modify: `user/linuxabi.c`

- [ ] **Step 1: Add failing kernel clone validation tests**

Add to `kernel/test/test_process.c`:

```c
#define TEST_CLONE_VM             0x00000100u
#define TEST_CLONE_FS             0x00000200u
#define TEST_CLONE_FILES          0x00000400u
#define TEST_CLONE_SIGHAND        0x00000800u
#define TEST_CLONE_PARENT_SETTID  0x00100000u
#define TEST_CLONE_THREAD         0x00010000u
#define TEST_CLONE_SETTLS         0x00080000u
#define TEST_CLONE_CHILD_CLEARTID 0x00200000u
#define TEST_CLONE_CHILD_SETTID   0x01000000u

static void test_clone_rejects_sighand_without_vm(ktest_case_t *tc)
{
    static process_t proc;
    process_t *cur = start_syscall_test_process(&proc);
    KTEST_ASSERT_NOT_NULL(tc, cur);

    KTEST_EXPECT_EQ(tc,
        syscall_handler(SYS_CLONE, TEST_CLONE_SIGHAND | SIGCHLD,
                        USER_STACK_TOP - 0x1000u, 0, 0, 0, 0),
        (uint32_t)-1);

    stop_syscall_test_process(cur);
}

static void test_clone_rejects_thread_without_sighand(ktest_case_t *tc)
{
    static process_t proc;
    process_t *cur = start_syscall_test_process(&proc);
    KTEST_ASSERT_NOT_NULL(tc, cur);

    KTEST_EXPECT_EQ(tc,
        syscall_handler(SYS_CLONE, TEST_CLONE_THREAD | TEST_CLONE_VM | SIGCHLD,
                        USER_STACK_TOP - 0x1000u, 0, 0, 0, 0),
        (uint32_t)-1);

    stop_syscall_test_process(cur);
}
```

Add both cases to `cases[]`.

- [ ] **Step 2: Add failing user wrapper declarations**

In `user/lib/syscall.h`, add clone constants and prototypes:

```c
#define CLONE_VM             0x00000100u
#define CLONE_FS             0x00000200u
#define CLONE_FILES          0x00000400u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_THREAD         0x00010000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u

int sys_clone(unsigned int flags, void *child_stack, int *parent_tid,
              void *tls, int *child_tid);
int sys_gettid(void);
int sys_set_tid_address(int *tidptr);
void sys_exit_group(int code);
```

In `user/linuxabi.c`, add probes that expect clone validation to work:

```c
check_eq("gettid returns current tid", sc0(SYS_GETTID), sc0(SYS_GETPID));
check_eq("set_tid_address returns tid", sc1(SYS_SET_TID_ADDRESS, (long)&tid_slot), sc0(SYS_GETTID));
check_eq("clone rejects CLONE_SIGHAND without CLONE_VM",
         sc5(SYS_CLONE, CLONE_SIGHAND | SIGCHLD, 0, 0, 0, 0), -1);
check_eq("clone rejects CLONE_THREAD without CLONE_SIGHAND",
         sc5(SYS_CLONE, CLONE_THREAD | CLONE_VM | SIGCHLD, 0, 0, 0, 0), -1);
```

Place these near the existing identity/TLS probes and update the expected summary count after the test passes.

- [ ] **Step 3: Run failing build/tests**

Run:

```bash
make KTEST=1 kernel
make -C user linuxabi
```

Expected: kernel build fails until `SYS_CLONE` exists, and user build fails until constants and wrappers exist consistently.

- [ ] **Step 4: Add kernel clone constants**

In `kernel/proc/syscall.h`, add:

```c
#define SYS_CLONE      120

#define CLONE_VM             0x00000100u
#define CLONE_FS             0x00000200u
#define CLONE_FILES          0x00000400u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_THREAD         0x00010000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u
```

- [ ] **Step 5: Add clone flag validation helper**

In `kernel/proc/syscall.c`, add:

```c
#define CLONE_EXIT_SIGNAL_MASK 0xFFu
#define CLONE_SUPPORTED_FLAGS \
    (CLONE_EXIT_SIGNAL_MASK | CLONE_VM | CLONE_FS | CLONE_FILES | \
     CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS | CLONE_PARENT_SETTID | \
     CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID)

static int syscall_clone_validate_flags(uint32_t flags)
{
    uint32_t unsupported = flags & ~CLONE_SUPPORTED_FLAGS;
    if (unsupported) {
        klog_hex("CLONE", "unsupported flags", unsupported);
        return -1;
    }
    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
        return -1;
    if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND))
        return -1;
    return 0;
}
```

- [ ] **Step 6: Add temporary clone syscall dispatcher**

Add:

```c
static uint32_t SYSCALL_NOINLINE syscall_case_clone(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    (void)eax;
    (void)ecx;
    (void)edx;
    (void)esi;
    (void)edi;
    (void)ebp;
    if (syscall_clone_validate_flags(ebx) != 0)
        return (uint32_t)-1;
    klog_hex("CLONE", "not yet creating child for flags", ebx);
    return (uint32_t)-1;
}
```

Add `case SYS_CLONE: return syscall_case_clone(...);` to `syscall_handler`.

- [ ] **Step 7: Add user wrappers**

In `user/lib/syscall.c`, add:

```c
int sys_clone(unsigned int flags, void *child_stack, int *parent_tid,
              void *tls, int *child_tid)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(120), "b"(flags), "c"(child_stack), "d"(parent_tid),
          "S"(tls), "D"(child_tid)
        : "memory"
    );
    return r;
}

int sys_gettid(void)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(224)
        : "memory"
    );
    return r;
}

int sys_set_tid_address(int *tidptr)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(258), "b"(tidptr)
        : "memory"
    );
    return r;
}

void sys_exit_group(int code)
{
    __asm__ volatile (
        "int $0x80"
        :: "a"(252), "b"(code)
        : "memory"
    );
    for (;;);
}
```

- [ ] **Step 8: Run validation tests**

Run:

```bash
make test-headless
make test-linux-abi
```

Expected: clone rejection tests pass, Linux ABI summary count is updated and passes, and no unknown syscall appears for `SYS_CLONE`.

- [ ] **Step 9: Commit**

Run:

```bash
git add kernel/proc/syscall.h kernel/proc/syscall.c kernel/test/test_process.c user/lib/syscall.h user/lib/syscall.c user/linuxabi.c
git commit -m "feat: add clone syscall validation"
```

---

## Task 5: Implement Clone Child Creation And Resource Sharing

**Files:**
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/sched.h`
- Modify: `kernel/proc/sched.c`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/proc/resources.c`
- Modify: `kernel/test/test_process.c`
- Modify: `kernel/test/test_sched.c`
- Modify: `kernel/test/test_uaccess.c`

- [ ] **Step 1: Write failing clone creation tests**

Add to `kernel/test/test_process.c`:

```c
static void test_clone_thread_shares_group_and_selected_resources(ktest_case_t *tc)
{
    static process_t seed;
    process_t *parent = start_syscall_test_process(&seed);
    uint32_t flags = TEST_CLONE_VM | TEST_CLONE_FS | TEST_CLONE_FILES |
                     TEST_CLONE_SIGHAND | TEST_CLONE_THREAD | SIGCHLD;
    KTEST_ASSERT_NOT_NULL(tc, parent);

    uint32_t tid = syscall_handler(SYS_CLONE, flags,
                                   USER_STACK_TOP - 0x1000u,
                                   0, 0, 0, 0);
    KTEST_ASSERT_NE(tc, tid, (uint32_t)-1);

    process_t *child = sched_find_pid(tid);
    KTEST_ASSERT_NOT_NULL(tc, child);
    KTEST_EXPECT_EQ(tc, child->tgid, parent->tgid);
    KTEST_EXPECT_EQ(tc, child->group, parent->group);
    KTEST_EXPECT_EQ(tc, child->as, parent->as);
    KTEST_EXPECT_EQ(tc, child->files, parent->files);
    KTEST_EXPECT_EQ(tc, child->fs_state, parent->fs_state);
    KTEST_EXPECT_EQ(tc, child->sig_actions, parent->sig_actions);
    KTEST_EXPECT_EQ(tc, parent->as->refs, 2u);

    sched_init();
}

static void test_clone_process_without_vm_gets_distinct_group_and_as(ktest_case_t *tc)
{
    static process_t seed;
    process_t *parent = start_syscall_test_process(&seed);
    KTEST_ASSERT_NOT_NULL(tc, parent);

    uint32_t tid = syscall_handler(SYS_CLONE, SIGCHLD,
                                   USER_STACK_TOP - 0x1000u,
                                   0, 0, 0, 0);
    KTEST_ASSERT_NE(tc, tid, (uint32_t)-1);

    process_t *child = sched_find_pid(tid);
    KTEST_ASSERT_NOT_NULL(tc, child);
    KTEST_EXPECT_EQ(tc, child->tgid, child->tid);
    KTEST_EXPECT_NE(tc, child->group, parent->group);
    KTEST_EXPECT_NE(tc, child->as, parent->as);

    sched_init();
}
```

Add both cases to `cases[]`.

- [ ] **Step 2: Run failing tests**

Run:

```bash
make test-headless
```

Expected: tests fail because clone validation exists but child creation still returns `-1`.

- [ ] **Step 3: Add clone child builder API**

In `kernel/proc/process.h`, declare:

```c
int process_clone(process_t *child_out, process_t *parent,
                  uint32_t flags, uint32_t child_stack,
                  uint32_t parent_tidptr, uint32_t tls,
                  uint32_t child_tidptr);
```

In `process.c`, implement `process_clone()` by following the existing `process_fork()` frame-copy path, with these differences:

```c
child_out->tid = 0;
child_out->pid = 0;
child_out->tgid = (flags & CLONE_THREAD) ? parent->tgid : 0;
child_out->group = (flags & CLONE_THREAD) ? parent->group : 0;
child_out->parent_pid = (flags & CLONE_THREAD) ? parent->parent_pid : parent->tgid;
child_out->sig_pending = 0;
child_out->sig_blocked = parent->sig_blocked;
child_out->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? child_tidptr : 0;
```

Resource selection rules:

```c
child_out->as = (flags & CLONE_VM) ? parent->as : clone_cow_address_space(parent->as);
child_out->files = (flags & CLONE_FILES) ? parent->files : duplicate_fd_table(parent->files);
child_out->fs_state = (flags & CLONE_FS) ? parent->fs_state : duplicate_fs_state(parent->fs_state);
child_out->sig_actions = (flags & CLONE_SIGHAND) ? parent->sig_actions : duplicate_sig_actions(parent->sig_actions);
```

Increment refcounts for shared resources. If any duplicate allocation fails, drop every resource already acquired and return `-1`.

When `child_stack != 0`, write the child's copied ISR frame user ESP slot to `child_stack`. The copied frame layout uses index 17 from the saved frame base, matching `sched_signal_check`.

- [ ] **Step 4: Add child TLS setup**

If `flags & CLONE_SETTLS`, treat the `tls` argument as an i386 TLS base for the same single user TLS slot Drunix already supports:

```c
child_out->user_tls_base = tls;
child_out->user_tls_limit = 0xFFFFFu;
child_out->user_tls_limit_in_pages = 1;
child_out->user_tls_present = 1;
```

If later testing shows musl passes a `struct user_desc *` instead of a base in this raw calling path, adjust `process_clone()` to copy the descriptor from user memory using the same validation as `set_thread_area`.

- [ ] **Step 5: Add parent/child TID pointer writes**

In `syscall_case_clone()`, after `process_clone()` succeeds but before `sched_add()`:

```c
if ((flags & CLONE_PARENT_SETTID) && parent_tidptr != 0) {
    uint32_t child_tid_preview = sched_peek_next_tid();
    if (uaccess_copy_to_user(parent, parent_tidptr,
                             &child_tid_preview,
                             sizeof(child_tid_preview)) != 0) {
        process_clone_rollback(child);
        return (uint32_t)-1;
    }
}
```

Add `sched_peek_next_tid()` to `sched.h`/`sched.c`, returning the next TID without incrementing it.

After `sched_add(child)` returns `ctid`, handle `CLONE_CHILD_SETTID`:

```c
if ((flags & CLONE_CHILD_SETTID) && child_tidptr != 0) {
    process_t *slot = sched_find_pid((uint32_t)ctid);
    uint32_t tid_value = (uint32_t)ctid;
    if (!slot || uaccess_copy_to_user(slot, child_tidptr,
                                      &tid_value, sizeof(tid_value)) != 0) {
        sched_force_remove_task((uint32_t)ctid);
        return (uint32_t)-1;
    }
}
```

Add `sched_force_remove_task()` as a testable rollback helper that removes a READY never-run task and releases its resources.

- [ ] **Step 6: Wire clone syscall to child creation**

Replace the temporary `syscall_case_clone()` body with:

```c
process_t *parent = sched_current();
process_t *child;
int ctid;

if (!parent || syscall_clone_validate_flags(ebx) != 0)
    return (uint32_t)-1;

child = kmalloc(sizeof(*child));
if (!child)
    return (uint32_t)-1;

if (process_clone(child, parent, ebx, ecx, edx, esi, edi) != 0) {
    kfree(child);
    return (uint32_t)-1;
}

ctid = sched_add(child);
kfree(child);
if (ctid < 0)
    return (uint32_t)-1;
return (uint32_t)ctid;
```

- [ ] **Step 7: Run clone creation tests**

Run:

```bash
make test-headless
```

Expected: clone creation tests pass and existing fork/CoW tests still pass.

- [ ] **Step 8: Commit**

Run:

```bash
git add kernel/proc/process.h kernel/proc/process.c kernel/proc/sched.h kernel/proc/sched.c kernel/proc/syscall.c kernel/proc/resources.c kernel/test
git commit -m "feat: create clone tasks with shared resources"
```

---

## Task 6: Implement Thread-Group Signal, Exit, And Wait Semantics

**Files:**
- Modify: `kernel/proc/task_group.h`
- Modify: `kernel/proc/task_group.c`
- Modify: `kernel/proc/sched.h`
- Modify: `kernel/proc/sched.c`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/test/test_sched.c`
- Modify: `kernel/test/test_process.c`

- [ ] **Step 1: Write failing exit/wait tests**

Add to `kernel/test/test_sched.c`:

```c
static void test_thread_exit_keeps_group_alive_until_last_task(ktest_case_t *tc)
{
    sched_init();
    static process_t leader, worker;
    init_dummy_proc(&leader);
    int leader_tid = sched_add(&leader);
    KTEST_ASSERT_TRUE(tc, leader_tid >= 1);
    process_t *leader_slot = sched_find_pid((uint32_t)leader_tid);
    KTEST_ASSERT_NOT_NULL(tc, leader_slot);

    init_dummy_proc(&worker);
    worker.group = leader_slot->group;
    worker.tgid = leader_slot->tgid;
    int worker_tid = sched_add(&worker);
    KTEST_ASSERT_TRUE(tc, worker_tid >= 1);
    KTEST_EXPECT_EQ(tc, task_group_live_count(leader_slot->group), 2u);

    KTEST_ASSERT_NOT_NULL(tc, sched_bootstrap());
    sched_mark_exit();
    KTEST_EXPECT_EQ(tc, task_group_live_count(leader_slot->group), 1u);
    KTEST_EXPECT_TRUE(tc, leader_slot->group->group_exit == 0);
}

static void test_exit_group_marks_all_group_tasks(ktest_case_t *tc)
{
    sched_init();
    static process_t leader, worker;
    init_dummy_proc(&leader);
    int leader_tid = sched_add(&leader);
    KTEST_ASSERT_TRUE(tc, leader_tid >= 1);
    process_t *leader_slot = sched_find_pid((uint32_t)leader_tid);
    KTEST_ASSERT_NOT_NULL(tc, leader_slot);

    init_dummy_proc(&worker);
    worker.group = leader_slot->group;
    worker.tgid = leader_slot->tgid;
    int worker_tid = sched_add(&worker);
    KTEST_ASSERT_TRUE(tc, worker_tid >= 1);

    KTEST_ASSERT_NOT_NULL(tc, sched_bootstrap());
    sched_mark_group_exit(7);

    process_t *worker_slot = sched_find_pid((uint32_t)worker_tid);
    KTEST_ASSERT_NOT_NULL(tc, worker_slot);
    KTEST_EXPECT_TRUE(tc, leader_slot->group->group_exit != 0);
    KTEST_EXPECT_EQ(tc, worker_slot->state, PROC_READY);
}
```

Add both to `cases[]`.

- [ ] **Step 2: Run failing tests**

Run:

```bash
make KTEST=1 kernel
```

Expected: compile failure for missing `sched_mark_group_exit` or behavior failure until exit semantics are implemented.

- [ ] **Step 3: Split task exit from group exit**

In `sched.h`, add:

```c
void sched_mark_group_exit(uint32_t status);
```

Update `sched_mark_exit()`:

- close only task-local state directly owned by the task,
- clear child TID if set,
- decrement `task_group.live_tasks`,
- mark the task `PROC_ZOMBIE`,
- only wake parent waiters when `live_tasks` becomes zero.

If a task is a `CLONE_THREAD` sibling and not the last task, it must not make the group waitable.

- [ ] **Step 4: Implement `sched_mark_group_exit`**

Add:

```c
void sched_mark_group_exit(uint32_t status)
{
    if (!g_current || !g_current->group)
        return;
    task_group_t *group = g_current->group;
    group->group_exit = 1;
    group->exit_status = (status & 0xFFu) << 8;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED &&
            proc_table[i].state != PROC_ZOMBIE &&
            proc_table[i].group == group) {
            if (&proc_table[i] != g_current)
                proc_table[i].state = PROC_READY;
        }
    }
    sched_mark_exit();
}
```

Then refine `schedule()` and signal-return paths so a task that observes `group->group_exit` exits before returning to user mode.

- [ ] **Step 5: Update syscalls**

In `syscall_case_exit_exit_group()`, branch on `eax`:

```c
if (eax == SYS_EXIT_GROUP)
    sched_mark_group_exit(ebx);
else
    sched_mark_exit();
schedule();
__builtin_unreachable();
```

`SYS_EXIT` exits only the current task. `SYS_EXIT_GROUP` exits the group.

- [ ] **Step 6: Update waitpid to reap thread groups**

Change `sched_waitpid(pid, options)` so `pid` resolves to a thread group TGID for process-shaped waits. It should:

- ignore non-leader `CLONE_THREAD` tasks as wait targets,
- return stopped status from group state,
- return exit status only after `live_tasks == 0`,
- release the group's zombie task slots and resource refs when reaped.

Keep `sched_find_pid(tid)` available for scheduler internals and tests.

- [ ] **Step 7: Update signal delivery**

Change `sched_send_signal(pid, signum)`:

- if `pid` matches a TGID, enqueue process-directed pending signal on that group,
- choose one eligible task in the group and wake it,
- keep exact TID delivery internal for fault-generated or future `tgkill` signals.

Update `sched_signal_check()` to check task-local pending first, then `task_group_take_process_signal(g_current->group, g_current->sig_blocked)`.

- [ ] **Step 8: Run signal/exit/wait tests**

Run:

```bash
make test-headless
make test-linux-abi
```

Expected: KTEST passes, Linux ABI process tests still pass, existing shell/job-control wait statuses remain compatible.

- [ ] **Step 9: Commit**

Run:

```bash
git add kernel/proc/task_group.h kernel/proc/task_group.c kernel/proc/sched.h kernel/proc/sched.c kernel/proc/syscall.c kernel/test/test_sched.c kernel/test/test_process.c
git commit -m "feat: add thread group exit and signal semantics"
```

---

## Task 7: Add Exec, Procfs, And Core-Dump Adjustments

**Files:**
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/sched.c`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/proc/core.c`
- Modify: `kernel/proc/mem_forensics.c`
- Modify: `kernel/fs/procfs.c`
- Modify: `kernel/test/test_vfs.c`
- Modify: `kernel/test/test_process.c`

- [ ] **Step 1: Write failing procfs TGID test**

In `kernel/test/test_vfs.c`, add a process fixture with two tasks in one group and assert `/proc` lists only the TGID once:

```c
static void test_proc_namespace_lists_thread_group_once(ktest_case_t *tc)
{
    static process_t leader;
    static process_t worker;
    char buf[512];

    sched_init();
    init_procfs_layout_process(&leader, 1);
    leader.saved_esp = 1;
    int leader_tid = sched_add(&leader);
    KTEST_ASSERT_TRUE(tc, leader_tid >= 1);
    process_t *leader_slot = sched_find_pid((uint32_t)leader_tid);
    KTEST_ASSERT_NOT_NULL(tc, leader_slot);

    init_procfs_layout_process(&worker, 1);
    worker.saved_esp = 1;
    worker.group = leader_slot->group;
    worker.tgid = leader_slot->tgid;
    int worker_tid = sched_add(&worker);
    KTEST_ASSERT_TRUE(tc, worker_tid >= 1);

    int n = vfs_getdents("proc", buf, sizeof(buf));
    KTEST_ASSERT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "1/") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "2/") == 0);

    sched_init();
}
```

Add to `cases[]`.

- [ ] **Step 2: Run failing procfs test**

Run:

```bash
make test-headless
```

Expected: test fails until procfs snapshots thread groups instead of task slots.

- [ ] **Step 3: Add group snapshot helper**

In `sched.h`:

```c
int sched_snapshot_tgids(uint32_t *tgid_out, uint32_t max, int include_zombie);
const task_group_t *sched_find_group(uint32_t tgid, int include_zombie);
```

Implement in `sched.c` by walking group table or process table and returning each TGID once.

- [ ] **Step 4: Update procfs**

Change `procfs_lookup_process(pid)` to look up the group leader task for TGID `pid`. Change PID directory listing to use `sched_snapshot_tgids()`. Status output should display:

```text
Pid:    <tgid>
Tgid:   <tgid>
Threads:<live_tasks>
```

When rendering fd/maps/vmstat, use the group leader or the first live task in the group.

- [ ] **Step 5: Update exec behavior**

In `syscall_case_execve()`, before replacing resources:

- if current group has more than one live task, set group-exit for siblings,
- wait or spin-yield in kernel until only current remains if all siblings are on task stacks that can exit,
- if safe quiescence cannot be proven in this implementation, return `-1` for multithreaded exec and keep single-thread exec unchanged.

For this plan, choose the conservative branch: return `-1` when `task_group_live_count(cur->group) > 1`. This is explicit, safe, and can be improved after thread tests exist.

- [ ] **Step 6: Update core/mem-forensics metadata**

When crash notes have room for Drunix-specific metadata, include faulting task identity:

```c
fault_note.tid = proc->tid;
fault_note.tgid = proc->tgid;
```

If existing note structs are fixed-size, add a new Drunix note type rather than changing Linux-compatible note layout.

- [ ] **Step 7: Run procfs/exec/core tests**

Run:

```bash
make test-headless
```

Expected: KTEST passes with `/proc` showing thread groups once.

- [ ] **Step 8: Commit**

Run:

```bash
git add kernel/proc kernel/fs/procfs.c kernel/test/test_vfs.c kernel/test/test_process.c
git commit -m "feat: expose thread groups to procfs and exec"
```

---

## Task 8: Add Userland Thread Test And Test Target

**Files:**
- Create: `user/threadtest.c`
- Modify: `user/Makefile`
- Modify: `user/lib/syscall.h`
- Modify: `user/lib/syscall.c`
- Modify: `Makefile`

- [ ] **Step 1: Add failing `threadtest` program**

Create `user/threadtest.c`:

```c
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

static void puts_line(const char *s)
{
    sys_write(s);
    sys_write("\n");
}

static void fail(const char *s)
{
    sys_write("THREADTEST FAIL ");
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
    return 0;
}
```

If `sys_yield()` is not declared in `user/lib/syscall.h`, add `int sys_yield(void);` and implement it as syscall 158 returning the kernel result.

- [ ] **Step 2: Add build entries**

In `user/Makefile`, add `threadtest` to `PROGS` and `C_PROGS`.

In top-level `Makefile`, add `threadtest` to `USER_PROGS`.

- [ ] **Step 3: Add `test-threadtest` target**

In top-level `Makefile`, add `threadtest` to `TEST_SUFFIXES` or add explicit logs. Add:

```make
test-threadtest:
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/threadtest INIT_ARG0=threadtest kernel disk
	$(call prepare_test_images,threadtest,threadtest.log)
	$(call qemu_headless_for,threadtest,30)
	$(PYTHON) tools/dufs_extract.py dufs-threadtest.img threadtest.log threadtest.log
	cat threadtest.log
	grep -q "THREADTEST PASS" threadtest.log
	! grep -q "THREADTEST FAIL" threadtest.log
	! grep -Eq "unknown syscall|Unhandled syscall" debugcon-threadtest.log
```

- [ ] **Step 4: Run failing userland test**

Run:

```bash
make test-threadtest
```

Expected before final clone fixes: build succeeds, boot runs `threadtest`, and the test fails on `clone failed` or a child TID behavior mismatch.

- [ ] **Step 5: Fix clone child entry behavior**

If children return to the instruction after `sys_clone()` but do not enter `worker_main()`, inspect the copied ISR frame in `process_clone()` and verify:

```c
((uint32_t *)child_frame)[11] = 0;          /* EAX */
((uint32_t *)child_frame)[17] = child_stack; /* user ESP */
```

The `sys_clone()` wrapper must use `"S"(tls)` and `"D"(child_tid)` so the kernel receives the fourth and fifth arguments in ESI and EDI.

- [ ] **Step 6: Run userland and kernel verification**

Run:

```bash
make test-threadtest
make test-headless
make test-linux-abi
```

Expected: `THREADTEST PASS`, KTEST `fail=0`, and Linux ABI checks pass with no unknown syscall logs.

- [ ] **Step 7: Commit**

Run:

```bash
git add Makefile user/Makefile user/threadtest.c user/lib/syscall.h user/lib/syscall.c
git commit -m "test: add clone thread userland test"
```

---

## Task 9: Documentation And Final Compatibility Pass

**Files:**
- Modify: `docs/ch15-processes.md`
- Modify: `docs/ch16-syscalls.md`
- Modify: `docs/ch19-signals.md`
- Modify: `docs/linux-elf-compat.md`
- Modify: `README.md`
- Modify: `tools/check_linux_i386_syscall_abi.py`

- [ ] **Step 1: Update syscall checker**

In `tools/check_linux_i386_syscall_abi.py`, add:

```python
"SYS_CLONE": 120,
```

Keep `SYS_GETTID`, `SYS_SET_TID_ADDRESS`, and `SYS_SET_THREAD_AREA` entries unchanged.

- [ ] **Step 2: Update process documentation**

In `docs/ch15-processes.md`, add a section after the `fork` discussion titled `Tasks, Thread Groups, And Clone`. Include these points:

- TID is the scheduler identity.
- TGID is the process identity returned by `getpid`.
- `fork` creates a new thread group.
- `clone` can either create a thread-group sibling or a process-shaped child depending on flags.
- `CLONE_VM` shares address space; without it, CoW clone behavior remains.

- [ ] **Step 3: Update syscall documentation**

In `docs/ch16-syscalls.md`, add `SYS_CLONE (120)` to the syscall table and document accepted flags:

```text
CLONE_VM, CLONE_FS, CLONE_FILES, CLONE_SIGHAND, CLONE_THREAD,
CLONE_SETTLS, CLONE_PARENT_SETTID, CLONE_CHILD_SETTID,
CLONE_CHILD_CLEARTID
```

State that namespace, ptrace, vfork suspension, robust futex, and SMP behavior are outside the supported subset.

- [ ] **Step 4: Update signal documentation**

In `docs/ch19-signals.md`, update signal state prose:

- signal actions are shared when `CLONE_SIGHAND` is used,
- each task has its own signal mask,
- process-directed pending signals live on the thread group,
- task-directed signals live on the task,
- `exit_group` terminates the thread group.

- [ ] **Step 5: Update compatibility docs and README**

In `docs/linux-elf-compat.md`, update the process row to mention `clone` support and current non-goals: futex joins and robust futex lists.

In `README.md`, update the first paragraph's process/scheduler sentence to mention Linux-style `clone` user threads if the feature is stable under `test-threadtest`.

- [ ] **Step 6: Run docs and compatibility verification**

Run:

```bash
make test-headless
make test-linux-abi
make test-threadtest
python3 tools/check_linux_i386_syscall_abi.py
```

Expected: all verification commands exit 0.

- [ ] **Step 7: Commit**

Run:

```bash
git add docs/ch15-processes.md docs/ch16-syscalls.md docs/ch19-signals.md docs/linux-elf-compat.md README.md tools/check_linux_i386_syscall_abi.py
git commit -m "docs: document linux clone threading"
```

---

## Final Verification

- [ ] **Step 1: Run full required checks**

Run:

```bash
make test-headless
make test-linux-abi
make test-threadtest
```

Expected: all three commands exit 0.

- [ ] **Step 2: Inspect relevant logs**

Run:

```bash
grep -q "KTEST.*SUMMARY pass=[0-9][0-9]* fail=0" debugcon-ktest.log
grep -q "LINUXABI SUMMARY passed" linuxabi.log
grep -q "THREADTEST PASS" threadtest.log
! grep -Eq "unknown syscall|Unhandled syscall" debugcon-linuxabi.log
! grep -Eq "unknown syscall|Unhandled syscall" debugcon-threadtest.log
```

Expected: every grep command exits 0.

- [ ] **Step 3: Confirm working tree state**

Run:

```bash
git status --short
```

Expected: clean working tree, or only intentionally untracked runtime logs/images that are already ignored.

