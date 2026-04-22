# Kernel Arch Boundary Phase 2 Design

## Goal

Complete the second architecture-boundary phase by moving IRQ registration,
dispatch, timer startup, and interrupt-enable sequencing behind
`kernel/arch/arch.h`.

Phase 2 must preserve both the current x86 kernel boot flow and the current
AArch64 Milestone 1 timer heartbeat while removing shared-kernel dependence on
x86-visible IRQ and PIT interfaces.

## Current Context

Phase 1 already moved shared time and console consumers onto `arch.h`, but the
interrupt and timer lifecycle is still x86-shaped:

- `kernel/kernel.c` directly calls `irq_dispatch_init()`, `pit_init()`, and
  `interrupts_enable()`
- `kernel/platform/pc/keyboard.c` and `kernel/platform/pc/mouse.c` include
  x86 `irq.h` directly for handler registration and unmasking
- x86 PIT programming still lives in `kernel/arch/x86/idt.c`
- x86 IRQ vector decoding, PIC masking, and EOI logic are still visible
  through `kernel/arch/x86/irq.h`
- arm64 still handles its timer interrupt as a special case in
  `kernel/arch/arm64/exceptions.c` rather than through a matching registry
  model

That leaves the shared startup path and platform driver initialization coupled
to x86 interrupt mechanics even though the repository now has separate
`kernel/arch/x86/` and `kernel/arch/arm64/` trees.

## Chosen Approach

Use a full IRQ/timer boundary for Phase 2.

This phase intentionally does more than add interrupt-state helpers. It also:

- makes IRQ registration a shared driver-facing service
- moves boot-time interrupt/timer startup behind `arch.h`
- keeps timer-line identity architecture-private instead of teaching shared
  code that "the periodic tick is IRQ0"

This design rejects two narrower alternatives:

1. A dispatch-only boundary, because it would leave `kernel/kernel.c` with
   x86-specific boot sequencing and force a follow-up cleanup phase.
2. A timer-only boundary, because it would create an awkward split where the
   timer path is abstracted but non-timer IRQs still depend on x86-private
   registration and masking interfaces.

## Shared Interface

Phase 2 extends `kernel/arch/arch.h` with IRQ/timer operations used by shared
startup and driver code.

### Required API

- `typedef void (*arch_irq_handler_fn)(void);`
- `void arch_irq_init(void);`
- `void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn);`
- `void arch_irq_mask(uint32_t irq);`
- `void arch_irq_unmask(uint32_t irq);`
- `void arch_timer_set_periodic_handler(arch_irq_handler_fn fn);`
- `void arch_timer_start(uint32_t hz);`
- `void arch_interrupts_enable(void);`

### Boundary Rules

The shared API must remain behavior-level:

- shared code may express handler intent, timer frequency, and masking needs
- shared code may not know about PIC remap offsets, PIT divisors, BCM2836
  local routing registers, or Generic Timer control registers
- shared code may not send EOIs or acknowledge hardware interrupts directly
- shared code may not encode timer IRQ numbers as cross-architecture facts

Trap installation stays out of this phase. `idt_init_early()` on x86 and
`VBAR_EL1` setup on arm64 remain architecture-owned until the later trap-entry
phase.

## Ownership Split

### Shared Kernel Ownership

Shared startup and driver code own:

- deciding which subsystems need periodic timer callbacks
- registering device handlers through `arch_irq_register()`
- requesting line masking or unmasking where a driver genuinely needs it
- choosing when the timer should start relative to other subsystem startup
- choosing when interrupts become globally live

### x86 Ownership

`kernel/arch/x86/` owns:

- IDT IRQ stub wiring and vector layout
- PIC remap and PIC mask state
- vector-to-IRQ translation
- PIC EOI sequencing
- PIT programming details
- any x86-private dispatch table storage

`kernel/arch/x86/irq.h` becomes private to the x86 implementation rather than
an interface used by platform code.

### arm64 Ownership

`kernel/arch/arm64/` owns:

- EL1 IRQ entry plumbing
- BCM2836 local interrupt source inspection
- Generic Timer routing and rearm
- architecture-owned handler registry and periodic timer callback storage
- diagnosis of spurious or unhandled architecture-local interrupt sources

Phase 2 does not attempt full arm64 device-IRQ parity. It only requires the
timer heartbeat path to use the new boundary shape instead of a hardcoded
special case.

### Platform Ownership

PC drivers under `kernel/platform/pc/` keep their platform-specific IRQ line
numbers, but they register those lines through `arch.h` instead of x86-private
headers.

