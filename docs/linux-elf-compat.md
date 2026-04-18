# Linux ELF Compatibility Matrix

Drunix treats Linux i386 ELF compatibility as a kernel ABI contract.  The
compatibility floor is tested in two layers:

- `make test-linux-abi` boots `bin/linuxabi`, a static Linux i386 ELF that uses
  raw `int 0x80` calls and checks 357 Linux-style return values directly.
- `make test-busybox-compat` boots `bin/bbcompat`, which runs BusyBox applets
  as integration coverage on top of the syscall layer.

The direct ABI test writes `linuxabi.log`; the BusyBox test writes
`bbcompat.log`.  Both targets also fail if the debug console reports an
unknown Linux syscall.

| Area | Linux i386 syscall surface | Direct ABI coverage | Integration coverage | Current notes |
| --- | --- | --- | --- | --- |
| ELF startup | `set_thread_area`, `set_tid_address`, `brk`, `mmap`, `mmap2`, `munmap` | TLS setup, tid-address setup, `brk`, old `mmap`, `mmap2`, `mprotect`, `munmap` | static musl programs, BusyBox startup | Static musl startup and explicit ABI probes both cover the TLS/thread-id path. |
| Identity | `getpid`, `gettid`, `getppid`, `getuid32`, `geteuid32`, `getgid32`, `getegid32`, `setuid32`, `setgid32`, `umask` | uid/gid, pid, root setuid/setgid, and stateful umask probes | BusyBox `id`, shell/app startup | Drunix currently runs Linux ELFs as uid/gid 0. |
| File I/O | `open`, `openat`, `creat`, `read`, `write`, `close`, `lseek`, `_llseek`, `readv`, `writev`, `sendfile64` | create/write/read/seek/vector-I/O/sendfile/close, plus `openat` absolute and dirfd-relative paths | BusyBox `cat`, `cp`, `dd`, checksums, archive helpers | `dup(2)` is implemented in addition to `dup2` and `fcntl(F_DUPFD)`. |
| File metadata | `stat`, `stat64`, `lstat64`, `fstat64`, `fstatat64`, `statx`, `statfs64`, `fstatfs64`, `access`, `faccessat`, `chmod`, `fchmodat`, `chown32`, `lchown`, `fchownat`, `readlink`, `readlinkat`, `utimensat`, `futimesat`, `truncate64`, `ftruncate64` | existence, stat sizes, `fstatat64` dirfd and `AT_EMPTY_PATH`, statfs block size, truncate sizes, chmod/chown no-ops, readlink/readlinkat symlink payloads and regular-file `EINVAL`, broken-link readlink truncation, `utimensat` flags, and `futimesat` success on regular files | BusyBox `stat`, `ls`, `find`, `realpath` | `lstat64` and `fstatat64(AT_SYMLINK_NOFOLLOW)` report symlink metadata; `stat64`, `open`, and intermediate path components follow symlinks with loop detection. |
| Directory ops | `mkdir`, `mkdirat`, `rmdir`, `chdir`, `getcwd`, `getdents`, `getdents64`, `rename`, `renameat`, `unlink`, `unlinkat`, `linkat`, `symlinkat` | mkdir/mkdirat/rmdir/chdir/getcwd/getdents/getdents64/rename/renameat/unlink/unlinkat, hardlink lifetime after unlink, relative symlink creation, symlinked-directory traversal, and `linkat(AT_SYMLINK_FOLLOW)` semantics | BusyBox `mkdir`, `rmdir`, `find`, `tree`, `tar` | Directory iteration returns Linux dirent records; `unlinkat(AT_REMOVEDIR)` removes empty directories. |
| FD control | `pipe`, `dup`, `dup2`, `fcntl64`, `ioctl`, `_newselect`, `poll` | pipe round trip, `dup`, `dup2`, `F_DUPFD`, `F_DUPFD_CLOEXEC`, fd flags, `TIOCGWINSZ`, `FIONREAD`, termios probes, readable/writable `poll`, and readable pipe `select` | BusyBox shells, text filters, terminal probes | `select` supports `nfds <= 32` and immediate readiness over the same fd types as `poll`. |
| Processes | `fork`, `vfork`, `clone`, `execve`, `waitpid`, `wait4`, `exit`, `exit_group`, `kill` | `fork`, rejected-invalid `clone` flag combinations, `waitpid`, `wait4`, Linux-encoded wait status, and `kill(pid, 0)` probes | BusyBox shell execution and applet subprocesses; `threadtest` covers shared-VM clone threads | `clone` supports the shared-resource threading subset. Futex joins, robust futex lists, namespaces, ptrace, and SMP behavior remain out of scope. |
| Time | `gettimeofday`, `clock_gettime`, `clock_gettime64`, `nanosleep` | realtime, monotonic, time64, timeval, and zero-interval nanosleep probes | BusyBox `date`, `sleep`, `timeout` | The test runner checks direct success semantics, not wall-clock accuracy. |
| Signals | `sigaction`, `rt_sigaction`, `sigprocmask`, `rt_sigprocmask`, `sigreturn`, `kill` | signal-mask set/query/unblock, `rt_sigprocmask` query, and `rt_sigaction` install/query/restore | BusyBox process-control setup | Handler delivery is still intentionally small; disposition storage now follows the Linux rt_sigaction contract. |
| Proc/sys info | `uname`, `sysinfo`, `sched_getaffinity`, priorities/session/process-group calls | `uname`, `sysinfo`, CPU affinity, priority, session, and process-group probes | BusyBox `uname`, `ps`, `pstree`, `nproc` | Existing integration coverage exercises these through applets. |
| Unsupported device/network areas | socket family, network interfaces, block devices, modules, I2C, USB, PCI | none | mostly BusyBox help-smoke coverage | Expected to grow once Drunix has real networking and device models. |

When adding a syscall, update:

1. `kernel/proc/syscall.h` with the Linux i386 number.
2. `kernel/proc/syscall.c` with Linux-compatible success and negative errno
   returns.
3. `tools/check_linux_i386_syscall_abi.py` if the syscall is part of the public
   compatibility floor.
4. `user/linuxabi.c` for direct return/errno semantics.
5. `user/bbcompat.c` if a BusyBox applet can exercise the behavior naturally.
