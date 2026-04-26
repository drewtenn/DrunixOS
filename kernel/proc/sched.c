/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * sched.c — round-robin scheduler plus signal, wait, and job-control coordination.
 */

#include "sched.h"
#include "process.h"
#include "arch.h"
#include "resources.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include "uaccess.h"
#include "core.h"
#include <stdint.h>

/* ── Process table ──────────────────────────────────────────────────────── */

static process_t proc_table[MAX_PROCS];
static process_t *g_current = 0; /* pointer into proc_table, or NULL */
static int g_need_switch = 0;
static uint32_t g_next_pid = 1;
static uint32_t g_ticks = 0;

/* ── Internal helpers ───────────────────────────────────────────────────── */

/*
 * sched_pick_next: round-robin from g_current, skipping non-READY slots.
 * Returns NULL if no READY process found.
 */
static process_t *sched_pick_next(void)
{
	int cur_idx = -1;
	if (g_current) {
		for (int i = 0; i < MAX_PROCS; i++) {
			if (&proc_table[i] == g_current) {
				cur_idx = i;
				break;
			}
		}
	}

	/* Search from the slot after current, wrapping around */
	for (int i = 1; i <= MAX_PROCS; i++) {
		int idx = (cur_idx + i) % MAX_PROCS;
		if (proc_table[idx].state == PROC_READY)
			return &proc_table[idx];
	}
	return 0;
}

static process_t *sched_find_slot(uint32_t pid)
{
	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].pid == pid && proc_table[i].state != PROC_UNUSED)
			return &proc_table[i];
	}
	return 0;
}

void sched_wait_queue_init(wait_queue_t *queue)
{
	if (!queue)
		return;
	queue->head = 0;
	queue->tail = 0;
}

static void sched_wait_queue_enqueue(wait_queue_t *queue, process_t *proc)
{
	if (!queue || !proc)
		return;

	proc->wait_next = 0;
	if (queue->tail)
		queue->tail->wait_next = proc;
	else
		queue->head = proc;
	queue->tail = proc;
}

static void sched_wait_queue_remove(wait_queue_t *queue, process_t *proc)
{
	if (!queue || !proc)
		return;

	process_t *prev = 0;
	process_t *cur = queue->head;
	while (cur) {
		if (cur == proc) {
			if (prev)
				prev->wait_next = cur->wait_next;
			else
				queue->head = cur->wait_next;
			if (queue->tail == cur)
				queue->tail = prev;
			break;
		}
		prev = cur;
		cur = cur->wait_next;
	}
}

static void sched_clear_wait(process_t *proc)
{
	if (!proc)
		return;

	if (proc->wait_queue)
		sched_wait_queue_remove(proc->wait_queue, proc);

	proc->wait_queue = 0;
	proc->wait_next = 0;
	proc->wait_deadline = 0;
	proc->wait_deadline_set = 0;
}

static int sched_make_ready(process_t *proc)
{
	if (!proc)
		return 0;

	sched_clear_wait(proc);
	if (proc->state == PROC_BLOCKED) {
		proc->state = PROC_READY;
		return 1;
	}
	return 0;
}

void sched_block(wait_queue_t *queue)
{
	if (!g_current)
		return;

	sched_clear_wait(g_current);
	if (queue) {
		sched_wait_queue_enqueue(queue, g_current);
		g_current->wait_queue = queue;
	}
	g_current->state = PROC_BLOCKED;
	schedule();
}

void sched_block_until(uint32_t deadline_tick)
{
	if (!g_current)
		return;

	sched_clear_wait(g_current);
	g_current->wait_deadline = deadline_tick;
	g_current->wait_deadline_set = 1;
	g_current->state = PROC_BLOCKED;
	schedule();
}

void sched_wake_all(wait_queue_t *queue)
{
	int woke_any = 0;

	if (!queue)
		return;

	process_t *proc = queue->head;
	queue->head = 0;
	queue->tail = 0;

	while (proc) {
		process_t *next = proc->wait_next;
		proc->wait_queue = 0;
		proc->wait_next = 0;
		proc->wait_deadline = 0;
		proc->wait_deadline_set = 0;
		if (proc->state == PROC_BLOCKED) {
			proc->state = PROC_READY;
			woke_any = 1;
		}
		proc = next;
	}

	if (woke_any)
		g_need_switch = 1;
}

