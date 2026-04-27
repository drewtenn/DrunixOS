/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>

/*
 * unistd.h — POSIX-style convenience layer over the raw INT 0x80 wrappers.
 *
 * Every function in this header is a one-liner that forwards to the
 * matching sys_* symbol in user/runtime/syscall.c.  The split exists so that
 * the bottom layer (syscall.{c,h}) stays a pure, self-contained description
 * of the kernel ABI — one wrapper per syscall number, no aliasing — and
 * application code can reach for the familiar POSIX names without knowing
 * the numeric syscall identifier.
 *
 *   syscall.h  →  "the syscall ABI: one wrapper per SYS_* number"
 *   unistd.h   →  "the POSIX spelling of the same thing"
 *
 * This mirrors how Linux+glibc layer things: the raw Linux ABI lives in
 * <sys/syscall.h> and arch/x86/entry/syscalls/syscall_32.tbl, and the
 * POSIX-shaped wrappers (sleep, getpid, fork, pipe, …) live in <unistd.h>
 * and are implemented in glibc.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Process control ─────────────────────────────────────────────────── */

int fork(void);
int execv(const char *path, char *const argv[]);
int execve(const char *path, char *const argv[], char *const envp[]);
extern char **environ;

/* Terminate the calling process without running any atexit() / stdio
 * cleanup.  Like POSIX _exit(2), this is the raw "goodbye" that goes
 * straight to SYS_EXIT; library cleanup (when we have any) belongs in
 * exit() up in stdlib.h. */
void _exit(int status);

int getpid(void);
int getppid(void);

/* ── Sleep ───────────────────────────────────────────────────────────── */

/*
 * Suspend the calling process for `seconds` whole seconds.
 * Returns 0 on full sleep, or the remaining whole seconds if interrupted.
 * The kernel does not currently implement sub-second sleep, so usleep()
 * and nanosleep() are deliberately absent — they will arrive alongside a
 * higher-resolution timer.
 */
unsigned int sleep(unsigned int seconds);

/* ── File descriptors ────────────────────────────────────────────────── */

int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
int close(int fd);

int dup2(int oldfd, int newfd);
int pipe(int fds[2]);

/* ── Filesystem ──────────────────────────────────────────────────────── */

int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);

/* POSIX getcwd returns buf on success or NULL on error; our sys_getcwd
 * returns a character count.  This wrapper adapts between the two. */
char *getcwd(char *buf, size_t size);

/* ── File seeking ────────────────────────────────────────────────────── */

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

long lseek(int fd, long offset, int whence);

/* ── Terminal interrogation ──────────────────────────────────────────── */

/*
 * Return 1 if fd refers to a TTY, 0 otherwise.
 * Implemented by calling sys_tcgetattr() on fd — which succeeds only for
 * real terminal descriptors — and treating a successful return as "yes".
 */
int isatty(int fd);

#ifdef __cplusplus
}
#endif

#endif
