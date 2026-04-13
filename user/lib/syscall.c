/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall.c — INT 0x80 syscall wrappers for ring-3 user programs.
 *
 * All inline assembly is centralised here.  User programs include
 * syscall.h and call these functions as ordinary C.
 *
 * ABI (Linux i386 compatible):
 *   EAX = syscall number
 *   EBX = arg1, ECX = arg2, EDX = arg3
 *   Return value in EAX (written back by the kernel's isr128 trampoline).
 *
 * Syscall numbers (kernel/proc/syscall.h):
 *   1   SYS_EXIT      ebx=exit_code
 *   2   SYS_FWRITE    ebx=fd, ecx=buf, edx=count  → bytes written in eax
 *   3   SYS_READ      ebx=fd, ecx=buf, edx=count  → bytes read in eax (0=EOF)
 *   4   SYS_WRITE     ebx=buf, ecx=count          → bytes written in eax (VGA)
 *   5   SYS_OPEN      ebx=filename ptr  → fd (>=3) on success, -1 on error
 *   6   SYS_CLOSE     ebx=fd  → 0 on success, -1 on error
 *   7   SYS_WAIT      ebx=pid  → exit_status on success, -1 on error
 *   8   SYS_CREATE    ebx=filename ptr  → writable fd (>=3), -1 on error
 *   9   SYS_UNLINK    ebx=filename ptr  → 0 on success, -1 on error
 *  10   SYS_FORK      (no args)  → child pid in parent, 0 in child, -1 on error
 *  11   SYS_EXEC      ebx=filename ptr, ecx=argv (char**), edx=argc,
 *                     esi=envp (char**), edi=envc
 *                     → does not return on success, -1 on error
 *                     (argv/argc and envp/envc must each agree:
 *                      both 0 or both non-zero)
 *  12   SYS_CLEAR     (no args)
 *  13   SYS_SCROLL_UP   ebx=rows  → scroll view back into history
 *  14   SYS_SCROLL_DOWN ebx=rows  → scroll view forward toward live
 *  15   SYS_MKDIR     ebx=name ptr  → 0 on success, -1 on error
 *  16   SYS_RMDIR     ebx=name ptr  → 0 on success, -1 on error
 *  17   SYS_CHDIR     ebx=path ptr  → 0 on success, -1 on error
 *  19   SYS_LSEEK     ebx=fd, ecx=offset, edx=whence → new_offset, -1 on error
 *  20   SYS_GETPID    (no args) → pid
 *  37   SYS_KILL      ebx=pid, ecx=signum → 0 on success, -1 on error
 *  38   SYS_RENAME    ebx=oldpath, ecx=newpath  → 0 on success, -1 on error
 *  45   SYS_BRK       ebx=new_brk (0=query) → actual new brk
 *  90   SYS_MMAP      ebx=old_mmap_args* → mapped address, -1 on error
 *  91   SYS_MUNMAP    ebx=addr, ecx=len → 0 on success, -1 on error
 *  64   SYS_GETPPID   (no args) → parent pid
 *  67   SYS_SIGACTION ebx=signum, ecx=handler_va, edx=old_handler_out*
 *                     → 0 on success, -1 on error; SIGKILL always returns -1
 * 106   SYS_STAT      ebx=path ptr, ecx=dufs_stat_t ptr  → 0 on success, -1 on error
 * 119   SYS_SIGRETURN (called only from the trampoline embedded in a signal frame)
 * 125   SYS_MPROTECT  ebx=addr, ecx=len, edx=prot → 0 on success, -1 on error
 * 126   SYS_SIGPROCMASK ebx=how (0=BLOCK,1=UNBLOCK,2=SETMASK), ecx=newmask*, edx=oldmask*
 *                     → 0 on success, -1 on error
 * 141   SYS_GETDENTS  ebx=path (NULL=root), ecx=buf, edx=bufsz  → bytes written in eax
 * 158   SYS_YIELD     (no args)          → voluntarily yield the timeslice
 * 162   SYS_SLEEP     ebx=seconds        → remaining whole seconds
 * 170   SYS_MODLOAD   ebx=filename ptr   → 0 on success, negative on error
 * 183   SYS_GETCWD    ebx=buf ptr, ecx=size → chars written (excl. NUL), -1 on error
 * 184   SYS_TCGETATTR ebx=fd, ecx=termios_t* → 0 on success, -1 on error
 * 185   SYS_TCSETATTR ebx=fd, ecx=action (0=TCSANOW,2=TCSAFLUSH), edx=termios_t*
 *                     → 0 on success, -1 on error
 * 186   SYS_SETPGID   ebx=pid (0=self), ecx=pgid (0=pid) → 0 on success, -1 on error
 * 187   SYS_GETPGID   ebx=pid (0=self) → pgid on success, -1 on error
 * 188   SYS_WAITPID   ebx=pid, ecx=options (WNOHANG|WUNTRACED) → encoded status, 0, -1
 * 189   SYS_TCSETPGRP ebx=fd, ecx=pgid → 0 on success, -1 on error
 * 190   SYS_TCGETPGRP ebx=fd → fg_pgid on success, -1 on error
 * 265   SYS_CLOCK_GETTIME ebx=clockid, ecx=sys_timespec_t* → 0 on success, -1 on error
 */

#include "syscall.h"

char **environ = 0;

typedef struct {
    unsigned int addr;
    unsigned int length;
    unsigned int prot;
    unsigned int flags;
    unsigned int fd;
    unsigned int offset;
} old_mmap_args_t;

/* Local strlen — avoids pulling in libc for a single helper. */
static int ustrlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

void sys_exit(int code)
{
    __asm__ volatile (
        "int $0x80"
        :: "a"(1), "b"(code)
        : "memory"
    );
    for (;;);   /* tell the compiler this path never returns */
}

void sys_write(const char *msg)
{
    /*
     * Convenience wrapper for null-terminated strings: compute the length in
     * user space and hand it to the kernel as (buf, count).  The kernel's
     * SYS_WRITE path no longer walks the buffer looking for a NUL — it prints
     * exactly `count` bytes — so sys_write_n below is safe for binary data.
     */
    int len = ustrlen(msg);
    int dummy;
    __asm__ volatile (
        "int $0x80"
        : "=a"(dummy)
        : "a"(4), "b"(msg), "c"(len)
        : "memory"
    );
}

void sys_write_n(const char *buf, int count)
{
    /*
     * Raw byte-count write.  Prints exactly `count` bytes starting at `buf`
     * — embedded NUL bytes are emitted like any other byte, which is what
     * lets the shell's `cat` built-in dump binary files correctly.
     */
    int dummy;
    __asm__ volatile (
        "int $0x80"
        : "=a"(dummy)
        : "a"(4), "b"(buf), "c"(count)
        : "memory"
    );
}

int sys_read(int fd, char *buf, int count)
{
    int r;
    /*
     * The kernel writes the character into *buf (via ECX) and returns the
     * byte count in EAX.  Using "=a"(r) captures the return value without
     * clobbering the data already written through the pointer.
     */
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(3), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return r;
}

int sys_open(const char *name)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(5), "b"(name)
        : "memory"
    );
    return r;
}

