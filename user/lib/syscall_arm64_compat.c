/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_arm64_compat.c - ARM64 implementation of the public Drunix syscall API.
 */

#include "syscall.h"

#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define ARM64_SYS_GETCWD 17
#define ARM64_SYS_DUP 23
#define ARM64_SYS_DUP3 24
#define ARM64_SYS_OPENAT 56
#define ARM64_SYS_CLOSE 57
#define ARM64_SYS_PIPE2 59
#define ARM64_SYS_LSEEK 62
#define ARM64_SYS_READ 63
#define ARM64_SYS_WRITE 64
#define ARM64_SYS_EXIT 93
#define ARM64_SYS_EXIT_GROUP 94
#define ARM64_SYS_SET_TID_ADDRESS 96
#define ARM64_SYS_CLOCK_GETTIME 113
#define ARM64_SYS_SCHED_YIELD 124
#define ARM64_SYS_KILL 129
#define ARM64_SYS_RT_SIGACTION 134
#define ARM64_SYS_RT_SIGPROCMASK 135
#define ARM64_SYS_SETPGID 154
#define ARM64_SYS_GETPGID 155
#define ARM64_SYS_GETPID 172
#define ARM64_SYS_GETPPID 173
#define ARM64_SYS_GETTID 178
#define ARM64_SYS_BRK 214
#define ARM64_SYS_MUNMAP 215
#define ARM64_SYS_CLONE 220
#define ARM64_SYS_EXECVE 221
#define ARM64_SYS_MMAP 222
#define ARM64_SYS_MPROTECT 226
#define ARM64_SYS_WAIT4 260

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

char **environ = 0;

static long syscall0(long nr)
{
	register long x0 __asm__("x0");
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
	return x0;
}

static long syscall1(long nr, long a0)
{
	register long x0 __asm__("x0") = a0;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
	return x0;
}

static long syscall2(long nr, long a0, long a1)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
	return x0;
}

static long syscall3(long nr, long a0, long a1, long a2)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0"
	                 : "+r"(x0)
	                 : "r"(x1), "r"(x2), "r"(x8)
	                 : "memory");
	return x0;
}

static long syscall4(long nr, long a0, long a1, long a2, long a3)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0"
	                 : "+r"(x0)
	                 : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
	                 : "memory");
	return x0;
}

static long syscall5(long nr, long a0, long a1, long a2, long a3, long a4)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0"
	                 : "+r"(x0)
	                 : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
	                 : "memory");
	return x0;
}

static long
syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0"
	                 : "+r"(x0)
	                 : "r"(x1),
	                   "r"(x2),
	                   "r"(x3),
	                   "r"(x4),
	                   "r"(x5),
	                   "r"(x8)
	                 : "memory");
	return x0;
}

static int ustrlen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

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

int sys_exec(const char *filename, char **argv, int argc)
{
	(void)argc;
	return sys_execve(filename, argv, environ);
}

int sys_execve(const char *filename, char **argv, char **envp)
{
	return (int)syscall3(ARM64_SYS_EXECVE, (long)filename, (long)argv, (long)envp);
}

int sys_wait(int pid)
{
	return sys_waitpid(pid, 0);
}

void sys_clear(void)
{
	(void)syscall0(SYS_DRUNIX_CLEAR);
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
	return (int)syscall4(
	    ARM64_SYS_RT_SIGPROCMASK, how, (long)set, (long)oldset, sizeof(unsigned int));
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
