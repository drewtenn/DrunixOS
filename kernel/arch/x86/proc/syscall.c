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

uint64_t syscall_dispatch_from_frame(arch_trap_frame_t *frame)
{
	uint64_t nr = arch_syscall_number(frame);
	uint64_t ret = syscall_handler((uint32_t)nr,
	                               (uint32_t)arch_syscall_arg0(frame),
	                               (uint32_t)arch_syscall_arg1(frame),
	                               (uint32_t)arch_syscall_arg2(frame),
	                               (uint32_t)arch_syscall_arg3(frame),
	                               (uint32_t)arch_syscall_arg4(frame),
	                               (uint32_t)arch_syscall_arg5(frame));
	arch_syscall_set_result(frame, ret);
	return ret;
}

/*
 * syscall_handler: shared syscall switch used by frame-based arch entry points
 * and kernel tests that call directly with decoded arguments.
 *
 * When this function runs, the CPU is in ring 0 but still using the
 * process's page directory (CR3 was not changed by the interrupt).
 * This means kernel code can walk the caller's user mappings, but it still
 * must validate every user pointer explicitly.  All user buffers and strings
 * are copied through uaccess helpers so bad pointers fail cleanly and kernel
 * writes into copy-on-write user pages allocate a private frame first.
 *
 * The INT 0x80 gate is a trap gate (type_attr=0xEF), so IF is NOT cleared
 * on entry — hardware interrupts (including keyboard IRQ1 and timer IRQ0)
 * remain active.  This allows SYS_READ to spin-wait for keyboard input and
 * allows the timer to fire (and sched_tick() to set need_switch) while a
 * process is blocked.  The context switch itself happens in syscall_common
 * (isr.asm) after this function returns, when sched_needs_switch() is true.
 *
 * Return value: written back to the saved EAX slot in isr.asm so the user
 * sees it in EAX after iret.
 */
