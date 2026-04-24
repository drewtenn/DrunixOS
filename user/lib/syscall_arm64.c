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

long arm64_sys_exit(int status)
{
	return arm64_syscall1(93, status);
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

long arm64_sys_truncate(const char *path, unsigned long len)
{
	return arm64_syscall2(45, (long)path, (long)len);
}

long arm64_sys_ftruncate(int fd, unsigned long len)
{
	return arm64_syscall2(46, fd, (long)len);
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

long arm64_sys_fchmodat(int dirfd, const char *path, int mode)
{
	return arm64_syscall3(53, dirfd, (long)path, mode);
}

long arm64_sys_fchownat(int dirfd,
                        const char *path,
                        unsigned int uid,
                        unsigned int gid,
                        int flags)
{
	return arm64_syscall6(54, dirfd, (long)path, uid, gid, flags, 0);
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

long arm64_sys_sync(void)
{
	return arm64_syscall0(81);
}

long arm64_sys_utimensat(int dirfd,
                         const char *path,
                         const void *times,
                         int flags)
{
	return arm64_syscall4(88, dirfd, (long)path, (long)times, flags);
}

long arm64_sys_set_tid_address(int *tidptr)
{
	return arm64_syscall1(96, (long)tidptr);
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

long arm64_sys_sched_getaffinity(int pid, unsigned long size, void *mask)
{
	return arm64_syscall3(123, pid, (long)size, (long)mask);
}

long arm64_sys_getpriority(int which, int who)
{
	return arm64_syscall2(141, which, who);
}

long arm64_sys_setpriority(int which, int who, int prio)
{
	return arm64_syscall3(140, which, who, prio);
}

long arm64_sys_getpgid(int pid)
{
	return arm64_syscall1(155, pid);
}

long arm64_sys_getsid(int pid)
{
	return arm64_syscall1(156, pid);
}

long arm64_sys_getrusage(int who, void *usage)
{
	return arm64_syscall2(165, who, (long)usage);
}

long arm64_sys_umask(int mask)
{
	return arm64_syscall1(166, mask);
}

long arm64_sys_gettimeofday(void *timeval, void *timezone)
{
	return arm64_syscall2(169, (long)timeval, (long)timezone);
}

long arm64_sys_getuid(void)
{
	return arm64_syscall0(174);
}

long arm64_sys_geteuid(void)
{
	return arm64_syscall0(175);
}

long arm64_sys_getgid(void)
{
	return arm64_syscall0(176);
}

long arm64_sys_getegid(void)
{
	return arm64_syscall0(177);
}

long arm64_sys_sysinfo(void *info)
{
	return arm64_syscall1(179, (long)info);
}

long arm64_sys_kill(int pid, int sig)
{
	return arm64_syscall2(129, pid, sig);
}

long arm64_sys_rt_sigaction(int sig,
                            const void *act,
                            void *oldact,
                            unsigned long size)
{
	return arm64_syscall4(134, sig, (long)act, (long)oldact, (long)size);
}

long arm64_sys_rt_sigprocmask(int how,
                              const void *set,
                              void *oldset,
                              unsigned long size)
{
	return arm64_syscall4(135, how, (long)set, (long)oldset, (long)size);
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

long arm64_sys_clone(unsigned long flags,
                     void *child_stack,
                     void *parent_tid,
                     void *tls,
                     void *child_tid)
{
	return arm64_syscall6(220,
	                      (long)flags,
	                      (long)child_stack,
	                      (long)parent_tid,
	                      (long)tls,
	                      (long)child_tid,
	                      0);
}

long arm64_sys_execve(const char *path,
                      char *const argv[],
                      char *const envp[])
{
	return arm64_syscall3(221, (long)path, (long)argv, (long)envp);
}

long arm64_sys_wait4(int pid, int *status, int options, void *rusage)
{
	return arm64_syscall4(260, pid, (long)status, options, (long)rusage);
}

long arm64_sys_prlimit64(int pid,
                         int resource,
                         const void *new_limit,
                         void *old_limit)
{
	return arm64_syscall4(
	    261, pid, resource, (long)new_limit, (long)old_limit);
}

long arm64_sys_statx(int dirfd,
                     const char *path,
                     int flags,
                     unsigned int mask,
                     void *statxbuf)
{
	return arm64_syscall6(
	    291, dirfd, (long)path, flags, (long)mask, (long)statxbuf, 0);
}
