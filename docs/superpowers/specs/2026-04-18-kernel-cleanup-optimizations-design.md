# Kernel Cleanup Optimizations Design

Date: 2026-04-18

## Summary

This design covers a cleanup-first optimization pass across the Drunix kernel.
The goal is to reduce duplicated control flow and obvious byte-copy loops while
preserving current behavior. The work intentionally avoids allocator rewrites,
block-cache changes, filesystem semantic changes, syscall ABI changes, and
return-code policy changes.

The pass has three parts:

- Path-oriented syscall cleanup in `kernel/proc/syscall.c`.
- VFS lookup and dispatch cleanup in `kernel/fs/vfs.c`.
- Mechanical DUFS copy/zero cleanup in `kernel/fs/fs.c`.

## Goals

- Reduce repeated allocation, path-resolution, VFS-call, and cleanup patterns in
  syscall handlers.
- Reduce repeated path normalization, mount lookup, relpath calculation, and
  cleanup paths in VFS functions.
- Replace exact byte-for-byte DUFS copy and zero loops with existing kernel
  primitives such as `k_memcpy` and `k_memset`.
- Keep changes reviewable as small, local refactors.
- Preserve existing tests as the main behavioral safety net.

## Non-Goals

- No syscall number, register, or ABI changes.
- No intentional change to path normalization, cwd handling, mount matching, or
  synthetic namespace behavior.
- No broad rewrite of `syscall.c`, `vfs.c`, DUFS, EXT3, the block cache, or the
  kernel heap allocator.
- No new filesystem features.
- No change to current return conventions, including existing generic `-1`
  returns and Linux-style negative errno returns.
- No build or test execution as part of writing this design document, because
  the compiler toolchain is still being installed.

## Current Observations

`kernel/proc/syscall.c` is the largest source file in the kernel and contains
many syscall cases with the same shape:

1. Allocate one or more 4096-byte path buffers.
2. Resolve a user pointer with `resolve_user_path` or `resolve_user_path_at`.
3. Call a VFS operation.
4. Free all scratch buffers on every success and error path.

This pattern appears in path-heavy syscalls such as `open`, `openat`, `mkdir`,
`mkdirat`, `rmdir`, `unlink`, `unlinkat`, `rename`, `renameat`, `linkat`,
`symlinkat`, `stat`, `readlink`, and module loading.

`kernel/fs/vfs.c` repeats another related pattern:

1. Normalize a raw path into a heap-allocated string.
2. Reject root or exact mount paths where needed.
3. Find the deepest matching mount.
4. Compute the relative path within that mount.
5. Dispatch to `/dev`, `/proc`, `/sys`, or a mounted filesystem.
6. Free the normalized path on every exit.

`kernel/fs/fs.c` contains DUFS read/write and initialization code with manual
byte loops that can be replaced by `k_memcpy` or `k_memset` where the operation
is exactly equivalent.

## Proposed Architecture

### Syscall Path Helpers

Add small `static` helpers near the existing syscall path helpers in
`kernel/proc/syscall.c`. These helpers should own path scratch allocation and
cleanup for common syscall shapes.

The helpers should be local to `syscall.c` and should not change public
interfaces. They should call the existing `copy_user_string_alloc`,
`resolve_user_path`, and `resolve_user_path_at` functions instead of replacing
the underlying path-resolution logic.

Candidate helper shapes:

- Resolve one user path into a scratch buffer, call a one-path operation, free
  the scratch buffer, and return the operation result.
- Resolve one `*at` user path with a directory fd, call a one-path operation,
  free the scratch buffer, and return the operation result.
- Resolve two user paths, call a two-path operation, free both scratch buffers,
  and return the operation result.
- Resolve two `*at` paths, call a two-path operation, free both scratch
  buffers, and return the operation result.

The syscall cases should remain responsible for syscall-specific validation,
flag filtering, and return translation. For example, `linkat` should still
validate `LINUX_AT_SYMLINK_FOLLOW`, and `readlink` should still manage its
target buffer and user-copy behavior.

