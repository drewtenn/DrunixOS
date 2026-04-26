/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall.c - INT 0x80 syscall entry point and dispatcher.
 *
 * This file owns the architecture-facing syscall_handler symbol, the central
 * syscall number switch, and the fallback unknown-syscall handler. Actual
 * syscall implementations live in focused syscall_*.c domain modules.
 */

#include "syscall.h"
#include "syscall/syscall_internal.h"
#include "syscall/syscall_linux.h"
#include "sched.h"
#include "klog.h"
#include <stdint.h>

static uint32_t SYSCALL_NOINLINE syscall_case_unknown(uint32_t eax)
{
	klog_uint("KERN", "unknown syscall", eax);
	return (uint32_t)-1;
}

#define ARM64_LINUX_SYS_WRITE 64u
#define ARM64_LINUX_SYS_DUP 23u
#define ARM64_LINUX_SYS_DUP3 24u
#define ARM64_LINUX_SYS_FCNTL 25u
#define ARM64_LINUX_SYS_IOCTL 29u
#define ARM64_LINUX_SYS_TRUNCATE 45u
#define ARM64_LINUX_SYS_FTRUNCATE 46u
#define ARM64_LINUX_SYS_MKDIRAT 34u
#define ARM64_LINUX_SYS_UNLINKAT 35u
#define ARM64_LINUX_SYS_STATFS 43u
#define ARM64_LINUX_SYS_FSTATFS 44u
#define ARM64_LINUX_SYS_FACCESSAT 48u
#define ARM64_LINUX_SYS_CHDIR 49u
#define ARM64_LINUX_SYS_FCHMODAT 53u
#define ARM64_LINUX_SYS_FCHOWNAT 54u
#define ARM64_LINUX_SYS_OPENAT 56u
#define ARM64_LINUX_SYS_CLOSE 57u
#define ARM64_LINUX_SYS_PIPE2 59u
#define ARM64_LINUX_SYS_GETDENTS64 61u
#define ARM64_LINUX_SYS_LSEEK 62u
#define ARM64_LINUX_SYS_READ 63u
#define ARM64_LINUX_SYS_READV 65u
#define ARM64_LINUX_SYS_WRITEV 66u
#define ARM64_LINUX_SYS_RENAMEAT 38u
#define ARM64_LINUX_SYS_READLINKAT 78u
#define ARM64_LINUX_SYS_NEWFSTATAT 79u
#define ARM64_LINUX_SYS_FSTAT 80u
#define ARM64_LINUX_SYS_SYNC 81u
#define ARM64_LINUX_SYS_UTIMENSAT 88u
#define ARM64_LINUX_SYS_EXIT 93u
#define ARM64_LINUX_SYS_EXIT_GROUP 94u
#define ARM64_LINUX_SYS_SET_TID_ADDRESS 96u
#define ARM64_LINUX_SYS_NANOSLEEP 101u
#define ARM64_LINUX_SYS_CLOCK_GETTIME 113u
#define ARM64_LINUX_SYS_CLOCK_NANOSLEEP 115u
#define ARM64_LINUX_SYS_SCHED_GETAFFINITY 123u
#define ARM64_LINUX_SYS_SCHED_YIELD 124u
#define ARM64_LINUX_SYS_KILL 129u
#define ARM64_LINUX_SYS_RT_SIGACTION 134u
#define ARM64_LINUX_SYS_RT_SIGPROCMASK 135u
#define ARM64_LINUX_SYS_SETPRIORITY 140u
#define ARM64_LINUX_SYS_GETPRIORITY 141u
#define ARM64_LINUX_SYS_SETPGID 154u
#define ARM64_LINUX_SYS_GETPGID 155u
#define ARM64_LINUX_SYS_GETSID 156u
#define ARM64_LINUX_SYS_SETSID 157u
#define ARM64_LINUX_SYS_UNAME 160u
#define ARM64_LINUX_SYS_GETRUSAGE 165u
#define ARM64_LINUX_SYS_UMASK 166u
#define ARM64_LINUX_SYS_GETTIMEOFDAY 169u
#define ARM64_LINUX_SYS_GETPID 172u
#define ARM64_LINUX_SYS_GETPPID 173u
#define ARM64_LINUX_SYS_GETUID 174u
#define ARM64_LINUX_SYS_GETEUID 175u
#define ARM64_LINUX_SYS_GETGID 176u
#define ARM64_LINUX_SYS_GETEGID 177u
#define ARM64_LINUX_SYS_GETTID 178u
#define ARM64_LINUX_SYS_SYSINFO 179u
#define ARM64_LINUX_SYS_BRK 214u
#define ARM64_LINUX_SYS_MUNMAP 215u
#define ARM64_LINUX_SYS_CLONE 220u
#define ARM64_LINUX_SYS_EXECVE 221u
#define ARM64_LINUX_SYS_MMAP 222u
#define ARM64_LINUX_SYS_MPROTECT 226u
#define ARM64_LINUX_SYS_WAIT4 260u
#define ARM64_LINUX_SYS_PRLIMIT64 261u
#define ARM64_LINUX_SYS_RENAMEAT2 276u
#define ARM64_LINUX_SYS_STATX 291u
#define ARM64_LINUX_SYS_GETCWD 17u