This means keyboard and mouse initialization remain PC-platform concerns
without turning their IRQ line numbers into fake architecture-independent
constants.

## Boot Flow And Dispatch Model

Phase 2 moves shared startup to this lifecycle:

1. Architecture-owned descriptor or vector setup happens the old way.
2. Shared code initializes scheduler- and clock-adjacent subsystems.
3. `arch_irq_init()` prepares the architecture-owned registry and masked state.
4. Shared startup installs the periodic callback with
   `arch_timer_set_periodic_handler()`.
5. Shared or platform code registers early device handlers with
   `arch_irq_register()`.
6. `arch_timer_start(SCHED_HZ)` programs the periodic timer source.
7. `arch_interrupts_enable()` makes external IRQ delivery live.

### Dispatch Expectations

- x86 IRQ entry still arrives through IDT vectors 32-47
- x86 privately decodes vector numbers, invokes the registered line handler,
  and sends the required PIC EOIs
- arm64 IRQ entry still arrives through the EL1 vector table
- arm64 privately identifies the timer source, rearms it, and invokes the
  registered periodic callback or other registered IRQ handler as appropriate
- unregistered or spurious IRQs remain architecture-owned diagnostics rather
  than shared-kernel policy decisions

The periodic scheduler tick is intentionally modeled as a callback registration
and timer-start operation, not as a shared hardcoded IRQ number.

## Migration Scope

Phase 2 should touch the minimum set of files needed to complete the boundary
in one coherent commit sequence.

### Shared Interface And Startup

- modify `kernel/arch/arch.h`
- modify `kernel/kernel.c`

### x86 Architecture Files

- modify `kernel/arch/x86/irq.c`
- modify `kernel/arch/x86/irq.h`
- modify `kernel/arch/x86/idt.c`
- modify `kernel/arch/x86/pit.c`
- modify any x86 build wiring if new private helpers are introduced

### arm64 Architecture Files

- modify `kernel/arch/arm64/irq.c`
- modify `kernel/arch/arm64/timer.c`
- modify `kernel/arch/arm64/exceptions.c`
- modify `kernel/arch/arm64/start_kernel.c` if needed to preserve the
  Milestone 1 heartbeat path through the new boundary

### Platform Drivers

- modify `kernel/platform/pc/keyboard.c`
- modify `kernel/platform/pc/mouse.c`

### Focused Verification

- add a repository check under `tools/` that fails if shared or platform code
  outside `kernel/arch/x86/` still includes x86 IRQ headers or calls
  x86-private IRQ setup directly

## Expected Outcome

After Phase 2:

- `kernel/kernel.c` no longer knows the x86-specific IRQ bring-up recipe
- platform drivers stop depending on x86 `irq.h`
- x86 still owns PIC, PIT, and IDT IRQ mechanics entirely within
  `kernel/arch/x86/`
- arm64 gains a matching registry-and-callback shape for its timer IRQ path
- the repository has a real shared interrupt boundary that later phases can
  build on for MMU, trap, and process migration work

## Verification

Phase 2 must pass the baseline verification used by the overall arch-boundary
effort:

- `python3 tools/test_kernel_layout.py`
- `python3 tools/test_generate_compile_commands.py`
- `make kernel`
- `make ARCH=arm64 build`

It must also add focused checks for this boundary:

- a repository test that fails if shared or platform code outside
  `kernel/arch/x86/` still includes x86 IRQ headers or calls x86-private IRQ
  bring-up functions
- an x86-focused check that proves the shared registration path still supports
  keyboard, mouse, and periodic tick hookup
- an arm64-focused build-path check that proves the timer heartbeat still
  links through the new callback boundary

## Risks And Constraints

- Phase 2 must not quietly expand into the later trap-entry phase
- Phase 2 must not force shared code to depend on synthetic global IRQ-number
  meanings beyond the line numbers owned by the active platform
- arm64 still has a much smaller hardware surface than x86, so the interface
  should only cover behavior already needed by current callers
- the repository should not pass through an intermediate state where x86 uses
  the new API but arm64 is repaired later

## Commit Strategy

Phase 2 should land as a small sequence of coherent commits:

1. Add the focused regression test and confirm it fails against the old tree.
2. Extend `arch.h` and implement the x86 and arm64 IRQ/timer adapters.
3. Move shared startup and PC platform drivers to the new API.
4. Re-run the focused regression test and the baseline build checks.
5. Land the phase with both architectures green.
