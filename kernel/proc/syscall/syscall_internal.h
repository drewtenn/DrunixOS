/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_internal.h - private syscall implementation contracts.
 *
 * This header is shared only by the syscall implementation modules. It
 * declares the dispatcher, per-domain syscall case handlers, and the small
 * set of helper routines needed across syscall_*.c files.
 */

#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

#include "process.h"
#include "tty.h"
#include "vfs.h"
#include <stdint.h>

#define SYSCALL_NOINLINE __attribute__((noinline))

typedef uint32_t (*syscall_one_path_op_t)(const char *path);
typedef uint32_t (*syscall_two_path_op_t)(const char *oldpath,
                                          const char *newpath);
typedef int (*syscall_path_resolver_t)(process_t *proc,
                                       uint32_t resolver_arg,
                                       uint32_t user_ptr,
                                       char *resolved,
                                       uint32_t resolved_sz);

typedef struct {
	syscall_path_resolver_t resolve;
	uint32_t resolver_arg;
} syscall_path_spec_t;

uint32_t syscall_case_nanosleep(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_nanosleep64(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_clock_nanosleep64(uint32_t ebx,
                                        uint32_t ecx,
                                        uint32_t edx,
                                        uint32_t esi);
uint32_t syscall_case_clock_gettime(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_clock_gettime64(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_gettimeofday(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_setsid(void);
uint32_t syscall_case_getsid(uint32_t ebx);
uint32_t syscall_case_setpgid(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_getpgid(uint32_t ebx);
uint32_t syscall_case_getpid(void);
uint32_t syscall_case_gettid(void);
uint32_t syscall_case_getppid(void);
uint32_t syscall_case_getuid32_getgid32_geteuid32_getegid32(void);
uint32_t syscall_case_setuid32_setgid32(uint32_t ebx);
uint32_t syscall_case_getrusage(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_getpriority(void);
uint32_t syscall_case_setpriority(void);
uint32_t syscall_case_sysinfo(uint32_t ebx);
uint32_t
syscall_case_prlimit64(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi);
uint32_t syscall_case_sched_getaffinity(uint32_t ecx, uint32_t edx);
uint32_t syscall_case_uname(uint32_t ebx);
uint32_t syscall_case_drunix_clear(void);
uint32_t syscall_case_drunix_scroll_up(uint32_t ebx);
uint32_t syscall_case_drunix_scroll_down(uint32_t ebx);
uint32_t syscall_case_yield(void);
uint32_t syscall_case_kill(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_sigaction(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_rt_sigaction(uint32_t ebx,
                                   uint32_t ecx,
                                   uint32_t edx,
                                   uint32_t esi);
uint32_t syscall_case_sigreturn(void);
uint32_t syscall_case_sigprocmask(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_rt_sigprocmask(uint32_t ebx,
                                     uint32_t ecx,
                                     uint32_t edx,
                                     uint32_t esi);
const char *syscall_process_cwd(const process_t *proc);
void syscall_set_process_cwd(process_t *proc, const char *cwd);
void kcwd_resolve(const char *cwd, const char *name, char *out, int outsz);
int linux_path_exists(process_t *cur, uint32_t user_path);
file_handle_t *proc_fd_entries(process_t *proc);
int fd_alloc(process_t *proc);
void fd_close_one(process_t *proc, unsigned fd);
char *syscall_alloc_path_scratch(void);
char *
copy_user_string_alloc(process_t *proc, uint32_t user_ptr, uint32_t max_len);
int resolve_user_path(process_t *proc,
                      uint32_t user_ptr,
                      char *out,
                      uint32_t outsz);
int resolve_user_path_at(process_t *proc,
                         uint32_t dirfd,
                         uint32_t user_ptr,
                         char *out,
                         uint32_t outsz);
void syscall_invlpg(uint32_t virt);
tty_t *syscall_tty_from_fd(process_t *cur, uint32_t fd, uint32_t *tty_idx_out);
int syscall_fd_is_console_output(const file_handle_t *fh);
int syscall_write_console_bytes(process_t *cur, const char *buf, uint32_t len);

uint32_t syscall_case_write(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_writev(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_writev64(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_readv(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_readv64(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_read(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t
syscall_case_sendfile64(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi);
uint32_t syscall_case_open(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_openat(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_close(uint32_t ebx);
uint32_t syscall_case_access(uint32_t ebx);
uint32_t syscall_case_faccessat(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_fchmodat(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_fchownat(uint32_t ebx, uint32_t ecx, uint32_t edi);
uint32_t syscall_case_futimesat(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_chmod(uint32_t ebx);
uint32_t syscall_case_lchown_chown32(uint32_t ebx);
uint32_t syscall_case_sync(void);
uint32_t syscall_case_umask(uint32_t ebx);
uint32_t syscall_case_readlink(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t
syscall_case_readlinkat(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi);
uint32_t syscall_case_truncate64(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_ftruncate64(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_utimensat(uint32_t ebx, uint32_t ecx, uint32_t esi);
uint32_t syscall_case_poll(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_ioctl(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_fcntl64(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_execve(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_creat(uint32_t ebx);
uint32_t syscall_case_unlink(uint32_t ebx);
uint32_t syscall_case_unlinkat(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_clone(
    uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi);
uint32_t syscall_case_fork_vfork(void);
uint32_t
syscall_case_newselect(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi);
uint32_t syscall_case_mkdir(uint32_t ebx);
uint32_t syscall_case_mkdirat(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_rmdir(uint32_t ebx);
uint32_t syscall_case_chdir(uint32_t ebx);
uint32_t syscall_case_getcwd(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_rename(uint32_t ebx, uint32_t ecx);
uint32_t
syscall_case_renameat(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi);
uint32_t syscall_case_linkat(
    uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi);
uint32_t syscall_case_symlinkat(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_getdents(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t
syscall_case_drunix_getdents_path(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_getdents64(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_stat(uint32_t ebx, uint32_t ecx);
uint32_t
syscall_case_stat64_lstat64(uint32_t nofollow, uint32_t ebx, uint32_t ecx);
uint32_t
syscall_case_fstatat64(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi);
uint32_t syscall_case_fstat64(uint32_t ebx, uint32_t ecx);
uint32_t
syscall_case_fstatat_arm64(uint32_t ebx,
                           uint32_t ecx,
                           uint32_t edx,
                           uint32_t esi);
uint32_t syscall_case_fstat_arm64(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_statx(
    uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi);
uint32_t syscall_case_statfs64(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_fstatfs64(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_statfs_arm64(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_fstatfs_arm64(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_set_thread_area(uint32_t ebx);
uint32_t syscall_case_set_tid_address(void);
uint32_t syscall_case_drunix_modload(uint32_t ebx);
uint32_t syscall_case_exit_exit_group(uint32_t exit_group, uint32_t ebx);
uint32_t syscall_case_brk(uint32_t ebx);
uint32_t syscall_case_mmap(uint32_t ebx);
uint32_t syscall_case_mmap2(uint32_t ebx,
                            uint32_t ecx,
                            uint32_t edx,
                            uint32_t esi,
                            uint32_t edi,
                            uint32_t ebp);
uint32_t syscall_case_munmap(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_mprotect(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_pipe(uint32_t ebx);
uint32_t syscall_case_pipe2(uint32_t eax,
                            uint32_t ebx,
                            uint32_t ecx,
                            uint32_t edx,
                            uint32_t esi,
                            uint32_t edi,
                            uint32_t ebp);
uint32_t syscall_case_dup(uint32_t ebx);
uint32_t syscall_case_dup2(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_drunix_tcgetattr(uint32_t ebx, uint32_t ecx);
uint32_t
syscall_case_drunix_tcsetattr(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_lseek(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t syscall_case_llseek(
    uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi);
uint32_t syscall_case_waitpid(uint32_t ebx, uint32_t ecx, uint32_t edx);
uint32_t
syscall_case_wait4(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi);
uint32_t syscall_case_drunix_tcsetpgrp(uint32_t ebx, uint32_t ecx);
uint32_t syscall_case_drunix_tcgetpgrp(uint32_t ebx);

#endif
