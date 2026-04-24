/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef USER_LIB_SYSCALL_ARM64_H
#define USER_LIB_SYSCALL_ARM64_H

long arm64_sys_write(int fd, const char *buf, unsigned long len);
long arm64_sys_openat(int dirfd, const char *path, int flags, int mode);
long arm64_sys_dup(int oldfd);
long arm64_sys_dup3(int oldfd, int newfd, int flags);
long arm64_sys_fcntl(int fd, int cmd, long arg);
long arm64_sys_ioctl(int fd, unsigned long request, void *arg);
long arm64_sys_truncate(const char *path, unsigned long len);
long arm64_sys_ftruncate(int fd, unsigned long len);
long arm64_sys_mkdirat(int dirfd, const char *path, int mode);
long arm64_sys_unlinkat(int dirfd, const char *path, int flags);
long arm64_sys_faccessat(int dirfd, const char *path, int mode);
long arm64_sys_chdir(const char *path);
long arm64_sys_fchmodat(int dirfd, const char *path, int mode);
long arm64_sys_fchownat(int dirfd,
                        const char *path,
                        unsigned int uid,
                        unsigned int gid,
                        int flags);
long arm64_sys_close(int fd);
long arm64_sys_pipe2(int pipefd[2], int flags);
long arm64_sys_lseek(int fd, long offset, int whence);
long arm64_sys_read(int fd, void *buf, unsigned long len);
long arm64_sys_readlinkat(int dirfd,
                          const char *path,
                          char *buf,
                          unsigned long len);
long arm64_sys_getpid(void);
long arm64_sys_getppid(void);
long arm64_sys_gettid(void);
long arm64_sys_getcwd(char *buf, unsigned long size);
long arm64_sys_fstat(int fd, void *statbuf);
long arm64_sys_sync(void);
long arm64_sys_utimensat(int dirfd,
                         const char *path,
                         const void *times,
                         int flags);
long arm64_sys_set_tid_address(int *tidptr);
long arm64_sys_newfstatat(int dirfd, const char *path, void *statbuf, int flags);
long arm64_sys_getdents64(int fd, void *dirp, unsigned long count);
long arm64_sys_uname(void *utsname);
long arm64_sys_clock_gettime(int clock_id, void *timespec);
long arm64_sys_sched_getaffinity(int pid, unsigned long size, void *mask);
long arm64_sys_getpriority(int which, int who);
long arm64_sys_setpriority(int which, int who, int prio);
long arm64_sys_getpgid(int pid);
long arm64_sys_getsid(int pid);
long arm64_sys_getrusage(int who, void *usage);
long arm64_sys_umask(int mask);
long arm64_sys_gettimeofday(void *timeval, void *timezone);
long arm64_sys_getuid(void);
long arm64_sys_geteuid(void);
long arm64_sys_getgid(void);
long arm64_sys_getegid(void);
long arm64_sys_sysinfo(void *info);
long arm64_sys_kill(int pid, int sig);
long arm64_sys_rt_sigaction(int sig,
                            const void *act,
                            void *oldact,
                            unsigned long size);
long arm64_sys_rt_sigprocmask(int how,
                              const void *set,
                              void *oldset,
                              unsigned long size);
long arm64_sys_brk(void *addr);
long arm64_sys_mmap(void *addr,
                    unsigned long len,
                    int prot,
                    int flags,
                    int fd,
                    unsigned long offset);
long arm64_sys_munmap(void *addr, unsigned long len);
long arm64_sys_mprotect(void *addr, unsigned long len, int prot);
long arm64_sys_clone(unsigned long flags,
                     void *child_stack,
                     void *parent_tid,
                     void *tls,
                     void *child_tid);
long arm64_sys_execve(const char *path,
                      char *const argv[],
                      char *const envp[]);
long arm64_sys_wait4(int pid, int *status, int options, void *rusage);
long arm64_sys_prlimit64(int pid,
                         int resource,
                         const void *new_limit,
                         void *old_limit);
long arm64_sys_statx(int dirfd,
                     const char *path,
                     int flags,
                     unsigned int mask,
                     void *statxbuf);

#endif
