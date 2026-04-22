# Kernel Architecture Boundary Phases Design

## Goal

Migrate the kernel from a path-level x86/ARM split to a real behavior-level
architecture split, while preserving the existing x86 mainline and the current
AArch64 Milestone 1 bring-up throughout the migration.

Each migration phase must:

- introduce only the minimum shared arch API needed by current callers
- move affected shared-kernel callers to the new API in the same phase
- keep both `make kernel` and `make ARCH=arm64 build` green
- end in a standalone commit before the next phase begins

## Current Context

The repository has already moved x86-specific files into `kernel/arch/x86/`
and PC-specific hardware into `kernel/platform/pc/`, but shared kernel code
still reaches directly into x86-oriented interfaces.

Current coupling points include:

- `clock.h` included directly from shared code such as `kernel/lib/klog.c`,
  `kernel/fs/*`, and `kernel/proc/syscall/*`
- `print_string()` exported from `kernel/kernel.c` and consumed by shared code
  such as `kernel/lib/klog.c` and `kernel/drivers/tty.c`
- x86-specific trap, MMU, and process assumptions still embedded in common
  control flow even where the files already live outside `kernel/arch/x86/`

This design treats the existing directory refactor as complete and focuses on
the next step: replacing implicit x86 contracts with explicit architecture
boundaries.

## Chosen Approach

Use a boundary-first phased migration.

This design rejects:

1. A subsystem-first migration, because the existing couplings cut across
   filesystems, logging, process code, and low-level architecture code.
2. A broad up-front `arch.h` scaffold with many stubs, because that creates a
   wide speculative API before the repo proves what it actually needs.

Instead, each phase adds a narrow slice to `kernel/arch/arch.h` and immediately
moves all currently relevant shared callers over to it in the same commit.

## Interface Shape

### Shared Interface

Create `kernel/arch/arch.h` as the main shared architecture-facing header for
common kernel code.

The header should stay behavior-oriented. It may expose operations such as:

- monotonic time or tick reads
- console output hooks
- IRQ enable/disable and interrupt state helpers
- TLB invalidation and active address-space switching
- trap and syscall setup hooks
- user-entry and context-switch handoff hooks

The header must not expose architecture-private details such as:

- x86 register names
- port I/O details
- PIC/PIT implementation details
- ARM exception register names
- architecture-specific trap frame layouts as shared structs unless they are
  intentionally opaque at the boundary

### Private Architecture Ownership

Concrete implementation stays under:

- `kernel/arch/x86/`
- `kernel/arch/arm64/`

Opaque or architecture-owned state should remain defined in those trees. Shared
code may hold pointers or use behavior-level functions, but it should not know
the concrete register layout or hardware mechanism behind an operation.

### Platform Ownership

PC hardware support remains under `kernel/platform/pc/`. Shared code should not
reach directly into PC hardware semantics unless a behavior-level kernel
interface explicitly requires it.

## Phase Plan

### Phase 1: Time And Console Boundary

Goal: remove shared-kernel dependencies on x86 time and console symbols.

Introduce the smallest `arch.h` surface needed to replace:

- direct `clock.h` inclusion from shared code
- direct `print_string()` usage from shared code

Move current callers over in the same phase, including:

- `kernel/lib/klog.c`
- `kernel/drivers/tty.c`
- shared files under `kernel/fs/`
- shared files under `kernel/proc/syscall/`
- any other common code that currently includes `clock.h` or calls
  `print_string()`

Expected outcome:

- shared code consumes time and console behavior through `arch.h`
- x86 implements the behavior using its current mechanisms
- arm64 provides implementations that preserve Milestone 1 build integrity

### Phase 2: IRQ And Timer Boundary

Goal: separate shared interrupt and tick consumers from x86 PIC/PIT details.

Introduce behavior-level hooks for:

- architecture interrupt init
- timer init
- IRQ enable/disable state
- shared tick-delivery hooks used by scheduler, timekeeping, or other common
  kernel consumers

Move shared callers to the new hooks without exposing PIC, PIT, or ARM timer
register details through the shared boundary.

