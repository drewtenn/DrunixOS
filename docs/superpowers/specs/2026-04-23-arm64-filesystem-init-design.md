# ARM64 Filesystem Init Design

## Goal

Advance from the current ARM64 embedded smoke-ELF bring-up to a real
filesystem-backed ARM64 PID 1 launch, while preserving the current shared
userspace split between x86 and ARM64.

This milestone does not target broad ARM64 userspace parity. It targets the
first normal init-style boot path on ARM64: open a real ARM64 ELF from the
filesystem, load it through the shared exec path, enter EL0, and keep the
system in console mode with no desktop.

## Desired End State

At the end of this milestone:

- ARM64 boots through the shared init launch path instead of hardwiring the
  embedded smoke ELF as the default userspace payload
- ARM64 can load a real filesystem-backed AArch64 ELF as PID 1
- the existing ARM64 syscall and fault path is exercised by a real init-style
  program rather than only by a built-in regression payload
- x86 remains on the same shared "filesystem-backed init" control flow
- the embedded ARM64 smoke ELF remains available only as an explicit fallback
  or debug bring-up path
- if PID 1 exits or crashes on ARM64, the kernel reports it and drops back to a
  controlled kernel console loop instead of hanging silently

## Constraints

- Keep the current "terminal, no desktop" target.
- Do not introduce shell-specific shortcuts into the shared init path.
- Preserve Linux/AArch64 ELF and process-entry conventions where already
  established in the prior userspace convergence milestone.
- Keep the smoke ELF in-tree, but do not allow accidental fallback to it.
- Reuse the shared exec and init-launch flow instead of creating a second
  ARM64-only boot path.
- Preserve current x86 boot and headless verification behavior.

## Chosen Approach

Use the normal shared PID 1 launch path as the architecture boundary, and make
ARM64 a real consumer of it.

The shared layer owns:

- selecting the configured init path
- opening the init program from the mounted filesystem
- invoking the normal exec/load path for PID 1
- shared argv/env/user-stack setup where it is already architecture-neutral
- common logging, failure reporting, and PID 1 lifecycle semantics

The ARM64 arch layer owns:

- validating and loading AArch64 ELF64 binaries
- constructing the initial EL0 register frame for real exec
- maintaining Linux/AArch64 stack alignment and entry conventions
- preserving ARM64 return-to-kernel behavior on exit and fault

The embedded smoke ELF remains as:

- an explicit fallback when enabled by a dedicated knob
- a narrow regression and bring-up tool for ARM64 EL0/syscall testing

This keeps the kernel moving toward one shared boot-to-init architecture rather
than one real x86 path plus one special-case ARM64 path.

## Architecture

### 1. Shared Init Launch Path

The shared kernel path that creates PID 1 should become the default ARM64 boot
target.

That path should:

- resolve the configured init path from the mounted filesystem
- open the file through the normal VFS path
- create or replace PID 1 through the shared exec loader flow
- report loader and launch failures with the exact path and failure reason

ARM64 should no longer hardcode the embedded smoke ELF as its primary userspace
launch behavior once the filesystem-backed path is ready.

### 2. ARM64 File-Backed Exec

ARM64 should reuse the existing shared exec orchestration but consume a real
file-backed ELF instead of a built-in byte array.

That means the ARM64 path must support:

- loading `ELF64` + `EM_AARCH64` from the filesystem-backed exec path
- mapping loadable segments into the target address space
- constructing the initial user stack with argv/env data required by the first
  init-style program
- synthesizing the initial EL0 register frame for the loaded entry point

This milestone should not widen the loader beyond what the first ARM64 init
program actually needs.

### 3. Smoke ELF Demotion

The embedded smoke ELF should remain available, but only behind explicit
control.

The default behavior should be:

- try the configured filesystem-backed ARM64 init path
- fail loudly if the file is missing or invalid
- only use the smoke ELF when an explicit build or boot configuration enables
  fallback

This avoids masking real init-launch failures while keeping the earlier bring-up
artifact useful for controlled regression work.

### 4. Console-First System Target

The system target remains console mode, no desktop.

This milestone should not reintroduce desktop dependencies into boot, init, or
console plumbing. PID 1 only needs enough syscall and terminal integration to
prove that the real filesystem-backed userspace path works on ARM64.

