/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef USER_LIB_SYSCALL_ARM64_H
#define USER_LIB_SYSCALL_ARM64_H

long arm64_sys_write(int fd, const char *buf, unsigned long len);
long arm64_sys_openat(int dirfd, const char *path, int flags, int mode);
long arm64_sys_close(int fd);
long arm64_sys_read(int fd, void *buf, unsigned long len);
long arm64_sys_getpid(void);
long arm64_sys_getppid(void);
long arm64_sys_gettid(void);
long arm64_sys_getcwd(char *buf, unsigned long size);

#endif
