# Rexc First-Class Userland Design

## Goal

Make Rexc a first-class Drunix userland compiler for both x86 and ARM64.
Rexc-built apps should be selectable as canonical `/bin/*` programs on either
architecture, and missing Rexc functionality should be implemented in the
compiler, backend, standard library, or Drunix runtime adapter instead of being
worked around inside individual app ports.

## Current State

Drunix already has a mixed C/C++ userland under `user/apps` and builds
architecture-specific binaries under `build/user/<arch>/bin`. The x86 userland
uses ELF32 i386 at `0x08048000`; ARM64 userland uses ELF64 AArch64 at
`0x02000000`. Both architectures generate linker scripts under
`build/user/<arch>/linker/user.ld`.

Rexc already has an `i386-drunix` target, but its Drunix link path still expects
the stale pre-layout files `user/user.ld`, `user/lib/crt0.o`, and
`user/lib/libc.a`. ARM64 support exists only for Darwin/Mach-O
(`arm64-macos`), so Rexc needs a distinct AArch64 ELF/Drunix target before it
can build native Drunix ARM64 user programs.

## Architecture

Rexc will expose two Drunix target families:

- `i386-drunix` for x86 userland.
- `aarch64-drunix` for ARM64 userland, with aliases `arm64-drunix` and
  `aarch64-elf-drunix`.

The Rexc CLI keeps the same shape for both architectures:

```sh
external/rexc/build/rexc app.rx --target i386-drunix \
  --drunix-root . -o build/user/x86/bin/app

external/rexc/build/rexc app.rx --target aarch64-drunix \
  --drunix-root . -o build/user/arm64/bin/app
```

For Drunix targets, Rexc should resolve generated build artifacts from the
Drunix checkout instead of depending on source-tree build outputs. The linker
script comes from `build/user/<arch>/linker/user.ld`, and Rexc should provide
its own target-specific startup/runtime object for the language-level process
contract: argument access, environment access, standard I/O hooks, file hooks,
and process exit.

ARM64 Drunix output must be ELF64 AArch64, not Mach-O. The existing
`arm64-macos` backend behavior remains separate and unchanged.

## Build Integration

Rexc app sources will live under `user/apps/rexc/<app>.rx`. The canonical app
manifest in `user/programs.mk` remains the source of truth for disk image
contents. A new Rexc selection list, initially:

```make
REXC_PROGS ?= hello echo printenv cat yes
```

marks canonical program names whose binaries are built from `.rx` sources. If a
program name is selected for Rexc, both the x86 and ARM64 userland build lanes
must build the Rexc binary at the normal output path:

```text
build/user/x86/bin/<app>
build/user/arm64/bin/<app>
```

The build must fail when a selected Rexc app cannot be built. It must not
silently fall back to the old C/C++ implementation. The old C/C++ sources can
remain in-tree as references or rollback material until the ports are mature.

## Initial Port Sequence

The first canonical replacements should be small enough to prove each runtime
capability in isolation:

- `hello`: Drunix Rexc ELF startup, stdout, argument access, and exit.
- `echo`: argument iteration and string output.
- `printenv`: environment plumbing.
- `cat`: file open, read, close, and buffered stdout.
- `yes`: long-running output and argument handling.

Larger programs should wait until Rexc has the language and runtime surface
they naturally need. For example, implement structs before state-heavy tools
such as `wc`, and implement richer syscall/process support before attempting
`shell`, `desktop`, compatibility probes, or stress tests.

## Validation

The feature is complete when both architecture lanes can build selected Rexc
programs into canonical `/bin/*` locations and boot-test them:

- Rexc target parsing, codegen, runtime, and CLI tests pass.
- `make ARCH=x86 build` and `make ARCH=arm64 build` build Rexc-selected apps.
- x86 shell smoke tests run the selected apps normally.
- ARM64 tests run the selected apps directly with `INIT_PROGRAM=bin/<app>` until
  shell execution is equivalent, then through the same shell path.
- `readelf` confirms ELF32 i386 for x86 and ELF64 AArch64 for ARM64.
