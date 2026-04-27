/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "syscall_arm64.h"
#include "syscall_arm64_asm.h"
#include "syscall_arm64_nr.h"

long arm64_sys_write(int fd, const char *buf, unsigned long len)
{
	return arm64_syscall3(ARM64_SYS_WRITE, fd, (long)buf, (long)len);
}

long arm64_sys_exit(int status)
{
	return arm64_syscall1(ARM64_SYS_EXIT, status);
}

long arm64_sys_openat(int dirfd, const char *path, int flags, int mode)
{
	return arm64_syscall4(ARM64_SYS_OPENAT, dirfd, (long)path, flags, mode);
}

long arm64_sys_dup(int oldfd)
{
	return arm64_syscall1(ARM64_SYS_DUP, oldfd);
}

long arm64_sys_dup3(int oldfd, int newfd, int flags)
{
	return arm64_syscall3(ARM64_SYS_DUP3, oldfd, newfd, flags);
}

long arm64_sys_fcntl(int fd, int cmd, long arg)
{
	return arm64_syscall3(ARM64_SYS_FCNTL, fd, cmd, arg);
}

long arm64_sys_ioctl(int fd, unsigned long request, void *arg)
{
	return arm64_syscall3(ARM64_SYS_IOCTL, fd, (long)request, (long)arg);
}

long arm64_sys_truncate(const char *path, unsigned long len)
{
	return arm64_syscall2(ARM64_SYS_TRUNCATE, (long)path, (long)len);
}

long arm64_sys_ftruncate(int fd, unsigned long len)
{
	return arm64_syscall2(ARM64_SYS_FTRUNCATE, fd, (long)len);
}

long arm64_sys_mkdirat(int dirfd, const char *path, int mode)
{
	return arm64_syscall3(ARM64_SYS_MKDIRAT, dirfd, (long)path, mode);
}

long arm64_sys_unlinkat(int dirfd, const char *path, int flags)
{
	return arm64_syscall3(ARM64_SYS_UNLINKAT, dirfd, (long)path, flags);
}

long arm64_sys_faccessat(int dirfd, const char *path, int mode)
{
	return arm64_syscall3(ARM64_SYS_FACCESSAT, dirfd, (long)path, mode);
}

long arm64_sys_chdir(const char *path)
{
	return arm64_syscall1(ARM64_SYS_CHDIR, (long)path);
}

long arm64_sys_fchmodat(int dirfd, const char *path, int mode)
{
	return arm64_syscall3(ARM64_SYS_FCHMODAT, dirfd, (long)path, mode);
}

long arm64_sys_fchownat(int dirfd,
                        const char *path,
                        unsigned int uid,
                        unsigned int gid,
                        int flags)
{
	return arm64_syscall6(
	    ARM64_SYS_FCHOWNAT, dirfd, (long)path, uid, gid, flags, 0);
}

long arm64_sys_close(int fd)
{
	return arm64_syscall1(ARM64_SYS_CLOSE, fd);
}

long arm64_sys_pipe2(int pipefd[2], int flags)
{
	return arm64_syscall2(ARM64_SYS_PIPE2, (long)pipefd, flags);
}

long arm64_sys_lseek(int fd, long offset, int whence)
{
	return arm64_syscall3(ARM64_SYS_LSEEK, fd, offset, whence);
}

long arm64_sys_read(int fd, void *buf, unsigned long len)
{
	return arm64_syscall3(ARM64_SYS_READ, fd, (long)buf, (long)len);
}

long arm64_sys_readlinkat(int dirfd,
                          const char *path,
                          char *buf,
                          unsigned long len)
{
	return arm64_syscall4(
	    ARM64_SYS_READLINKAT, dirfd, (long)path, (long)buf, (long)len);
}

long arm64_sys_getpid(void)
{
	return arm64_syscall0(ARM64_SYS_GETPID);
}

long arm64_sys_getppid(void)
{
	return arm64_syscall0(ARM64_SYS_GETPPID);
}

long arm64_sys_gettid(void)
{
	return arm64_syscall0(ARM64_SYS_GETTID);
}

long arm64_sys_getcwd(char *buf, unsigned long size)
{
	return arm64_syscall2(ARM64_SYS_GETCWD, (long)buf, (long)size);
}

long arm64_sys_fstat(int fd, void *statbuf)
{
	return arm64_syscall2(ARM64_SYS_FSTAT, fd, (long)statbuf);
}

long arm64_sys_sync(void)
{
	return arm64_syscall0(ARM64_SYS_SYNC);
}

long arm64_sys_utimensat(int dirfd,
                         const char *path,
                         const void *times,
                         int flags)
{
	return arm64_syscall4(
	    ARM64_SYS_UTIMENSAT, dirfd, (long)path, (long)times, flags);
}

long arm64_sys_set_tid_address(int *tidptr)
{
	return arm64_syscall1(ARM64_SYS_SET_TID_ADDRESS, (long)tidptr);
}

long arm64_sys_newfstatat(int dirfd, const char *path, void *statbuf, int flags)
{
	return arm64_syscall4(
	    ARM64_SYS_NEWFSTATAT, dirfd, (long)path, (long)statbuf, flags);
}

