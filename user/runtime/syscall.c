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
 * Syscall numbers (kernel/proc/syscall.h) use Linux i386 public values.
 * Drunix-only convenience calls use the SYS_DRUNIX_* private range.
 *  19   SYS_LSEEK     ebx=fd, ecx=offset, edx=whence → new_offset, -1 on error
 *  41   SYS_DUP       ebx=oldfd → newfd, -1 on error
 */

#include "syscall.h"
#include "ustrlen.h"

char **environ = 0;

typedef struct {
	unsigned int addr;
	unsigned int length;
	unsigned int prot;
	unsigned int flags;
	unsigned int fd;
	unsigned int offset;
} old_mmap_args_t;

void sys_exit(int code)
{
	__asm__ volatile("int $0x80" ::"a"(1), "b"(code) : "memory");
	for (;;)
		; /* tell the compiler this path never returns */
}

void sys_write(const char *msg)
{
	sys_fwrite(1, msg, ustrlen(msg));
}

void sys_write_n(const char *buf, int count)
{
	sys_fwrite(1, buf, count);
}

int sys_read(int fd, char *buf, int count)
{
	int r;
	/*
     * The kernel writes the character into *buf (via ECX) and returns the
     * byte count in EAX.  Using "=a"(r) captures the return value without
     * clobbering the data already written through the pointer.
     */
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(3), "b"(fd), "c"(buf), "d"(count)
	                 : "memory");
	return r;
}

int sys_open(const char *name)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(5), "b"(name), "c"(0), "d"(0)
	                 : "memory");
	return r;
}

int sys_open_flags(const char *name, int flags, int mode)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(5), "b"(name), "c"(flags), "d"(mode)
	                 : "memory");
	return r;
}

int sys_create(const char *name)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(8), "b"(name) : "memory");
	return r;
}

int sys_fwrite(int fd, const char *buf, int count)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(4), "b"(fd), "c"(buf), "d"(count)
	                 : "memory");
	return r;
}

int sys_close(int fd)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(6), "b"(fd) : "memory");
	return r;
}

int sys_ioctl(int fd, unsigned int request, void *arg)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(54), "b"(fd), "c"(request), "d"(arg)
	                 : "memory");
	return r;
}

int sys_exec(const char *filename, char **argv, int argc)
{
	(void)argc;
	return sys_execve(filename, argv, environ);
}

int sys_execve(const char *filename, char **argv, char **envp)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(11), "b"(filename), "c"(argv), "d"(envp)
	                 : "memory");
	return r;
}

int sys_wait(int pid)
{
	return sys_waitpid(pid, 0);
}

void sys_clear(void)
{
	int dummy;
	__asm__ volatile("int $0x80" : "=a"(dummy) : "a"(4000) : "memory");
}

int sys_display_claim(void)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(4009) : "memory");
	return r;
}

int sys_poll(sys_pollfd_t *fds, unsigned int nfds, int timeout)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(168), "b"(fds), "c"(nfds), "d"(timeout)
	                 : "memory");
	return r;
}

int sys_getdents(const char *path, char *buf, int size)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(4008), "b"(path), "c"(buf), "d"(size)
	                 : "memory");
	return r;
}

unsigned int sys_sleep(unsigned int seconds)
{
	sys_timespec_t req;
	sys_timespec_t rem;
	int r;
	req.tv_sec = (long)seconds;
	req.tv_nsec = 0;
	rem.tv_sec = 0;
	rem.tv_nsec = 0;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(162), "b"(&req), "c"(&rem)
	                 : "memory");
	return r == 0 ? 0u : (unsigned int)rem.tv_sec;
}
/* The POSIX sleep() wrapper lives in user/runtime/unistd.c. */

int sys_mkdir(const char *name)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(39), "b"(name) : "memory");
	return r;
}

int sys_modload(const char *path)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(4003), "b"(path) : "memory");
	return r;
}

int sys_unlink(const char *name)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(10), "b"(name) : "memory");
	return r;
}

int sys_rmdir(const char *name)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(40), "b"(name) : "memory");
	return r;
}

int sys_rename(const char *oldpath, const char *newpath)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(38), "b"(oldpath), "c"(newpath)
	                 : "memory");
	return r;
}

unsigned int sys_brk(unsigned int new_brk)
{
	unsigned int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(45), "b"(new_brk) : "memory");
	return r;
}

void *sys_mmap(void *addr,
               unsigned int length,
               int prot,
               int flags,
               int fd,
               unsigned int offset)
{
	unsigned int r;
	old_mmap_args_t args;

	args.addr = (unsigned int)addr;
	args.length = length;
	args.prot = (unsigned int)prot;
	args.flags = (unsigned int)flags;
	args.fd = (unsigned int)fd;
	args.offset = offset;

	__asm__ volatile("int $0x80" : "=a"(r) : "a"(90), "b"(&args) : "memory");
	return (void *)r;
}

int sys_munmap(void *addr, unsigned int length)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(91), "b"(addr), "c"(length)
	                 : "memory");
	return r;
}

int sys_mprotect(void *addr, unsigned int length, int prot)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(125), "b"(addr), "c"(length), "d"(prot)
	                 : "memory");
	return r;
}

int sys_fork(void)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(2) : "memory");
	return r;
}

void sys_scroll_up(int rows)
{
	int dummy;
	__asm__ volatile("int $0x80"
	                 : "=a"(dummy)
	                 : "a"(4001), "b"(rows)
	                 : "memory");
}

