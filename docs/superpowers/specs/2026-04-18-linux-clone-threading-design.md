# Linux Clone Threading Design

Date: 2026-04-18

## Context

Drunix currently has a preemptive single-CPU scheduler built around process
descriptors, wait queues, signals, job control, ELF loading, copy-on-write
`fork`, and per-process virtual-memory bookkeeping. A runnable entity is also
the unit of process identity: `getpid` and `gettid` both return the same PID,
`set_tid_address` only returns that PID, and there is no `clone` syscall.

The goal is to add user-space multi-threading through Linux i386 `clone(2)`,
not kernel-only threads and not SMP. The first design should be broad enough to
model meaningful Linux clone flag combinations instead of only accepting one
pthread-shaped flag set.

## Goals

- Add Linux i386 `SYS_CLONE` as syscall number 120.
- Split runnable task identity from process/thread-group identity.
- Make `gettid` return a unique task ID and `getpid` return the thread-group
  ID.
- Support broad Linux-ish `clone` flag behavior for address space, file table,
  filesystem state, signal handlers, TLS, TID pointers, and exit signal.
- Preserve existing `fork`, `vfork`, `execve`, shell, signal, wait, and job
  control behavior while moving them onto the new model.
- Implement closer Linux-style thread-group signal, exit, and wait semantics in
  the first pass rather than deferring those fundamentals.
- Add a small userland clone/thread test program to prove that multiple user
  threads can run in one address space.
- Keep the system single-CPU. This feature is user threading, not SMP.

## Non-Goals

- Booting or scheduling on multiple CPU cores.
- Kernel worker threads as a standalone feature.
- Full namespace, cgroup, ptrace, seccomp, capability, or scheduler-policy
  support.
- A complete pthread library in the first pass.
- Robust futex lists or a full futex syscall implementation.
- Exact reproduction of every Linux thread-group edge case before Drunix has
  tests that need it.

## Chosen Approach

Use a layered compatibility model.

Keep `process_t` as the scheduler's concrete runnable descriptor during the
first implementation, but reinterpret it as a task descriptor. Then split the
state that `clone` can share into refcounted subobjects: address space, file
descriptor table, filesystem state, signal actions, and thread-group metadata.

This avoids a big-bang rewrite while still moving toward a real task model.
Each resource object can be introduced, tested, and wired through existing
`fork`, `exec`, and scheduler paths before `clone` depends on it fully.

## Task Model

Every schedulable entity is a task. A task owns:

- unique TID,
- kernel stack,
- saved kernel ESP and register context,
- task state,
- wait-queue links and timeout state,
- FPU/SSE state,
- per-task TLS descriptor state,
- per-task signal mask,
- per-task pending thread-directed signals,
- clear-child-tid pointer for `set_tid_address` and `CLONE_CHILD_CLEARTID`.

The current `process_t` can carry these fields initially. Naming can remain
transitional during the implementation, but code should move toward treating it
as the scheduler task slot.

## Thread Groups

A new thread-group object represents what userland sees as a process. It owns:

- TGID/PID returned by `getpid`,
- group leader TID,
- parent relationship used by wait and exit notification,
- process group, session, and controlling TTY identity,
- group-exit state and exit status,
- configured exit signal for non-thread clone children,
- process-directed pending signal queue,
- child reaping state,
- member task count and task membership.

`CLONE_THREAD` creates a new task in the caller's thread group. Without
`CLONE_THREAD`, `clone` creates a new thread group whose leader is the child
task. Existing `fork` is equivalent to creating a new task in a new thread
group with duplicated resources and copy-on-write address space.

## Shared Resource Objects

Resources that `clone` can share become refcounted objects.

### Address Space

The address-space object owns:

- page directory physical address,
- VMA array and count,
- heap, stack, and mmap placement metadata,
- image start/end metadata,
- process name and argument summary used for procfs and core dumps.