extern void arm64_console_loop(void) __attribute__((weak));

static uint64_t arm64_syscall_ret32(uint32_t value)
{
	return (uint64_t)(int64_t)(int32_t)value;
}

static uint64_t arm64_syscall_write(arch_trap_frame_t *frame)
{
	uint64_t fd = arch_syscall_arg0(frame);
	const char *buf = (const char *)(uintptr_t)arch_syscall_arg1(frame);
	uint32_t len = (uint32_t)arch_syscall_arg2(frame);

	if (sched_current())
		return arm64_syscall_ret32(syscall_case_write(
		    (uint32_t)fd,
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));

	if (fd != 1u && fd != 2u)
		return (uint64_t)(int64_t)-9;
	arch_console_write(buf, len);
	return len;
}

static uint64_t arm64_syscall_dup3(arch_trap_frame_t *frame)
{
	uint32_t oldfd = (uint32_t)arch_syscall_arg0(frame);
	uint32_t newfd = (uint32_t)arch_syscall_arg1(frame);
	uint32_t flags = (uint32_t)arch_syscall_arg2(frame);
	uint32_t ret;

	if ((flags & ~LINUX_O_CLOEXEC) != 0 || oldfd == newfd)
		return (uint64_t)(int64_t)-LINUX_EINVAL;

	ret = syscall_case_dup2(oldfd, newfd);
	if ((int32_t)ret >= 0 && (flags & LINUX_O_CLOEXEC) != 0) {
		process_t *cur = sched_current();

		if (cur && ret < MAX_FDS)
			proc_fd_entries(cur)[ret].cloexec = 1u;
	}
	return arm64_syscall_ret32(ret);
}

static uint64_t arm64_syscall_exit(arch_trap_frame_t *frame, uint32_t exit_group)
{
	uint32_t status = (uint32_t)arch_syscall_arg0(frame);

	if (sched_current())
		return syscall_case_exit_exit_group(exit_group, status);

	if (arm64_console_loop) {
		frame->elr_el1 = (uintptr_t)arm64_console_loop;
		frame->spsr_el1 = 0x5u;
		return 0u;
	}
	return (uint64_t)-1;
}