## Data Flow

### ARM64 Boot To Filesystem-Backed PID 1

1. ARM64 boots into the kernel and initializes UART, timer, memory management,
   scheduler, VFS, and storage required for the root image.
2. The shared init-launch path selects the configured init program path.
3. Shared code opens that path from the mounted filesystem.
4. Shared exec orchestration routes the file-backed ELF through the ARM64
   loader path.
5. ARM64 ELF loading validates `ELF64` + `EM_AARCH64`, maps segments, prepares
   the user stack, and builds the initial EL0 register frame.
6. The scheduler runs PID 1 in EL0.
7. PID 1 uses the existing ARM64 syscall path for startup, serial output, and
   controlled exit behavior.
8. If PID 1 exits or faults, the kernel logs the outcome and returns to a
   controlled kernel console state.

### Fallback Behavior

Fallback should be explicit rather than automatic.

- If the configured ARM64 init binary is missing, the kernel logs the exact
  path and failure.
- If the file exists but is not a valid AArch64 ELF, the loader reports that
  specific error.
- The embedded smoke ELF runs only when a dedicated fallback knob is enabled.
- Without that knob, ARM64 boot failure stays visible and diagnosable.

### x86 Preservation

x86 should continue to follow the same shared init-launch shape:

- filesystem-backed init program
- shared exec flow
- x86-specific ELF and user-entry behavior at the arch edge

The kernel architecture should now consistently be "shared boot-to-init flow,
arch-specific entry/return/load details."

## Error Handling

### Init Path Resolution

- Missing init paths must log the full configured path.
- Open failures must identify whether the issue was lookup, access, or loader
  related where the current kernel interfaces make that possible.

### ELF Validation

- Invalid ELF class or machine must fail cleanly and explicitly.
- ARM64 must not silently accept x86 user binaries.
- The smoke ELF fallback must not activate automatically on generic ELF loader
  failures unless fallback is explicitly enabled.

### PID 1 Lifecycle

- If ARM64 PID 1 exits normally, the kernel should log the exit and return to a
  controlled kernel console state.
- If ARM64 PID 1 faults, the kernel should log enough register and fault
  context for serial debugging and then return to the same controlled kernel
  console state.
- Kernel-mode ARM64 faults remain out of scope for this milestone.

## Scope

### In Scope

- ARM64 filesystem-backed PID 1 launch through the normal shared init path
- ARM64 file-backed exec plumbing for at least one real init-style AArch64 ELF
- argv/env/user-stack setup sufficient for that first real ARM64 init program
- build and image plumbing needed to place the ARM64 init binary into the boot
  image
- serial-log verification that PID 1 starts and uses the current ARM64
  userspace syscall path
- explicit smoke-ELF fallback controls
- x86 preservation on the same shared init-launch structure

### Out Of Scope

- broad ARM64 syscall parity
- ARM64 `fork`/`clone` parity
- full signal delivery parity
- desktop or framebuffer work
- broad ARM64 libc or toolchain parity
- automatic fallback heuristics
- forcing `/bin/shell` to be the first ARM64 PID 1 if a smaller init-style
  binary is the lower-risk milestone target

## Testing Strategy

Testing should prove three things.

### 1. ARM64 Real Init Launch

Add focused boot verification that confirms:

- ARM64 attempts the configured filesystem-backed init path
- PID 1 reaches EL0 from that file-backed path
- serial logs show the expected init success markers
- PID 1 exit or teardown returns control to the kernel console state

### 2. Fallback Discipline

Add regression coverage that proves:

- the smoke ELF does not run by default
- the smoke ELF does run when the explicit fallback knob is enabled
- missing or invalid ARM64 init binaries fail loudly when fallback is disabled

### 3. x86 Stability

Keep the current x86 verification path green:

- existing headless tests
- existing kernel build path
- any current boot checks that confirm console-first operation without desktop

## Success Criteria

This milestone is complete when:

- ARM64 boots a real filesystem-backed AArch64 ELF as PID 1
- the default ARM64 boot path no longer depends on the embedded smoke ELF
- fallback to the smoke ELF is explicit and test-covered
- x86 remains functional on the same shared init-launch architecture
- merged verification proves console-first behavior with no desktop dependency
