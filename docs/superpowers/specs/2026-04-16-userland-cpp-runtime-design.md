# Userland C++ Runtime Design

## Purpose

Drunix should start its C++ conversion in userland, not in the kernel. The
first milestone adds side-by-side C and C++ user programs while leaving the
kernel and existing C user programs unchanged.

The goal is not to import a hosted C++ environment. Drunix should own the
runtime code that makes C++ work in ring 3, keep the syscall ABI stable, and
use cross-toolchain support only where it is explicit and necessary.

## Current Context

Drunix is a 32-bit x86 freestanding OS. User programs are built with
`x86_64-elf-gcc`, linked with `x86_64-elf-ld`, loaded at virtual address
`0x400000`, and entered through `user/lib/crt0.asm`.

The kernel creates this process entry stack:

```text
[esp + 0] = argc
[esp + 4] = argv
[esp + 8] = envp
```

The current `crt0` stores `envp` in the user libc global `environ`, calls
`main(argc, argv, envp)`, and passes the return value to `sys_exit`.

The user allocator already provides `malloc`, `free`, `realloc`, and `sbrk`
backed by `SYS_BRK`. That allocator is sufficient for the first C++ allocation
operators.

The local cross toolchain provides `x86_64-elf-g++` and `libgcc.a`, but does
not provide `libsupc++.a` or `libstdc++.a` in the compiler search path.

## Decisions

The first C++ milestone targets `user/` only.

Existing C user programs remain valid and continue to build as C programs.
New C++ user programs can be added as `.cpp` files and built into `/bin`
beside the C programs.

The C++ runtime is repo-owned and lives under `user/lib`. Drunix does not depend
on host or cross `libstdc++` or `libsupc++` for this milestone.

The build may link selected compiler support libraries such as `libgcc.a`, but
only through explicit Makefile rules. No implicit host C++ runtime dependency is
allowed.

The first proof program is a small C++ user binary, tentatively
`user/cpphello.cpp`, packaged as `/bin/cpphello`.

## Architecture

The user build becomes mixed-language:

- `.c` files compile with `x86_64-elf-gcc`.
- `.cpp` files compile with `x86_64-elf-g++`.
- C and C++ user programs share the existing ELF layout in `user/user.ld`.
- C and C++ user programs share the existing syscall ABI and user libc.
- Existing C programs continue to link through the current C runtime objects.

C++ support is layered onto the existing user runtime rather than replacing it.
The kernel process loader, syscall layer, DUFS disk packaging, and entry stack
contract remain unchanged.

## Components

### `user/Makefile`

Add C++ compiler variables, flags, pattern rules, program lists, and link rules
without rewriting the existing C targets. The Makefile should make C++ runtime
objects explicit so accidental hosted runtime linkage is visible during review.

Expected additions:

- `CXX := x86_64-elf-g++`
- `CXXFLAGS` matching the freestanding user C flags where applicable
- a `.cpp.o` compile rule
- C++ runtime object list
- `cpphello` target
- inclusion of `cpphello` in the top-level disk image program list

### `user/lib/crt0.asm` And Startup Hooks

Preserve `_start` and the existing process stack contract. Add calls around
`main` so C++ constructors and destructors have defined execution points.

Startup order:

1. Read `argc`, `argv`, and `envp` from the stack.
2. Store `envp` in `environ`.
3. Run C++ constructors from `.init_array`.
4. Call `main(argc, argv, envp)`.
5. Run C++ destructors from `.fini_array`.
6. Call `sys_exit(main_return)`.

The startup hook implementation may live in C or C++ under `user/lib`, but the
assembly entry point remains the single ELF entry point.

### `user/user.ld`

Extend the linker script with C++ section support while preserving the current
load address and page-aligned BSS end.

Required sections:

- `.init_array`
- `.fini_array`
- compiler-emitted read-only metadata sections needed by supported C++ features

The linker script should expose begin and end symbols for constructor and
destructor arrays so startup code can iterate them without hard-coded addresses.

### `user/lib/cxxrt.*`

Provide the small runtime surface that C++ programs need in Drunix userland.

