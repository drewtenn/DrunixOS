# ARM64 Console Terminal Design

## Goal

Reach a working split where:

- x86 keeps the existing full kernel behavior
- arm64 boots through the shared architecture boundary instead of x86-only
  assumptions
- arm64 presents a usable serial console terminal in text mode
- desktop and framebuffer paths are out of scope for the arm64 target

This design treats "console terminal, no desktop" as the new ARM milestone that
replaces the older framebuffer-desktop endpoint for the near-term bring-up.

## Constraints

- Keep `main` quality expectations for x86 intact.
- Keep `make ARCH=arm64 build` and `make ARCH=arm64 check` green at every
  coherent checkpoint.
- Do not force arm64 to emulate x86 trap frames, CR3 semantics, VGA text mode,
  PS/2 input, ATA, or i386 user-entry conventions.
- Prefer behavior-level boundaries over wide speculative APIs.

## Target Shape

### x86

x86 remains the feature-complete reference path:

- desktop may continue to exist there
- PC storage, PS/2, and i386 userspace remain x86-owned
- shared kernel code should stop depending on x86 implementation details

### arm64

arm64 should boot into a UART-backed kernel terminal with:

- prompt-driven text input/output
- line editing for basic interactive use
- timer-backed uptime/tick reporting
- no framebuffer, window manager, mouse, or desktop shell

The first terminal is kernel-mode. That is the fastest honest route to a usable
console while the process, syscall, and ELF64 boundaries are still being moved.

## Chosen Approach

Use a staged convergence:

1. Introduce a small shared console-terminal component that is independent of
   desktop, scheduler, storage, and userspace.
2. Wire arm64 boot to that terminal through UART input/output.
3. Continue Phase 4 and Phase 5 boundary extraction so process, trap, and
   context mechanics stop leaking x86 assumptions into shared code.
4. Keep the arm64 console terminal as the visible boot target until ARM64
   userspace, storage, and a shell are genuinely ready.

This intentionally rejects trying to drag the x86 desktop or i386 shell-launch
path onto arm64 prematurely.

## Why A Kernel-Mode Terminal First

The current x86 shell path depends on:

- ELF32/i386 loader expectations
- x86 trap-frame shape
- x86 user-entry assembly
- scheduler/context-switch semantics tied to i386 state
- PC storage for loading `/bin/shell`

Trying to reach an ARM64 user shell before separating those concerns would mix
porting, ABI definition, and device bring-up into one unstable step. A
kernel-mode terminal gives an immediate usable endpoint while preserving a clean
path for the later ABI work.

## Console Terminal Requirements

The shared terminal should provide:

- banner and prompt rendering
- printable character echo
- backspace handling
- CR/LF normalization
- small built-in command set
- output only through `arch_console_write()`

The initial built-in commands should stay small and diagnostic:

- `help`
- `clear`
- `echo`
- `ticks`
- `uptime`
- `mem`

The command surface is intentionally narrow. It is a bring-up terminal, not a
replacement shell.

## Boundary Work After Terminal Bring-Up

### Phase 4

Move trap/syscall/user-entry ownership behind the arch layer:

- arch-owned trap frame types
- arch-owned trap install/setup
- arch-owned first-entry and post-trap return helpers
- shared code consuming behavior-level process-entry services only

### Phase 5

Move process and context mechanics behind the arch layer:

- arch-owned saved-context storage
- arch-owned context switch entry points
- scheduler calling behavior-level switch hooks
- process structures no longer embedding x86-specific register semantics in
  shared state

These phases should preserve current x86 behavior while making the ARM path
buildable and honest.

## Explicit Non-Goals

- ARM framebuffer desktop
- USB input
- SD/eMMC storage in this first console milestone
- pretending the ARM target already has full user-process parity when it does
  not

## Success Criteria

- `make ARCH=arm64 run` shows an ARM console banner and prompt on serial
- `make ARCH=arm64 check` verifies the new console boot signature instead of
  the old heartbeat-only loop
- x86 still builds and runs without regression
- shared kernel code continues moving away from x86-only process and trap
  assumptions in separate verified slices
