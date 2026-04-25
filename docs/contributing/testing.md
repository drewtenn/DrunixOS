# Testing Policy

Drunix keeps public test workflows architecture-neutral. Contributors should be
able to ask for a test intent by name without knowing whether the current build
is x86 or arm64.

## Public Targets

Public phony target names must be architecture-neutral across x86 and arm64.
Use names such as `check-shell-prompt`, `check-kernel-unit`, and
`test-headless` for the intent. Select the architecture with `ARCH=x86` or
`ARCH=arm64`, not by publishing separate top-level target names.

Public test intents should be shared between architectures whenever possible.
If the exact same host test can drive both targets, keep one shared tool and
pass `--arch` or use `ARCH` from make. If a test cannot be literally shared,
each architecture must still have an architecture-specific test that covers the
same intent.

When adding or changing a public test intent, update
`tools/test_intent_manifest.py`. `tools/check_test_intent_coverage.py` reads
that manifest and verifies that every listed intent has coverage for both x86
and arm64.

## Kernel Unit Tests

Kernel unit tests run on both architectures through `test-headless` and
`check`. Shared KTEST cases belong in shared test files so both kernels compile
the same coverage. Architecture-specific KTEST cases are allowed when the code
under test is architecture-owned, but they need paired intent coverage in
`tools/test_intent_manifest.py`.

## Useful Checks

For lightweight verification while editing test wiring or policy docs, use:

```sh
python3 tools/check_test_intent_coverage.py
make -n ARCH=x86 check
make -n ARCH=arm64 check
make -n ARCH=x86 test-headless
make -n ARCH=arm64 test-headless
```

Before submitting code changes that affect tests, run the real target for each
architecture you touched:

```sh
make ARCH=x86 check
make ARCH=arm64 check
```

These commands may boot QEMU. For documentation-only edits, the dry-run and
manifest checks above are usually enough.
