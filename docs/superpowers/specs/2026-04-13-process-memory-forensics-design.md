# Process Memory Forensics Design

Date: 2026-04-13
Status: Approved for planning

## Summary

Build a unified process-memory forensics layer for DrunixOS that explains both
live and crashed user-process memory state from one kernel-side source of
truth. The first release targets the current high-value debugging problems in
the system: bad mappings, protection faults, copy-on-write surprises, lazy
heap faults, anonymous `mmap()` behavior, and grow-down stack issues.

The feature will expose one canonical memory-forensics model through two
surfaces:

- live inspection through new procfs files under `/proc/<pid>/...`
- post-mortem inspection through richer ELF core-dump notes

The implementation will start with crash-oriented value, but the design must
support both surfaces without duplicating memory interpretation logic.

## Goals

- Make runtime process-memory bugs easier to explain and reproduce.
- Preserve the exact memory story of a crashing process in its core dump.
- Provide a live procfs view that matches the core-dump view as closely as
  possible.
- Reuse one collector/classifier pipeline instead of maintaining parallel
  procfs-only and core-only logic.
- Keep the feature focused on user-process memory debugging rather than generic
  kernel tracing.

## Non-Goals

- Building a general kernel debugger protocol.
- Symbolication, source-line mapping, or stack unwinding.
- Kernel-memory forensics for ring-0 faults.
- A generic event-tracing system for syscalls or scheduling.
- A fully machine-readable stable debug ABI in the first release.

## User Outcomes

After this feature lands, a developer debugging a process-memory issue should
be able to answer:

- Why did this process receive `SIGSEGV`, `SIGBUS`, or another crash signal?
- Did the faulting address fall inside a known region or outside all VMAs?
- Was the access blocked by protection, missing backing, copy-on-write
  promotion failure, stack growth limits, or another classified condition?
- How much virtual space was reserved versus actually backed by present user
  pages?
- What did the process memory layout look like while alive, and what final
  layout was captured at crash time?

## Scope

The first release covers user-process address-space reporting for:

- executable image ranges
- heap reservation and current `brk`
- grow-down user stack state
- anonymous private `mmap()` regions
- crash context recorded in `process_t`

The first release must not add new memory-management behavior. It is a
debugging and observability feature layered on top of the existing VMA, fault,
procfs, and core-dump systems.

To support richer fault reporting, this feature may extend the stored crash
metadata in `process_t` with additional raw fault fields such as page-fault
error bits. That is observability-only state, not new memory-management
behavior.

## Architecture

The design is split into three pieces.

### 1. Memory Forensics Collector

Add a kernel helper that inspects a `process_t` and produces a structured
forensics summary. This collector is the canonical source of truth for memory
debug output. It must not depend on procfs formatting or ELF note layout.

Responsibilities:

- walk the process's known memory regions and classify them
- summarize reserved bytes versus currently mapped user pages
- expose current heap and stack bounds
- expose crash context when present
- classify the recorded crash address against VMAs and page-table state

The collector output should be structured enough that multiple renderers can
consume it without re-deriving facts from raw kernel structures.

### 2. Procfs Live Surface

Expose compact live forensic files under `/proc/<pid>/`:

- `maps` remains the region-oriented view and may be enhanced with collector
  labels or attributes where that improves clarity
- `vmstat` reports compact memory totals and counts
- `fault` reports the most recently recorded crash or fault context when one is
  available

Procfs rendering should be a thin formatting layer over the collector output.
It should not become a second memory-analysis subsystem.

### 3. Core-Dump Note Integration

Extend the ELF core-dump writer to emit extra notes derived from the same
collector output. The notes should preserve both raw crash facts and the
kernel's derived interpretation of the crash.

This keeps post-mortem analysis aligned with procfs and avoids drift between
"live" and "dead" process debugging tools.

## Canonical Forensics Model

The collector must answer four questions.

### Region Inventory

For each reported region, record:

- start and end address
- region category
- protection flags
- short label

Expected region categories in the first release:

- image text/data
- heap reservation
- anonymous `mmap`
- active stack window
- stack growth reserve or boundary-adjacent stack region when relevant

### Backing State

For each region, report both reserved space and mapped space:

- reserved bytes
- present user-backed page count or bytes

This distinction is required because DrunixOS now supports lazy heap growth,
lazy anonymous `mmap()`, grow-down stacks, and copy-on-write fork.

