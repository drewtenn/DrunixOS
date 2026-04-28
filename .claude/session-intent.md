---
created: 2026-04-28
command: /octo:plan
---

# Session Intent Contract

## Job Statement

Produce a **prioritized feature roadmap** for the Rexy compiler (`external/rexc/`),
covering language features and standard library, organized into P0/P1/P2 tiers.
This is a research/ideation session — output is the roadmap document, not implementation.

## Context

- **Project:** Rexy systems language + `rexc` compiler (host-portable, targets x86_64 / arm64; Drunix is primary proving ground)
- **Current maturity:** Working subset — language compiles non-trivial programs (see `examples/core.rx`, `examples/stdlib.rx`) but is missing many features expected of a modern systems language
- **What works today (from `grammar/Rexy.g4` + examples):**
  - Primitives: `i8..i64`, `u8..u64`, `bool`, `char`, `str` literals
  - Raw pointers `*T`, address-of `&`, deref `*`, pointer arithmetic, indexing
  - Static scalars + static arrays (with initializers)
  - Functions, `extern fn`, modules (`mod`/`use`), visibility (`pub`)
  - Control flow: `if/else`, `while`, `for` (C-style), `match` (integer/bool/char patterns only)
  - Pre/postfix `++`/`--`, logical/comparison/arithmetic ops, `as` casts
  - Single-parameter "handle types" (`IDENT<T>`) — recently typed (per git log)
  - Stdlib surface visible: `println`, `print_*`, `read_line`, `read_i32`, `parse_*`, `str_*`, `strlen`
- **Known gaps (high-level):** structs/enums, sum types, generics, closures, slices, dynamic memory, error/option types, traits, references with lifetimes, destructuring, iterators, comptime/const, format strings, robust stdlib

## Success Criteria

- [ ] **Clear understanding** — a roadmap document I can refer back to when picking next work
- [ ] Features are tier-ranked (P0 / P1 / P2) with rationale
- [ ] Coverage is balanced across language features and standard library
- [ ] Roadmap reflects actual gaps in current rexc, not generic compiler boilerplate
- [ ] Each feature entry is concrete enough to scope into a future task

## Boundaries

- **In scope:** Language features (syntax, types, semantics) and standard library APIs
- **Out of scope (for this session):** Compiler internals (IR, optimization passes, codegen), tooling (LSP, formatter, debugger, package manager) — the user explicitly excluded these
- **Not implementing anything** — output is documentation only
- **No commits** — leave the roadmap as a draft for review

## Constraints

- None flagged by user (no time pressure, stakes, architecture, or skill constraints selected)

## User Profile (from intent capture)

| Dimension | Answer |
|---|---|
| Goal | Research compiler features |
| Knowledge | Some familiarity (working subset exists) |
| Clarity | Implicit "general direction" — knows the area, wants the survey |
| Success | Clear understanding |
| Scope | Language features + Standard library |
| Organization | By priority tiers (P0/P1/P2) |