uint32_t syscall_handler(uint32_t eax,
                         uint32_t ebx,
                         uint32_t ecx,
                         uint32_t edx,
                         uint32_t esi,
                         uint32_t edi,
                         uint32_t ebp)
{
	switch (eax) {
	case SYS_WRITE:
		return syscall_case_write(ebx, ecx, edx);

	case SYS_WRITEV:
		return syscall_case_writev(ebx, ecx, edx);

	case SYS_READV:
		return syscall_case_readv(ebx, ecx, edx);

	case SYS_READ:
		return syscall_case_read(ebx, ecx, edx);

	case SYS_SENDFILE64:
		return syscall_case_sendfile64(ebx, ecx, edx, esi);

	case SYS_OPEN:
		return syscall_case_open(ebx, ecx);

	case SYS_OPENAT:
		return syscall_case_openat(ebx, ecx, edx);

	case SYS_CLOSE:
		return syscall_case_close(ebx);

	case SYS_ACCESS:
		return syscall_case_access(ebx);

	case SYS_FACCESSAT:
		return syscall_case_faccessat(ebx, ecx, edx);

	case SYS_FCHMODAT:
		return syscall_case_fchmodat(ebx, ecx);

	case SYS_FCHOWNAT:
		return syscall_case_fchownat(ebx, ecx, edi);

	case SYS_FUTIMESAT:
		return syscall_case_futimesat(ebx, ecx);

	case SYS_CHMOD:
		return syscall_case_chmod(ebx);

	case SYS_LCHOWN:
	case SYS_CHOWN32:
		return syscall_case_lchown_chown32(ebx);

	case SYS_SYNC:
		return syscall_case_sync();

	case SYS_UMASK:
		return syscall_case_umask(ebx);

	case SYS_SETSID:
		return syscall_case_setsid();

	case SYS_GETSID:
		return syscall_case_getsid(ebx);

	case SYS_READLINK:
		return syscall_case_readlink(ebx, ecx, edx);

	case SYS_READLINKAT:
		return syscall_case_readlinkat(ebx, ecx, edx, esi);

	case SYS_GETPRIORITY:
		return syscall_case_getpriority();

	case SYS_SETPRIORITY:
		return syscall_case_setpriority();

	case SYS_SYSINFO:
		return syscall_case_sysinfo(ebx);

	case SYS_PRLIMIT64:
		return syscall_case_prlimit64(ebx, ecx, edx, esi);

	case SYS_TRUNCATE64:
		return syscall_case_truncate64(ebx, ecx, edx);

	case SYS_FTRUNCATE64:
		return syscall_case_ftruncate64(ebx, ecx, edx);

	case SYS_SCHED_GETAFFINITY:
		return syscall_case_sched_getaffinity(ecx, edx);

	case SYS_UTIMENSAT:
		return syscall_case_utimensat(ebx, ecx, esi);

	case SYS_POLL:
		return syscall_case_poll(ebx, ecx);

	case SYS_IOCTL:
		return syscall_case_ioctl(ebx, ecx, edx);

	case SYS_FCNTL64:
		return syscall_case_fcntl64(ebx, ecx, edx);

	case SYS_EXECVE:
		return syscall_case_execve(ebx, ecx, edx);

	case SYS_CREAT:
		return syscall_case_creat(ebx);

	case SYS_UNLINK:
		return syscall_case_unlink(ebx);

	case SYS_UNLINKAT:
		return syscall_case_unlinkat(ebx, ecx, edx);

	case SYS_FORK:
	case SYS_VFORK:
		return syscall_case_fork_vfork();

	case SYS_CLONE:
		return syscall_case_clone(ebx, ecx, edx, esi, edi);

	case SYS_DRUNIX_CLEAR:
		return syscall_case_drunix_clear();

	case SYS_DRUNIX_SCROLL_UP:
		return syscall_case_drunix_scroll_up(ebx);

	case SYS_DRUNIX_SCROLL_DOWN:
		return syscall_case_drunix_scroll_down(ebx);

	case SYS_YIELD:
		return syscall_case_yield();

	case SYS_NANOSLEEP:
		return syscall_case_nanosleep(ebx, ecx);

	case SYS_GETRUSAGE:
		return syscall_case_getrusage(ebx, ecx);

	case SYS_MKDIR:
		return syscall_case_mkdir(ebx);

	case SYS_MKDIRAT:
		return syscall_case_mkdirat(ebx, ecx);

	case SYS_RMDIR:
		return syscall_case_rmdir(ebx);

	case SYS_DUP:
		return syscall_case_dup(ebx);

	case SYS_CHDIR:
		return syscall_case_chdir(ebx);

	case SYS_GETCWD:
		return syscall_case_getcwd(ebx, ecx);

	case SYS_RENAME:
		return syscall_case_rename(ebx, ecx);

	case SYS_RENAMEAT:
		return syscall_case_renameat(ebx, ecx, edx, esi);

	case SYS_LINKAT:
		return syscall_case_linkat(ebx, ecx, edx, esi, edi);

	case SYS_SYMLINKAT:
		return syscall_case_symlinkat(ebx, ecx, edx);

	case SYS_GETDENTS:
		return syscall_case_getdents(ebx, ecx, edx);

	case SYS__NEWSELECT:
		return syscall_case_newselect(ebx, ecx, edx, esi);

	case SYS_DRUNIX_GETDENTS_PATH:
		return syscall_case_drunix_getdents_path(ebx, ecx, edx);

	case SYS_GETDENTS64:
		return syscall_case_getdents64(ebx, ecx, edx);

	case SYS_STAT:
		return syscall_case_stat(ebx, ecx);

	case SYS_STAT64:
	case SYS_LSTAT64:
		return syscall_case_stat64_lstat64(eax == SYS_LSTAT64, ebx, ecx);

	case SYS_FSTATAT64:
		return syscall_case_fstatat64(ebx, ecx, edx, esi);

	case SYS_FSTAT64:
		return syscall_case_fstat64(ebx, ecx);

	case SYS_STATX:
		return syscall_case_statx(ebx, ecx, edx, esi, edi);

	case SYS_STATFS64:
		return syscall_case_statfs64(ebx, ecx, edx);

	case SYS_FSTATFS64:
		return syscall_case_fstatfs64(ebx, ecx, edx);

	case SYS_CLOCK_GETTIME:
		return syscall_case_clock_gettime(ebx, ecx);

	case SYS_CLOCK_GETTIME64:
		return syscall_case_clock_gettime64(ebx, ecx);

	case SYS_GETTIMEOFDAY:
		return syscall_case_gettimeofday(ebx, ecx);

	case SYS_UNAME:
		return syscall_case_uname(ebx);

	case SYS_SET_THREAD_AREA:
		return syscall_case_set_thread_area(ebx);

	case SYS_SET_TID_ADDRESS:
		return syscall_case_set_tid_address();

	case SYS_DRUNIX_MODLOAD:
		return syscall_case_drunix_modload(ebx);

	case SYS_EXIT:
	case SYS_EXIT_GROUP:
		return syscall_case_exit_exit_group(eax == SYS_EXIT_GROUP, ebx);

	case SYS_BRK:
		return syscall_case_brk(ebx);

	case SYS_MMAP:
		return syscall_case_mmap(ebx);

	case SYS_MMAP2:
		return syscall_case_mmap2(ebx, ecx, edx, esi, edi, ebp);

	case SYS_MUNMAP:
		return syscall_case_munmap(ebx, ecx);

	case SYS_MPROTECT:
		return syscall_case_mprotect(ebx, ecx, edx);

	case SYS_PIPE:
		return syscall_case_pipe(ebx);

	case SYS_PIPE2:
		return syscall_case_pipe2(eax, ebx, ecx, edx, esi, edi, ebp);

	case SYS_DUP2:
		return syscall_case_dup2(ebx, ecx);

	case SYS_KILL:
		return syscall_case_kill(ebx, ecx);

	case SYS_SIGACTION:
		return syscall_case_sigaction(ebx, ecx, edx);

	case SYS_RT_SIGACTION:
		return syscall_case_rt_sigaction(ebx, ecx, edx, esi);

	case SYS_SIGRETURN:
		return syscall_case_sigreturn();

	case SYS_SIGPROCMASK:
		return syscall_case_sigprocmask(ebx, ecx, edx);

	case SYS_RT_SIGPROCMASK:
		return syscall_case_rt_sigprocmask(ebx, ecx, edx, esi);

	case SYS_DRUNIX_TCGETATTR:
		return syscall_case_drunix_tcgetattr(ebx, ecx);

	case SYS_DRUNIX_TCSETATTR:
		return syscall_case_drunix_tcsetattr(ebx, ecx, edx);

	case SYS_SETPGID:
		return syscall_case_setpgid(ebx, ecx);

	case SYS_GETPGID:
		return syscall_case_getpgid(ebx);

	case SYS_LSEEK:
		return syscall_case_lseek(ebx, ecx, edx);

	case SYS__LLSEEK:
		return syscall_case_llseek(ebx, ecx, edx, esi, edi);

	case SYS_GETPID:
		return syscall_case_getpid();

	case SYS_GETTID:
		return syscall_case_gettid();

	case SYS_GETPPID:
		return syscall_case_getppid();

	case SYS_GETUID32:
	case SYS_GETGID32:
	case SYS_GETEUID32:
	case SYS_GETEGID32:
		return syscall_case_getuid32_getgid32_geteuid32_getegid32();

	case SYS_SETUID32:
	case SYS_SETGID32:
		return syscall_case_setuid32_setgid32(ebx);

	case SYS_WAITPID:
		return syscall_case_waitpid(ebx, ecx, edx);

	case SYS_WAIT4:
		return syscall_case_wait4(ebx, ecx, edx, esi);

	case SYS_DRUNIX_TCSETPGRP:
		return syscall_case_drunix_tcsetpgrp(ebx, ecx);

	case SYS_DRUNIX_TCGETPGRP:
		return syscall_case_drunix_tcgetpgrp(ebx);

	default:
		return syscall_case_unknown(eax);
	}
}