static void sched_reap(process_t *zombie)
{
	int had_resources;

	if (!zombie || zombie->state != PROC_ZOMBIE)
		return;

	had_resources =
	    zombie->as || zombie->files || zombie->fs_state || zombie->sig_actions;
	sched_clear_wait(zombie);
	sched_wait_queue_init(&zombie->state_waiters);
	proc_resource_put_all(zombie);
	if (!had_resources)
		process_release_user_space(zombie);
	process_release_kstack(zombie);
	task_group_put(zombie->group);
	zombie->group = 0;
	zombie->state = PROC_UNUSED;
	zombie->pid = 0;
	zombie->tid = 0;
	zombie->tgid = 0;
}

static void sched_reap_group(task_group_t *group)
{
	if (!group)
		return;

	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].group == group && proc_table[i].state == PROC_ZOMBIE)
			sched_reap(&proc_table[i]);
	}
}

static int sched_group_has_live_slots(task_group_t *group)
{
	if (!group)
		return 0;

	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].group == group &&
		    proc_table[i].state != PROC_UNUSED &&
		    proc_table[i].state != PROC_ZOMBIE)
			return 1;
	}
	return 0;
}

static void sched_wake_group_waiters(task_group_t *group)
{
	if (!group)
		return;

	sched_wake_all(&group->state_waiters);
	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].group == group && proc_table[i].state == PROC_ZOMBIE)
			sched_wake_all(&proc_table[i].state_waiters);
	}
}

static process_t *sched_find_group_member(uint32_t tgid)
{
	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].tgid == tgid && proc_table[i].state != PROC_UNUSED)
			return &proc_table[i];
	}
	return 0;
}

static process_t *sched_find_group_signal_recipient(task_group_t *group,
                                                    int signum)
{
	uint32_t bit = 1u << signum;

	if (!group)
		return 0;

	for (int i = 0; i < MAX_PROCS; i++) {
		process_t *proc = &proc_table[i];

		if (proc->group != group || proc->state == PROC_UNUSED ||
		    proc->state == PROC_ZOMBIE)
			continue;
		if ((proc->sig_blocked & bit) != 0 && signum != SIGKILL)
			continue;
		return proc;
	}
	return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void sched_init(void)
{
	for (int i = 0; i < MAX_PROCS; i++)
		proc_table[i].state = PROC_UNUSED;
	task_group_table_init();
	g_current = 0;
	g_need_switch = 0;
	g_next_pid = 1;
	g_ticks = 0;
}

process_t *sched_bootstrap(void)
{
	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].state == PROC_READY) {
			proc_table[i].state = PROC_RUNNING;
			g_current = &proc_table[i];
			return g_current;
		}
	}
	return 0;
}

int sched_add(process_t *proc)
{
	/* Find a free slot. Zombie slots are reaped by the waiter in waitpid(). */
	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].state == PROC_UNUSED) {
			proc->tid = g_next_pid++;
			proc->pid = proc->tid;
			if (proc->tgid == 0)
				proc->tgid = proc->tid;
			if (proc->pgid == 0)
				proc->pgid = proc->tgid;
			if (proc->sid == 0)
				proc->sid = proc->tgid;
			if (!proc->group) {
				proc->group = task_group_create(proc->tgid,
				                                proc->tid,
				                                proc->parent_pid,
				                                proc->pgid,
				                                proc->sid,
				                                proc->tty_id,
				                                SIGCHLD);
				if (!proc->group)
					return -1;
			} else {
				task_group_get(proc->group);
				proc->tgid = task_group_tgid(proc->group);
				proc->pgid = proc->group->pgid;
				proc->sid = proc->group->sid;
				proc->tty_id = proc->group->tty_id;
			}
			task_group_add_task(proc->group);
			proc->state = PROC_READY;
			proc_table[i] = *proc;

			/* Build the initial kernel stack frame for a new (non-forked)
             * process.  process_fork() sets arch_state.context to the child's
             * custom
             * frame; process_create() leaves it 0. */
			if (proc_table[i].arch_state.context == 0)
				process_build_initial_frame(&proc_table[i]);

#ifndef KTEST_ENABLED
			klog_uint("SCHED", "process added, pid", proc_table[i].pid);
#endif
			return (int)proc_table[i].pid;
		}
	}
	return -1;
}

uint32_t sched_peek_next_tid(void)
{
	return g_next_pid;
}

