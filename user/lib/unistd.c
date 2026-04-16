/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * unistd.c — POSIX convenience wrappers that forward to the raw INT 0x80
 * wrappers in syscall.c.
 *
 * The split matters for layering:
 *
 *   user program
 *       ↓  POSIX spelling (fork, sleep, read, write, pipe, dup2, ...)
 *   user/lib/unistd.c      ← this file
 *       ↓  one-line forwarding calls
 *   user/lib/syscall.c     ← the raw ABI, one wrapper per SYS_* number
 *       ↓  int $0x80
 *   kernel/proc/syscall.c
 *
 * Every function here is a one-liner.  Nothing in unistd.c issues an
 * `int 0x80` itself — that privilege stays in syscall.c so the ABI layer
 * never competes with the POSIX layer for ownership of the calling
 * convention.
 */

#include "unistd.h"
#include "syscall.h"

/* ── Process control ─────────────────────────────────────────────────── */

int fork(void) { return sys_fork(); }

int execv(const char *path, char *const argv[])
{
    /* Count argv up to the NULL terminator, matching POSIX shape. */
    int argc = 0;
    if (argv)
        while (argv[argc]) argc++;
    return sys_exec(path, (char **)argv, argc);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return sys_execve(path, (char **)argv, (char **)envp);
}

void _exit(int status)
{
    sys_exit(status);
    for (;;) { }   /* unreachable */
}

int getpid (void) { return sys_getpid();  }
int getppid(void) { return sys_getppid(); }

/* ── Sleep ───────────────────────────────────────────────────────────── */

unsigned int sleep(unsigned int seconds)
{
    return sys_sleep(seconds);
}

/* ── File descriptors ────────────────────────────────────────────────── */

int read(int fd, void *buf, size_t count)
{
    return sys_read(fd, (char *)buf, (int)count);
}

int write(int fd, const void *buf, size_t count)
{
    return sys_fwrite(fd, (const char *)buf, (int)count);
}

int close(int fd) { return sys_close(fd); }

int dup2(int oldfd, int newfd) { return sys_dup2(oldfd, newfd); }

int pipe(int fds[2]) { return sys_pipe(fds); }

/* ── Filesystem ──────────────────────────────────────────────────────── */

int unlink(const char *path) { return sys_unlink(path); }
int rmdir (const char *path) { return sys_rmdir(path);  }
int chdir (const char *path) { return sys_chdir(path);  }

char *getcwd(char *buf, size_t size)
{
    if (!buf || size == 0) return 0;
    int n = sys_getcwd(buf, (int)size);
    if (n < 0) return 0;
    return buf;
}

/* ── File seeking ────────────────────────────────────────────────────── */

long lseek(int fd, long offset, int whence)
{
    return sys_lseek(fd, (int)offset, whence);
}

/* ── Terminal interrogation ──────────────────────────────────────────── */

int isatty(int fd)
{
    termios_t t;
    return sys_tcgetattr(fd, &t) == 0 ? 1 : 0;
}