uint64_t syscall_dispatch_from_frame(arch_trap_frame_t *frame)
{
	uint64_t ret;
	uint64_t nr;

	if (!frame)
		return (uint64_t)-1;

	arch_user_sync_from_active();
	nr = arch_syscall_number(frame);
	switch (nr) {
	case ARM64_LINUX_SYS_GETCWD:
		ret = arm64_syscall_ret32(syscall_case_getcwd(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_DUP:
		ret = arm64_syscall_ret32(
		    syscall_case_dup((uint32_t)arch_syscall_arg0(frame)));
		break;
	case ARM64_LINUX_SYS_DUP3:
		ret = arm64_syscall_dup3(frame);
		break;
	case ARM64_LINUX_SYS_FCNTL:
		ret = arm64_syscall_ret32(syscall_case_fcntl64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_IOCTL:
		ret = arm64_syscall_ret32(syscall_case_ioctl(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_TRUNCATE:
		ret = arm64_syscall_ret32(syscall_case_truncate64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    0));
		break;
	case ARM64_LINUX_SYS_FTRUNCATE:
		ret = arm64_syscall_ret32(syscall_case_ftruncate64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    0));
		break;
	case ARM64_LINUX_SYS_RENAMEAT:
		ret = arm64_syscall_ret32(syscall_case_renameat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case ARM64_LINUX_SYS_OPENAT:
		ret = arm64_syscall_ret32(syscall_case_openat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_FACCESSAT:
		ret = arm64_syscall_ret32(syscall_case_faccessat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_CHDIR:
		ret = arm64_syscall_ret32(
		    syscall_case_chdir((uint32_t)arch_syscall_arg0(frame)));
		break;
	case ARM64_LINUX_SYS_FCHMODAT:
		ret = arm64_syscall_ret32(syscall_case_fchmodat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_FCHOWNAT:
		ret = arm64_syscall_ret32(syscall_case_fchownat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg4(frame)));
		break;
	case ARM64_LINUX_SYS_MKDIRAT:
		ret = arm64_syscall_ret32(syscall_case_mkdirat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_UNLINKAT:
		ret = arm64_syscall_ret32(syscall_case_unlinkat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_STATFS:
		ret = arm64_syscall_ret32(syscall_case_statfs_arm64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_FSTATFS:
		ret = arm64_syscall_ret32(syscall_case_fstatfs_arm64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_CLOSE:
		ret = arm64_syscall_ret32(
		    syscall_case_close((uint32_t)arch_syscall_arg0(frame)));
		break;
	case ARM64_LINUX_SYS_PIPE2:
		ret = arm64_syscall_ret32(syscall_case_pipe2(
		    (uint32_t)nr,
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    0,
		    0,
		    0,
		    0));
		break;
	case ARM64_LINUX_SYS_GETDENTS64:
		ret = arm64_syscall_ret32(syscall_case_getdents64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_LSEEK:
		ret = arm64_syscall_ret32(syscall_case_lseek(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_READ:
		ret = arm64_syscall_ret32(syscall_case_read(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_READV:
		ret = arm64_syscall_ret32(syscall_case_readv64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_READLINKAT:
		ret = arm64_syscall_ret32(syscall_case_readlinkat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case ARM64_LINUX_SYS_NEWFSTATAT:
		ret = arm64_syscall_ret32(syscall_case_fstatat_arm64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case ARM64_LINUX_SYS_FSTAT:
		ret = arm64_syscall_ret32(syscall_case_fstat_arm64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_SYNC:
		ret = arm64_syscall_ret32(syscall_case_sync());
		break;
	case ARM64_LINUX_SYS_UTIMENSAT:
		ret = arm64_syscall_ret32(syscall_case_utimensat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case ARM64_LINUX_SYS_WRITE:
		ret = arm64_syscall_write(frame);
		break;
	case ARM64_LINUX_SYS_WRITEV:
		ret = arm64_syscall_ret32(syscall_case_writev64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_EXIT:
		ret = arm64_syscall_exit(frame, 0u);
		break;
	case ARM64_LINUX_SYS_EXIT_GROUP:
		ret = arm64_syscall_exit(frame, 1u);
		break;
	case ARM64_LINUX_SYS_SCHED_YIELD:
		ret = arm64_syscall_ret32(syscall_case_yield());
		break;
	case ARM64_LINUX_SYS_SET_TID_ADDRESS:
		ret = arm64_syscall_ret32(syscall_case_set_tid_address());
		break;
	case ARM64_LINUX_SYS_NANOSLEEP:
		ret = arm64_syscall_ret32(syscall_case_nanosleep64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_SCHED_GETAFFINITY:
		ret = arm64_syscall_ret32(syscall_case_sched_getaffinity(
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_KILL:
		ret = arm64_syscall_ret32(syscall_case_kill(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_RT_SIGACTION:
		ret = arm64_syscall_ret32(syscall_case_rt_sigaction(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case ARM64_LINUX_SYS_RT_SIGPROCMASK:
		ret = arm64_syscall_ret32(syscall_case_rt_sigprocmask(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case ARM64_LINUX_SYS_SETPRIORITY:
		ret = arm64_syscall_ret32(syscall_case_setpriority());
		break;
	case ARM64_LINUX_SYS_GETPRIORITY:
		ret = arm64_syscall_ret32(syscall_case_getpriority());
		break;
	case ARM64_LINUX_SYS_SETPGID:
		ret = arm64_syscall_ret32(syscall_case_setpgid(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_GETPGID:
		ret = arm64_syscall_ret32(
		    syscall_case_getpgid((uint32_t)arch_syscall_arg0(frame)));
		break;
	case ARM64_LINUX_SYS_GETSID:
		ret = arm64_syscall_ret32(
		    syscall_case_getsid((uint32_t)arch_syscall_arg0(frame)));
		break;
	case ARM64_LINUX_SYS_SETSID:
		ret = arm64_syscall_ret32(syscall_case_setsid());
		break;
	case ARM64_LINUX_SYS_CLOCK_GETTIME:
		ret = arm64_syscall_ret32(syscall_case_clock_gettime(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_CLOCK_NANOSLEEP:
		ret = arm64_syscall_ret32(syscall_case_clock_nanosleep64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case ARM64_LINUX_SYS_UNAME:
		ret = arm64_syscall_ret32(
		    syscall_case_uname((uint32_t)arch_syscall_arg0(frame)));
		break;
	case ARM64_LINUX_SYS_GETRUSAGE:
		ret = arm64_syscall_ret32(syscall_case_getrusage(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_UMASK:
		ret = arm64_syscall_ret32(
		    syscall_case_umask((uint32_t)arch_syscall_arg0(frame)));
		break;
	case ARM64_LINUX_SYS_GETTIMEOFDAY:
		ret = arm64_syscall_ret32(syscall_case_gettimeofday(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_GETPID:
		ret = arm64_syscall_ret32(syscall_case_getpid());
		break;
	case ARM64_LINUX_SYS_GETPPID:
		ret = arm64_syscall_ret32(syscall_case_getppid());
		break;
	case ARM64_LINUX_SYS_GETUID:
	case ARM64_LINUX_SYS_GETEUID:
	case ARM64_LINUX_SYS_GETGID:
	case ARM64_LINUX_SYS_GETEGID:
		ret = arm64_syscall_ret32(
		    syscall_case_getuid32_getgid32_geteuid32_getegid32());
		break;
	case ARM64_LINUX_SYS_GETTID:
		ret = arm64_syscall_ret32(syscall_case_gettid());
		break;
	case ARM64_LINUX_SYS_SYSINFO:
		ret = arm64_syscall_ret32(
		    syscall_case_sysinfo((uint32_t)arch_syscall_arg0(frame)));
		break;
	case ARM64_LINUX_SYS_BRK:
		ret = arm64_syscall_ret32(
		    syscall_case_brk((uint32_t)arch_syscall_arg0(frame)));
		break;
	case ARM64_LINUX_SYS_MUNMAP:
		ret = arm64_syscall_ret32(syscall_case_munmap(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case ARM64_LINUX_SYS_CLONE:
		ret = arm64_syscall_ret32(syscall_case_clone(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame),
		    (uint32_t)arch_syscall_arg4(frame)));
		break;
	case ARM64_LINUX_SYS_EXECVE:
		ret = arm64_syscall_ret32(syscall_case_execve(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_MMAP: {
		uint64_t offset = arch_syscall_arg5(frame);

		if ((offset & 0xFFFu) != 0) {
			ret = (uint64_t)(int64_t)-22;
			break;
		}
		ret = arm64_syscall_ret32(syscall_case_mmap2(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame),
		    (uint32_t)arch_syscall_arg4(frame),
		    (uint32_t)(offset >> 12)));
		break;
	}
	case ARM64_LINUX_SYS_MPROTECT:
		ret = arm64_syscall_ret32(syscall_case_mprotect(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_WAIT4:
		ret = arm64_syscall_ret32(syscall_case_wait4(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case ARM64_LINUX_SYS_RENAMEAT2:
		if ((uint32_t)arch_syscall_arg4(frame) != 0) {
			ret = (uint64_t)(int64_t)-LINUX_EINVAL;
			break;
		}
		ret = arm64_syscall_ret32(syscall_case_renameat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case SYS_NANOSLEEP:
		ret = arm64_syscall_ret32(syscall_case_nanosleep(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case SYS_FORK:
		ret = arm64_syscall_ret32(syscall_case_fork_vfork());
		break;
	case SYS_STAT:
		ret = arm64_syscall_ret32(syscall_case_stat(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case SYS_DRUNIX_CLEAR:
		ret = arm64_syscall_ret32(syscall_case_drunix_clear());
		break;
	case SYS_DRUNIX_SCROLL_UP:
		ret = arm64_syscall_ret32(
		    syscall_case_drunix_scroll_up((uint32_t)arch_syscall_arg0(frame)));
		break;
	case SYS_DRUNIX_SCROLL_DOWN:
		ret = arm64_syscall_ret32(syscall_case_drunix_scroll_down(
		    (uint32_t)arch_syscall_arg0(frame)));
		break;
	case SYS_DRUNIX_MODLOAD:
		ret = arm64_syscall_ret32(
		    syscall_case_drunix_modload((uint32_t)arch_syscall_arg0(frame)));
		break;
	case SYS_DRUNIX_TCGETATTR:
		ret = arm64_syscall_ret32(syscall_case_drunix_tcgetattr(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case SYS_DRUNIX_TCSETATTR:
		ret = arm64_syscall_ret32(syscall_case_drunix_tcsetattr(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case SYS_DRUNIX_TCSETPGRP:
		ret = arm64_syscall_ret32(syscall_case_drunix_tcsetpgrp(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame)));
		break;
	case SYS_DRUNIX_TCGETPGRP:
		ret = arm64_syscall_ret32(
		    syscall_case_drunix_tcgetpgrp((uint32_t)arch_syscall_arg0(frame)));
		break;
	case SYS_DRUNIX_GETDENTS_PATH:
		ret = arm64_syscall_ret32(syscall_case_drunix_getdents_path(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame)));
		break;
	case ARM64_LINUX_SYS_PRLIMIT64:
		ret = arm64_syscall_ret32(syscall_case_prlimit64(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame)));
		break;
	case ARM64_LINUX_SYS_STATX:
		ret = arm64_syscall_ret32(syscall_case_statx(
		    (uint32_t)arch_syscall_arg0(frame),
		    (uint32_t)arch_syscall_arg1(frame),
		    (uint32_t)arch_syscall_arg2(frame),
		    (uint32_t)arch_syscall_arg3(frame),
		    (uint32_t)arch_syscall_arg4(frame)));
		break;
	default:
		ret = (uint64_t)-38;
		break;
		}
		arch_syscall_set_result(frame, ret);
		arch_user_sync_to_active();
		return ret;
	}

uint32_t syscall_handler(uint32_t eax,
                         uint32_t ebx,
                         uint32_t ecx,
                         uint32_t edx,
                         uint32_t esi,
                         uint32_t edi,
                         uint32_t ebp)
{
	(void)esi;
	(void)edi;
	(void)ebp;

	switch (eax) {
	case ARM64_LINUX_SYS_WRITE:
		if (ebx != 1u && ebx != 2u)
			return (uint32_t)-1;
		arch_console_write((const char *)(uintptr_t)ecx, edx);
		return edx;
	case ARM64_LINUX_SYS_SCHED_YIELD:
		return syscall_case_yield();
	case ARM64_LINUX_SYS_EXIT:
	case ARM64_LINUX_SYS_EXIT_GROUP:
		if (sched_current())
			return syscall_case_exit_exit_group(
			    eax == ARM64_LINUX_SYS_EXIT_GROUP, ebx);
		return 0;
	default:
		return syscall_case_unknown(eax);
	}
}