int sched_force_remove_task(uint32_t tid)
{
	process_t *proc = sched_find_slot(tid);

	if (!proc || proc->state != PROC_READY)
		return -1;

	sched_clear_wait(proc);
	proc_resource_put_all(proc);
	process_release_kstack(proc);
	task_group_remove_task(proc->group);
	task_group_put(proc->group);
	k_memset(proc, 0, sizeof(*proc));
	return 0;
}

process_t *sched_current(void)
{
	return g_current;
}

void sched_exec_current(process_t *replacement)
{
	uint32_t tid;
	uint32_t tgid;
	uint32_t pid;
	task_group_t *group;

	if (!g_current || !replacement)
		return;

	tid = g_current->tid;
	tgid = g_current->tgid;
	pid = g_current->pid;
	group = g_current->group;

	*g_current = *replacement;
	kfree(replacement);
	g_current->tid = tid;
	g_current->tgid = tgid;
	g_current->pid = pid;
	g_current->group = group;
	g_current->state = PROC_RUNNING;

	arch_context_prepare(g_current);
	arch_context_switch(
	    (arch_context_t *)0, (arch_context_t)g_current->arch_state.context,
	    g_current->pd_phys);

	for (;;)
		arch_idle_wait();
}

uint32_t sched_current_pid(void)
{
	return g_current ? g_current->pid : 0;
}

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

uint32_t sched_current_ppid(void)
{
	return g_current ? g_current->parent_pid : 0;
}

void sched_set_exit_status(uint32_t status)
{
	/* Linux-style encoding: (exit_code << 8), low 7 bits = 0 for normal exit */
	if (g_current)
		g_current->exit_status = (status & 0xFF) << 8;
}

void sched_mark_exit(void)
{
	if (!g_current)
		return;
	uint32_t exiting_pid = g_current->pid;
	task_group_t *group = g_current->group;
	uint32_t zero = 0;

	if (group && group->group_exit)
		g_current->exit_status = group->exit_status;
	if (g_current->clear_child_tid != 0)
		(void)uaccess_copy_to_user(
		    g_current, g_current->clear_child_tid, &zero, sizeof(zero));
	proc_resource_put_files(g_current);
	task_group_remove_task(group);
	if (group && task_group_live_count(group) == 0) {
		if (!group->group_exit)
			group->exit_status = g_current->exit_status;
		sched_wake_group_waiters(group);
	}
	g_current->state = PROC_ZOMBIE;
	klog_uint("SCHED", "process exited, pid", exiting_pid);
	if (!group || task_group_live_count(group) == 0)
		sched_wake_all(&g_current->state_waiters);
}

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
			if (&proc_table[i] != g_current) {
				sched_clear_wait(&proc_table[i]);
				proc_table[i].state = PROC_READY;
			}
		}
	}
	sched_mark_exit();
}

void sched_mark_signaled(int sig, int dumped_core)
{
	if (!g_current)
		return;

	uint32_t exiting_pid = g_current->pid;
	g_current->exit_status = (uint32_t)(sig & 0x7F);
	if (dumped_core)
		g_current->exit_status |= 0x80u;

	sched_mark_exit();
	klog_uint("SCHED", "process signaled, pid", exiting_pid);
}

void sched_mark_stopped(int sig)
{
	if (!g_current)
		return;
	uint32_t stopped_pid = g_current->pid;
	g_current->state = PROC_STOPPED;
	/* Linux-style encoding: (stop_signal << 8) | 0x7F */
	g_current->exit_status = ((uint32_t)(sig & 0xFF) << 8) | 0x7F;
	klog_uint("SCHED", "process stopped, pid", stopped_pid);

	/* Send SIGCHLD to the parent and wake waitpid sleepers on this child. */
	uint32_t ppid = g_current->parent_pid;
	if (ppid != 0)
		sched_send_signal(ppid, SIGCHLD);
	sched_wake_all(&g_current->state_waiters);
}

