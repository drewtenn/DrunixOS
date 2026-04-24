/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef USER_LIB_SYSCALL_ARM64_H
#define USER_LIB_SYSCALL_ARM64_H

long arm64_sys_write(int fd, const char *buf, unsigned long len);

#endif
