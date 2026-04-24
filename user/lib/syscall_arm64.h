/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef USER_LIB_SYSCALL_ARM64_H
#define USER_LIB_SYSCALL_ARM64_H

long arm64_sys_write(int fd, const char *buf, unsigned long len);
long arm64_sys_openat(int dirfd, const char *path, int flags, int mode);
long arm64_sys_dup(int oldfd);
long arm64_sys_dup3(int oldfd, int newfd, int flags);
long arm64_sys_fcntl(int fd, int cmd, long arg);
long arm64_sys_ioctl(int fd, unsigned long request, void *arg);
long arm64_sys_mkdirat(int dirfd, const char *path, int mode);
long arm64_sys_unlinkat(int dirfd, const char *path, int flags);
long arm64_sys_faccessat(int dirfd, const char *path, int mode);
long arm64_sys_chdir(const char *path);
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
long arm64_sys_newfstatat(int dirfd, const char *path, void *statbuf, int flags);
long arm64_sys_getdents64(int fd, void *dirp, unsigned long count);
long arm64_sys_uname(void *utsname);
long arm64_sys_clock_gettime(int clock_id, void *timespec);
long arm64_sys_gettimeofday(void *timeval, void *timezone);
long arm64_sys_brk(void *addr);
long arm64_sys_mmap(void *addr,
                    unsigned long len,
                    int prot,
                    int flags,
                    int fd,
                    unsigned long offset);
long arm64_sys_munmap(void *addr, unsigned long len);
long arm64_sys_mprotect(void *addr, unsigned long len, int prot);

#endif
