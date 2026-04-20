# C Coding Style

Drunix C code follows Linux kernel coding style by default, with the explicit
repo exceptions listed here. When this file and an external standard disagree,
this file is authoritative for Drunix.

## Baseline

Use the Linux kernel coding style as the starting point for kernel and userland
C files:

- Put function opening braces on the next line.
- Put non-function block opening braces on the same line as the statement.
- Prefer 80-column lines, with exceptions when splitting would make the code
  harder to read or hide useful information.
- Avoid multiple statements on one line.
- Keep functions focused. Split complex functions into named helpers when that
  makes the code easier to review and test.
- Use a shared cleanup exit when a function owns resources that must be released
  on several error paths.
- Prefer comments that explain what or why. Do not comment obvious mechanics.

Use SEI CERT C as the safety and reliability overlay. Treat string handling,
array bounds, integer behavior, memory lifetime, error handling, API boundaries,
and undefined behavior as coding-standard concerns, not only security concerns.

## Drunix Exceptions

Use tabs for C block indentation, configured as 4 columns. Do not use spaces as
the primary indentation unit.

When a source file is touched for a behavior change, apply this policy to the
entire file in the same change set. Keep the diff reviewable by avoiding
unrelated rewrites, but do not leave half of a touched file in an older style.

For pure style cleanup, handle one source file per commit.

Generated data tables and dense binary data files may keep their existing
layout when reflowing them would make the data harder to audit.

## Error And Cleanup Style

Document the error-return convention for a subsystem before changing return
values. In particular, do not silently convert between generic `-1`, unsigned
`(uint32_t)-1`, Linux negative errno values, or subsystem-specific negatives.

For functions with more than one allocated resource, prefer either a small
resource-owning helper or named cleanup labels such as `out_free_path:`. Keep
label names descriptive.

## Mechanical Checks

At minimum, style review should check:

- trailing whitespace
- same-line function opening braces
- long lines outside documented exceptions
- one-line conditionals in kernel code
- repeated literal capacities that should have a named constant

Style tooling is enforced by default: `make scan` must either complete cleanly
or fail the build. Use `SCAN_FAIL=0` only for local auditing when you need the
full report without stopping at the first failing scanner.

The build exposes the current mechanical checks as separate targets:

- `make compile-commands` generates `compile_commands.json` for scanner and
  editor tooling.
- `make format-check` runs `clang-format` in dry-run mode with the repo
  `.clang-format` file.
- `make cppcheck` runs Cppcheck through the generated compilation database.
- `make sparse-check` runs Sparse over kernel C sources.
- `make scan` runs the formatter check and both scanners.

These targets fail by default when they find issues. Use `SCAN_FAIL=0` for a
reporting-only scan while paying down the existing baseline.
