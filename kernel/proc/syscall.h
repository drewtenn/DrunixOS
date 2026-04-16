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
#define SYS_UNLINK     10
#define SYS_EXECVE     11
#define SYS_CHDIR      12
#define SYS_LSEEK      19
#define SYS_GETPID     20
#define SYS_KILL       37
#define SYS_RENAME     38
#define SYS_MKDIR      39
#define SYS_RMDIR      40
#define SYS_PIPE       42
#define SYS_BRK        45
#define SYS_IOCTL      54
#define SYS_SETPGID    57
#define SYS_DUP2       63
#define SYS_GETPPID    64
#define SYS_SIGACTION  67
#define SYS_GETTIMEOFDAY 78
#define SYS_MMAP       90
#define SYS_MUNMAP     91
#define SYS_STAT      106
#define SYS_SIGRETURN 119
#define SYS_UNAME     122
#define SYS_MPROTECT  125
#define SYS_SIGPROCMASK 126
#define SYS_GETPGID   132
#define SYS_GETDENTS  141
#define SYS_YIELD     158
#define SYS_NANOSLEEP 162
#define SYS_GETCWD    183
#define SYS_MMAP2     192
#define SYS_FSTAT64   197
#define SYS_SET_THREAD_AREA 243
#define SYS_EXIT_GROUP 252
#define SYS_SET_TID_ADDRESS 258
#define SYS_CLOCK_GETTIME 265
#define SYS_CLOCK_GETTIME64 403

#define SYS_DRUNIX_CLEAR       4000
#define SYS_DRUNIX_SCROLL_UP   4001
#define SYS_DRUNIX_SCROLL_DOWN 4002
#define SYS_DRUNIX_MODLOAD     4003
#define SYS_DRUNIX_TCGETATTR   4004
#define SYS_DRUNIX_TCSETATTR   4005
#define SYS_DRUNIX_TCSETPGRP   4006
#define SYS_DRUNIX_TCGETPGRP   4007

#define PROT_NONE      0x0u
#define PROT_READ      0x1u
#define PROT_WRITE     0x2u
#define PROT_EXEC      0x4u

#define MAP_PRIVATE    0x02u
#define MAP_ANONYMOUS  0x20u

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
