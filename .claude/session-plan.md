---
created: 2026-04-28
command: /octo:plan
intent_contract: .claude/session-intent.md
---

# Session Plan — Rexy Compiler Feature Roadmap

## What You'll End Up With

A roadmap document (suggested location: `external/rexc/docs/roadmap.md`) listing
language features and standard-library additions for `rexc`, grouped into:

- **P0 — Foundational** (must-have to grow beyond current working subset; unblocks most other work)
- **P1 — Important** (significantly raises expressiveness or stdlib usefulness)
- **P2 — Nice to have** (polish, ergonomics, or specialized features)

Each entry includes: name, one-paragraph description, rationale for tier, dependencies, and rough effort signal.

## How We'll Get There

### Phase Weights

```
DISCOVER ████████████████████ 50%
  Survey compiler/language features broadly + assess what current rexc has.
  Multi-AI perspectives recommended (Codex + Gemini + Claude) so the
  brainstormed feature universe isn't biased to one model's training.

DEFINE   ██████████ 25%
  Bucket features into P0/P1/P2 with rationale. Decide what to *exclude*
  (out-of-scope tooling, compiler internals). Lock the roadmap shape.

DEVELOP  ██████ 15%
  Write the roadmap document itself. Format, dependencies, effort signals.

DELIVER  ████ 10%
  Sanity-check prioritization against current rexc state. Cross-reference
  examples/grammar to make sure no listed feature is actually already done.
```

### Execution Commands

To execute this plan end-to-end:

```bash
/octo:embrace "build a P0/P1/P2 feature roadmap for the rexc compiler covering language features and stdlib"
```

Or run phases individually (Discover/Define carry the weight here):

- `/octo:discover` — research compiler/language feature universe + current rexc gaps
- `/octo:define` — tier the features
- `/octo:develop` — write the roadmap doc
- `/octo:deliver` — review the roadmap against actual rexc state

## Provider Availability

- 🔴 Codex CLI: **available ✓**
- 🟡 Gemini CLI: **available ✓**
- 🟣 Perplexity: not configured ✗
- 🟤 OpenCode: not installed ✗
- 🟦 Copilot CLI: available ✓
- 🔵 Claude: available ✓

Multi-AI orchestration (Codex + Gemini + Claude) is viable and recommended for
the Discover phase — surveying compiler features benefits from multiple training
distributions.

## Initial Feature Universe (seed for Discover phase)

This is a **starting list** based on a quick read of `grammar/Rexy.g4` and the
examples. Discover phase will expand and challenge it; Define phase will tier it.

### Language features — likely candidates

**Likely P0 (foundational):**
- Structs (record types) with field access
- Enums / sum types with associated data
- Slices / fat pointers (`&[T]`, `&mut [T]`) — currently only raw `*T` + length-by-convention
- Heap allocation primitives or stdlib `Box`/`alloc` API
- `Result<T,E>` / `Option<T>` (or chosen error model)
- String type beyond `str` literal (owned strings, `&str` view distinction)
- Tuples (or named-return analog)
- Const items / compile-time constants (currently only `static`)

**Likely P1 (important):**
- Generics on functions and types (only single-param handle types exist today)
- Pattern matching beyond integer/bool/char (struct/enum destructuring, ranges, guards)
- References with borrow rules (or whatever ownership story rexy chooses)
- Closures / function values / function pointers
- Trait or interface mechanism
- Type inference improvements (let without explicit type)
- Format string / `printf`-style or `format!` macro
- Iterators / `for x in expr` loop form
- `impl` blocks / methods on types
- Operator overloading (or `impl` of arithmetic traits)

**Likely P2 (nice to have):**
- Macros (declarative or proc)
- Async/await
- Lifetime annotations (if borrow checker lands)
- Const generics
- Where-clauses
- Trait objects / dynamic dispatch
- Inline assembly blocks
- Unsafe blocks (formal boundary)
- Range expressions (`0..n`)
- Tuple/array destructuring in `let`
- String interpolation
- Doc comments + extractable docs

### Standard library — likely candidates

**Likely P0:**
- Heap allocator API (`alloc`, `free`, or `Box`-equivalent)
- Owned string type + builder (`StringBuf`?)
- Dynamic array / `Vec<T>` equivalent
- Slice operations (len, iter, index-bounded access)
- Result/Option helpers (`unwrap`, `map`, `?` if error propagation lands)
- Memory utilities (`memcpy`, `memset`, `memcmp` — likely in `core`)

**Likely P1:**
- HashMap / ordered map
- File I/O (`open`, `read`, `write`, `close`)
- Process / env access (`args`, `env`, `exit`)
- Time (`now`, `sleep` — `sleep` already exists as user app)
- Iterators trait/adapter set (`map`, `filter`, `collect`)
- Formatting traits (`Display`, `Debug` analog)
- Numeric conversions / overflow-checked ops

**Likely P2:**
- Networking (`tcp`, `udp`)
- Threading / synchronization primitives
- Random number generation
- Hashing (non-cryptographic + cryptographic)
- JSON / serialization
- Regex
- Date/time beyond basic clock
- Path manipulation utilities

## Debate Checkpoints (optional)

These are decision points where multi-AI debate would add value:

- 🔸 **After Discover:** "Which features belong in P0 vs P1?" — adversarial debate (Codex argues for minimalism, Gemini argues for ergonomic completeness)
- 🔸 **Ownership model:** Borrow checker vs raw pointers + manual rules vs GC — high-stakes design decision worth `/octo:debate` on its own

These are surfaced for awareness; not required to run.

## Success Criteria

From the intent contract:

- [ ] Tier-ranked roadmap (P0/P1/P2) with rationale per item
- [ ] Coverage balanced across language features and standard library
- [ ] Reflects actual rexc gaps (cross-checked against grammar + examples)
- [ ] Each entry concrete enough to scope into a future task

## Next Steps

1. Review this plan
2. (Optional) adjust scope or weights — re-run `/octo:plan`
3. Execute when ready: `/octo:embrace "build a P0/P1/P2 feature roadmap for the rexc compiler covering language features and stdlib"`
