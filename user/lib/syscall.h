/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SYSCALL_H
#define SYSCALL_H

/*
 * File/directory metadata filled by sys_stat().
 * type: 1 = regular file, 2 = directory.
 */
typedef struct {
    unsigned int type;
    unsigned int size;
    unsigned int link_count;
    unsigned int mtime;
} dufs_stat_t;

/* ── Signal constants ──────────────────────────────────────────────────── */

/* Signal dispositions for sys_sigaction(). */
#define SIG_DFL  ((void (*)(int))0)   /* default action */
#define SIG_IGN  ((void (*)(int))1)   /* ignore signal  */

/* Signal numbers (Linux-compatible). */
#define SIGINT   2    /* Ctrl-C                  — default: terminate */
#define SIGILL   4    /* illegal instruction                        */
#define SIGTRAP  5    /* breakpoint / trap                          */
#define SIGABRT  6    /* abort                                      */
#define SIGFPE   8    /* arithmetic exception                       */
#define SIGKILL  9    /* uncatchable kill         — default: terminate */
#define SIGSEGV  11   /* invalid memory reference                    */
#define SIGPIPE  13   /* write to broken pipe     — default: terminate */
#define SIGTERM  15   /* polite termination       — default: terminate */
#define SIGCHLD  17   /* child process exited     — default: ignore    */
#define SIGCONT  18   /* continue a stopped proc  — default: continue  */
#define SIGSTOP  19   /* uncatchable stop         — default: stop      */
#define SIGTSTP  20   /* terminal stop (Ctrl-Z)   — default: stop      */

/* sys_sigprocmask `how` values. */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

/* sys_waitpid option flags. */
#define WNOHANG    1   /* return immediately if no child has changed state */
#define WUNTRACED  2   /* also return when a child stops (SIGSTOP/SIGTSTP) */

/* Wait status decoding macros (Linux-compatible encoding). */
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WCOREDUMP(s)    (((s) & 0x80) != 0)
#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)     (((s) >> 8) & 0xFF)

/* ── TTY / termios ─────────────────────────────────────────────────────── */

/* termios c_lflag bits */
#define ICANON  (1u << 0)   /* canonical (line-buffered) mode */
#define ECHO    (1u << 1)   /* echo input characters          */
#define ECHOE   (1u << 2)   /* echo ERASE as BS SP BS         */
#define ISIG    (1u << 3)   /* generate SIGINT on Ctrl+C      */

/* sys_tcsetattr `action` values */
#define TCSANOW   0
#define TCSAFLUSH 2

typedef struct {
    unsigned int c_lflag;
} termios_t;

typedef struct {
    long tv_sec;
    long tv_nsec;
} sys_timespec_t;

/* ── Memory mapping ───────────────────────────────────────────────────── */

#define PROT_NONE      0x0u
#define PROT_READ      0x1u
#define PROT_WRITE     0x2u
#define PROT_EXEC      0x4u

#define MAP_PRIVATE    0x02u
#define MAP_ANONYMOUS  0x20u
#define MAP_ANON       MAP_ANONYMOUS
#define MAP_FAILED     ((void *)-1)

/*
 * syscall.h — user-space syscall interface.
 *
 * Plain C prototypes backed by INT 0x80 (Linux i386 ABI).
 * Implementations live in syscall.c — no assembly visible here.
 *
 * Calling convention: EAX = syscall number, EBX = arg1, ECX = arg2, EDX = arg3.
 */

/* Terminate the current process. Never returns. */
void sys_exit(int code);

/* Write a null-terminated string to VGA output. */
void sys_write(const char *msg);

/* Write exactly `count` bytes from buf to VGA output.  Unlike sys_write,
 * this does not stop at an embedded NUL byte — use it for binary data. */
void sys_write_n(const char *buf, int count);

/* Read up to count bytes from fd into buf. Returns bytes read, 0 at EOF. */
int sys_read(int fd, char *buf, int count);

/* Open a file by name (read-only). Returns a file descriptor (>=3) or -1 if not found. */
int sys_open(const char *name);

/*
 * Create a new file (or truncate an existing one) for writing.
 * Returns a writable file descriptor (>=3) or -1 on error.
 */
int sys_create(const char *name);

/*
 * Write count bytes from buf into the file at fd's current offset.
 * fd must have been returned by sys_create.
 * Returns bytes written, or -1 on error.
 */
