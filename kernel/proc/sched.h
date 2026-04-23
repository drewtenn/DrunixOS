/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SCHED_H
#define SCHED_H

#include "process.h"
#include <stdint.h>

/* Maximum number of concurrent processes (shell + up to 7 children). */
#define MAX_PROCS 8

/* PIT frequency programmed in idt.c: 1193182 / 11932 ≈ 100 Hz. */
#define SCHED_HZ 100u

/*
 * sched_init: zero the process table.  Call once at kernel startup before
 * any process is created.
 */
void sched_init(void);

/*
 * sched_bootstrap: find the first READY process, promote it to RUNNING,
 * and return a pointer to its process descriptor in the table.  Call this
 * once at kernel startup to get a pointer suitable for process_launch().
 * Returns NULL if no READY process exists.
 */
process_t *sched_bootstrap(void);

/*
 * sched_add: copy the filled-in process descriptor into an empty slot in the
 * process table, assign a PID, and mark the process PROC_READY.
 *
 * Returns the new PID (>= 1) on success, or -1 if the table is full.
 */
int sched_add(process_t *proc);

/*
 * sched_peek_next_tid: return the task ID that the next successful sched_add()
 * will assign, without consuming it.
 */
uint32_t sched_peek_next_tid(void);

/*
 * sched_force_remove_task: remove a READY task that has not run yet and
 * release the resources acquired for it. Used to roll back clone setup after
 * sched_add() has made the child visible.
 */
int sched_force_remove_task(uint32_t tid);

/*
 * sched_exec_current: replace the currently running process descriptor with
 * `replacement` and immediately context-switch onto its freshly built kernel
 * stack. Does not return.
 */
void sched_exec_current(process_t *replacement);

/*
 * sched_current: return a pointer to the PROC_RUNNING entry in the process
 * table, or NULL if no process is currently running.
 */
process_t *sched_current(void);

/*
 * sched_current_pid: return the PID of the running process, or 0 if none.
 * Safe to call from the exception handler without pulling in process.h.
 */
uint32_t sched_current_pid(void);

/*
 * sched_current_tid / sched_current_tgid: return the task ID and thread-group
 * ID of the running task, or 0 when no task is running.
 */
uint32_t sched_current_tid(void);
uint32_t sched_current_tgid(void);

/*
 * sched_current_group: return the running task's thread group, or NULL.
 */
task_group_t *sched_current_group(void);

/*
 * sched_current_ppid: return the parent PID of the running process, or 0.
 */
uint32_t sched_current_ppid(void);

/*
 * sched_mark_exit: mark the current process as PROC_ZOMBIE and wake any
 * process that was waiting for it.  Does NOT free the kernel stack (that
 * happens in sched_add when the slot is reused, after we have safely
 * switched away from that stack).
 */
void sched_mark_exit(void);

/*
 * sched_mark_group_exit: request exit for every task in the current task's
 * thread group, then exit the current task with the supplied status code.
 */
void sched_mark_group_exit(uint32_t status);

/*
 * sched_mark_signaled: mark the current process as a zombie due to signal
 * termination.  `dumped_core` sets the Linux-compatible 0x80 core bit in the
 * stored wait status when non-zero.
 */
void sched_mark_signaled(int sig, int dumped_core);

/*
 * sched_set_exit_status: store the exit code in the current process before
 * calling sched_mark_exit().  The value is read by sched_wait().
 */
void sched_set_exit_status(uint32_t status);

/*
 * sched_tick: called from the timer IRQ handler; sets the need-switch flag.
 */
void sched_tick(void);

/*
 * sched_ticks: return the global scheduler tick counter.
 */
uint32_t sched_ticks(void);

/*
 * Wait-option flags for sched_waitpid / sys_waitpid.
 */
#define WNOHANG 1   /* return immediately if no child has changed state */
#define WUNTRACED 2 /* also return when a child stops (SIGSTOP/SIGTSTP) */

/*
 * sched_wait: block the current process until the process with the given PID
 * becomes a zombie (exits).  Returns the zombie's exit_status on success, or
 * -1 if the PID is not found.  Uses schedule() to block.
 *
 * Return value is Linux-style encoded: (exit_code << 8).
 */
int sched_wait(uint32_t pid);

/*
 * sched_waitpid: like sched_wait, but supports option flags.
 *
 *   WNOHANG:   return 0 immediately if no child has exited/stopped yet.
 *   WUNTRACED: also return when a child enters PROC_STOPPED.
 *
 * Return value is Linux-style encoded wait status:
 *   Exited:  (exit_code << 8)         — low 7 bits == 0
 *   Signaled: signal | 0x80_if_core   — low 7 bits = signal number
 *   Stopped: (stop_signal << 8) | 0x7F — low 7 bits == 0x7F
 *   0 with WNOHANG: child exists but has not changed state yet
 *  -1: no such process
 */
int sched_waitpid(uint32_t pid, int options);

/*
 * sched_wait_queue_init: initialise a wait queue head.
 */