int sys_create(const char *name)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(8), "b"(name)
        : "memory"
    );
    return r;
}

int sys_fwrite(int fd, const char *buf, int count)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(2), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return r;
}

int sys_close(int fd)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(6), "b"(fd)
        : "memory"
    );
    return r;
}

int sys_exec(const char *filename, char **argv, int argc)
{
    int envc = 0;
    if (environ)
        while (environ[envc]) envc++;
    return sys_execve(filename, argv, argc, environ, envc);
}

int sys_execve(const char *filename, char **argv, int argc, char **envp, int envc)
{
    int r;
    if (argc == 0)
        argv = 0;
    if (envc == 0)
        envp = 0;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(11), "b"(filename), "c"(argv), "d"(argc), "S"(envp), "D"(envc)
        : "memory"
    );
    return r;
}

int sys_wait(int pid)
{
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(7), "b"(pid)
        : "memory"
    );
    return ret;
}

void sys_clear(void)
{
    int dummy;
    __asm__ volatile (
        "int $0x80"
        : "=a"(dummy)
        : "a"(12)
        : "memory"
    );
}

int sys_getdents(const char *path, char *buf, int size)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(141), "b"(path), "c"(buf), "d"(size)
        : "memory"
    );
    return r;
}

unsigned int sys_sleep(unsigned int seconds)
{
    unsigned int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(162), "b"(seconds)
        : "memory"
    );
    return r;
}
/* The POSIX sleep() wrapper lives in user/lib/unistd.c. */

int sys_mkdir(const char *name)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(15), "b"(name)
        : "memory"
    );
    return r;
}

int sys_modload(const char *path)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(170), "b"(path)
        : "memory"
    );
    return r;
}

