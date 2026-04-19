/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/*
 * Public system call numbers follow the Linux i386 INT 0x80 ABI.  User code
 * places the syscall number in EAX and arguments in EBX, ECX, EDX, ESI, EDI.
 * Drunix-only calls live in the SYS_DRUNIX_* private range below so they do
 * not collide with Linux binaries.
 *
 * SYS_TCSETATTR action values:
 *    0 = TCSANOW   — apply immediately
 *    2 = TCSAFLUSH — apply after flushing unread input
 *
 * SYS_SIGPROCMASK how values:
 *    0 = SIG_BLOCK    — add newmask bits to blocked set
 *    1 = SIG_UNBLOCK  — remove newmask bits from blocked set
 *    2 = SIG_SETMASK  — replace blocked set with newmask
 */
#define SYS_EXIT        1
#define SYS_FORK        2
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_WAITPID     7
#define SYS_CREAT       8
#define SYS_CHMOD      15
#define SYS_LCHOWN     16
#define SYS_UNLINK     10
#define SYS_EXECVE     11
#define SYS_CHDIR      12
#define SYS_ACCESS     33
#define SYS_SYNC       36
#define SYS_LSEEK      19
#define SYS_GETPID     20
#define SYS_KILL       37
#define SYS_RENAME     38
#define SYS_MKDIR      39
#define SYS_RMDIR      40
#define SYS_DUP        41
#define SYS_PIPE       42
#define SYS_BRK        45
#define SYS_IOCTL      54
#define SYS_SETPGID    57
#define SYS_UMASK      60
#define SYS_DUP2       63
#define SYS_GETPPID    64
#define SYS_SETSID     66
#define SYS_SIGACTION  67
#define SYS_GETTIMEOFDAY 78
#define SYS_READLINK   85
#define SYS_MMAP       90
#define SYS_MUNMAP     91
#define SYS_GETPRIORITY 96
#define SYS_SETPRIORITY 97
#define SYS_STAT      106
#define SYS_LSTAT64   107
#define SYS_WAIT4     114
#define SYS_SYSINFO   116
#define SYS_SIGRETURN 119
#define SYS_CLONE     120
#define SYS_UNAME     122
#define SYS_MPROTECT  125
#define SYS_SIGPROCMASK 126
#define SYS_GETPGID   132
#define SYS__LLSEEK   140
#define SYS_GETDENTS  141
#define SYS__NEWSELECT 142
#define SYS_GETSID    147
#define SYS_READV     145
#define SYS_WRITEV    146
#define SYS_YIELD     158
#define SYS_NANOSLEEP 162
#define SYS_POLL      168
#define SYS_RT_SIGACTION 174
#define SYS_RT_SIGPROCMASK 175
#define SYS_CHOWN32   212
#define SYS_GETCWD    183
#define SYS_VFORK     190
#define SYS_MMAP2     192
#define SYS_TRUNCATE64 193
#define SYS_FTRUNCATE64 194
#define SYS_STAT64    195
#define SYS_FSTAT64   197
#define SYS_GETUID32  199
#define SYS_GETGID32  200
#define SYS_GETEUID32 201
#define SYS_GETEGID32 202
#define SYS_SETUID32  213
#define SYS_SETGID32  214
#define SYS_GETDENTS64 220
#define SYS_FCNTL64   221
#define SYS_GETTID    224
#define SYS_SENDFILE64 239
#define SYS_SCHED_GETAFFINITY 242
#define SYS_SET_THREAD_AREA 243
#define SYS_EXIT_GROUP 252
#define SYS_SET_TID_ADDRESS 258
#define SYS_CLOCK_GETTIME 265
#define SYS_STATFS64  268
#define SYS_FSTATFS64 269
#define SYS_OPENAT    295
#define SYS_MKDIRAT   296
#define SYS_FCHOWNAT  298
#define SYS_FUTIMESAT 299
#define SYS_FSTATAT64 300
#define SYS_UNLINKAT  301
#define SYS_RENAMEAT  302
#define SYS_LINKAT    303
#define SYS_SYMLINKAT 304
#define SYS_READLINKAT 305
#define SYS_FCHMODAT  306
#define SYS_FACCESSAT 307
#define SYS_UTIMENSAT 320
#define SYS_PRLIMIT64 340
#define SYS_STATX     383
#define SYS_CLOCK_GETTIME64 403

#define SYS_DRUNIX_CLEAR       4000
#define SYS_DRUNIX_SCROLL_UP   4001
#define SYS_DRUNIX_SCROLL_DOWN 4002
#define SYS_DRUNIX_MODLOAD     4003
#define SYS_DRUNIX_TCGETATTR   4004
#define SYS_DRUNIX_TCSETATTR   4005
#define SYS_DRUNIX_TCSETPGRP   4006
#define SYS_DRUNIX_TCGETPGRP   4007
#define SYS_DRUNIX_GETDENTS_PATH 4008

#define PROT_NONE      0x0u
#define PROT_READ      0x1u
#define PROT_WRITE     0x2u
#define PROT_EXEC      0x4u

#define MAP_PRIVATE    0x02u
#define MAP_ANONYMOUS  0x20u

#define CLONE_VM             0x00000100u
#define CLONE_FS             0x00000200u
#define CLONE_FILES          0x00000400u
#define CLONE_SIGHAND        0x00000800u
#define CLONE_THREAD         0x00010000u
#define CLONE_SETTLS         0x00080000u
#define CLONE_PARENT_SETTID  0x00100000u
#define CLONE_CHILD_CLEARTID 0x00200000u
#define CLONE_CHILD_SETTID   0x01000000u

/*
 * syscall_handler: C dispatcher called from the INT 0x80 trampoline in isr.asm.
 *
 * The trampoline extracts register values from the saved iret frame and passes
 * them as ordinary C arguments.
 *
 * eax: syscall number
 * ebx: first argument (arg1)
 * ecx: second argument (arg2)
 * edx: third argument (arg3)
 * esi: fourth argument (arg4)
 * edi: fifth argument (arg5)
 * ebp: sixth argument (arg6, used by Linux i386 mmap2)
 */
uint32_t syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx,
                         uint32_t edx, uint32_t esi, uint32_t edi,
                         uint32_t ebp);

#ifdef KTEST_ENABLED
int syscall_stdout_would_fallback(void *desktop,
                                  uint32_t pid,
                                  const char *buf,
                                  uint32_t len);
#endif

#endif