int sys_fwrite(int fd, const char *buf, int count);

/* Close a file descriptor previously returned by sys_open. Returns 0 or -1. */
int sys_close(int fd);

/*
 * Current process environment, initialized by crt0 from the kernel-provided
 * process stack.  POSIX execv() inherits this vector; execve() can replace it.
 */
extern char **environ;

/* Replace the current process image with the named ELF.
 *
 * argv: NULL-terminated char*[] passed to the new main(). May be NULL
 *       when argc == 0 (no arguments).
 * argc: number of argv entries not counting the NULL terminator.
 *
 * Does not return on success. Returns -1 on error.
 */
int sys_exec(const char *filename, char **argv, int argc);
int sys_execve(const char *filename, char **argv, int argc, char **envp, int envc);

/* Wait for process pid to exit.  Returns the child's exit status, or -1. */
int sys_wait(int pid);

/* Clear the VGA screen. */
void sys_clear(void);

/*
 * Enumerate entries in path (NULL = root) into buf.
 * Root-level directory names are returned with a trailing '/'.
 * Returns bytes written.
 */
int sys_getdents(const char *path, char *buf, int size);

/*
 * Create a directory at the root level.
 * Returns 0 on success, -1 on error (duplicate, invalid name, or table full).
 */
int sys_mkdir(const char *name);

/*
 * Load a kernel module from the named file on disk.
 * The file must be an ELF32 relocatable object (.o) exporting module_init().
 * Returns 0 on success, negative on error.
 */
int sys_modload(const char *path);

/*
 * Delete a file by name.
 * Returns 0 on success, -1 if the file is not found or on error.
 */
int sys_unlink(const char *name);

/*
 * Rename or move a file or directory.
 *
 * oldpath and newpath use the same path format as sys_open ("file" or
 * "dir/file").  The kernel updates the directory entry in place — no
 * file data is copied.  If newpath names an existing file it is
 * atomically replaced.  Moving a directory into an existing directory
 * is rejected by the kernel (DUFS does not support nesting).
 * Returns 0 on success, -1 on error.
 */
int sys_rename(const char *oldpath, const char *newpath);

/*
 * Remove an empty directory at the root level.
 * Returns 0 on success, -1 if not found, not empty, or on error.
 */
int sys_rmdir(const char *name);

/*
 * Change the calling process's current working directory to path.
 * path may be absolute (leading '/'), relative, or the special form "..".
 * Pass NULL or "" to go to the filesystem root.
 * Returns 0 on success, -1 if path does not name an existing directory.
 */
int sys_chdir(const char *path);

/*
 * Copy the calling process's current working directory into buf (NUL-
 * terminated, without a leading slash).  An empty string means root.
 * Returns the number of characters written (excluding NUL), or -1 on error.
 */
int sys_getcwd(char *buf, int size);

/*
 * Fork the current process.
 *
 * Creates a child process that is an exact copy of the caller's address space,
 * registers, and open-file table.  Both parent and child resume execution at
 * the instruction after the sys_fork() call.
 *
 * Returns the child's PID in the parent, 0 in the child, -1 on error.
 */
int sys_fork(void);

/*
 * Create a pipe.
 *
 * Fills fds[0] with the read end and fds[1] with the write end.
 * Returns 0 on success, -1 on error.
 */
int sys_pipe(int fds[2]);

/*
 * Duplicate old_fd to new_fd, closing new_fd first if it is open.
 * If old_fd == new_fd the call is a no-op.
 * Returns new_fd on success, -1 on error.
 */
int sys_dup2(int old_fd, int new_fd);

/*
 * Move the program break to new_brk, growing (or querying) the heap.
 * Pass 0 to query the current break without changing it.
 * Returns the actual break after the call.  On failure the kernel returns
 * the unchanged old break — callers must compare to detect failure.
 */
unsigned int sys_brk(unsigned int new_brk);
void *sys_mmap(void *addr, unsigned int length, int prot, int flags,
               int fd, unsigned int offset);
int sys_munmap(void *addr, unsigned int length);
int sys_mprotect(void *addr, unsigned int length, int prot);

