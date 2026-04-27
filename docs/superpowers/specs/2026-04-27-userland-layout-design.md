# Userland Layout Design

## Goal

Organize the userland tree so source files are separated from build artifacts,
and make every supported target use the same artifact layout. The current x86
path builds programs and objects directly under `user/`, while arm64 already
uses an out-of-tree build directory. As the app list grows, the flat source
tree and in-place artifacts make it harder to scan, clean, and reason about.

## Source Layout

Userland sources will be grouped by role:

```text
user/
  apps/          # user programs: shell, utilities, test payloads, desktop
  runtime/       # crt, syscall wrappers, libc, allocator, C++ runtime
  third_party/   # vendored userland dependencies such as nanojpeg
  linker/        # linker script templates
  Makefile
  programs.mk
```

The manifest in `user/programs.mk` remains the single source of truth for the
program set and language lanes. App sources move from `user/<name>.c` and
`user/<name>.cpp` to `user/apps/<name>.c` and `user/apps/<name>.cpp`.
Runtime sources move from `user/lib/` to `user/runtime/`. Vendored nanojpeg
sources move under `user/third_party/nanojpeg/`. `user/user.ld.in` moves to
`user/linker/user.ld.in`.

## Artifact Layout

All targets will emit userland artifacts under one architecture-keyed build
root:

```text
build/
  user/
    x86/
      bin/<program>
      obj/apps/<program>.o
      obj/runtime/<runtime>.o
      runtime/libc.a
      linker/user.ld
    arm64/
      bin/<program>
      obj/apps/<program>.o
      obj/runtime/<runtime>.o
      runtime/libc.a
      linker/user.ld
```

The existing `build/arm64-user` convention will be replaced by
`build/user/arm64` so x86 and arm64 follow the same structure. Generated linker
scripts move out of `user/` and into each architecture build directory.

## Build Changes

The userland makefile will compile from the new source paths and write all x86
outputs to `build/user/x86`. The arm64 make rules in `kernel/arch/arm64/arch.mk`
will use the same source path variables and produce outputs in
`build/user/arm64`.

The top-level disk image rules will package binaries from the matching
architecture build directory while preserving guest paths such as `/bin/shell`
and `/bin/desktop`. Debug targets will load symbols from
`build/user/<arch>/bin/<APP>`.

## Tooling And Tests

Tests and helper scripts that currently assume flat paths such as
`user/shell.c`, `user/lib/stdio.c`, or `user/<program>.o` will be updated to
the new source and artifact layout. `compile_commands.json` generation will
emit entries using source paths under `user/apps` and `user/runtime`, with
outputs under `build/user/<arch>/obj`.

The root `.gitignore` can drop the long list of ignored `user/<program>`
binaries because userland build output will live under the already ignored
`build/` directory.

## Validation

The implementation should verify the new layout with targeted checks:

- `make -C user print-progs`
- `make user/shell` or the updated equivalent target
- `make disk`
- `python3 tools/check_userland_runtime_lanes.py`
- `python3 tools/test_generate_compile_commands.py`
- `python3 tools/test_make_targets_arch_neutral.py`

If cross-toolchains are unavailable locally, the implementation should still run
the Python layout checks and report which build commands could not be verified.