int sys_unlink(const char *name)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(9), "b"(name)
        : "memory"
    );
    return r;
}

int sys_rmdir(const char *name)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(16), "b"(name)
        : "memory"
    );
    return r;
}

int sys_rename(const char *oldpath, const char *newpath)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(38), "b"(oldpath), "c"(newpath)
        : "memory"
    );
    return r;
}

unsigned int sys_brk(unsigned int new_brk)
{
    unsigned int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(45), "b"(new_brk)
        : "memory"
    );
    return r;
}

void *sys_mmap(void *addr, unsigned int length, int prot, int flags,
               int fd, unsigned int offset)
{
    unsigned int r;
    old_mmap_args_t args;

    args.addr = (unsigned int)addr;
    args.length = length;
    args.prot = (unsigned int)prot;
    args.flags = (unsigned int)flags;
    args.fd = (unsigned int)fd;
    args.offset = offset;

    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(90), "b"(&args)
        : "memory"
    );
    return (void *)r;
}

int sys_munmap(void *addr, unsigned int length)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(91), "b"(addr), "c"(length)
        : "memory"
    );
    return r;
}

int sys_mprotect(void *addr, unsigned int length, int prot)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(125), "b"(addr), "c"(length), "d"(prot)
        : "memory"
    );
    return r;
}

int sys_fork(void)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(10)
        : "memory"
    );
    return r;
}

void sys_scroll_up(int rows)
{
    int dummy;
    __asm__ volatile (
        "int $0x80"
        : "=a"(dummy)
        : "a"(13), "b"(rows)
        : "memory"
    );
}

void sys_scroll_down(int rows)
{
    int dummy;
    __asm__ volatile (
        "int $0x80"
        : "=a"(dummy)
        : "a"(14), "b"(rows)
        : "memory"
    );
}

int sys_chdir(const char *path)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(17), "b"(path)
        : "memory"
    );
    return r;
}

int sys_getcwd(char *buf, int size)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(183), "b"(buf), "c"(size)
        : "memory"
    );
    return r;
}

int sys_stat(const char *path, dufs_stat_t *st)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(106), "b"(path), "c"(st)
        : "memory"
    );
    return r;
}

int sys_pipe(int fds[2])
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(171), "b"(fds)
        : "memory"
    );
    return r;
}

int sys_dup2(int old_fd, int new_fd)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(172), "b"(old_fd), "c"(new_fd)
        : "memory"
    );
    return r;
}

int sys_kill(int pid, int signum)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(37), "b"(pid), "c"(signum)
        : "memory"
    );
    return r;
}

int sys_sigaction(int signum, void (*handler)(int), void (**old)(int))
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(67), "b"(signum), "c"(handler), "d"(old)
        : "memory"
    );
    return r;
}

int sys_sigprocmask(int how, unsigned int *set, unsigned int *oldset)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(126), "b"(how), "c"(set), "d"(oldset)
        : "memory"
    );
    return r;
}

int sys_tcgetattr(int fd, termios_t *t)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(184), "b"(fd), "c"(t)
        : "memory"
    );
    return r;
}

int sys_tcsetattr(int fd, int action, const termios_t *t)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(185), "b"(fd), "c"(action), "d"(t)
        : "memory"
    );
    return r;
}

int sys_setpgid(int pid, int pgid)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(186), "b"(pid), "c"(pgid)
        : "memory"
    );
    return r;
}

int sys_getpgid(int pid)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(187), "b"(pid)
        : "memory"
    );
    return r;
}

int sys_lseek(int fd, int offset, int whence)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(19), "b"(fd), "c"(offset), "d"(whence)
        : "memory"
    );
    return r;
}

int sys_getpid(void)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(20)
        : "memory"
    );
    return r;
}

int sys_getppid(void)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(64)
        : "memory"
    );
    return r;
}

int sys_waitpid(int pid, int options)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(188), "b"(pid), "c"(options)
        : "memory"
    );
    return r;
}

int sys_tcsetpgrp(int fd, int pgid)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(189), "b"(fd), "c"(pgid)
        : "memory"
    );
    return r;
}

int sys_tcgetpgrp(int fd)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(190), "b"(fd)
        : "memory"
    );
    return r;
}

int sys_clock_gettime(int clock_id, sys_timespec_t *ts)
{
    int r;
    __asm__ volatile (
        "int $0x80"
        : "=a"(r)
        : "a"(265), "b"(clock_id), "c"(ts)
        : "memory"
    );
    return r;
}