`CLONE_VM` shares this object. Without `CLONE_VM`, the child receives a
copy-on-write clone of the caller's address space, preserving existing `fork`
behavior.

### File Descriptor Table

The fd-table object owns `open_files[MAX_FDS]`.

`CLONE_FILES` shares this object. Without `CLONE_FILES`, the table is
duplicated, with pipe, VFS, and procfs references adjusted so closing either
side remains safe. Existing fd close and `process_close_all_fds` logic should
move behind fd-table helpers before broad clone support depends on it.

### Filesystem State

The filesystem-state object owns cwd and umask.

`CLONE_FS` shares this object. Without `CLONE_FS`, cwd and umask are copied.

### Signal Actions

The signal-actions object owns `sig_handlers[NSIG]`.

`CLONE_SIGHAND` shares this object and requires `CLONE_VM`, matching Linux's
important invariant. Without `CLONE_SIGHAND`, dispositions are copied.

Each task still owns its own signal mask and thread-directed pending set.

## Clone Syscall

Add `SYS_CLONE` to `kernel/proc/syscall.h`:

```c
#define SYS_CLONE 120
```

The Linux i386 raw syscall form used by Drunix is:

```c
clone(flags, child_stack, parent_tidptr, tls, child_tidptr)
```

The parent receives the child TID. The child starts at the same user EIP with
`EAX = 0`, using `child_stack` as its user ESP when supplied. The child gets a
fresh kernel stack and a scheduler frame shaped like the existing `fork` child
frame.

Flag validation:

- `CLONE_THREAD` requires `CLONE_SIGHAND`.
- `CLONE_SIGHAND` requires `CLONE_VM`.
- `CLONE_SETTLS` installs the requested child TLS state before first run.
- `CLONE_PARENT_SETTID` writes the child TID to `parent_tidptr` in the parent.
- `CLONE_CHILD_SETTID` writes the child TID to `child_tidptr` in the child
  address space before the child runs.
- `CLONE_CHILD_CLEARTID` stores `child_tidptr` as the child's clear-child-tid
  pointer.
- The low byte of `flags` is the exit signal for non-`CLONE_THREAD` children.

Unsupported flags should fail cleanly with `-1` and log the unsupported mask.
The first implementation should not silently accept namespace, ptrace, vfork
suspension, robust futex, or unrelated flags.

## Identity Syscalls

`SYS_GETTID` returns the current task's TID.

`SYS_GETPID` returns the current task's thread-group ID.

`SYS_GETPPID` returns the parent thread-group ID for process-shaped children.

`SYS_SET_TID_ADDRESS` stores the supplied pointer in the current task's
clear-child-tid field and returns the current TID.

## Signals

Signals split into two pending queues:

- task-directed pending signals on each task,
- process-directed pending signals on the thread group.

Signal dispositions live in the signal-actions object. Tasks have independent
signal masks. Delivery still occurs at the existing safe point before returning
to ring 3, but `sched_signal_check` must consider both task-directed and
process-directed pending signals.

Process-directed delivery selects an eligible task in the group. Prefer a task
that is not masking the signal and can be woken if it is interruptibly blocked.
Thread-directed delivery targets a specific TID. `kill(pid, sig)` uses
thread-group identity for positive PIDs. Exact TID-targeted delivery can be
added later through `tgkill`; it is not required for the first thread test.

Stop and continue behavior remains process-group and thread-group aware:
foreground TTY signals target the foreground process group, and each target
thread group coordinates delivery to its member tasks.

## Exit And Reaping

`SYS_EXIT` terminates the calling task. If it is the last live task in the
thread group, the group becomes waitable, stores the final wait status, and
notifies the parent with the configured exit signal.

If the group leader exits while sibling tasks remain, the group stays alive and
waitability is deferred until the whole group exits.

`SYS_EXIT_GROUP` initiates group exit. All live tasks in the group are marked
for termination or made runnable so they can observe the group-exit state and
leave. Shared resources are released when the last reference disappears.