int sched_waitpid(uint32_t pid, int options)
{
	/*
     * Loop: block until the target becomes a zombie, stops (if WUNTRACED),
     * or disappears.  schedule() returns when this process is rescheduled.
     */
	for (;;) {
		process_t *target = sched_find_slot(pid);
		task_group_t *group;
		if (!target)
			target = sched_find_group_member(pid);
		if (!target)
			return -1; /* no such process or group */
		if (target->tgid != target->tid)
			return -1; /* non-leader clone thread */
		group = target->group;

		/* Zombie: return its encoded exit status */
		if (target->state == PROC_ZOMBIE &&
		    (!group || !sched_group_has_live_slots(group))) {
			int status = (group && group->group_exit)
			                 ? (int)group->exit_status
			                 : (int)target->exit_status;
			if (group)
				sched_reap_group(group);
			else
				sched_reap(target);
			return status;
		}

		/* Stopped: return encoded stop status if WUNTRACED */
		if ((options & WUNTRACED) && target->state == PROC_STOPPED)
			return (int)target->exit_status;

		/* WNOHANG: don't block, return 0 to indicate "still running" */
		if (options & WNOHANG)
			return 0;

		/* Target is still alive — block until it exits or stops. */
		if (group)
			sched_block(&group->state_waiters);
		else
			sched_block(&target->state_waiters);
		/* Re-check the target's state after waking. */
	}
}

int sched_wait(uint32_t pid)
{
	return sched_waitpid(pid, 0);
}

void sched_tick(void)
{
	g_ticks++;
	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].state == PROC_BLOCKED &&
		    proc_table[i].wait_deadline_set &&
		    (int32_t)(g_ticks - proc_table[i].wait_deadline) >= 0)
			sched_make_ready(&proc_table[i]);
	}
	g_need_switch = 1;
}

uint32_t sched_ticks(void)
{
	return g_ticks;
}

/*
 * schedule: pick the next READY process and switch to it via the arch context
 * handoff.
 *
 * Blocking callers arrive here through sched_block() / sched_block_until(),
 * which queue the current process and transition it to PROC_BLOCKED first.
 * For timer preemption the state is still PROC_RUNNING; schedule() moves
 * it to PROC_READY.  For zombies (SYS_EXIT), the state is PROC_ZOMBIE;
 * schedule() skips saving and the zombie's kernel stack is abandoned.
 */
void schedule(void)
{
	process_t *prev = g_current;
	process_t *next = sched_pick_next();
	if (!next) {
		if (prev && prev->state != PROC_RUNNING) {
			/*
             * No READY task exists right now, but the current task is blocked
             * or has exited. Idle in the kernel until an interrupt wakes a
             * task (keyboard input, pipe writer, SIGCONT, etc.), then retry.
             */
			do {
				arch_idle_wait();
				next = sched_pick_next();
			} while (!next);
		} else {
			return; /* no other READY process — stay on current */
		}
	}

	/*
     * A blocked syscall can wake the current process from an IRQ while it is
     * still inside schedule().  In that case sched_pick_next() may hand us
     * back the same process.  The arch handoff cannot safely switch a stack
     * to itself, because it snapshots old_esp before the callee-saved pushes
     * and would "return" through stale stack contents.
     */
	if (next == prev) {
		prev->state = PROC_RUNNING;
		return;
	}

	/* Save prev's FPU/SSE state (skip for zombies — they won't resume) */
	if (prev && prev->state != PROC_ZOMBIE)
		arch_fpu_save(prev);

	/* Timer preemption: prev is still RUNNING → mark it READY.
     * Blocking callers already set their state before calling schedule(). */
	if (prev && prev->state == PROC_RUNNING)
		prev->state = PROC_READY;

	/* Switch to the next process */
	g_current = next;
	next->state = PROC_RUNNING;
	arch_context_prepare(next);

	/* Swap kernel stacks + page directory.
     * For zombies, pass NULL to skip saving (their stack is abandoned). */
	arch_context_t *save_ptr =
	    (prev && prev->state != PROC_ZOMBIE)
	        ? &prev->arch_state.context
	        : (arch_context_t *)0;
	arch_context_switch(
	    save_ptr, (arch_context_t)next->arch_state.context, next->pd_phys);
	/* Execution resumes here when this process is rescheduled. */
}

void schedule_if_needed(void)
{
	if (!g_need_switch)
		return;
	g_need_switch = 0;

	if (!arch_irq_frame_is_user(arch_current_irq_frame()))
		return;

	schedule();
}

void sched_send_signal_to_pgid(uint32_t pgid, int signum)
{
	if (signum < 1 || signum >= NSIG || pgid == 0)
		return;
	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].pgid == pgid && proc_table[i].state != PROC_UNUSED &&
		    proc_table[i].state != PROC_ZOMBIE) {
			sched_send_signal(proc_table[i].pid, signum);
		}
	}
}

