/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * process.c - Linux i386 process identity and session syscalls.
 *
 * Contains pid/tid/uid/gid/session/process-group compatibility handlers.
 * Process creation, exec, wait, and memory-management syscalls belong in
 * separate domain files.
 */

#include "syscall_internal.h"
#include "process.h"
#include "sched.h"
#include <stdint.h>

#define LINUX_EPERM 1
#define LINUX_ESRCH 3

uint32_t SYSCALL_NOINLINE syscall_case_setsid(void)
{
	process_t *cur = sched_current();

	if (!cur)
		return (uint32_t)-1;
	cur->sid = cur->pid;
	cur->pgid = cur->pid;
	return cur->sid;
}

uint32_t SYSCALL_NOINLINE syscall_case_getsid(uint32_t ebx)
{
	process_t *cur = sched_current();
	process_t *target;
	uint32_t pid = ebx;

	if (!cur)
		return (uint32_t)-1;
	if (pid == 0 || pid == cur->pid)
		return cur->sid;
	target = sched_find_pid(pid);
	return target ? target->sid : (uint32_t)-LINUX_ESRCH;
}

uint32_t SYSCALL_NOINLINE syscall_case_setpgid(uint32_t ebx, uint32_t ecx)
{
	process_t *cur = sched_current();
	process_t *target;
	uint32_t target_pid;
	uint32_t new_pgid;

	if (!cur)
		return (uint32_t)-1;
	target_pid = ebx ? ebx : cur->pid;
	new_pgid = ecx ? ecx : target_pid;

	if (target_pid == cur->pid) {
		target = cur;
	} else {
		target = sched_find_pid(target_pid);
		if (!target || target->parent_pid != cur->pid)
			return (uint32_t)-LINUX_ESRCH;
	}

	if (target->sid != cur->sid)
		return (uint32_t)-LINUX_EPERM;
	if (target->pid == target->sid && new_pgid != target->pgid)
		return (uint32_t)-LINUX_EPERM;
	if (new_pgid != target_pid &&
	    !sched_session_has_pgid(target->sid, new_pgid))
		return (uint32_t)-LINUX_EPERM;

	target->pgid = new_pgid;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_getpgid(uint32_t ebx)
{
	process_t *cur = sched_current();
	process_t *target;

	if (!cur)
		return (uint32_t)-1;
	if (ebx == 0 || ebx == cur->pid)
		return cur->pgid;
	target = sched_find_pid(ebx);
	if (!target)
		return (uint32_t)-LINUX_ESRCH;
	return target->pgid;
}

uint32_t SYSCALL_NOINLINE syscall_case_getpid(void)
{

	return sched_current_tgid();
}

uint32_t SYSCALL_NOINLINE syscall_case_gettid(void)
{

	return sched_current_tid();
}

uint32_t SYSCALL_NOINLINE syscall_case_getppid(void)
{

	return sched_current_ppid();
}

uint32_t SYSCALL_NOINLINE
syscall_case_getuid32_getgid32_geteuid32_getegid32(void)
{

	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_setuid32_setgid32(uint32_t ebx)
{

	return ebx == 0 ? 0 : (uint32_t)-LINUX_EPERM;
}