void sys_scroll_down(int rows)
{
	int dummy;
	__asm__ volatile("int $0x80"
	                 : "=a"(dummy)
	                 : "a"(4002), "b"(rows)
	                 : "memory");
}

int sys_chdir(const char *path)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(12), "b"(path) : "memory");
	return r;
}

int sys_getcwd(char *buf, int size)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(183), "b"(buf), "c"(size)
	                 : "memory");
	return r;
}

int sys_stat(const char *path, dufs_stat_t *st)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(106), "b"(path), "c"(st)
	                 : "memory");
	return r;
}

int sys_pipe(int fds[2])
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(42), "b"(fds) : "memory");
	return r;
}

int sys_dup(int old_fd)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(41), "b"(old_fd) : "memory");
	return r;
}

int sys_dup2(int old_fd, int new_fd)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(63), "b"(old_fd), "c"(new_fd)
	                 : "memory");
	return r;
}

int sys_kill(int pid, int signum)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(37), "b"(pid), "c"(signum)
	                 : "memory");
	return r;
}

int sys_sigaction(int signum, void (*handler)(int), void (**old)(int))
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(67), "b"(signum), "c"(handler), "d"(old)
	                 : "memory");
	return r;
}

int sys_sigprocmask(int how, unsigned int *set, unsigned int *oldset)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(126), "b"(how), "c"(set), "d"(oldset)
	                 : "memory");
	return r;
}

int sys_tcgetattr(int fd, termios_t *t)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(4004), "b"(fd), "c"(t)
	                 : "memory");
	return r;
}

int sys_tcsetattr(int fd, int action, const termios_t *t)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(4005), "b"(fd), "c"(action), "d"(t)
	                 : "memory");
	return r;
}

int sys_setpgid(int pid, int pgid)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(57), "b"(pid), "c"(pgid)
	                 : "memory");
	return r;
}

int sys_getpgid(int pid)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(132), "b"(pid) : "memory");
	return r;
}

int sys_lseek(int fd, int offset, int whence)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(19), "b"(fd), "c"(offset), "d"(whence)
	                 : "memory");
	return r;
}

int sys_getpid(void)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(20) : "memory");
	return r;
}

__attribute__((naked)) int sys_clone(unsigned int flags,
                                     void *child_stack,
                                     int *parent_tid,
                                     void *tls,
                                     int *child_tid)
{
	__asm__ volatile("push %ebp\n"
	                 "mov %esp, %ebp\n"
	                 "push %edi\n"
	                 "push %esi\n"
	                 "push %ebx\n"

	                 /*
         * The child returns from INT 0x80 with ESP set to child_stack. Seed
         * that stack with the same callee-save/return frame this wrapper will
         * pop, plus room for the caller's cdecl argument cleanup after ret.
         * Otherwise the child would either return through uninitialized
         * memory or advance ESP above the supplied stack top.
         */
	                 "mov 12(%ebp), %ecx\n"
	                 "test %ecx, %ecx\n"
	                 "jz 1f\n"
	                 "sub $52, %ecx\n"
	                 "mov 0(%esp), %eax\n"
	                 "mov %eax, 0(%ecx)\n"
	                 "mov 4(%esp), %eax\n"
	                 "mov %eax, 4(%ecx)\n"
	                 "mov 8(%esp), %eax\n"
	                 "mov %eax, 8(%ecx)\n"
	                 "mov 0(%ebp), %eax\n"
	                 "mov %eax, 12(%ecx)\n"
	                 "mov 4(%ebp), %eax\n"
	                 "mov %eax, 16(%ecx)\n"
	                 "1:\n"

	                 "mov $120, %eax\n"
	                 "mov 8(%ebp), %ebx\n"
	                 "mov 16(%ebp), %edx\n"
	                 "mov 20(%ebp), %esi\n"
	                 "mov 24(%ebp), %edi\n"
	                 "int $0x80\n"
	                 "pop %ebx\n"
	                 "pop %esi\n"
	                 "pop %edi\n"
	                 "pop %ebp\n"
	                 "ret\n");
}

int sys_gettid(void)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(224) : "memory");
	return r;
}

int sys_set_tid_address(int *tidptr)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(258), "b"(tidptr) : "memory");
	return r;
}

int sys_yield(void)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(158) : "memory");
	return r;
}

void sys_exit_group(int code)
{
	__asm__ volatile("int $0x80" ::"a"(252), "b"(code) : "memory");
	for (;;)
		;
}

int sys_getppid(void)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(64) : "memory");
	return r;
}

int sys_waitpid(int pid, int options)
{
	int r;
	int status = 0;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(7), "b"(pid), "c"(&status), "d"(options)
	                 : "memory");
	return r < 0 ? r : status;
}

int sys_waitpid_status(int pid, int *status, int options)
{
	int r;
	int local_status = 0;

	if (!status)
		status = &local_status;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(7), "b"(pid), "c"(status), "d"(options)
	                 : "memory");
	return r;
}

int sys_tcsetpgrp(int fd, int pgid)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(4006), "b"(fd), "c"(pgid)
	                 : "memory");
	return r;
}

int sys_tcgetpgrp(int fd)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(4007), "b"(fd) : "memory");
	return r;
}

int sys_clock_gettime(int clock_id, sys_timespec_t *ts)
{
	int r;
	__asm__ volatile("int $0x80"
	                 : "=a"(r)
	                 : "a"(265), "b"(clock_id), "c"(ts)
	                 : "memory");
	return r;
}
