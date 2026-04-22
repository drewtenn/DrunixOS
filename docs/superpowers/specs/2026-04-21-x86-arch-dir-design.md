# x86 Arch Directory Design

## Goal

Move the current x86-specific sources that live directly under `kernel/arch/`
into a dedicated `kernel/arch/x86/` directory so the x86 tree matches the
existing architecture-oriented layout used by `kernel/arch/arm64/`.

This change is intentionally limited to `kernel/arch/`. It does not move
top-level boot sources, linker scripts, or kernel entry assembly in this pass.

## Scope

In scope:

- Move the current x86 architecture implementation files from
  `kernel/arch/` to `kernel/arch/x86/`
- Update build metadata so x86 objects are compiled from their new paths
- Update source includes and include search paths as needed for the new layout
- Preserve current x86 build behavior

Out of scope:

- Moving `boot/`
- Moving `kernel/kernel-entry.asm`
- Moving `kernel/kernel.ld`
- Introducing a new shared cross-architecture abstraction layer
- Changing ARM64 sources or behavior beyond any shared include-path fallout

## Current State

The repository already has `kernel/arch/arm64/` with its own `arch.mk`,
linker script, and architecture-local sources. By contrast, the x86 sources
still live directly in `kernel/arch/`, and the x86 build references those
paths explicitly from shared make fragments such as `kernel/objects.mk` and
`Makefile`.

That makes the architecture layout inconsistent and leaves x86-specific code in
what now reads like a shared directory.

## Proposed Change

Create `kernel/arch/x86/` and move the current x86 architecture files there.
After the move, `kernel/arch/` becomes the parent architecture namespace with
subdirectories for each architecture, starting with `x86/` and `arm64/`.

The x86 object list in `kernel/objects.mk` will be updated to point at
`kernel/arch/x86/...` object paths. Any explicit rules in `Makefile` that
reference x86 architecture files under `kernel/arch/` will be updated to use
the new `kernel/arch/x86/` paths instead.

Source includes will be updated only as needed to keep the build working under
the new directory structure. This should remain a mechanical path update rather
than a functional rewrite.

## Build And Include Strategy

The refactor should preserve the current top-level build flow:

- x86 remains the default `ARCH`
- `kernel/objects.mk` continues to define the x86 object list
- `kernel/arch/arm64/arch.mk` remains unchanged

To support the move cleanly:

- The x86 include path will be adjusted so x86-local headers can still be found
  without forcing unrelated code to know exact relative file locations
- Any direct path references in assembly, C, or make rules will be updated to
  the new `kernel/arch/x86/` location

No compatibility shims or forwarding headers are planned in this pass.

## Risks And Mitigations

Risk: stale build rules or object paths still point at `kernel/arch/...`.
Mitigation: update both object lists and any special-case make rules that name
specific x86 files.

Risk: header resolution breaks after removing `kernel/arch` as the implicit x86
include root.
Mitigation: update include search paths and any explicit includes during the
same patch set.

Risk: the refactor accidentally expands into boot-path restructuring.
Mitigation: keep all non-`kernel/arch` x86 files in place for this change.

## Verification

At minimum, verify that the x86 tree still builds after the move. The expected
verification target is an x86 build-oriented make target such as `make kernel`
or another equivalent target that exercises the renamed source paths and object
generation.

If any build failure appears, fix only pathing and dependency fallout caused by
the directory move. Behavioral changes are out of scope.

## Success Criteria

- All former x86 files under `kernel/arch/` live under `kernel/arch/x86/`
- The x86 build uses the new file paths successfully
- `kernel/arch/arm64/` remains intact
- No boot, linker, or top-level kernel-entry files move in this pass