process_t *sched_find_pid(uint32_t pid)
{
	return (process_t *)sched_find_process(pid, 0);
}

const process_t *sched_find_process(uint32_t pid, int include_zombie)
{
	process_t *proc = sched_find_slot(pid);
	if (!proc)
		return 0;
	if (!include_zombie && proc->state == PROC_ZOMBIE)
		return 0;
	return proc;
}

int sched_snapshot_pids(uint32_t *pid_out, uint32_t max, int include_zombie)
{
	uint32_t count = 0;

	if (!pid_out || max == 0)
		return 0;

	for (int i = 0; i < MAX_PROCS && count < max; i++) {
		if (proc_table[i].state == PROC_UNUSED)
			continue;
		if (!include_zombie && proc_table[i].state == PROC_ZOMBIE)
			continue;
		pid_out[count++] = proc_table[i].pid;
	}

	return (int)count;
}

int sched_snapshot_tgids(uint32_t *tgid_out, uint32_t max, int include_zombie)
{
	uint32_t count = 0;

	if (!tgid_out || max == 0)
		return 0;

	for (int i = 0; i < MAX_PROCS && count < max; i++) {
		uint32_t tgid;
		int seen = 0;

		if (proc_table[i].state == PROC_UNUSED)
			continue;
		if (!include_zombie && proc_table[i].state == PROC_ZOMBIE)
			continue;
		tgid = proc_table[i].tgid;
		if (tgid == 0)
			continue;
		for (uint32_t j = 0; j < count; j++) {
			if (tgid_out[j] == tgid) {
				seen = 1;
				break;
			}
		}
		if (!seen)
			tgid_out[count++] = tgid;
	}

	return (int)count;
}

const task_group_t *sched_find_group(uint32_t tgid, int include_zombie)
{
	if (tgid == 0)
		return 0;

	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].tgid != tgid || proc_table[i].state == PROC_UNUSED)
			continue;
		if (!include_zombie && proc_table[i].state == PROC_ZOMBIE)
			continue;
		return proc_table[i].group;
	}
	return 0;
}

int sched_session_has_pgid(uint32_t sid, uint32_t pgid)
{
	if (sid == 0 || pgid == 0)
		return 0;

	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].pgid == pgid && proc_table[i].sid == sid &&
		    proc_table[i].state != PROC_UNUSED &&
		    proc_table[i].state != PROC_ZOMBIE)
			return 1;
	}
	return 0;
}

/* ── Signal subsystem ───────────────────────────────────────────────────── */

void sched_send_signal(uint32_t pid, int signum)
{
	task_group_t *group = 0;
	process_t *recipient = 0;

	if (signum < 1 || signum >= NSIG)
		return;

	for (int i = 0; i < MAX_PROCS; i++) {
		if (proc_table[i].pid == pid && proc_table[i].state != PROC_UNUSED &&
		    proc_table[i].state != PROC_ZOMBIE) {
			if (proc_table[i].tgid == pid)
				group = proc_table[i].group;
			else
				recipient = &proc_table[i];
			break;
		}
	}

	if (!group && !recipient) {
		recipient = sched_find_group_member(pid);
		if (recipient)
			group = recipient->group;
	}

	if (group) {
		task_group_set_process_signal(group, signum);
		recipient = sched_find_group_signal_recipient(group, signum);
	}

	if (recipient) {

		/*
             * SIGCONT: if the process is stopped, wake it immediately.
             * Clear any pending stop signals (they would re-stop it).
             */
		if (signum == SIGCONT) {
			recipient->sig_pending &= ~((1u << SIGSTOP) | (1u << SIGTSTP));
			if (recipient->state == PROC_STOPPED) {
				recipient->state = PROC_READY;
				klog_uint("SCHED", "process continued, pid", pid);
			}
		}

		/*
             * SIGSTOP/SIGTSTP: clear any pending SIGCONT (Linux semantics:
             * stop and continue cancel each other while pending).
             */
		if (signum == SIGSTOP || signum == SIGTSTP) {
			recipient->sig_pending &= ~(1u << SIGCONT);
		}

		if (!group)
			recipient->sig_pending |= (1u << signum);

		/* Wake blocked processes so they get a chance to receive it. */
		if (recipient->state == PROC_BLOCKED ||
		    recipient->state == PROC_STOPPED) {
			/* PROC_STOPPED can only be woken by SIGCONT (above) or
                 * a fatal signal like SIGKILL.  For SIGKILL, force wake. */
			if (recipient->state == PROC_STOPPED && signum != SIGCONT)
				recipient->state =
				    (signum == SIGKILL) ? PROC_READY : PROC_STOPPED;
			else if (recipient->state == PROC_BLOCKED)
				sched_make_ready(recipient);
		}

		g_need_switch = 1;
		return;
	}
}

