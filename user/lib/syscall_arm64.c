/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "syscall_arm64.h"

static long arm64_syscall0(long nr)
{
	register long x0 __asm__("x0");
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
	return x0;
}

static long arm64_syscall1(long nr, long a0)
{
	register long x0 __asm__("x0") = a0;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
	return x0;
}

static long arm64_syscall2(long nr, long a0, long a1)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x8 __asm__("x8") = nr;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
	return x0;
}

static long arm64_syscall3(long nr, long a0, long a1, long a2)
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

static long arm64_syscall4(long nr, long a0, long a1, long a2, long a3)
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

static long
arm64_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
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

long arm64_sys_write(int fd, const char *buf, unsigned long len)
{
	return arm64_syscall3(64, fd, (long)buf, (long)len);
}

long arm64_sys_openat(int dirfd, const char *path, int flags, int mode)
{
	return arm64_syscall4(56, dirfd, (long)path, flags, mode);
}

long arm64_sys_dup(int oldfd)
{
	return arm64_syscall1(23, oldfd);
}

long arm64_sys_dup3(int oldfd, int newfd, int flags)
{
	return arm64_syscall3(24, oldfd, newfd, flags);
}

long arm64_sys_fcntl(int fd, int cmd, long arg)
{
	return arm64_syscall3(25, fd, cmd, arg);
}

long arm64_sys_ioctl(int fd, unsigned long request, void *arg)
{
	return arm64_syscall3(29, fd, (long)request, (long)arg);
}

long arm64_sys_mkdirat(int dirfd, const char *path, int mode)
{
	return arm64_syscall3(34, dirfd, (long)path, mode);
}

long arm64_sys_unlinkat(int dirfd, const char *path, int flags)
{
	return arm64_syscall3(35, dirfd, (long)path, flags);
}

long arm64_sys_faccessat(int dirfd, const char *path, int mode)
{
	return arm64_syscall3(48, dirfd, (long)path, mode);
}

long arm64_sys_chdir(const char *path)
{
	return arm64_syscall1(49, (long)path);
}

long arm64_sys_close(int fd)
{
	return arm64_syscall1(57, fd);
}

long arm64_sys_pipe2(int pipefd[2], int flags)
{
	return arm64_syscall2(59, (long)pipefd, flags);
}

long arm64_sys_lseek(int fd, long offset, int whence)
{
	return arm64_syscall3(62, fd, offset, whence);
}

long arm64_sys_read(int fd, void *buf, unsigned long len)
{
	return arm64_syscall3(63, fd, (long)buf, (long)len);
}

long arm64_sys_readlinkat(int dirfd,
                          const char *path,
                          char *buf,
                          unsigned long len)
{
	return arm64_syscall4(78, dirfd, (long)path, (long)buf, (long)len);
}

long arm64_sys_getpid(void)
{
	return arm64_syscall0(172);
}

long arm64_sys_getppid(void)
{
	return arm64_syscall0(173);
}

long arm64_sys_gettid(void)
{
	return arm64_syscall0(178);
}

long arm64_sys_getcwd(char *buf, unsigned long size)
{
	return arm64_syscall2(17, (long)buf, (long)size);
}

long arm64_sys_fstat(int fd, void *statbuf)
{
	return arm64_syscall2(80, fd, (long)statbuf);
}

long arm64_sys_newfstatat(int dirfd, const char *path, void *statbuf, int flags)
{
	return arm64_syscall4(79, dirfd, (long)path, (long)statbuf, flags);
}

long arm64_sys_getdents64(int fd, void *dirp, unsigned long count)
{
	return arm64_syscall3(61, fd, (long)dirp, (long)count);
}

long arm64_sys_uname(void *utsname)
{
	return arm64_syscall1(160, (long)utsname);
}

long arm64_sys_clock_gettime(int clock_id, void *timespec)
{
	return arm64_syscall2(113, clock_id, (long)timespec);
}

long arm64_sys_gettimeofday(void *timeval, void *timezone)
{
	return arm64_syscall2(169, (long)timeval, (long)timezone);
}

long arm64_sys_brk(void *addr)
{
	return arm64_syscall1(214, (long)addr);
}

long arm64_sys_mmap(void *addr,
                    unsigned long len,
                    int prot,
                    int flags,
                    int fd,
                    unsigned long offset)
{
	return arm64_syscall6(222, (long)addr, (long)len, prot, flags, fd, offset);
}

long arm64_sys_munmap(void *addr, unsigned long len)
{
	return arm64_syscall2(215, (long)addr, (long)len);
}

long arm64_sys_mprotect(void *addr, unsigned long len, int prot)
{
	return arm64_syscall3(226, (long)addr, (long)len, prot);
}
