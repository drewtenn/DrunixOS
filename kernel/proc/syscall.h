/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/*
 * System call numbers (Linux i386 ABI for familiarity).
 * User code places the syscall number in EAX before executing INT 0x80.
 * Arguments follow the Linux i386 convention: EBX, ECX, EDX, ESI, EDI.
 *
 * Number  Name          Arguments → Return
 *      1  SYS_EXIT      ebx=code
 *      2  SYS_FWRITE    ebx=fd, ecx=buf, edx=count → bytes written
 *      3  SYS_READ      ebx=fd, ecx=buf, edx=count → bytes read
 *      4  SYS_WRITE     ebx=buf, ecx=count → bytes written (VGA)
 *      5  SYS_OPEN      ebx=filename → fd, -1
 *      6  SYS_CLOSE     ebx=fd → 0, -1
 *      7  SYS_WAIT      ebx=pid → exit_status, -1
 *      8  SYS_CREATE    ebx=filename → writable fd, -1
 *      9  SYS_UNLINK    ebx=filename → 0, -1
 *     10  SYS_FORK      → child_pid (parent), 0 (child), -1
 *     11  SYS_EXEC      ebx=filename, ecx=argv, edx=argc,
 *                       esi=envp, edi=envc → no return on success, -1
 *     12  SYS_CLEAR
 *     13  SYS_SCROLL_UP   ebx=rows
 *     14  SYS_SCROLL_DOWN ebx=rows
 *     15  SYS_MKDIR     ebx=name → 0, -1
 *     16  SYS_RMDIR     ebx=name → 0, -1
 *     17  SYS_CHDIR     ebx=path → 0, -1
 *     19  SYS_LSEEK     ebx=fd, ecx=offset, edx=whence → new_offset, -1
 *     20  SYS_GETPID    → pid
 *     37  SYS_KILL      ebx=pid (>0) or -pgid, ecx=signum → 0, -1
 *     38  SYS_RENAME    ebx=oldpath, ecx=newpath → 0, -1
 *     45  SYS_BRK       ebx=new_brk → actual_brk
 *     90  SYS_MMAP      ebx=old_mmap_args* → mapped address, -1
 *     91  SYS_MUNMAP    ebx=addr, ecx=len → 0, -1
 *     64  SYS_GETPPID   → parent_pid
 *     67  SYS_SIGACTION ebx=signum, ecx=handler_va, edx=old_handler_out* → 0, -1
 *    106  SYS_STAT      ebx=path, ecx=vfs_stat_t* → 0, -1
 *    119  SYS_SIGRETURN (no args; restores context from user-stack signal frame)
 *    125  SYS_MPROTECT  ebx=addr, ecx=len, edx=prot → 0, -1
 *    126  SYS_SIGPROCMASK ebx=how, ecx=newmask*, edx=oldmask* → 0, -1
 *    141  SYS_GETDENTS  ebx=path, ecx=buf, edx=bufsz → bytes
 *    158  SYS_YIELD
 *    162  SYS_SLEEP     ebx=seconds → remaining_seconds
 *    170  SYS_MODLOAD   ebx=filename → 0, negative
 *    171  SYS_PIPE      ebx=int[2]* → 0, -1   (fds[0]=read, fds[1]=write)
 *    172  SYS_DUP2      ebx=old_fd, ecx=new_fd → new_fd, -1
 *    183  SYS_GETCWD    ebx=buf, ecx=size → bytes written, -1
 *    184  SYS_TCGETATTR ebx=fd, ecx=termios_t* → 0, -1
 *    185  SYS_TCSETATTR ebx=fd, ecx=action, edx=termios_t* → 0, -1
 *    186  SYS_SETPGID   ebx=pid (0=self/direct child), ecx=pgid (0=pid) → 0, -1
 *    187  SYS_GETPGID   ebx=pid (0=self) → pgid, -1
 *    188  SYS_WAITPID   ebx=pid, ecx=options → encoded status, 0 (WNOHANG), -1
 *    189  SYS_TCSETPGRP ebx=fd, ecx=pgid → 0, -1
 *    190  SYS_TCGETPGRP ebx=fd → fg_pgid, -1
 *    265  SYS_CLOCK_GETTIME ebx=clockid, ecx=timespec* → 0, -1
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
#define SYS_FWRITE      2
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_WAIT        7
#define SYS_CREATE      8
#define SYS_UNLINK      9
#define SYS_FORK       10
#define SYS_EXEC       11
#define SYS_CLEAR      12
#define SYS_SCROLL_UP  13
#define SYS_SCROLL_DOWN 14
#define SYS_MKDIR      15
#define SYS_RMDIR      16
#define SYS_CHDIR      17
#define SYS_LSEEK      19
#define SYS_GETPID     20
#define SYS_KILL       37
#define SYS_RENAME     38
#define SYS_BRK        45
#define SYS_MMAP       90
#define SYS_MUNMAP     91
#define SYS_GETPPID    64
#define SYS_SIGACTION  67
#define SYS_STAT      106
#define SYS_SIGRETURN 119
#define SYS_MPROTECT  125
#define SYS_SIGPROCMASK 126
#define SYS_GETDENTS  141
#define SYS_YIELD     158
#define SYS_SLEEP     162
#define SYS_MODLOAD   170
#define SYS_PIPE      171
#define SYS_DUP2      172
#define SYS_GETCWD    183
#define SYS_TCGETATTR 184
#define SYS_TCSETATTR 185
#define SYS_SETPGID   186
#define SYS_GETPGID   187
#define SYS_WAITPID   188
#define SYS_TCSETPGRP 189
#define SYS_TCGETPGRP 190
#define SYS_CLOCK_GETTIME 265

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
 */
uint32_t syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx,
                         uint32_t edx, uint32_t esi, uint32_t edi);

#endif