int sched_process_has_unblocked_signal(const process_t *proc)
{
	uint32_t blocked;

	if (!proc)
		return 0;

	blocked = proc->sig_blocked;
	if ((proc->sig_pending & ~blocked) != 0)
		return 1;
	if (proc->group && (proc->group->sig_pending & ~blocked) != 0)
		return 1;
	return 0;
}

/*
 * sched_send_sigint_foreground: called by the keyboard driver on Ctrl+C.
 * Delegates to tty_ctrl_c(0), which routes SIGINT to the active foreground
 * process group for tty0.
 */
extern void tty_ctrl_c(int tty_idx);

void sched_send_sigint_foreground(void)
{
	tty_ctrl_c(0);
}

void sched_record_user_fault(const arch_trap_frame_t *frame,
                             uint64_t fault_addr,
                             int signum)
{
	if (!g_current)
		return;
	if (signum < 1 || signum >= NSIG)
		return;

	klog_uint("FAULT", "pid", g_current->pid);
	klog_uint("FAULT", "signum", (uint32_t)signum);
	klog_hex("FAULT", "eip", frame ? arch_trap_frame_ip(frame) : 0);
	if ((fault_addr >> 32) != 0)
		klog_uint("FAULT", "fault_addr_hi", (uint32_t)(fault_addr >> 32));
	klog_hex("FAULT", "fault_addr_lo", (uint32_t)fault_addr);

	g_current->crash.valid = 1;
	g_current->crash.signum = (uint32_t)signum;
	g_current->crash.fault_addr = fault_addr;
	k_memcpy(&g_current->crash.frame, frame, sizeof(g_current->crash.frame));

	g_current->sig_pending |= (1u << signum);
	g_current->sig_blocked &= ~(1u << signum);
}

/*
 * build_signal_frame: push a signal invocation frame onto the user stack and
 * redirect the saved EIP/ESP in the kernel frame to call the handler.
 *
 * Signal frame layout (32 bytes, growing downward from old user ESP):
 *
 *   offset  0   ret_addr    → address of trampoline bytes (frame+24)
 *   offset  4   signum      int argument to the handler
 *   offset  8   saved_eip   original user EIP (restored by SYS_SIGRETURN)
 *   offset 12   saved_eflags
 *   offset 16   saved_eax   (syscall return value, or EAX at IRQ point)
 *   offset 20   saved_esp   original user ESP
 *   offset 24   trampoline  8 bytes: mov eax, SYS_SIGRETURN; int 0x80; nop
 *
 * After iret the CPU enters the handler with:
 *   EIP  = handler_va
 *   ESP  = old_esp - 32   (points at ret_addr)
 *   [ESP]   = ret_addr    → trampoline
 *   [ESP+4] = signum
 */
/*
 * is_fatal_default: return 1 if the default action for `sig` is termination.
 * SIGCHLD is ignored by default.  SIGSTOP/SIGTSTP stop (handled separately).
 * SIGCONT continues (handled separately).  Every other signal is fatal.
 */
static int is_fatal_default(int sig)
{
	return (sig != SIGCHLD && sig != SIGSTOP && sig != SIGTSTP &&
	        sig != SIGCONT);
}

static int dumps_core_default(int sig)
{
	return (sig == SIGTRAP || sig == SIGILL || sig == SIGFPE || sig == SIGSEGV);
}

static uint32_t sched_signal_handler(const process_t *proc, int signum)
{
	if (!proc || signum < 0 || signum >= NSIG)
		return SIG_DFL;
	if (proc->sig_actions)
		return proc->sig_actions->handlers[signum];
	return proc->sig_handlers[signum];
}

