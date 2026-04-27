# Rexc First-Class Userland Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Rexc a first-class Drunix userland compiler for both x86 and ARM64, with canonical `/bin/*` apps buildable from `.rx` sources on both architectures.

**Architecture:** Add distinct Drunix Rexc targets for ELF32 i386 and ELF64 AArch64. Keep Darwin ARM64 separate. Rexc acts as the compiler driver, like `gcc`, `g++`, or `rustc`: it owns source compilation, assembly, runtime/startup selection, linker invocation, temporary files, and target diagnostics. Drunix build rules select canonical app names and invoke Rexc with a source file, target, Drunix root, and output path.

**Tech Stack:** C++17 Rexc compiler, GNU Make, x86_64-elf and aarch64-elf binutils, Drunix user runtime/linker scripts, Python/QEMU smoke tests.

---

## File Structure

- Modify Rexc target and CLI/linking code in `external/rexc/include/rexc/target.hpp`, `external/rexc/src/target.cpp`, and `external/rexc/src/main.cpp`.
- Extend ARM64 code generation in `external/rexc/src/codegen_arm64.cpp` without changing `arm64-macos` semantics.
- Add or extend Drunix runtime adapters under `external/rexc/src/stdlib/sys/`.
- Keep compiler-driver behavior in Rexc, not in Drunix Make rules: Make should not assemble Rexc output, order Rexc runtime objects, or invoke `ld` directly for Rexc apps.
- Add Rexc sources under `user/apps/rexc/`.
- Update x86 userland build rules in `user/Makefile`.
- Update ARM64 userland build rules in `kernel/arch/arm64/arch.mk`.
- Extend userland manifests in `user/programs.mk`.
- Add or update smoke tests in `external/rexc/tests/` and Drunix `tools/`.
- Update Rexc and Drunix docs after behavior lands.

## Tasks

### Task 1: Add Drunix Target Modeling

- [ ] Add `AArch64Drunix` to Rexc's target enum.
- [ ] Parse `aarch64-drunix`, `arm64-drunix`, and `aarch64-elf-drunix`.
- [ ] Make `is_drunix_target()` return true for both Drunix targets.
- [ ] Make `target_triple_name()` return stable names for both Drunix targets.
- [ ] Add target parsing tests covering the new aliases and preserving existing aliases.

### Task 2: Fix Drunix Link Layout

- [ ] Treat Rexc as the driver for Drunix userland compilation, not as an assembly emitter that Make finishes manually.
- [ ] Preserve the normal compiler-driver modes:
  - `-S` writes target assembly.
  - `-c` writes a relocatable object.
  - executable output writes a final Drunix ELF executable.
- [ ] Keep the assemble/link/runtime flow inside Rexc for executable output, including temporary file creation and cleanup.
- [ ] Report missing target tools with compiler-style diagnostics that name the missing tool and selected target.
- [ ] Update Rexc's `--drunix-root` link path to use generated Drunix artifacts:
  - x86 linker script: `build/user/x86/linker/user.ld`
  - ARM64 linker script: `build/user/arm64/linker/user.ld`
- [ ] Remove assumptions about `user/user.ld`, `user/lib/crt0.o`, and `user/lib/libc.a`.
- [ ] Select assembler/linker tools by Drunix target:
  - x86: `x86_64-elf-as`, `x86_64-elf-ld`
  - ARM64: `aarch64-elf-as`, `aarch64-elf-ld`
- [ ] Add CLI smoke tests for successful Drunix executable linking on both targets.

### Task 3: Add AArch64 Drunix Codegen

- [ ] Split ARM64 backend target policy so Darwin/Mach-O and Drunix/ELF symbol rules are explicit.
- [ ] Emit GNU AArch64 ELF assembly for Drunix:
  - no leading underscore on symbols
  - ELF section names for text, rodata, data, and bss
  - AAPCS64 calls, returns, and stack frames
  - string literals, statics, pointer ops, branches, and stdlib calls
- [ ] Add codegen tests for AArch64 Drunix functions, calls, strings, statics, branches, pointer indexing, and stdlib calls.
- [ ] Re-run existing `arm64-macos` tests to prove Darwin output did not regress.

### Task 4: Implement Drunix Rexc Runtime Adapters

- [ ] Implement or fix x86 Drunix startup/runtime assembly:
  - read `argc`, `argv`, and `envp` from the i386 process-start stack
  - populate `__rexc_argc`, `__rexc_argv`, and `__rexc_envp`
  - call Rexc `main() -> i32`
  - exit through the Drunix syscall ABI
