# ARM64 Userspace Convergence Design

## Goal

Advance from the current ARM64 kernel-mode console milestone to the first real
ARM64 user-mode process, while continuing to converge x86 and ARM64 behind one
shared userspace architecture boundary.

This phase does not target `/bin/shell` on ARM64 yet. It targets a real
Linux/AArch64-style smoke-test ELF that proves the kernel can:

- load an ARM64 user executable
- enter EL0
- handle at least a minimal syscall path
- return to EL0 or terminate cleanly
- capture and report user faults through the shared process boundary

## Desired End State

At the end of this phase:

- x86 still boots and passes its existing headless verification path
- ARM64 still boots to a serial console when user-mode launch is not active or
  after the smoke test completes
- shared process and syscall code stop embedding x86-only user-entry
  assumptions
- ARM64 can launch one real user ELF in EL0 and observe expected success over
  serial logs

## Constraints

- Keep the current "terminal, no desktop" target for ARM64.
- Do not add shell-specific shortcuts to the ARM64 ABI path.
- Follow Linux/AArch64 conventions for ELF class/machine validation, general
  register calling convention, stack alignment, and syscall register usage.
- Preserve current x86 behavior through adapter layers rather than rewriting
  the working i386 path in place.
- Keep boundary changes behavior-level, not broad speculative abstractions.

## Chosen Approach

Use a shared userspace convergence step with arch-owned execution adapters.

The shared layer owns:

- process lifecycle semantics
- common exec/create flow
- generic ELF loading pipeline
- generic syscall dispatch contract
- generic crash and fault capture contract

The arch layer owns:

- trap frame definitions
- user entry and return mechanism
- syscall entry and argument extraction
- saved user register state
- context-switch handoff details
- architecture-specific ELF ABI details
- architecture-specific fault decoding

This keeps the kernel on one shared control flow with two real backends instead
of preserving x86 semantics in shared code and bolting ARM64 onto them.

## Architecture

### 1. Shared Userspace ABI Surface

Introduce or extend arch-facing hooks so shared code deals in behavior-level
operations rather than register-layout details. The next phase should provide
interfaces for:

- building an initial user context from entry point and stack state
- building an exec replacement context
- identifying whether a trap came from user mode
- extracting syscall number and arguments from an arch trap frame
- writing syscall return values back into the arch trap frame
- classifying synchronous user faults into shared process/crash semantics
- exporting register state for diagnostics and core metadata

The goal is that shared process/scheduler/syscall code no longer needs to know
whether user entry is x86 `iret`-based or ARM64 `eret`-based.

### 2. ELF Loader Split

Keep a single shared ELF load pipeline, but split architecture-specific ELF ABI
rules out of the current i386-only loader path.

Shared logic should continue to own:

- file reads
- segment walking
- mapping pages into a target address space
- copying segment bytes
- zero-filling BSS
- computing image bounds and heap start

Arch-specific logic should own:

- supported ELF class and machine validation
- per-arch header structures
- entry-state conventions
- aux vector and stack ABI details that differ by architecture

The immediate requirement is support for:

- x86: ELF32 + `EM_386`
- arm64: ELF64 + AArch64 machine validation

This phase may keep the loader implementation split internally if that is the
lowest-risk route, but the outward process-create path should remain shared and
architecture-neutral.

### 3. Arch Entry Paths

#### x86

x86 keeps its current working user-entry path, but it stays behind the newer
arch adapter layer:

- `INT 0x80` syscall entry remains valid
- `iret`-based first user entry remains valid
- existing saved context and scheduler behavior remain intact

The objective for x86 in this phase is preservation, not redesign.

#### arm64

arm64 gains a real EL0 entry/return path:

- real initial EL0 register frame construction
- real synchronous exception return through `eret`
- real syscall trap entry using Linux/AArch64-compatible register conventions
- real user-fault classification for EL0-origin exceptions

The ARM64 path must not emulate x86 frame shapes or x86 syscall calling
conventions.

### 4. First ARM64 User Payload

The first ARM64 payload is a tiny ABI smoke-test ELF, not `/bin/shell`.

It should validate the minimum viable userspace contract:

- execution reaches user entry
- one or more syscalls successfully enter and return
- a controlled exit tears the process down cleanly
- a controlled exit must prove clean teardown in this phase