uintptr_t sched_signal_check(uintptr_t frame_ctx)
{
	arch_trap_frame_t *frame = (arch_trap_frame_t *)frame_ctx;

	if (!g_current)
		return frame_ctx;

	arch_trap_frame_sanitize(g_current, frame);

	if (!arch_irq_frame_is_user(frame_ctx))
		return frame_ctx;

	if (g_current->group && g_current->group->group_exit) {
		sched_mark_exit();
		schedule();
		return frame_ctx;
	}

	uint32_t deliverable = g_current->sig_pending & ~g_current->sig_blocked;

	/* Synchronous faults take precedence over older async pending signals. */
	int signum = -1;
	int group_signal = 0;
	if (g_current->crash.valid && g_current->crash.signum < NSIG &&
	    (deliverable & (1u << g_current->crash.signum))) {
		signum = (int)g_current->crash.signum;
	} else {
		/* Otherwise deliver the lowest-numbered pending signal. */
		for (int i = 1; i < NSIG; i++) {
			if (deliverable & (1u << i)) {
				signum = i;
				break;
			}
		}
	}
	if (signum < 0) {
		uint32_t group_signum = task_group_take_process_signal(
		    g_current->group, g_current->sig_blocked);
		if (group_signum != 0) {
			signum = (int)group_signum;
			group_signal = 1;
		}
	}
	if (signum < 0)
		return frame_ctx;

	/* Clear the pending bit before delivery. */
	if (!group_signal)
		g_current->sig_pending &= ~(1u << signum);

	int from_crash =
	    g_current->crash.valid && g_current->crash.signum == (uint32_t)signum;
	uint32_t handler = sched_signal_handler(g_current, signum);
	if (from_crash && handler == SIG_IGN)
		handler = SIG_DFL;

	/*
     * SIGSTOP: always stops, regardless of handler.  Cannot be caught or
     * ignored (like SIGKILL cannot be caught).
     */
	if (signum == SIGSTOP) {
		klog_uint("SIGNAL", "SIGSTOP pid", g_current->pid);
		sched_mark_stopped(SIGSTOP);
		schedule();
		return frame_ctx; /* resumes here after SIGCONT */
	}

	/*
     * SIGTSTP: stops if SIG_DFL, delivered to handler if caught, discarded
     * if SIG_IGN.
     */
	if (signum == SIGTSTP) {
		if (handler == SIG_DFL) {
			klog_uint("SIGNAL", "SIGTSTP (default stop) pid", g_current->pid);
			sched_mark_stopped(SIGTSTP);
			schedule();
			return frame_ctx; /* resumes here after SIGCONT */
		}
		/* SIG_IGN or user handler: fall through to generic handling below. */
	}

	/*
     * SIGCONT: the wake-up already happened in sched_send_signal().  If a
     * user handler is installed, deliver it.  Otherwise discard the signal.
     */
	if (signum == SIGCONT) {
		if (handler > SIG_IGN) {
			if (arch_signal_setup_frame(g_current, frame, signum, handler) !=
			    0)
				goto bad_signal_frame;
		}
		return frame_ctx;
	}

	if (handler == SIG_IGN) {
		if (from_crash)
			g_current->crash.valid = 0;
		return frame_ctx;
	} else if (handler == SIG_DFL) {
		if (is_fatal_default(signum)) {
			int dumped_core = 0;
			if (from_crash && dumps_core_default(signum))
				dumped_core = (core_dump_process(g_current, signum) == 0);
			if (from_crash)
				g_current->crash.valid = 0;
			klog_uint("SIGNAL", "default-kill pid", g_current->pid);
			klog_uint("SIGNAL", "default-kill signum", (uint32_t)signum);
			klog_uint(
			    "SIGNAL", "default-kill from_crash", (uint32_t)from_crash);
			sched_mark_signaled(signum, dumped_core);
			schedule();       /* zombie — switches away, never returns here */
			return frame_ctx; /* unreachable; keeps compiler happy */
		}
		/* SIGCHLD with SIG_DFL: discarded (ignored by default). */
		if (from_crash)
			g_current->crash.valid = 0;
		return frame_ctx;
	} else {
		if (from_crash)
			g_current->crash.valid = 0;
		if (arch_signal_setup_frame(g_current, frame, signum, handler) != 0)
			goto bad_signal_frame;
		return frame_ctx;
	}

bad_signal_frame:
	klog_uint("SIGNAL", "bad handler frame pid", g_current->pid);
	sched_mark_signaled(SIGSEGV, 0);
	schedule();
	return frame_ctx;
}