### VFS Lookup Context

Add an internal lookup context type in `kernel/fs/vfs.c`, for example:

```c
typedef struct {
    char *norm;
    int mount_idx;
    const vfs_mount_t *mnt;
    const char *rel;
} vfs_lookup_ctx_t;
```

Pair it with helpers that initialize and release the context:

- Normalize the path.
- Optionally reject root and exact mount paths for mutation-like operations.
- Find the matching mount.
- Store the mount pointer and relative path.
- Free `norm` through one cleanup helper.

Use this helper where it reduces duplicated control flow without obscuring the
special cases. Good candidates are `vfs_resolve`, `vfs_getdents`,
`vfs_readlink`, `vfs_stat_common`, and mutation setup used by `vfs_create`,
`vfs_unlink`, `vfs_mkdir`, `vfs_rmdir`, `vfs_rename`, `vfs_link`, and
`vfs_symlink`.

Synthetic namespaces should remain explicit. The cleanup should not hide
`/dev`, `/proc`, and `/sys` behavior behind a generic callback table unless the
existing code naturally becomes clearer that way.

### DUFS Copy Primitives

Replace manual byte loops in `kernel/fs/fs.c` only when the equivalent primitive
call is obvious.

Good candidates:

- Data copies in `fs_read` and `fs_write`.
- Zeroing full scratch blocks or structs when the loop is plainly a memset.
- Small initialization loops where `k_memset` makes intent clearer.

Do not rewrite directory-entry name padding loops unless the exact behavior is
already clear and covered by tests. These loops often encode string truncation
and NUL-padding semantics, so leaving them alone is safer for this pass.

## Error Handling

The cleanup must preserve existing error behavior. In particular:

- If a syscall currently returns `(uint32_t)-1` for allocation or path
  resolution failure, it should continue to do so.
- If a syscall currently returns a Linux-style negative errno for a validation
  case, it should continue to do so.
- Existing cleanup ordering should be preserved where it matters.
- Helpers should avoid swallowing operation-specific return values.

The main benefit of the helpers is reducing missed frees and inconsistent
cleanup paths, not changing how callers observe errors.

## Testing Strategy

Do not build or run tests while the compiler is being installed.

When the toolchain is ready, verification should be:

1. Run the normal compile target, such as `make kernel` or the repository's
   preferred equivalent.
2. Run `make check`.
3. If failures appear around filesystem, VFS, or syscall behavior, inspect the
   focused tests in `kernel/test/test_vfs.c` and `kernel/test/test_process.c`.
4. Review the diff manually for return-code changes, missed frees, and changed
   path semantics.

## Implementation Sequence

1. Refactor path-heavy syscall cases with local helpers in `syscall.c`.
2. Run the compile/test checks when the compiler is available.
3. Refactor repeated VFS lookup and cleanup patterns in `vfs.c`.
4. Run the compile/test checks again.
5. Replace exact DUFS copy and zero loops in `fs.c`.
6. Run the compile/test checks again.

This order gives the highest cleanup payoff first while keeping each stage
reviewable and reversible.

## Risks

- Path syscalls can regress if a helper accidentally changes a return value or
  resolves against the wrong directory fd.
- VFS cleanup can regress synthetic namespace behavior if root, exact mount
  paths, or relpaths are mishandled.
- DUFS copy cleanup is low risk, but directory-entry loops should be left alone
  unless their padding semantics remain identical.

## Acceptance Criteria

- The refactor removes duplicated path-buffer cleanup logic from syscall cases.
- VFS functions have fewer repeated normalization and mount-lookup cleanup
  paths while preserving explicit synthetic namespace behavior.
- DUFS read/write byte-copy loops use `k_memcpy` where equivalent.
- No intended behavior changes are introduced.
- Once the compiler is installed, `make check` passes.