Expected outcome:

- timer and interrupt-facing shared code stops depending on x86-specific entry
  points
- x86 and arm64 each own their low-level interrupt and timer mechanics

### Phase 3: MMU And Address-Space Boundary

Goal: isolate active address-space and TLB semantics behind arch-owned hooks.

Introduce behavior-level hooks for:

- switching the active address space
- invalidating mappings or TLB entries
- any shared MM entry points that currently assume x86 paging internals

This phase should not attempt a rewrite of higher-level VM policy. It only
removes direct architectural assumptions from shared code.

Expected outcome:

- shared MM/VMA/process code stops reaching into x86 paging details directly
- architecture-specific paging mechanics stay inside `kernel/arch/x86/mm/` and
  `kernel/arch/arm64/`

### Phase 4: Trap, Syscall Entry, And User-Entry Boundary

Goal: separate shared kernel control flow from x86 trap-entry conventions.

Introduce behavior-level hooks for:

- trap initialization
- syscall entry setup
- transition into user mode
- architecture-owned trap-frame handling where shared code needs a boundary

This phase should stop common code from assuming an x86-specific trap or entry
shape while preserving the current x86 syscall path and the AArch64 bring-up
path.

Expected outcome:

- shared code sees architecture-defined trap and user-entry services rather
  than x86 frame assumptions
- architecture-specific trap formats remain private

### Phase 5: Process And Context Boundary

Goal: isolate scheduler-facing execution context details behind the arch layer.

Introduce behavior-level hooks and opaque state for:

- context switching
- per-thread or per-process architecture state
- first-entry process state preparation where still needed by shared code

This phase should move the scheduler and process control flow onto explicit
arch-owned context operations rather than implicit x86 assumptions.

Expected outcome:

- shared process and scheduler code depends on architecture-owned context hooks
- x86 switch assembly and arm64 equivalents are fully behind the boundary

## Commit Strategy

Each phase must land as a separate commit after verification passes. There
should be no mixed-mode period where:

- the shared API exists but callers still use the old direct dependency
- x86 works but arm64 is repaired in a later commit
- architecture-private details are exposed temporarily “just to get through”
  the phase

The repository should be coherent at every commit boundary.

## Verification

Every phase must run the same baseline verification before commit:

- `python3 tools/test_kernel_layout.py`
- `python3 tools/test_generate_compile_commands.py`
- `make kernel`
- `make ARCH=arm64 build`

Each phase may add focused verification relevant to its boundary:

- Phase 1 should confirm shared code no longer includes `clock.h` directly or
  calls `print_string()` from common modules.
- Phase 2 should confirm timer and interrupt initialization still link and
  build correctly on both architectures.
- Phase 3 should confirm shared MM callers no longer reach into x86 paging
  implementation details directly.
- Phase 4 should confirm trap and syscall entry points build cleanly through
  the new architecture boundary.
- Phase 5 should confirm scheduler and process context paths build through
  architecture-owned context hooks.

## Risks And Mitigations

### Risk: `arch.h` grows too quickly

Mitigation: only add API surface required by current callers in the active
phase. Do not predeclare later-phase hooks unless they are required now.

### Risk: shared code still leaks x86 assumptions after a phase “looks done”

Mitigation: each phase should include targeted grep or test coverage for the
direct dependency being removed.

### Risk: arm64 Milestone 1 regresses while chasing x86 cleanup

Mitigation: `make ARCH=arm64 build` is mandatory at the end of every phase, not
optional catch-up.

### Risk: a single phase becomes too large

Mitigation: split by boundary, not by subsystem. If a phase grows beyond a
coherent behavior seam, break it into sub-phases before implementation.

## Non-Goals

This design does not include:

- completing the full AArch64 port beyond preserving Milestone 1
- moving PC hardware support out of `kernel/platform/pc/`
- rewriting higher-level subsystems such as VFS, ext3, scheduler policy, or
  logging behavior beyond what is necessary to consume the new arch boundary
- designing one giant final architecture API up front
