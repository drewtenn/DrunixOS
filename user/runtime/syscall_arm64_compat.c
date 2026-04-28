/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_arm64_compat.c - ARM64 implementation of the public Drunix syscall API.
 */

#include "syscall.h"
#include "syscall_arm64_asm.h"
#include "syscall_arm64_nr.h"
#include "ustrlen.h"

#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200

#define SYS_NANOSLEEP 162
#define SYS_FORK 2
#define SYS_RENAME 38
#define SYS_STAT 106
#define SYS_DRUNIX_CLEAR 4000
#define SYS_DRUNIX_SCROLL_UP 4001
#define SYS_DRUNIX_SCROLL_DOWN 4002
#define SYS_DRUNIX_MODLOAD 4003
#define SYS_DRUNIX_TCGETATTR 4004
#define SYS_DRUNIX_TCSETATTR 4005
#define SYS_DRUNIX_TCSETPGRP 4006
#define SYS_DRUNIX_TCGETPGRP 4007
#define SYS_DRUNIX_GETDENTS_PATH 4008
#define SYS_DRUNIX_DISPLAY_CLAIM 4009

char **environ = 0;

#define syscall0 arm64_syscall0
#define syscall1 arm64_syscall1
#define syscall2 arm64_syscall2
#define syscall3 arm64_syscall3
#define syscall4 arm64_syscall4
#define syscall5 arm64_syscall5
#define syscall6 arm64_syscall6

void sys_exit(int code)
{
	(void)syscall1(ARM64_SYS_EXIT, code);
	for (;;)
		;
}

void sys_write(const char *msg)
{
	sys_fwrite(1, msg, ustrlen(msg));
}

void sys_write_n(const char *buf, int count)
{
	sys_fwrite(1, buf, count);
}

int sys_read(int fd, char *buf, int count)
{
	return (int)syscall3(ARM64_SYS_READ, fd, (long)buf, count);
}

int sys_open(const char *name)
{
	return sys_open_flags(name, SYS_O_RDONLY, 0);
}

int sys_open_flags(const char *name, int flags, int mode)
{
	return (int)syscall4(ARM64_SYS_OPENAT, AT_FDCWD, (long)name, flags, mode);
}

int sys_create(const char *name)
{
	return sys_open_flags(name, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, 0644);
}

int sys_fwrite(int fd, const char *buf, int count)
{
	return (int)syscall3(ARM64_SYS_WRITE, fd, (long)buf, count);
}

int sys_close(int fd)
{
	return (int)syscall1(ARM64_SYS_CLOSE, fd);
}

int sys_ioctl(int fd, unsigned int request, void *arg)
{
	return (int)syscall3(ARM64_SYS_IOCTL, fd, request, (long)arg);
}

int sys_exec(const char *filename, char **argv, int argc)
{
	(void)argc;
	return sys_execve(filename, argv, environ);
}

int sys_execve(const char *filename, char **argv, char **envp)
{
	return (int)syscall3(
	    ARM64_SYS_EXECVE, (long)filename, (long)argv, (long)envp);
}

int sys_wait(int pid)
{
	return sys_waitpid(pid, 0);
}

void sys_clear(void)
{
	(void)syscall0(SYS_DRUNIX_CLEAR);
}

int sys_display_claim(void)
{
	return (int)syscall0(SYS_DRUNIX_DISPLAY_CLAIM);
}

int sys_poll(sys_pollfd_t *fds, unsigned int nfds, int timeout)
{
	sys_timespec_t ts;
	sys_timespec_t *tsp = 0;

	if (timeout >= 0) {
		ts.tv_sec = timeout / 1000;
		ts.tv_nsec = (long)(timeout % 1000) * 1000000L;
		tsp = &ts;
	}
	return (int)syscall5(ARM64_SYS_PPOLL,
	                     (long)fds,
	                     nfds,
	                     (long)tsp,
	                     0,
	                     0);
}

int sys_getdents(const char *path, char *buf, int size)
{
	return (int)syscall3(SYS_DRUNIX_GETDENTS_PATH, (long)path, (long)buf, size);
}

int sys_mkdir(const char *name)
{
	return (int)syscall3(34, AT_FDCWD, (long)name, 0755);
}

int sys_modload(const char *path)
{
	return (int)syscall1(SYS_DRUNIX_MODLOAD, (long)path);
}

int sys_unlink(const char *name)
{
	return (int)syscall3(35, AT_FDCWD, (long)name, 0);
}

int sys_rename(const char *oldpath, const char *newpath)
{
	return (int)syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
}

int sys_rmdir(const char *name)
{
	return (int)syscall3(35, AT_FDCWD, (long)name, AT_REMOVEDIR);
}

int sys_chdir(const char *path)
{
	return (int)syscall1(49, (long)path);
}

int sys_getcwd(char *buf, int size)
{
	return (int)syscall2(ARM64_SYS_GETCWD, (long)buf, size);
}

int sys_fork(void)
{
	return (int)syscall0(SYS_FORK);
}

int sys_pipe(int fds[2])
{
	return (int)syscall2(ARM64_SYS_PIPE2, (long)fds, 0);
}

int sys_dup(int old_fd)
{
	return (int)syscall1(ARM64_SYS_DUP, old_fd);
}

int sys_dup2(int old_fd, int new_fd)
{
	if (old_fd == new_fd)
		return new_fd;
	return (int)syscall3(ARM64_SYS_DUP3, old_fd, new_fd, 0);
}

