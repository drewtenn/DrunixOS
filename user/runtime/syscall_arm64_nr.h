/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * syscall_arm64_nr.h — Linux AArch64 EABI syscall numbers.
 *
 * Single source of truth for the numeric constants that both
 * syscall_arm64.c (low-level parity helpers consumed by arm64init.elf)
 * and syscall_arm64_compat.c (Drunix's portable user libc on ARM64)
 * use to issue `svc #0`.
 *
 * Reference: Linux's arch/arm64/include/asm/unistd*.h.  Per
 * docs/contributing/linux-reference.md, these are externally-defined
 * ABI numbers, not Linux source.
 */

#ifndef USER_LIB_SYSCALL_ARM64_NR_H
#define USER_LIB_SYSCALL_ARM64_NR_H

#define ARM64_SYS_GETCWD 17
#define ARM64_SYS_DUP 23
#define ARM64_SYS_DUP3 24
#define ARM64_SYS_FCNTL 25
#define ARM64_SYS_IOCTL 29
#define ARM64_SYS_MKDIRAT 34
#define ARM64_SYS_UNLINKAT 35
#define ARM64_SYS_TRUNCATE 45
#define ARM64_SYS_FTRUNCATE 46
#define ARM64_SYS_FACCESSAT 48
#define ARM64_SYS_CHDIR 49
#define ARM64_SYS_FCHMODAT 53
#define ARM64_SYS_FCHOWNAT 54
#define ARM64_SYS_OPENAT 56
#define ARM64_SYS_CLOSE 57
#define ARM64_SYS_PIPE2 59
#define ARM64_SYS_GETDENTS64 61
#define ARM64_SYS_LSEEK 62
#define ARM64_SYS_READ 63
#define ARM64_SYS_WRITE 64
#define ARM64_SYS_PPOLL 73
#define ARM64_SYS_READLINKAT 78
#define ARM64_SYS_NEWFSTATAT 79
#define ARM64_SYS_FSTAT 80
#define ARM64_SYS_SYNC 81
#define ARM64_SYS_UTIMENSAT 88
#define ARM64_SYS_EXIT 93
#define ARM64_SYS_EXIT_GROUP 94
#define ARM64_SYS_SET_TID_ADDRESS 96
#define ARM64_SYS_CLOCK_GETTIME 113
#define ARM64_SYS_SCHED_GETAFFINITY 123
#define ARM64_SYS_SCHED_YIELD 124
#define ARM64_SYS_KILL 129
#define ARM64_SYS_RT_SIGACTION 134
#define ARM64_SYS_RT_SIGPROCMASK 135
#define ARM64_SYS_SETPRIORITY 140
#define ARM64_SYS_GETPRIORITY 141
#define ARM64_SYS_SETPGID 154
#define ARM64_SYS_GETPGID 155
#define ARM64_SYS_GETSID 156
#define ARM64_SYS_UNAME 160
#define ARM64_SYS_GETRUSAGE 165
#define ARM64_SYS_UMASK 166
#define ARM64_SYS_GETTIMEOFDAY 169
#define ARM64_SYS_GETPID 172
#define ARM64_SYS_GETPPID 173
#define ARM64_SYS_GETUID 174
#define ARM64_SYS_GETEUID 175
#define ARM64_SYS_GETGID 176
#define ARM64_SYS_GETEGID 177
#define ARM64_SYS_GETTID 178
#define ARM64_SYS_SYSINFO 179
#define ARM64_SYS_BRK 214
#define ARM64_SYS_MUNMAP 215
#define ARM64_SYS_CLONE 220
#define ARM64_SYS_EXECVE 221
#define ARM64_SYS_MMAP 222
#define ARM64_SYS_MPROTECT 226
#define ARM64_SYS_WAIT4 260
#define ARM64_SYS_PRLIMIT64 261
#define ARM64_SYS_STATX 291

#endif