/*
 * Block the calling process for up to `seconds` whole seconds.
 * Returns 0 on full sleep, or the remaining whole seconds if interrupted.
 *
 * The POSIX-style sleep() wrapper lives in user/lib/unistd.h and forwards
 * here.  Call sys_sleep() directly only if you deliberately want to bypass
 * the libc layer.
 */
unsigned int sys_sleep(unsigned int seconds);

/*
 * Send signal `signum` to the process with the given `pid`.
 * If `pid` is negative, deliver it to every process in process group `-pid`.
 * Returns 0 on success, -1 if signum is out of range or pid not found.
 */
int sys_kill(int pid, int signum);

/*
 * Install a signal handler for signal `signum`.
 * `handler` may be SIG_DFL, SIG_IGN, or a function pointer void (*)(int).
 * If `old` is non-NULL, the previous handler is written there.
 * SIGKILL cannot be caught — returns -1 in that case.
 * Returns 0 on success, -1 on error.
 */
int sys_sigaction(int signum, void (*handler)(int), void (**old)(int));

/*
 * Modify the signal mask of the calling process.
 * how: SIG_BLOCK, SIG_UNBLOCK, or SIG_SETMASK.
 * set:    new mask to apply (may be NULL to query only).
 * oldset: receives the previous mask (may be NULL).
 * Returns 0 on success, -1 on error.
 */
int sys_sigprocmask(int how, unsigned int *set, unsigned int *oldset);

/*
 * Scroll the terminal view back (up) or forward (down) by `rows` lines.
 * Has no effect when there is no recorded history, or when already at the
 * live position (sys_scroll_down).  Only meaningful while the shell is
 * blocked waiting for input — new output snaps the view back to live.
 */
void sys_scroll_up(int rows);
void sys_scroll_down(int rows);

/*
 * Retrieve metadata for the file or directory at path.
 * Fills *st and returns 0 on success, -1 if not found.
 * Works for both regular files and directories.
 */
int sys_stat(const char *path, dufs_stat_t *st);

/*
 * Get the terminal attributes for the TTY associated with fd.
 * Returns 0 on success, -1 if fd is not a TTY.
 */
int sys_tcgetattr(int fd, termios_t *t);

/*
 * Set the terminal attributes for the TTY associated with fd.
 * action: TCSANOW = apply immediately; TCSAFLUSH = apply after draining input.
 * Returns 0 on success, -1 on error.
 */
int sys_tcsetattr(int fd, int action, const termios_t *t);

/*
 * Set the process group ID of process `pid` (0 = calling process) to `pgid`
 * (0 = use the process's own pid). The caller may change its own pgid or
 * one of its direct children's pgid.
 * Returns 0 on success, -1 on error.
 */
int sys_setpgid(int pid, int pgid);

/*
 * Return the process group ID of process `pid` (0 = calling process).
 * Returns the pgid on success, -1 on error.
 */
int sys_getpgid(int pid);

/* ── File seeking ─────────────────────────────────────────────────────── */

#define SEEK_SET  0   /* set offset to `offset` bytes from start    */
#define SEEK_CUR  1   /* set offset to current position + `offset`  */
#define SEEK_END  2   /* set offset to file size + `offset`         */

/*
 * Reposition the file offset of an open fd.
 * Returns the new offset on success, -1 on error (e.g. fd is a pipe/TTY).
 */
int sys_lseek(int fd, int offset, int whence);

/* ── Process identity ─────────────────────────────────────────────────── */

/* Return the PID of the calling process. */
int sys_getpid(void);

/* Return the PID of the calling process's parent. */
int sys_getppid(void);

/*
 * Wait for process `pid` to change state.
 * options: WNOHANG, WUNTRACED (or both ORed together).
 * Returns:
 *   > 0: Linux-encoded wait status (use WIFEXITED/WIFSTOPPED macros)
 *     0: (WNOHANG) child exists but has not changed state
 *    -1: no such process
 */
int sys_waitpid(int pid, int options);

/*
 * Set the foreground process group of the TTY associated with fd.
 * Returns 0 on success, -1 on error.
 */
int sys_tcsetpgrp(int fd, int pgid);

/*
 * Get the foreground process group of the TTY associated with fd.
 * Returns the pgid on success, -1 on error.
 */
int sys_tcgetpgrp(int fd);

/* Read kernel clock time. clock_id 0 is CLOCK_REALTIME. */
int sys_clock_gettime(int clock_id, sys_timespec_t *ts);

#endif