### Crash Context

When `process_t` contains a valid crash record, preserve:

- delivered signal number
- recorded `cr2`
- saved `eip`
- page-fault error bits when the crash originated from a page fault
- whether the fault address was inside a known region
- a derived classification string

The design must preserve raw fault facts first, then add derived
classification. The system should never discard low-level details in favor of a
single simplified label.

If the current crash record does not already retain a required raw field, the
implementation must extend the crash metadata explicitly rather than infer the
missing value later from incomplete state.

### Summary Totals

The first release should expose at least:

- total reserved bytes
- total mapped bytes
- heap reserved bytes
- heap mapped bytes
- stack reserved bytes
- stack mapped bytes
- anonymous `mmap` reserved bytes
- anonymous `mmap` mapped bytes
- image mapped bytes
- VMA count

## Crash Classification

The classifier should use the recorded crash address, error bits, VMA lookup,
and page-table state to produce explicit categories such as:

- unmapped address outside all VMAs
- access inside VMA but blocked by protection
- copy-on-write write fault
- stack growth beyond allowed limit
- reserved lazy region with missing backing
- unknown

The first release should only classify cases the kernel can justify from
existing facts. If a case cannot be classified confidently, the output must say
`unknown` rather than guessing.

## Procfs Behavior

### `/proc/<pid>/vmstat`

This file is the compact summary view. It should be human-readable and bounded
in size. It must report the summary totals from the collector without forcing a
developer to reverse-engineer them from `maps`.

### `/proc/<pid>/fault`

This file reports the most recent recorded crash context for the process.

Behavior:

- for a live process with no recorded crash, return a clear empty-state report
  rather than an error
- for a process with crash data, include raw fields and derived classification
- if classification cannot be completed safely, report `unknown`

### `/proc/<pid>/maps`

`maps` remains the detailed region listing. It can reuse the collector's region
labels and attributes, but it should stay recognizable as the detailed layout
view rather than becoming a duplicate of `vmstat`.

## Core-Dump Behavior

The core dump must continue to preserve the process image as it does today, but
with additional notes derived from the forensics collector.

The new notes should preserve:

- crash facts captured from the process record
- memory-summary totals
- region metadata needed to interpret the captured address space

The post-mortem notes should represent the same interpretation that procfs
would render for the process at the moment of crash.

## Error Handling And Limits

- Rendering must use bounded buffers so procfs and core-note generation remain
  predictable.
- If data cannot be collected safely, mark the field or classification
  `unknown`.
- Procfs access must remain read-only and side-effect free.
- The feature must not introduce any mutation to VMA state, page tables, or
  crash records during reporting.

## Testing Strategy

### Collector Unit Tests

Add focused tests for:

- region classification
- reserved versus mapped totals
- crash-address region lookup
- crash classification for known cases
- `unknown` classification when facts are insufficient

### Procfs Integration Tests

Extend procfs and VFS tests to cover:

- `/proc/<pid>/vmstat` on a live process
- `/proc/<pid>/fault` with no crash record
- `/proc/<pid>/fault` with a populated crash record
- agreement between collector-backed values and rendered procfs output

### Crash-Path Verification

Use targeted repro programs or runtime tests to exercise:

- invalid unmapped access
- protection fault
- copy-on-write write fault
- lazy heap access that should resolve without becoming a crash

Verification should confirm that:

- crashes produce the richer core notes
- live procfs output and post-mortem notes agree where they describe the same
  facts
- non-crashing lazy-allocation cases do not leave misleading crash reports

## Risks And Mitigations

### Risk: Divergence Between Procfs And Core Dumps

Mitigation: both surfaces must consume one collector output rather than reading
kernel structures independently.

### Risk: Overstating Crash Interpretation

Mitigation: preserve raw fields alongside derived classification and use
`unknown` when evidence is incomplete.

### Risk: Scope Creep Into General Debugging Infrastructure

Mitigation: keep the first release limited to memory forensics for user
processes and do not fold in unrelated tracing, scheduler events, or symbol
work.

## Recommended Implementation Order

1. Introduce the collector data model and summary/counting helpers.
2. Add crash classification based on existing crash-record inputs.
3. Expose `vmstat` and `fault` through procfs.
4. Extend core-dump note generation to use the same collector output.
5. Add small userland helpers later if the procfs surface proves useful.

This order delivers crash-first value while keeping the live-inspection path on
the same internal abstraction.