- [ ] Implement ARM64 Drunix startup/runtime assembly:
  - read `argc`, `argv`, and `envp` from the AAPCS64 process-start stack
  - populate the same Rexc runtime symbols
  - call Rexc `main() -> i32`
  - exit through the Drunix ARM64 syscall ABI
- [ ] Provide both targets with stdlib hooks for `sys_read`, `sys_write`, `sys_exit`, file open/create/close, `sys_args_len`, `sys_arg`, `sys_env_len`, and `sys_env_at`.
- [ ] Add runtime assembly tests proving both adapters emit the required symbols.

### Task 5: Integrate Rexc Into Drunix Userland Builds

- [ ] Add `user/apps/rexc/`.
- [ ] Extend `user/programs.mk` with `REXC_PROGS ?= hello echo printenv cat yes`.
- [ ] Teach `user/Makefile` to build selected x86 canonical names by invoking Rexc once per app, like a normal compiler driver:
  - `$(REXC) user/apps/rexc/<app>.rx --target i386-drunix --drunix-root .. -o ../build/user/x86/bin/<app>`
- [ ] Teach `kernel/arch/arm64/arch.mk` to build selected ARM64 canonical names by invoking Rexc once per app:
  - `$(REXC) user/apps/rexc/<app>.rx --target aarch64-drunix --drunix-root . -o build/user/arm64/bin/<app>`
- [ ] Do not add Make rules that pipe Rexc assembly into `as` or manually invoke `ld` for Rexc apps; that orchestration belongs in Rexc.
- [ ] Ensure disk image rules continue to package from `build/user/<arch>/bin/<app>` without knowing which language produced the binary.
- [ ] Ensure selected Rexc app build failures fail the whole build instead of falling back to C/C++.

### Task 6: Port Initial Canonical Apps

- [ ] Create `user/apps/rexc/hello.rx` to prove startup, stdout, args, and exit.
- [ ] Create `user/apps/rexc/echo.rx` to prove argv iteration and string output.
- [ ] Create `user/apps/rexc/printenv.rx` to prove environment access.
- [ ] Create `user/apps/rexc/cat.rx` to prove file open/read/close and buffered stdout.
- [ ] Create `user/apps/rexc/yes.rx` to prove long-running output and argv handling.
- [ ] Do not add app-local workarounds for missing Rexc features; implement the missing compiler/runtime support first.

### Task 7: Add Drunix Behavior Tests

- [ ] Extend x86 shell smoke tests to run `hello`, `echo`, `printenv PATH`, and `cat hello.txt` through the normal shell path.
- [ ] Add timeout-controlled `yes` coverage that confirms repeated output without hanging the suite.
- [ ] For ARM64, boot selected Rexc apps directly with `INIT_PROGRAM=bin/<app>` until shell execution is equivalent.
- [ ] Preserve existing ARM64 syscall parity tests and add Rexc app checks beside them.
- [ ] Add `readelf` checks for ELF32 i386 and ELF64 AArch64 Rexc app outputs.

### Task 8: Update Documentation

- [ ] Update `external/rexc/README.md` with both Drunix targets and current generated linker-script paths.
- [ ] Update Drunix README/build docs to mention Rexc userland selection and the `REXC_PROGS` build variable.
- [ ] Document that missing Rexc language/runtime features are compiler work, not app-local workaround work.

## Verification

Run these before calling the work complete:

```sh
ctest --test-dir external/rexc/build --output-on-failure
make ARCH=x86 build
make ARCH=arm64 build
make ARCH=x86 test-headless
make ARCH=arm64 check
x86_64-elf-readelf -h build/user/x86/bin/hello
aarch64-elf-readelf -h build/user/arm64/bin/hello
```

Expected results:

- Rexc tests pass.
- Both architecture builds produce selected Rexc apps at `build/user/<arch>/bin/<app>`.
- x86 and ARM64 smoke tests exercise the selected Rexc apps.
- `readelf` reports ELF32 Intel 80386 for x86 and ELF64 AArch64 for ARM64.
- Drunix Make rules invoke Rexc as a single compiler driver command for Rexc
  apps; no Make rule manually assembles or links Rexc output.

## Assumptions

- x86 and ARM64 are equal first-class Rexc userland targets.
- Canonical app replacement happens only after both architecture lanes pass.
- ARM64 Drunix uses ELF64 AArch64, not Mach-O/Darwin output.
- Missing Rexc support is implemented in Rexc/compiler/runtime layers before app ports depend on it.
