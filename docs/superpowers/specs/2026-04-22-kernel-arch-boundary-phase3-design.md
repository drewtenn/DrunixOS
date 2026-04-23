# Kernel Arch Boundary Phase 3 Design

## Goal

Complete the third architecture-boundary phase by introducing a real shared
MM boundary and using it to bring up the first usable AArch64 MMU,
physical-memory, and address-space implementation.

This phase intentionally combines the arch-boundary Phase 3 goal with the ARM
port's Milestone 3 goal. It must:

- remove shared-kernel dependence on x86 paging and `cr3` assumptions
- split PMM policy from PMM allocation mechanics
- move shared allocators and VM paths onto a mapping API rather than direct
  physical-address dereferences
- add arm64 support for kernel mappings, per-address-space primitives, active
  address-space switching, and TLB invalidation
- keep the current x86 kernel behavior intact while preserving a clean path to
  Milestone 4 user-entry work

## Current Context

Phase 2 already moved time, console, IRQ, and timer behavior behind
`kernel/arch/arch.h`, but the memory-management surface is still strongly
x86-shaped.

Current coupling points include:

- `kernel/kernel.c` directly calls `pmm_init()`, `paging_init()`,
  `paging_identity_map_kernel_range()`, and
  `paging_mark_range_write_combining()`
- `kernel/mm/fault.c`, `kernel/proc/uaccess.c`, and
  `kernel/proc/syscall/mem.c` walk and rewrite raw x86 PTE state
- `kernel/proc/process.c`, `kernel/proc/elf.c`, and related tests treat a
  process address space as "physical address of the x86 page directory"
- `kernel/gui/desktop.c` reads and writes `cr3` directly to force framebuffer
  presentation through the kernel page tables
- `kernel/mm/slab.c` formats slabs by casting a physical page to a pointer
- `kernel/mm/kheap.c` and other shared code assume fresh kernel-owned pages
  are directly addressable without an explicit mapping step
- `kernel/arch/x86/mm/pmm.c` mixes generic frame allocation with Multiboot
  parsing, VGA-era exclusions, fixed page-table reservations, and framebuffer
  carve-outs

arm64 currently has no MMU, PMM, or address-space implementation at all. The
existing arm64 tree is still a Milestone 1 bring-up path with UART, timer, and
IRQ handling only.

## Chosen Approach

Use a boundary-first combined milestone with a normalized mapping descriptor.

This phase rejects:

1. An opaque callback-only MM boundary, because it would force a VM-policy
   rewrite in the same milestone as the architecture split.
2. A temporary arm64 direct-map model, because it would bake a second
   architecture-specific contract into shared allocators and VM code.
3. Separate per-architecture PMM implementations, because the allocator and
   refcounting behavior are already generic and do not justify duplication.

Instead, this phase introduces:

- a shared arch/MM interface with opaque address-space handles
- an arch-neutral mapping descriptor used by shared VM code
- a shared PMM core fed by arch-supplied memory ranges
- a temporary physical-page access API for shared code that must touch frame
  contents

The boundary becomes real in the same milestone that arm64 starts using it.

## Shared MM Boundary

Phase 3 extends `kernel/arch/arch.h` with MM operations used by shared boot,
allocator, VM, and process code.

### Boundary Shape

The shared MM surface should include behavior-level operations for:

- MM bootstrap after early architecture setup
- kernel mapping and unmapping of physical ranges
- temporary kernel access to a physical page
- address-space create, clone, switch, and destroy
- page mapping, unmapping, and permission updates within an address space
- mapping lookup through a normalized descriptor
- page or address-space TLB invalidation where shared code needs the semantic

Shared code must stop including x86 paging headers directly. Concrete page-table
formats, TTBR values, CR3 encoding, PAT/MAIR details, and raw TLB opcodes stay
inside `kernel/arch/x86/mm/` and `kernel/arch/arm64/mm/`.

### Opaque Address-Space Ownership

Shared code may continue storing an address-space field in process state, but
that field becomes an opaque arch/MM handle rather than a directly indexed
page-directory physical address.

The shared contract is:

- shared code owns VMA policy, CoW policy, lazy allocation policy, and process
  lifecycle
- arch code owns page-table objects, address-space register programming, kernel
  and user translation encoding, and TLB maintenance

### Normalized Mapping Descriptor

Shared VM code still needs to reason about mappings, so the boundary should
return an arch-neutral mapping descriptor rather than hide all state behind
opaque callbacks.