unsigned int sys_brk(unsigned int new_brk)
{
	return (unsigned int)syscall1(ARM64_SYS_BRK, new_brk);
}

void *sys_mmap(void *addr,
               unsigned int length,
               int prot,
               int flags,
               int fd,
               unsigned int offset)
{
	return (void *)syscall6(
	    ARM64_SYS_MMAP, (long)addr, length, prot, flags, fd, offset);
}

int sys_munmap(void *addr, unsigned int length)
{
	return (int)syscall2(ARM64_SYS_MUNMAP, (long)addr, length);
}

int sys_mprotect(void *addr, unsigned int length, int prot)
{
	return (int)syscall3(ARM64_SYS_MPROTECT, (long)addr, length, prot);
}

unsigned int sys_sleep(unsigned int seconds)
{
	sys_timespec_t req;
	sys_timespec_t rem;
	int r;
	req.tv_sec = (long)seconds;
	req.tv_nsec = 0;
	rem.tv_sec = 0;
	rem.tv_nsec = 0;
	r = (int)syscall2(SYS_NANOSLEEP, (long)&req, (long)&rem);
	return r == 0 ? 0u : (unsigned int)rem.tv_sec;
}

int sys_kill(int pid, int signum)
{
	return (int)syscall2(ARM64_SYS_KILL, pid, signum);
}

int sys_sigaction(int signum, void (*handler)(int), void (**old)(int))
{
	unsigned int act[8];
	unsigned int oldact[8];
	int r;

	for (unsigned int i = 0; i < 8; i++) {
		act[i] = 0;
		oldact[i] = 0;
	}

	act[0] = (unsigned int)(unsigned long)handler;
	r = (int)syscall4(ARM64_SYS_RT_SIGACTION,
	                  signum,
	                  (long)act,
	                  old ? (long)oldact : 0,
	                  sizeof(unsigned int));
	if (r == 0 && old)
		*old = (void (*)(int))(unsigned long)oldact[0];
	return r;
}

int sys_sigprocmask(int how, unsigned int *set, unsigned int *oldset)
{
	return (int)syscall4(ARM64_SYS_RT_SIGPROCMASK,
	                     how,
	                     (long)set,
	                     (long)oldset,
	                     sizeof(unsigned int));
}

void sys_scroll_up(int rows)
{
	(void)syscall1(SYS_DRUNIX_SCROLL_UP, rows);
}

void sys_scroll_down(int rows)
{
	(void)syscall1(SYS_DRUNIX_SCROLL_DOWN, rows);
}

int sys_stat(const char *path, dufs_stat_t *st)
{
	return (int)syscall2(SYS_STAT, (long)path, (long)st);
}

int sys_tcgetattr(int fd, termios_t *t)
{
	return (int)syscall2(SYS_DRUNIX_TCGETATTR, fd, (long)t);
}

int sys_tcsetattr(int fd, int action, const termios_t *t)
{
	return (int)syscall3(SYS_DRUNIX_TCSETATTR, fd, action, (long)t);
}

int sys_setpgid(int pid, int pgid)
{
	return (int)syscall2(ARM64_SYS_SETPGID, pid, pgid);
}

int sys_getpgid(int pid)
{
	return (int)syscall1(ARM64_SYS_GETPGID, pid);
}

int sys_lseek(int fd, int offset, int whence)
{
	return (int)syscall3(ARM64_SYS_LSEEK, fd, offset, whence);
}

int sys_getpid(void)
{
	return (int)syscall0(ARM64_SYS_GETPID);
}

int sys_clone(unsigned int flags,
              void *child_stack,
              int *parent_tid,
              void *tls,
              int *child_tid)
{
	return (int)syscall5(ARM64_SYS_CLONE,
	                     flags,
	                     (long)child_stack,
	                     (long)parent_tid,
	                     (long)tls,
	                     (long)child_tid);
}

int sys_gettid(void)
{
	return (int)syscall0(ARM64_SYS_GETTID);
}

int sys_set_tid_address(int *tidptr)
{
	return (int)syscall1(ARM64_SYS_SET_TID_ADDRESS, (long)tidptr);
}

int sys_yield(void)
{
	return (int)syscall0(ARM64_SYS_SCHED_YIELD);
}

void sys_exit_group(int code)
{
	(void)syscall1(ARM64_SYS_EXIT_GROUP, code);
	for (;;)
		;
}

int sys_getppid(void)
{
	return (int)syscall0(ARM64_SYS_GETPPID);
}

int sys_waitpid(int pid, int options)
{
	int r;
	int status = 0;
	r = (int)syscall4(ARM64_SYS_WAIT4, pid, (long)&status, options, 0);
	return r < 0 ? r : status;
}

int sys_waitpid_status(int pid, int *status, int options)
{
	int local_status = 0;

	if (!status)
		status = &local_status;
	return (int)syscall4(ARM64_SYS_WAIT4, pid, (long)status, options, 0);
}

int sys_tcsetpgrp(int fd, int pgid)
{
	return (int)syscall2(SYS_DRUNIX_TCSETPGRP, fd, pgid);
}

int sys_tcgetpgrp(int fd)
{
	return (int)syscall1(SYS_DRUNIX_TCGETPGRP, fd);
}

int sys_clock_gettime(int clock_id, sys_timespec_t *ts)
{
	return (int)syscall2(ARM64_SYS_CLOCK_GETTIME, clock_id, (long)ts);
}