long arm64_sys_getdents64(int fd, void *dirp, unsigned long count)
{
	return arm64_syscall3(ARM64_SYS_GETDENTS64, fd, (long)dirp, (long)count);
}

long arm64_sys_uname(void *utsname)
{
	return arm64_syscall1(ARM64_SYS_UNAME, (long)utsname);
}

long arm64_sys_clock_gettime(int clock_id, void *timespec)
{
	return arm64_syscall2(ARM64_SYS_CLOCK_GETTIME, clock_id, (long)timespec);
}

long arm64_sys_sched_getaffinity(int pid, unsigned long size, void *mask)
{
	return arm64_syscall3(
	    ARM64_SYS_SCHED_GETAFFINITY, pid, (long)size, (long)mask);
}

long arm64_sys_getpriority(int which, int who)
{
	return arm64_syscall2(ARM64_SYS_GETPRIORITY, which, who);
}

long arm64_sys_setpriority(int which, int who, int prio)
{
	return arm64_syscall3(ARM64_SYS_SETPRIORITY, which, who, prio);
}

long arm64_sys_getpgid(int pid)
{
	return arm64_syscall1(ARM64_SYS_GETPGID, pid);
}

long arm64_sys_getsid(int pid)
{
	return arm64_syscall1(ARM64_SYS_GETSID, pid);
}

long arm64_sys_getrusage(int who, void *usage)
{
	return arm64_syscall2(ARM64_SYS_GETRUSAGE, who, (long)usage);
}

long arm64_sys_umask(int mask)
{
	return arm64_syscall1(ARM64_SYS_UMASK, mask);
}

long arm64_sys_gettimeofday(void *timeval, void *timezone)
{
	return arm64_syscall2(
	    ARM64_SYS_GETTIMEOFDAY, (long)timeval, (long)timezone);
}

long arm64_sys_getuid(void)
{
	return arm64_syscall0(ARM64_SYS_GETUID);
}

long arm64_sys_geteuid(void)
{
	return arm64_syscall0(ARM64_SYS_GETEUID);
}

long arm64_sys_getgid(void)
{
	return arm64_syscall0(ARM64_SYS_GETGID);
}

long arm64_sys_getegid(void)
{
	return arm64_syscall0(ARM64_SYS_GETEGID);
}

long arm64_sys_sysinfo(void *info)
{
	return arm64_syscall1(ARM64_SYS_SYSINFO, (long)info);
}

long arm64_sys_kill(int pid, int sig)
{
	return arm64_syscall2(ARM64_SYS_KILL, pid, sig);
}

long arm64_sys_rt_sigaction(int sig,
                            const void *act,
                            void *oldact,
                            unsigned long size)
{
	return arm64_syscall4(
	    ARM64_SYS_RT_SIGACTION, sig, (long)act, (long)oldact, (long)size);
}

long arm64_sys_rt_sigprocmask(int how,
                              const void *set,
                              void *oldset,
                              unsigned long size)
{
	return arm64_syscall4(
	    ARM64_SYS_RT_SIGPROCMASK, how, (long)set, (long)oldset, (long)size);
}

long arm64_sys_brk(void *addr)
{
	return arm64_syscall1(ARM64_SYS_BRK, (long)addr);
}

long arm64_sys_mmap(void *addr,
                    unsigned long len,
                    int prot,
                    int flags,
                    int fd,
                    unsigned long offset)
{
	return arm64_syscall6(
	    ARM64_SYS_MMAP, (long)addr, (long)len, prot, flags, fd, offset);
}

long arm64_sys_munmap(void *addr, unsigned long len)
{
	return arm64_syscall2(ARM64_SYS_MUNMAP, (long)addr, (long)len);
}

long arm64_sys_mprotect(void *addr, unsigned long len, int prot)
{
	return arm64_syscall3(ARM64_SYS_MPROTECT, (long)addr, (long)len, prot);
}

long arm64_sys_clone(unsigned long flags,
                     void *child_stack,
                     void *parent_tid,
                     void *tls,
                     void *child_tid)
{
	return arm64_syscall6(ARM64_SYS_CLONE,
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
	return arm64_syscall3(
	    ARM64_SYS_EXECVE, (long)path, (long)argv, (long)envp);
}

long arm64_sys_wait4(int pid, int *status, int options, void *rusage)
{
	return arm64_syscall4(
	    ARM64_SYS_WAIT4, pid, (long)status, options, (long)rusage);
}

long arm64_sys_prlimit64(int pid,
                         int resource,
                         const void *new_limit,
                         void *old_limit)
{
	return arm64_syscall4(
	    ARM64_SYS_PRLIMIT64, pid, resource, (long)new_limit, (long)old_limit);
}

long arm64_sys_statx(int dirfd,
                     const char *path,
                     int flags,
                     unsigned int mask,
                     void *statxbuf)
{
	return arm64_syscall6(ARM64_SYS_STATX,
	                      dirfd,
	                      (long)path,
	                      flags,
	                      (long)mask,
	                      (long)statxbuf,
	                      0);
}