Initial runtime symbols:

- global `operator new`
- global `operator new[]`
- global `operator delete`
- global `operator delete[]`
- sized delete variants if the compiler emits them
- `__cxa_pure_virtual`
- constructor and destructor array runners

Allocation policy:

- `new` and `new[]` call `malloc`.
- `delete` and `delete[]` call `free`.
- allocation failure terminates the process with a diagnostic until exception
  throwing from `new` is deliberately supported.

### `user/lib/cxxabi.*`

Keep ABI glue separate from ergonomic runtime helpers when the implementation
grows beyond allocation and constructor support. This boundary prevents the C++
ABI layer from becoming mixed with application-facing utilities.

Candidate ABI symbols for phased support include:

- `__cxa_atexit`
- `__cxa_finalize`
- guard variable helpers for function-local statics
- RTTI-related symbols if required by compiler output
- exception allocation and unwinding hooks when exceptions are implemented

### User C Headers

Headers intended for both C and C++ must use `extern "C"` guards. This applies
first to syscall and libc headers included by C++ programs.

The C ABI remains the source of truth. C++ wrappers can be added later, but they
are not part of this milestone unless needed by the smoke program.

### `user/cpphello.cpp`

Add one C++ smoke program. It should prove side-by-side userland support by
building and packaging with the C programs.

The smoke program should exercise:

- calling existing C libc or syscall functions
- a simple class
- a global constructor
- `new` and `delete`
- virtual dispatch if it links without additional ABI work

RTTI and exceptions should not be hidden inside `cpphello`. They should have
separate proof programs or tests if included in an implementation plan.

## Runtime Flow

C++ user programs use the same kernel-to-user transition as C programs.

```text
kernel process_create
  -> user _start
  -> store environ
  -> run .init_array constructors
  -> main(argc, argv, envp)
  -> run .fini_array destructors
  -> sys_exit(status)
```

No kernel loader changes are required for this milestone.

## Feature Scope

The first implementation plan should target these features:

- compiling `.cpp` user programs
- linking C++ user programs without `libstdc++` or `libsupc++`
- global constructors through `.init_array`
- global destructors through `.fini_array` or a compatible finalization path
- `new` and `delete` backed by the user allocator
- pure virtual call handling
- C headers usable from C++
- one C++ smoke binary packaged into the disk image

RTTI and exceptions are accepted as later submilestones unless the
implementation plan explicitly sizes and tests them. They require more ABI and
unwind work than the basic C++ object model.

## Error Handling

Unsupported C++ features should fail at link time when possible. Missing ABI
symbols are preferable to silent runtime corruption.

Runtime failures should terminate clearly:

- allocation failure in throwing `new`: print a short diagnostic and call
  `sys_exit(1)` until exception support exists
- pure virtual call: print a short diagnostic and call `sys_exit(1)`
- unimplemented ABI path: print a short diagnostic and call `sys_exit(1)`

Diagnostics should use existing user output paths. If output is not practical
from a specific runtime path, terminating is still required.

## Testing

Verification for the milestone should include:

- C user programs still compile.
- C++ user runtime objects compile with freestanding C++ flags.
- `cpphello` links without `libstdc++` or `libsupc++`.
- `make disk` packages both C and C++ user binaries into DUFS.
- `make test-halt` continues to pass.

If RTTI or exceptions are included in an implementation plan, add focused C++
programs for them rather than folding them into `cpphello`.

## Out Of Scope

This milestone does not convert kernel code to C++.

This milestone does not convert all user programs to C++.

This milestone does not add a hosted C++ standard library.

This milestone does not require exceptions, RTTI, templates-heavy utilities, or
standard containers unless a later approved implementation plan adds them
explicitly.

## Acceptance Criteria

The design is implemented when:

- the feature branch builds mixed C and C++ user programs
- existing C user binaries still build and package
- `/bin/cpphello` is present in the disk image
- C++ global constructors run before `main`
- C++ destructors run after `main` returns
- `new` and `delete` use the existing user allocator
- the build has no dependency on `libstdc++` or `libsupc++`
- `make test-halt` passes
