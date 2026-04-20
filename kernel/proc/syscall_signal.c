/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_signal.c - Linux i386 signal syscalls.
 *
 * Owns Linux signal disposition, mask, delivery probe, and sigreturn
 * compatibility handlers. Scheduler signal primitives remain in sched/process
 * code; this file only translates syscall ABI state.
 */

#include "syscall_internal.h"
#include "kstring.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
#include <stdint.h>

#define LINUX_EINVAL 22

static void signal_put_u32(uint8_t *buf, uint32_t off, uint32_t value)
{
	buf[off + 0] = (uint8_t)(value & 0xFFu);
	buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
	buf[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
	buf[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t signal_get_u32(const uint8_t *buf, uint32_t off)
{
	return (uint32_t)buf[off + 0] | ((uint32_t)buf[off + 1] << 8) |
	       ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

static int syscall_apply_sigmask(process_t *cur,
                                 uint32_t how,
                                 uint32_t newmask,
                                 int has_newmask)
{
	uint32_t old;

	if (!cur)
		return -1;

	old = cur->sig_blocked;
	if (has_newmask) {
		switch (how) {
		case 0: /* SIG_BLOCK   */
			cur->sig_blocked = old | newmask;
			break;
		case 1: /* SIG_UNBLOCK */
			cur->sig_blocked = old & ~newmask;
			break;
		case 2: /* SIG_SETMASK */
			cur->sig_blocked = newmask;
			break;
		default:
			return -1;
		}
		cur->sig_blocked &= ~((1u << SIGKILL) | (1u << SIGSTOP));
	}

	return 0;
}

static int syscall_copy_rt_sigset_to_user(process_t *cur,
                                          uint32_t user_dst,
                                          uint32_t sigset_size,
                                          uint32_t mask)
{
	uint8_t out[128];

	if (user_dst == 0)
		return 0;
	if (!cur || sigset_size < sizeof(uint32_t) || sigset_size > sizeof(out))
		return -1;

	k_memset(out, 0, sizeof(out));
	out[0] = (uint8_t)(mask & 0xFFu);
	out[1] = (uint8_t)((mask >> 8) & 0xFFu);
	out[2] = (uint8_t)((mask >> 16) & 0xFFu);
	out[3] = (uint8_t)((mask >> 24) & 0xFFu);
	return uaccess_copy_to_user(cur, user_dst, out, sigset_size);
}

uint32_t SYSCALL_NOINLINE syscall_case_kill(uint32_t ebx, uint32_t ecx)
{
	/*
	 * ebx = target pid (> 0) or process group id (< 0)
	 * ecx = signal number
	 *
	 * Positive `ebx` targets a single process.  Negative `ebx` targets
	 * every process in that process group, mirroring kill(2).
	 *
	 * Signal 0 is the Linux-compatible existence probe: it does not
	 * deliver anything, but it still validates that the target exists.
	 *
	 * Returns 0 on success, -ESRCH if the target does not exist, and -1
	 * if signum is out of range.
	 */
	int sig = (int)ecx;
	int32_t target = (int32_t)ebx;

	if (sig < 0 || sig >= NSIG)
		return (uint32_t)-LINUX_EINVAL;
	if (target > 0) {
		if (!sched_find_pid((uint32_t)target))
			return (uint32_t)-3;
		if (sig == 0)
			return 0;
		sched_send_signal((uint32_t)target, sig);
		return 0;
	}
	if (target < 0) {
		process_t *cur = sched_current();
		uint32_t pgid = (uint32_t)(-target);
		if (!cur || !sched_session_has_pgid(cur->sid, pgid))
			return (uint32_t)-3;
		if (sig == 0)
			return 0;
		sched_send_signal_to_pgid(pgid, sig);
		return 0;
	}
	{
		process_t *cur = sched_current();
		if (!cur)
			return (uint32_t)-1;
		if (sig == 0)
			return 0;
		sched_send_signal_to_pgid(cur->pgid, sig);
		return 0;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_sigaction(uint32_t ebx,
                                                 uint32_t ecx,
                                                 uint32_t edx)
{
	/*
	 * ebx = signal number
	 * ecx = new handler: SIG_DFL (0), SIG_IGN (1), or a user VA
	 * edx = pointer to uint32_t to receive the old handler, or 0
	 *
	 * Installs a new signal disposition for signal `ebx`.  SIGKILL (9)
	 * and SIGSTOP (19) cannot be caught or ignored.
	 *
	 * Returns 0 on success, -1 on error.
	 */
	process_t *cur = sched_current();
	int sig;
	uint32_t *handlers;

	if (!cur)
		return (uint32_t)-1;

	sig = (int)ebx;
	if (sig < 1 || sig >= NSIG)
		return (uint32_t)-LINUX_EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP)
		return (uint32_t)-LINUX_EINVAL;
	if (ecx > SIG_IGN && ecx >= USER_STACK_TOP)
		return (uint32_t)-1;

	handlers =
	    cur->sig_actions ? cur->sig_actions->handlers : cur->sig_handlers;
	if (edx && uaccess_copy_to_user(
	               cur, edx, &handlers[sig], sizeof(handlers[sig])) != 0)
		return (uint32_t)-1;

	handlers[sig] = ecx;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_rt_sigaction(uint32_t ebx,
                                                    uint32_t ecx,
                                                    uint32_t edx,
                                                    uint32_t esi)
{
	/*
	 * Linux i386 rt_sigaction(signum, act, oldact, sigsetsize).  Drunix
	 * stores the handler disposition and currently ignores flags, restorer,
	 * and mask fields.
	 */
	process_t *cur = sched_current();
	uint8_t kact[32];
	uint8_t kold[32];
	uint32_t *handlers;
	uint32_t handler = 0;
	int sig = (int)ebx;

	if (!cur)
		return (uint32_t)-1;
	if (sig < 1 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
		return (uint32_t)-LINUX_EINVAL;
	if (esi < sizeof(uint32_t) || esi > 128u)
		return (uint32_t)-LINUX_EINVAL;
	handlers =
	    cur->sig_actions ? cur->sig_actions->handlers : cur->sig_handlers;
	if (edx != 0) {
		k_memset(kold, 0, sizeof(kold));
		signal_put_u32(kold, 0u, handlers[sig]);
		if (uaccess_copy_to_user(cur, edx, kold, sizeof(kold)) != 0)
			return (uint32_t)-1;
	}
	if (ecx != 0) {
		if (uaccess_copy_from_user(cur, kact, ecx, sizeof(kact)) != 0)
			return (uint32_t)-1;
		handler = signal_get_u32(kact, 0u);
		if (handler > SIG_IGN && handler >= USER_STACK_TOP)
			return (uint32_t)-1;
		handlers[sig] = handler;
	}
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_sigreturn(void)
{
	/*
	 * Restores the process context saved on the user stack before the
	 * signal handler was called.  Called exclusively by the trampoline code
	 * embedded in the signal frame, not by user programs directly.
	 */
	process_t *cur = sched_current();
	uint32_t sf[6];
	uint32_t *kframe;
	uint32_t user_esp;

	if (!cur)
		return (uint32_t)-1;
	kframe = (uint32_t *)(cur->kstack_top - 76);
	user_esp = kframe[17];
	if (uaccess_copy_from_user(cur, sf, user_esp - 4, sizeof(sf)) != 0) {
		sched_mark_signaled(SIGSEGV, 0);
		schedule();
		__builtin_unreachable();
	}

	kframe[14] = sf[2];
	kframe[16] = sf[3];
	kframe[11] = sf[4];
	kframe[17] = sf[5];
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_sigprocmask(uint32_t ebx,
                                                   uint32_t ecx,
                                                   uint32_t edx)
{
	/*
	 * ebx = how:  0 = SIG_BLOCK, 1 = SIG_UNBLOCK, 2 = SIG_SETMASK
	 * ecx = pointer to new mask (uint32_t bitmask), or 0 to query only
	 * edx = pointer to receive old mask (uint32_t), or 0
	 */
	process_t *cur = sched_current();
	uint32_t old;
	uint32_t newmask = 0;

	if (!cur)
		return (uint32_t)-1;

	old = cur->sig_blocked;
	if (edx && uaccess_copy_to_user(cur, edx, &old, sizeof(old)) != 0)
		return (uint32_t)-1;

	if (ecx && uaccess_copy_from_user(cur, &newmask, ecx, sizeof(newmask)) != 0)
		return (uint32_t)-1;
	if (syscall_apply_sigmask(cur, ebx, newmask, ecx != 0) != 0)
		return (uint32_t)-1;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_rt_sigprocmask(uint32_t ebx,
                                                      uint32_t ecx,
                                                      uint32_t edx,
                                                      uint32_t esi)
{
	/*
	 * ebx = how, ecx = sigset_t *new, edx = sigset_t *old,
	 * esi = sigset size.  Drunix stores a 32-bit signal mask; Linux i386
	 * libc may pass a wider sigset_t, so copy the low word and zero-fill the
	 * rest on output.
	 */
	process_t *cur = sched_current();
	uint32_t old;
	uint32_t newmask = 0;

	if (!cur || esi < sizeof(uint32_t) || esi > 128u)
		return (uint32_t)-1;

	old = cur->sig_blocked;
	if (syscall_copy_rt_sigset_to_user(cur, edx, esi, old) != 0)
		return (uint32_t)-1;
	if (ecx && uaccess_copy_from_user(cur, &newmask, ecx, sizeof(newmask)) != 0)
		return (uint32_t)-1;
	if (syscall_apply_sigmask(cur, ebx, newmask, ecx != 0) != 0)
		return (uint32_t)-1;
	return 0;
}