void sched_wait_queue_init(wait_queue_t *queue);

/*
 * sched_block: block the current process on `queue` until another context
 * calls sched_wake_all() or a signal wakes it.
 */
void sched_block(wait_queue_t *queue);

/*
 * sched_block_until: block the current process until `deadline_tick` or a
 * signal wakes it.
 */
void sched_block_until(uint32_t deadline_tick);

/*
 * sched_wake_all: wake every process queued on `queue`.
 */
void sched_wake_all(wait_queue_t *queue);

/*
 * schedule: the voluntary context-switch entry point.  Picks the next READY
 * process, saves callee-saved regs + ESP via switch_context(), and switches
 * to the next process's kernel stack.
 *
 * Called from:
 *   - schedule_if_needed() (IRQ preemption path, after EOI)
 *   - Blocking syscalls: waitpid, pipe read/write, TTY read, sleep
 *   - SYS_EXIT (zombie → never returns)
 *   - SYS_YIELD
 *
 * Returns when this process is rescheduled.
 */
void schedule(void);

/*
 * schedule_if_needed: called from irq_common (isr.asm) after irq_dispatch
 * returns.  If a context switch is pending (set by sched_tick or a wake
 * function), and the interrupted code was ring 3, calls schedule().
 */
void schedule_if_needed(void);

/*
 * sched_mark_stopped: transition the current process to PROC_STOPPED and
 * store the stop signal in its wait status.  Sends SIGCHLD to the parent
 * and wakes it if it was waiting.  Caller must call schedule() afterwards.
 */
void sched_mark_stopped(int sig);

/*
 * sched_send_signal_to_pgid: send `signum` to every process in the given
 * process group.  No-op if pgid == 0.  Safe to call from IRQ context.
 */
void sched_send_signal_to_pgid(uint32_t pgid, int signum);

/*
 * sched_find_pid: return a pointer to the process_t for the given PID, or
 * NULL if the PID is not found / is a zombie.
 */
process_t *sched_find_pid(uint32_t pid);

/*
 * sched_find_process: return a pointer to the process descriptor for `pid`.
 *
 * If include_zombie is zero, zombie processes are filtered out (matching
 * sched_find_pid).  If non-zero, zombies remain visible so procfs can expose
 * them until they are reaped.
 */
const process_t *sched_find_process(uint32_t pid, int include_zombie);

/*
 * sched_snapshot_pids: copy live PIDs into pid_out in ascending table order.
 *
 * Returns the number of PIDs written.  Zombie processes are included when
 * include_zombie is non-zero.
 */
int sched_snapshot_pids(uint32_t *pid_out, uint32_t max, int include_zombie);

/*
 * sched_snapshot_tgids: copy each visible thread-group ID once into tgid_out.
 */
int sched_snapshot_tgids(uint32_t *tgid_out, uint32_t max, int include_zombie);

/*
 * sched_find_group: return the task group with the given TGID, if any member
 * is visible under the include_zombie policy.
 */
const task_group_t *sched_find_group(uint32_t tgid, int include_zombie);

/*
 * sched_session_has_pgid: returns non-zero when `pgid` names a live process
 * group that belongs to session `sid`.
 */
int sched_session_has_pgid(uint32_t sid, uint32_t pgid);

/*
 * sched_send_signal: set signal `signum` as pending for the process with
 * the given PID.  If the target is blocked it is transitioned to PROC_READY
 * so it can run and receive the signal.  Safe to call from IRQ context.
 * No-op if the PID is not found.
 */
void sched_send_signal(uint32_t pid, int signum);

/*
 * sched_send_sigint_foreground: called by the keyboard driver on Ctrl-C.
 *
 * Delivers Ctrl-C through tty_ctrl_c(tty0), which routes SIGINT to the
 * active foreground process group for that terminal.
 */
void sched_send_sigint_foreground(void);

/*
 * sched_record_user_fault: capture the current process's ring-3 trap frame
 * and queue the corresponding signal for delivery on the exception return
 * path.  The signal is forcibly unmasked so synchronous faults do not spin
 * forever re-entering the same exception.
 */
void sched_record_user_fault(const arch_trap_frame_t *frame,
                             uint32_t cr2,
                             int signum);

/*
 * sched_signal_check: inspect the current process's pending signals and,
 * if a deliverable signal exists, either terminate the process (SIG_DFL) or
 * build a signal frame on the user stack and redirect the saved EIP to the
 * user handler address.
 *
 * `frame_esp` is the value of ESP at the gs slot of the saved register frame
 * (the base of the pusha/segment-register block on the kernel stack).
 * Called from isr.asm immediately before each iret — on both the syscall
 * and IRQ return paths.
 *
 * Returns the frame_esp to use after the call.  If the signal's default
 * action terminates the process, this function calls schedule() to switch
 * away (the zombie's frame is abandoned).  Otherwise it returns frame_esp
 * unchanged.
 */
uint32_t sched_signal_check(uint32_t frame_esp);

#endif