The descriptor should expose only semantics shared callers actually use:

- mapped or not
- physical frame address
- readable
- writable
- executable
- user-visible
- copy-on-write

The descriptor must not expose:

- raw PDE/PTE bit layouts
- x86 page-directory structure
- arm64 descriptor bits or level format
- architecture-specific cache encoding beyond any explicit shared kernel
  mapping flags that current callers require

Shared callers update mappings through arch/MM helper operations that consume
the normalized flags rather than by writing raw entry words.

### Temporary Physical-Page Access

This phase must provide an explicit story for touching frame contents without a
direct map.

Shared code should use a temporary page-access API when it needs to zero, copy,
format, or inspect a physical frame. That API is page-granular and short-lived:

- PMM pages allocated for slab growth are accessed through the temporary mapping
  API while the slab header and freelist are initialized
- CoW paths use it to copy old-frame contents into a new frame
- file-backed or anonymous page fill paths use it to zero or populate pages
- page-table pages remain architecture-owned and are never manipulated by
  shared code through raw pointers

This keeps `phys == virt` out of shared code without forcing a permanent kernel
linear map contract into the public interface.

## PMM Split

The PMM becomes a shared allocator core plus arch-provided physical-memory
policy.

### Shared PMM Core Responsibilities

The shared PMM core owns:

- the allocation bitmap
- per-frame refcounts
- `alloc`, `free`, `incref`, `decref`, and free-page counting
- initialization from normalized usable and reserved ranges

This logic should move out of the current x86-specific `pmm.c` home and become
architecture-neutral shared code.

### Arch Memory-Map Responsibilities

The architecture layer supplies:

- usable RAM ranges
- reserved ranges that must never be allocated
- early boot exclusions such as kernel image, bootstrap page tables, firmware
  memory, MMIO windows, and framebuffer/device carve-outs

On x86, the arch side keeps:

- Multiboot memmap parsing
- the first-MiB reservation
- current framebuffer carve-out logic
- reservations for the kernel image and any early paging structures that remain
  pinned

On arm64, the arch side starts with:

- a fixed BCM2837 usable RAM layout
- exclusions for the kernel image, bootstrap tables, and peripheral MMIO
  window
- no DTB parsing yet

### PMM Boundary Rule

The PMM core should not know where memory information came from. It consumes
normalized physical ranges and manages frames. All platform and firmware
interpretation remains arch-owned.

## Kernel Mapping Model

This phase intentionally avoids making a direct-map kernel model part of the
shared contract.

The kernel mapping rules are:

- each architecture may keep whatever internal kernel mapping layout it needs
  to function efficiently
- every address space includes the same kernel half or kernel-owned shared
  mappings as defined by the active architecture
- shared code may request kernel mappings and temporary frame access through
  the arch/MM interface
- shared code may not assume that a physical address is itself a valid kernel
  virtual address

This is the key design cut for `kheap`, `slab`, fault handling, and user-access
helpers. They move to explicit mapping operations instead of leaning on the x86
identity map.

## arm64 Bring-Up Target

The arm64 side of this milestone should bring up the first real translation
regime under the new boundary.

### Required arm64 MMU Configuration

- 4 KB granule
- 48-bit VA configuration
- caches enabled
- kernel mappings for the kernel image, writable kernel memory, UART/timer and
  peripheral MMIO, and any bootstrap allocations required by the current boot
  path

Although the MMU is configured for a 48-bit VA space, shared kernel and
process-facing code may continue using the current low-address ranges until the
later 64-bit user-ABI milestone broadens that contract.

### Required arm64 Address-Space Primitives

This milestone must add arm64 support for:

- creating a fresh address space
- cloning an address space with the shared CoW policy hooks required by
  current VM code
- mapping and unmapping user pages
- switching the active address space
- invalidating one mapping or an address-space scope through TLBI

This is enough for the shared VM layer to compile and for arm64 to own real
per-process MM primitives, even though a real arm64 user ELF and user-entry ABI
remain out of scope until the next milestone.

## Shared Caller Migration

Phase 3 should migrate every current shared MM caller that reaches into x86
internals.

### Shared Boot And Kernel Mapping Callers

Move `kernel/kernel.c` off direct x86 PMM and paging calls. Shared startup
should request:

- PMM bootstrap through the shared core plus arch memory ranges
- architecture MM bootstrap
- kernel mapping of framebuffer or device memory through the new MM boundary
- any shared cache-mode request through a behavior-level kernel mapping flag,
  not through x86 PAT details