User-fault injection is explicitly deferred to the following phase unless it is
needed to make the basic EL0 fault path work at all.

The binary should stay purpose-built and narrow. It is an ABI proof, not a user
 interface.

## Data Flow

### ARM64 Boot To User ELF

1. ARM64 boots into the kernel and initializes UART, timer, MM, IRQs, and the
   scheduler.
2. The kernel sets up whatever minimal runtime environment is needed for the
   first user ELF.
3. Shared process creation selects the ARM64 executable path based on the
   target binary and architecture.
4. Shared ELF load orchestration validates the ARM64 executable, maps segments,
   computes image bounds, and prepares user memory state.
5. Shared process code asks the ARM64 arch layer to synthesize the initial EL0
   register context.
6. The scheduler switches to the process through the arch context adapter.
7. ARM64 transitions to EL0 and begins executing the smoke-test binary.
8. A userspace syscall or fault traps back into EL1 through the ARM64 exception
   path.
9. Shared syscall or fault handling runs through the new behavior-level
   contract.
10. The arch layer returns to EL0 or the process exits and the kernel resumes
    control.

### x86 Preservation Path

The x86 control flow should continue to follow the same shared process and
syscall orchestration, but the arch edge remains:

- ELF32/i386 executable path
- `INT 0x80` syscall entry
- `iret`-based user return
- existing x86 scheduler/context handoff

That is the target shape of the split: common flow in the middle, arch-specific
entry and return at the edge.

## Error Handling

The phase should be explicit about failure modes.

### User Executable Validation

- Unsupported ELF class or machine must fail cleanly during load.
- The error should be attributable in logs or return paths without relying on
  x86-only assumptions.

### ARM64 User Faults

- Synchronous exceptions originating from EL0 should be classified through the
  shared fault path.
- The owning process should terminate cleanly when the fault is fatal.
- The kernel should preserve enough register and fault state to debug the
  failure from serial logs and existing crash/core infrastructure where
  applicable.

### ARM64 Kernel Faults

- Kernel-origin exceptions on ARM64 may still hard-stop in this phase.
- This phase only requires EL0-origin exceptions to be handled as process
  faults rather than unconditional machine halt paths.

### Syscall Path

- Unsupported ARM64 syscalls should fail cleanly via the generic syscall layer.
- The smoke-test binary should exercise only the small syscall subset required
  for proof of bring-up.

## Scope

### In Scope

- shared interfaces needed for real ARM64 userspace entry
- ARM64 ELF64 executable loading for the smoke-test binary
- ARM64 EL0 entry and return
- ARM64 syscall trap entry and minimal syscall return path
- ARM64 user fault classification and clean process teardown
- scheduler/context integration sufficient for one real ARM64 user process
- focused regression guards for the shared userspace boundary
- x86 preservation through existing adapter-backed paths

### Out Of Scope

- `/bin/shell` on ARM64
- full ARM64 signal delivery parity
- ARM64 fork or clone parity
- ARM64 desktop or framebuffer work
- broad ARM64 userspace toolchain parity
- storage or device expansion unrelated to the smoke-test path

## Testing Strategy

Testing should prove three things.

### 1. Boundary Integrity

Add or extend focused guards so shared code does not regress back toward:

- x86-only user-entry declarations in shared headers
- x86 trap-frame assumptions in shared syscall or fault paths
- x86-specific syscall argument extraction in shared code

### 2. x86 Stability

Keep the current x86 verification path green:

- kernel build
- headless tests
- existing process and syscall behavior

### 3. ARM64 Userspace Bring-Up

Add a focused ARM64 verification path that boots the kernel, launches the
smoke-test ELF, and checks serial logs for the expected milestones:

- loader start or process launch marker
- successful EL0 execution marker
- successful syscall round-trip marker
- clean exit marker

If fault testing is included in this phase, the log should also show that the
fault was attributed to the user process rather than treated as a generic
kernel halt.

## Phase Deliverable

The deliverable for this phase is not a shell. It is a real ARM64 EL0 process
running through shared kernel process machinery with Linux/AArch64 ELF and
syscall conventions, verified by a narrow smoke-test binary and guarded against
architecture-boundary regressions.
