# ARM64 Console Terminal Implementation Plan

> **For agentic workers:** Execute this plan in order. Keep x86 stable, keep
> `ARCH=arm64` buildable, and verify each slice before moving on.

**Goal:** Bring up a usable UART-backed console terminal on arm64 while
continuing the remaining kernel architecture-boundary work needed for a real
arm/x86 split. Desktop stays out of scope for the arm64 target.

**Architecture:** Deliver a kernel-mode console terminal first, then continue
the trap/process boundary extraction behind it. This avoids coupling the ARM
bring-up to x86 desktop, PC storage, or i386 user-entry paths.

**Tech Stack:** Freestanding C, GNU Make, QEMU AArch64, existing x86 kernel
tests, focused Python boundary guards

---

## File Structure

- Create: `docs/superpowers/specs/2026-04-23-arm64-console-terminal-design.md`
- Create: `docs/superpowers/plans/2026-04-23-arm64-console-terminal.md`
- Create: `tools/test_arm64_console_terminal.py`
- Create: `kernel/console/terminal.h`
- Create: `kernel/console/terminal.c`
- Create: `kernel/test/test_console_terminal.c`
- Modify: `kernel/tests.mk`
- Modify: `kernel/arch/arm64/uart.h`
- Modify: `kernel/arch/arm64/uart.c`
- Modify: `kernel/arch/arm64/start_kernel.c`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `Makefile`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/sched.c`
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/arch/x86/*` as needed for Phase 4/5 ownership cleanup
- Modify: `kernel/arch/arm64/*` as needed for Phase 4/5 stubs or real hooks

## Task 1: Add Red Tests For The ARM Console Terminal

- [ ] Add a focused Python check that boots the ARM kernel and fails until a
      stable console banner and prompt are present.
- [ ] Add a host-side kernel unit test for the terminal parser and command
      execution path.
- [ ] Run both checks first and confirm they fail for the expected reason.

## Task 2: Implement The Shared Kernel Console Terminal

- [ ] Introduce a small terminal engine with line editing, prompt rendering,
      and a minimal built-in command set.
- [ ] Keep the terminal independent of desktop, scheduler, VFS, and userspace.
- [ ] Route all output through `arch_console_write()`.

## Task 3: Wire The ARM64 Boot Path To The Terminal

- [ ] Add UART polling support for non-blocking RX.
- [ ] Replace the heartbeat-only ARM idle loop with terminal initialization and
      serial input polling.
- [ ] Preserve timer ticks so terminal commands can report uptime and tick
      count.
- [ ] Update `make ARCH=arm64 check` to validate the new boot signature.

## Task 4: Continue Phase 4 Trap And User-Entry Boundary Work

- [ ] Separate shared process structures from x86 trap-frame ownership.
- [ ] Move x86-only trap/user-entry declarations out of shared headers where
      practical.
- [ ] Add arch-owned placeholders or hooks so arm64 no longer depends on x86
      symbols to compile future process work.

## Task 5: Continue Phase 5 Process And Context Boundary Work

- [ ] Start moving scheduler/process context semantics behind behavior-level
      arch hooks.
- [ ] Keep x86 using its current assembly path through an adapter layer.
- [ ] Leave arm64 with honest stubs where full context switching is not yet
      implemented, rather than embedding x86 assumptions in shared code.

## Verification

Run fresh verification before claiming milestone progress:

- `python3 tools/test_arm64_console_terminal.py`
- `make test KTEST=1`
- `python3 tools/test_kernel_layout.py`
- `python3 tools/test_generate_compile_commands.py`
- `make kernel`
- `make ARCH=arm64 build`
- `make ARCH=arm64 check`