### Shared VM And Process Callers

Move these paths to the normalized mapping descriptor and arch/MM operations:

- `kernel/mm/fault.c`
- `kernel/proc/uaccess.c`
- `kernel/proc/syscall/mem.c`
- `kernel/proc/process.c`
- `kernel/proc/elf.c`
- `kernel/proc/mem_forensics.c`
- any tests that currently inspect raw x86 page-directory or PTE pointers

Shared callers may still ask questions such as:

- is this page mapped
- is it writable
- is it user-visible
- does it carry CoW state
- which physical frame backs it

But they must ask them through the normalized descriptor rather than by
interpreting architecture entry words.

### Desktop And Framebuffer Callers

`kernel/gui/desktop.c` must stop reading and writing raw `cr3`.

If framebuffer presentation needs to operate in a kernel-owned address space,
the arch layer should provide an explicit operation for that semantic. Shared
desktop code should not know how an architecture switches page tables to make
that happen.

## Implementation Shape

This phase is one milestone, but it should still be implemented in risk-limiting
stages.

Recommended sequence:

1. Introduce the shared PMM core and arch memory-range hooks while preserving
   current x86 behavior.
2. Introduce the shared arch/MM interface and adapt x86 paging behind it.
3. Move shared MM, process, and framebuffer callers to the new boundary.
4. Add arm64 MMU bootstrap, kernel mappings, and temporary physical-page access
   support.
5. Add arm64 address-space create, clone, switch, mapping, lookup, and TLB
   invalidation support.

The repository should not pass through a state where shared callers are partly
ported but still require raw x86 paging helpers to build.

## Non-Goals

This milestone does not include:

- ELF64 loader support
- arm64 user-entry or syscall ABI work
- DTB-based RAM discovery
- a redesign of VMA policy or `mmap` semantics beyond removing architecture
  assumptions
- making the full x86 in-kernel process test suite architecture-neutral in the
  same step

## Expected Outcome

After Phase 3:

- shared MM and process code stop depending on x86 paging internals
- PMM allocation mechanics are shared while memory discovery and exclusions are
  architecture-owned
- `kheap`, `slab`, and VM fault paths use explicit mapping services rather than
  `phys == virt`
- x86 continues to provide the existing MM behavior through the new boundary
- arm64 builds with a real MMU, kernel mappings, shared allocators using the
  new interface, and the first usable per-address-space primitives

This milestone ends with a real MM boundary in place, not a temporary arm64
shim.

## Verification

Phase 3 must keep the baseline checks green:

- `python3 tools/test_kernel_layout.py`
- `python3 tools/test_generate_compile_commands.py`
- `make kernel`
- `make check`
- `make ARCH=arm64 build`

It must also add focused verification for the new boundary:

- a repository test that fails if shared code outside `kernel/arch/` still
  includes x86 paging headers or uses raw `cr3` / `invlpg` operations
- focused shared-PMM tests that cover normalized usable/reserved range
  initialization and refcount behavior
- focused x86-backed tests for normalized mapping descriptor behavior, because
  x86 remains the runnable architecture in the current environment
- an arm64 build-path check that proves the new MMU and arch/MM objects are
  wired into the build

## Risks And Constraints

- the mapping descriptor must stay narrow; if it exposes architecture layout,
  the boundary becomes fake
- the mapping descriptor must still expose enough semantics for CoW,
  `mprotect`, lazy allocation, and user-copy paths to remain clean shared code
- `kheap` and `slab` need a disciplined temporary page-access model or they
  will simply recreate a hidden direct map
- framebuffer presentation is a hidden architecture leak today and must move
  fully behind the boundary in this phase
- arm64 MMU enable is sensitive to MAIR, TCR, SCTLR, table-format correctness,
  and TLB maintenance ordering, so shared code must stay entirely above those
  details

## Commit Strategy

Phase 3 should land as a coherent sequence of commits:

1. Add focused boundary and PMM regression tests and confirm they fail against
   the old tree.
2. Introduce the shared PMM core plus x86 and arm64 memory-range providers.
3. Introduce the arch/MM boundary and adapt x86 behind it.
4. Move shared MM, process, and framebuffer callers to the new boundary.
5. Add arm64 MMU and address-space support behind the same interface.
6. Re-run the focused tests and the baseline build checks before landing.