On task exit, `CLONE_CHILD_CLEARTID` clears the user pointer if present. The
first milestone does not require futex wake support; a userland join test can
poll shared completion state and yield. This keeps the clear-child-tid ABI state
correct while leaving efficient blocking joins for a futex milestone.

## Wait Semantics

`waitpid` reaps child thread groups, not individual sibling tasks.

Non-`CLONE_THREAD` clone children are waitable like normal processes. Their
exit signal comes from the low byte of `flags`.

`CLONE_THREAD` siblings are not waitable through `waitpid`. Userland threading
uses its own join mechanism in shared memory for the first milestone.

Stopped and continued states should remain visible to parents using the current
Linux-compatible wait-status encoding, but the state belongs to the thread
group rather than an arbitrary individual task.

## Exec

`execve` remains process-shaped.

For a single-threaded group, exec replaces the caller's address space as it
does today, while preserving process identity, fd table, cwd, signal mask, and
other documented exec-surviving state.

For a multithreaded group, exec initiates group teardown before replacing the
calling task's image. The first pass can implement this conservatively:
terminate sibling tasks, wait until the caller is the only live task in the
group, then install the new image. This matches Linux's broad behavior without
requiring every leader and signal corner case immediately.

## Procfs And Core Dumps

`/proc/<pid>` should continue to describe thread groups using TGIDs. The first
implementation does not need a Linux-compatible `/proc/<pid>/task/<tid>` tree,
but the design should leave room for it.

Core dumps remain group/process artifacts. A synchronous fault in one task can
terminate the group according to signal disposition and group-exit rules. The
captured crash frame should record both the faulting TID and the TGID after
the task model adds those fields.

## Userland Test Program

Add a small program such as `user/threadtest.c` that calls raw Linux i386
`clone` directly before Drunix grows a full pthread-like library.

The test should cover:

- creating multiple threads with separate user stacks,
- `getpid` returning one TGID across all threads,
- `gettid` returning unique TIDs,
- shared memory mutation through `CLONE_VM`,
- shared fd behavior through `CLONE_FILES`,
- TLS setup through `CLONE_SETTLS` if practical,
- `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`, and `CLONE_CHILD_CLEARTID`,
- one worker calling `exit` without killing the group,
- `exit_group` terminating remaining workers.

The first join mechanism can use a shared `volatile` completion array plus
`sys_yield` polling. A later futex implementation can replace this with a
blocking join and proper child-cleartid wakeup.

## Kernel Tests

Extend in-kernel tests around the new boundaries:

- TID and TGID allocation.
- Thread-group membership and leader behavior.
- Clone flag validation.
- Share vs duplicate behavior for address space, fd table, filesystem state,
  and signal actions.
- Resource reference counts on task exit, group exit, wait reaping, and clone
  failure rollback.
- Child stack and initial frame construction for clone.
- `getpid`, `gettid`, and `set_tid_address`.
- Signal delivery to task-directed and process-directed queues.
- `exit`, `exit_group`, and `waitpid` behavior for thread and non-thread
  clones.

## Error Handling

Invalid clone flag combinations return `-1`.

User pointers for TID writes, TLS descriptors, and child stacks must be
validated through the existing uaccess path. Failed parent-side TID writes
should abort clone before the child is visible. Failed child-side setup should
roll back the child task and release any resource references already acquired.

Resource allocation must be rollback-safe. A failed clone must not leave shared
resource refcounts elevated, process table slots occupied, or partially linked
thread-group membership.

If `execve` in a multithreaded group cannot quiesce sibling tasks safely, it
should fail rather than leaving the group half-replaced.

## Documentation

Update process, syscall, signal, and user-runtime documentation after the code
lands. The docs should explain the distinction between TID and TGID, the clone
flag subset Drunix supports, and the current limitation that joins are polling
until futex support exists.
