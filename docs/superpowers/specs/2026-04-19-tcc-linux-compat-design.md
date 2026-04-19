# TinyCC Linux Compatibility Design

## Goal

Bring up the first compiler inside Drunix by running TinyCC as a static Linux
i386 user program under Drunix's Linux syscall ABI. The first success case is:

```sh
/bin/tcc /tmp/hello.c -o /tmp/hello
/tmp/hello
```

This is intentionally a Linux-compatibility milestone, not a Drunix-specific
compiler port. Every kernel or userland fix required by TinyCC should improve
Drunix's ability to run ordinary Linux i386 programs.

## Non-Goals

This slice does not attempt to port GCC, G++, dynamic linking, package
management, or a full native Drunix target toolchain. It also does not require
self-hosting the kernel build inside Drunix. Those are later milestones once the
Linux compatibility layer can host real compiler workloads.

## Approach

TinyCC is the first compiler because it is small, can be built as a static i386
Linux executable, and has an integrated assembler and linker. That keeps the
initial dependency chain short: Drunix must satisfy TinyCC's Linux syscall and
filesystem expectations before it must host GNU binutils or GCC's multi-process
driver pipeline.

The build system will gain a TCC compatibility lane alongside the existing
Linux ABI probes. The host prepares a static Linux i386 `tcc` binary and adds it
to the generated disk image. A small test fixture writes or ships a `hello.c`
source file, invokes `/bin/tcc`, runs the produced binary, and records a log
that the host-side test target can extract and validate.

## Filesystem Layout

The generated Drunix disk image should include a minimal compiler environment:

```txt
/bin/tcc
/tmp/
/usr/include/
/usr/lib/
```

The first milestone can keep headers and libraries minimal. If TinyCC can
compile a no-libc or tiny-libc `hello.c` using its own include expectations,
only those files should be included. If it needs startup objects or archives,
they should be placed under `/usr/lib` rather than added through Drunix-only
paths.

Writable temporary files should use Linux-style paths such as `/tmp`. If TCC
expects temporary-file behavior that Drunix lacks, the fix belongs in path,
VFS, or syscall compatibility rather than in a special compiler mode.

## Compatibility Surface

The TCC test should drive missing behavior from observed failures. Likely areas
include:

- file operations: `open`, `openat`, `read`, `write`, `close`, `lseek`,
  `unlink`, `rename`, `access`
- metadata: `stat64`, `lstat64`, `fstat64`, `fstatat64`, `statx`
- memory: `brk`, `mmap2`, `munmap`, `mprotect`
- process behavior: `execve`, `fork` or `vfork`, `waitpid`, `wait4`
- descriptors: `pipe`, `dup`, `dup2`, `fcntl64`
- time and environment: `gettimeofday`, `clock_gettime`, `uname`, `getcwd`,
  `chdir`

The implementation should prefer Linux-compatible return values, errno
encoding, struct layouts, and path semantics. Drunix-only shortcuts are allowed
only as temporary diagnostics and must not be part of the final test path.

## Test Plan

Add a headless test target similar to the existing Linux ABI and BusyBox lanes:

```sh
make test-tcc
```

The target should boot Drunix with the compiler test as the initial program or
through a simple scripted runner, then extract a `tcc.log` from the writable
disk image. The log should include clear milestone lines such as:

```txt
TCCCOMPAT: version ok
TCCCOMPAT: compile ok
TCCCOMPAT: run ok
TCCCOMPAT PASS
```

The host-side target should fail if:

- `tcc.log` is missing,
- the pass marker is absent,
- a compile or run marker is absent,
- the debug console log contains `unknown syscall` or `Unhandled syscall`.

This makes TinyCC an executable Linux compatibility probe. Later tests can
expand from one-file `hello.c` to compile-only, multi-file, include-path,
archive-linking, and C preprocessor cases.

## Later Milestones

After TinyCC can compile and run a simple program inside Drunix, the next
compiler milestones are:

1. Compile a multi-file C program.
2. Compile against the Drunix user runtime packaged as a real sysroot.
3. Run GNU binutils tools as static Linux i386 programs.
4. Run the GCC C driver far enough to compile a small C file.
5. Add G++ only after the C compiler path and Linux process/filesystem
   compatibility are stable.

Each step should keep the same principle: use compiler workloads to close Linux
compatibility gaps first, then build a clean native Drunix target toolchain once
the hosted environment is strong enough.
