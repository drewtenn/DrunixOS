# Kernel Cleanup Optimizations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor duplicated kernel syscall/VFS cleanup paths and mechanical DUFS byte-copy loops without changing observable behavior.

**Architecture:** Keep the cleanup local to the existing large kernel files instead of splitting modules during this pass. Add small `static` helpers in `syscall.c` and `vfs.c`, then replace exact byte loops in DUFS with existing `k_memcpy`/`k_memset` primitives.

**Tech Stack:** Freestanding C kernel, KTEST in-kernel test suite, GNU Make, QEMU headless checks, existing Drunix helpers (`kmalloc`, `kfree`, `k_memcpy`, `k_memset`, `uaccess`, VFS APIs).

---

## Scope And File Map

**Modify:**

- `kernel/proc/syscall.c`
  - Add local helpers for one-path and two-path syscall patterns.
  - Refactor path-heavy syscall cases to use the helpers where the current behavior is simple and identical.

- `kernel/fs/vfs.c`
  - Add an internal lookup context helper that owns normalized path lifetime.
  - Refactor repeated normalization, mount lookup, relpath setup, and cleanup paths.

- `kernel/fs/fs.c`
  - Replace exact manual byte-copy and zeroing loops with `k_memcpy` and `k_memset`.
  - Leave directory entry name-padding loops unchanged unless the byte-for-byte behavior is obvious.

**Test/Inspect:**

- `kernel/test/test_process.c`
  - Existing syscall/path coverage: cwd, open/create/append, blockdev fd path, BusyBox helper syscalls.

- `kernel/test/test_vfs.c`
  - Existing VFS coverage: no-mount behavior, child mount precedence, root listings, `/dev`, `/sys`, `/proc`.

- `kernel/test/test_fs.c`
  - Existing DUFS coverage: write/readback, truncate zero-fill, create/unlink, subdirectory create.

No new public headers or exported APIs are planned.

---

### Task 1: Baseline Verification

**Files:**
- Read: `docs/superpowers/specs/2026-04-18-kernel-cleanup-optimizations-design.md`
- Read: `kernel/proc/syscall.c`
- Read: `kernel/fs/vfs.c`
- Read: `kernel/fs/fs.c`
- Test: existing KTEST suite

- [ ] **Step 1: Confirm the worktree starts clean**

Run:

```bash
git status --short
```

Expected: no output.

- [ ] **Step 2: Compile the kernel before refactoring**

Run:

```bash
make kernel
```

Expected: command exits 0 and produces the normal kernel/ISO build artifacts.

- [ ] **Step 3: Run the full headless kernel test suite**

Run:

```bash
make check
```

Expected: command exits 0. If the command prints a KTEST summary, it should report zero failures.

- [ ] **Step 4: Inspect current path-heavy syscall patterns**

Run:

```bash
rg -n "kmalloc\\(4096\\)|resolve_user_path|resolve_user_path_at|vfs_mkdir|vfs_rmdir|vfs_unlink|vfs_rename|vfs_link|vfs_symlink|vfs_stat|vfs_readlink" kernel/proc/syscall.c
```

Expected: output includes the syscall cases that allocate path buffers and call VFS operations. Use this as the checklist for Tasks 2 and 3.

---

### Task 2: Add Syscall Path Helpers And Refactor One-Path Operations

**Files:**
- Modify: `kernel/proc/syscall.c`
- Test: `kernel/test/test_process.c`
- Test: full KTEST suite

- [ ] **Step 1: Add local helper types and one-path helpers**

Edit `kernel/proc/syscall.c` near `resolve_user_path_at`. Add these helpers after `resolve_user_path_at` and before `syscall_execve`:

```c
typedef uint32_t (*syscall_resolved_path_op_t)(const char *path);

static uint32_t syscall_with_resolved_path(process_t *cur,
                                           uint32_t user_path,
                                           syscall_resolved_path_op_t op)
{
    char *rpath;
    uint32_t ret;

    if (!cur || !op)
        return (uint32_t)-1;

    rpath = (char *)kmalloc(4096);
    if (!rpath)
        return (uint32_t)-1;

    if (resolve_user_path(cur, user_path, rpath, 4096) != 0) {
        kfree(rpath);
        return (uint32_t)-1;
    }

    ret = op(rpath);
    kfree(rpath);
    return ret;
}

static uint32_t syscall_with_resolved_path_at(process_t *cur,
                                              uint32_t dirfd,
                                              uint32_t user_path,
                                              syscall_resolved_path_op_t op)
{
    char *rpath;
    uint32_t ret;

    if (!cur || !op)
        return (uint32_t)-1;

    rpath = (char *)kmalloc(4096);
    if (!rpath)
        return (uint32_t)-1;

    if (resolve_user_path_at(cur, dirfd, user_path, rpath, 4096) != 0) {
        kfree(rpath);
        return (uint32_t)-1;
    }

    ret = op(rpath);
    kfree(rpath);
    return ret;
}

static uint32_t syscall_vfs_mkdir_op(const char *path)
{
    return (uint32_t)vfs_mkdir(path);
}

static uint32_t syscall_vfs_rmdir_op(const char *path)
{
    return (uint32_t)vfs_rmdir(path);
}

static uint32_t syscall_vfs_unlink_op(const char *path)
{
    return (uint32_t)vfs_unlink(path);
}
```

- [ ] **Step 2: Refactor `mkdir`, `mkdirat`, `rmdir`, `unlink`, and `unlinkat`**

Replace only the repeated allocation/resolve/call/free bodies. Keep syscall-specific flag validation in the syscall case.

Use these final function bodies:

```c
static uint32_t SYSCALL_NOINLINE syscall_case_mkdir(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    process_t *cur = sched_current();

    (void)eax;
    (void)ecx;
    (void)edx;
    (void)esi;
    (void)edi;
    (void)ebp;
    return syscall_with_resolved_path(cur, ebx, syscall_vfs_mkdir_op);
}

static uint32_t SYSCALL_NOINLINE syscall_case_mkdirat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    process_t *cur = sched_current();

    (void)eax;
    (void)edx;
    (void)esi;
    (void)edi;
    (void)ebp;
    return syscall_with_resolved_path_at(cur, ebx, ecx,
                                         syscall_vfs_mkdir_op);
}

static uint32_t SYSCALL_NOINLINE syscall_case_rmdir(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    process_t *cur = sched_current();

    (void)eax;
    (void)ecx;
    (void)edx;
    (void)esi;
    (void)edi;
    (void)ebp;
    return syscall_with_resolved_path(cur, ebx, syscall_vfs_rmdir_op);
}

static uint32_t SYSCALL_NOINLINE syscall_case_unlink(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    process_t *cur = sched_current();

    (void)eax;
    (void)ecx;
    (void)edx;
    (void)esi;
    (void)edi;
    (void)ebp;
    return syscall_with_resolved_path(cur, ebx, syscall_vfs_unlink_op);
}

static uint32_t SYSCALL_NOINLINE syscall_case_unlinkat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    process_t *cur = sched_current();

    (void)eax;
    (void)esi;
    (void)edi;
    (void)ebp;
    if (edx != 0)
        return (uint32_t)-22;
    return syscall_with_resolved_path_at(cur, ebx, ecx,
                                         syscall_vfs_unlink_op);
}
```

If the existing `unlinkat` accepts a specific flag value instead of rejecting all nonzero flags, preserve the existing condition from the current code and only replace the allocation/resolve/call/free block.

- [ ] **Step 3: Compile after the one-path syscall refactor**

Run:

```bash
make kernel
```

Expected: command exits 0.

- [ ] **Step 4: Run behavior tests**

Run:

```bash
make check
```

Expected: command exits 0. Existing process and filesystem syscall behavior remains unchanged.

- [ ] **Step 5: Commit the one-path syscall cleanup**

Run:

```bash
git add kernel/proc/syscall.c
git commit -m "refactor: deduplicate one-path syscall cleanup"
```

Expected: commit succeeds and includes only `kernel/proc/syscall.c`.

---

### Task 3: Refactor Two-Path Syscall Operations

**Files:**
- Modify: `kernel/proc/syscall.c`
- Test: `kernel/test/test_process.c`
- Test: full KTEST suite

- [ ] **Step 1: Add two-path helper types and operations**

Edit `kernel/proc/syscall.c` near the helpers from Task 2. Add:

```c
typedef uint32_t (*syscall_resolved_path2_op_t)(const char *oldpath,
                                                const char *newpath);

static uint32_t syscall_with_two_resolved_paths(process_t *cur,
                                                uint32_t old_user_path,
                                                uint32_t new_user_path,
                                                syscall_resolved_path2_op_t op)
{
    char *oldpath;
    char *newpath;
    uint32_t ret;

    if (!cur || !op)
        return (uint32_t)-1;

    oldpath = (char *)kmalloc(4096);
    if (!oldpath)
        return (uint32_t)-1;

    newpath = (char *)kmalloc(4096);
    if (!newpath) {
        kfree(oldpath);
        return (uint32_t)-1;
    }

    if (resolve_user_path(cur, old_user_path, oldpath, 4096) != 0 ||
        resolve_user_path(cur, new_user_path, newpath, 4096) != 0) {
        kfree(oldpath);
        kfree(newpath);
        return (uint32_t)-1;
    }

    ret = op(oldpath, newpath);
    kfree(oldpath);
    kfree(newpath);
    return ret;
}

static uint32_t syscall_with_two_resolved_paths_at(process_t *cur,
                                                   uint32_t old_dirfd,
                                                   uint32_t old_user_path,
                                                   uint32_t new_dirfd,
                                                   uint32_t new_user_path,
                                                   syscall_resolved_path2_op_t op)
{
    char *oldpath;
    char *newpath;
    uint32_t ret;

    if (!cur || !op)
        return (uint32_t)-1;

    oldpath = (char *)kmalloc(4096);
    if (!oldpath)
        return (uint32_t)-1;

    newpath = (char *)kmalloc(4096);
    if (!newpath) {
        kfree(oldpath);
        return (uint32_t)-1;
    }

    if (resolve_user_path_at(cur, old_dirfd, old_user_path, oldpath, 4096) != 0 ||
        resolve_user_path_at(cur, new_dirfd, new_user_path, newpath, 4096) != 0) {
        kfree(oldpath);
        kfree(newpath);
        return (uint32_t)-1;
    }

    ret = op(oldpath, newpath);
    kfree(oldpath);
    kfree(newpath);
    return ret;
}

static uint32_t syscall_vfs_rename_op(const char *oldpath,
                                      const char *newpath)
{
    return (uint32_t)vfs_rename(oldpath, newpath);
}

static uint32_t syscall_vfs_link_follow_op(const char *oldpath,
                                           const char *newpath)
{
    return (uint32_t)vfs_link(oldpath, newpath, 1u);
}

static uint32_t syscall_vfs_link_nofollow_op(const char *oldpath,
                                             const char *newpath)
{
    return (uint32_t)vfs_link(oldpath, newpath, 0u);
}
```

- [ ] **Step 2: Refactor `rename`, `renameat`, and `linkat`**

Use the helpers from Step 1 while preserving current validation.

For `rename`:

```c
static uint32_t SYSCALL_NOINLINE syscall_case_rename(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    process_t *cur = sched_current();

    (void)eax;
    (void)edx;
    (void)esi;
    (void)edi;
    (void)ebp;
    return syscall_with_two_resolved_paths(cur, ebx, ecx,
                                           syscall_vfs_rename_op);
}
```

For `renameat`:

```c
static uint32_t SYSCALL_NOINLINE syscall_case_renameat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    process_t *cur = sched_current();

    (void)eax;
    (void)edi;
    (void)ebp;
    return syscall_with_two_resolved_paths_at(cur, ebx, ecx, edx, esi,
                                              syscall_vfs_rename_op);
}
```

For `linkat`, keep the flag check and choose the operation by `LINUX_AT_SYMLINK_FOLLOW`:

```c
static uint32_t SYSCALL_NOINLINE syscall_case_linkat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    process_t *cur = sched_current();
    syscall_resolved_path2_op_t op;

    (void)eax;
    (void)ebp;
    if ((edi & ~LINUX_AT_SYMLINK_FOLLOW) != 0)
        return (uint32_t)-22;

    op = (edi & LINUX_AT_SYMLINK_FOLLOW) ?
         syscall_vfs_link_follow_op :
         syscall_vfs_link_nofollow_op;
    return syscall_with_two_resolved_paths_at(cur, ebx, ecx, edx, esi, op);
}
```

- [ ] **Step 3: Leave `symlinkat`, `readlink`, `readlinkat`, `open`, and `openat` alone unless the refactor is exact**

These syscalls include extra target buffers, open flags, fd installation, or user-copy handling. Only refactor them if the final code is shorter and the return conventions remain identical.

For `open` and `openat`, an exact helper is allowed with this shape:

```c
static uint32_t syscall_open_user_path(process_t *cur, uint32_t user_path,
                                       uint32_t flags)
{
    char *rpath;
    int fd;

    if (!cur)
        return (uint32_t)-1;
    rpath = (char *)kmalloc(4096);
    if (!rpath)
        return (uint32_t)-1;
    if (resolve_user_path(cur, user_path, rpath, 4096) != 0) {
        kfree(rpath);
        return (uint32_t)-1;
    }
    fd = syscall_open_resolved_path(cur, rpath, flags);
    kfree(rpath);
    return (uint32_t)fd;
}
```

If adding this helper, also add the `openat` variant using `resolve_user_path_at`.

- [ ] **Step 4: Compile after the two-path syscall refactor**

Run:

```bash
make kernel
```

Expected: command exits 0.

- [ ] **Step 5: Run behavior tests**

Run:

```bash
make check
```

Expected: command exits 0.

- [ ] **Step 6: Inspect syscall return behavior**

Run:

```bash
git diff -- kernel/proc/syscall.c
```

Expected: the diff removes duplicated allocation/free code and does not change syscall case labels, syscall numbers, VFS calls, or existing validation checks.

- [ ] **Step 7: Commit the two-path syscall cleanup**

Run:

```bash
git add kernel/proc/syscall.c
git commit -m "refactor: deduplicate two-path syscall cleanup"
```

Expected: commit succeeds and includes only `kernel/proc/syscall.c`.

---

### Task 4: Add VFS Lookup Context Helpers

**Files:**
- Modify: `kernel/fs/vfs.c`
- Test: `kernel/test/test_vfs.c`
- Test: full KTEST suite

- [ ] **Step 1: Add the lookup context type and cleanup helper**

Edit `kernel/fs/vfs.c` near the `vfs_mount_t` definition. Add:

```c
typedef struct {
    char *norm;
    int mount_idx;
    const vfs_mount_t *mnt;
    const char *rel;
} vfs_lookup_ctx_t;
```

Then add these helpers after `vfs_relpath`:

```c
static void vfs_lookup_ctx_clear(vfs_lookup_ctx_t *ctx)
{
    if (!ctx)
        return;
    if (ctx->norm)
        kfree(ctx->norm);
    ctx->norm = 0;
    ctx->mount_idx = -1;
    ctx->mnt = 0;
    ctx->rel = 0;
}

static int vfs_lookup_ctx_init(vfs_lookup_ctx_t *ctx, const char *path)
{
    if (!ctx)
        return -1;

    ctx->norm = vfs_normalize_alloc(path);
    ctx->mount_idx = -1;
    ctx->mnt = 0;
    ctx->rel = 0;

    if (!ctx->norm)
        return -1;

    ctx->mount_idx = vfs_find_mount_for_path(ctx->norm);
    if (ctx->mount_idx < 0)
        return -1;

    ctx->mnt = &vfs_mounts[ctx->mount_idx];
    ctx->rel = vfs_relpath(ctx->mnt, ctx->norm);
    return 0;
}

static int vfs_mutation_lookup_ctx_init(vfs_lookup_ctx_t *ctx,
                                        const char *path)
{
    if (vfs_lookup_ctx_init(ctx, path) != 0)
        return -1;
    if (ctx->norm[0] == '\0' || vfs_find_mount_exact(ctx->norm) >= 0)
        return -1;
    return 0;
}
```

When using `vfs_lookup_ctx_init`, callers must call `vfs_lookup_ctx_clear` before returning if `ctx.norm` is non-null. If `vfs_lookup_ctx_init` returns `-1`, still call `vfs_lookup_ctx_clear` when the local context may contain `norm`.

- [ ] **Step 2: Refactor `vfs_readlink` with the lookup context**

Replace the body of `vfs_readlink` with:

```c
int vfs_readlink(const char *path, char *buf, uint32_t bufsz)
{
    vfs_lookup_ctx_t ctx;
    int rc;

    ctx.norm = 0;
    if (!buf || bufsz == 0)
        return -22;
    if (vfs_lookup_ctx_init(&ctx, path) != 0) {
        vfs_lookup_ctx_clear(&ctx);
        return -2;
    }
    if (ctx.norm[0] == '\0' || vfs_find_mount_exact(ctx.norm) >= 0) {
        vfs_lookup_ctx_clear(&ctx);
        return -22;
    }
    if (ctx.mnt->kind != VFS_MOUNT_KIND_FS || !ctx.mnt->ops->readlink) {
        vfs_lookup_ctx_clear(&ctx);
        return -22;
    }

    rc = ctx.mnt->ops->readlink(ctx.mnt->ops->ctx, ctx.rel, buf, bufsz);
    vfs_lookup_ctx_clear(&ctx);
    return rc;
}
```

Confirm the old `mount_idx < 0` path returned `-2`, while root/exact mount and unsupported backend returned `-22`.

- [ ] **Step 3: Compile after adding the helper and first VFS conversion**

Run:

```bash
make kernel
```

Expected: command exits 0.

- [ ] **Step 4: Run VFS behavior tests**

Run:

```bash
make check
```

Expected: command exits 0, including `/dev`, `/sys`, `/proc`, root listing, and child mount tests.

- [ ] **Step 5: Commit the VFS lookup helper foundation**

Run:

```bash
git add kernel/fs/vfs.c
git commit -m "refactor: add vfs lookup context helper"
```

Expected: commit succeeds and includes only `kernel/fs/vfs.c`.

---

### Task 5: Refactor Repeated VFS Lookup Callers

**Files:**
- Modify: `kernel/fs/vfs.c`
- Test: `kernel/test/test_vfs.c`
- Test: full KTEST suite

- [ ] **Step 1: Refactor `vfs_stat_common`**

Keep the root and exact-mount fast paths explicit. Use `vfs_lookup_ctx_t` for the mounted path branch.

The final structure should be:

```c
static int vfs_stat_common(const char *path, vfs_stat_t *st, uint32_t follow)
{
    vfs_lookup_ctx_t ctx;
    int rc;

    ctx.norm = 0;
    if (!st)
        return -1;

    if (vfs_lookup_ctx_init(&ctx, path) != 0) {
        if (ctx.norm && ctx.norm[0] == '\0' && vfs_has_mounts()) {
            vfs_dir_stat(st);
            vfs_lookup_ctx_clear(&ctx);
            return 0;
        }
        vfs_lookup_ctx_clear(&ctx);
        return -1;
    }

    if (ctx.norm[0] == '\0' || vfs_find_mount_exact(ctx.norm) >= 0) {
        vfs_dir_stat(st);
        vfs_lookup_ctx_clear(&ctx);
        return 0;
    }

    if (ctx.mnt->kind == VFS_MOUNT_KIND_DEVFS) {
        rc = devfs_stat(ctx.rel, st);
        vfs_lookup_ctx_clear(&ctx);
        return rc;
    }

    if (ctx.mnt->kind == VFS_MOUNT_KIND_PROCFS) {
        rc = procfs_stat(ctx.rel, st);
        vfs_lookup_ctx_clear(&ctx);
        return rc;
    }

    if (ctx.mnt->kind == VFS_MOUNT_KIND_SYSFS) {
        rc = sysfs_stat(ctx.rel, st);
        vfs_lookup_ctx_clear(&ctx);
        return rc;
    }

    if (!follow && ctx.mnt->ops->lstat) {
        rc = ctx.mnt->ops->lstat(ctx.mnt->ops->ctx, ctx.rel, st);
        vfs_lookup_ctx_clear(&ctx);
        return rc;
    }

    if (ctx.mnt->ops->stat) {
        rc = ctx.mnt->ops->stat(ctx.mnt->ops->ctx, ctx.rel, st);
        vfs_lookup_ctx_clear(&ctx);
        return rc;
    }

    {
        uint32_t ino;
        uint32_t sz;

        rc = ctx.mnt->ops->open ?
             ctx.mnt->ops->open(ctx.mnt->ops->ctx, ctx.rel, &ino, &sz) : -1;
        if (rc == 0) {
            vfs_filelike_stat(st);
            st->size = sz;
        }
        vfs_lookup_ctx_clear(&ctx);
        return rc;
    }
}
```

If this exact structure changes an edge case during implementation, preserve the old edge case and keep the lookup context only where it remains behavior-neutral.

- [ ] **Step 2: Refactor `vfs_mutation_mount`**

Replace its manual normalization and mount lookup with `vfs_mutation_lookup_ctx_init`, but keep the existing output contract: `*norm_out` owns the allocated string and must be freed by the caller.

Use this body:

```c
static int vfs_mutation_mount(const char *path, char **norm_out,
                              const vfs_mount_t **mnt_out, const char **rel_out)
{
    vfs_lookup_ctx_t ctx;

    ctx.norm = 0;
    if (!norm_out || !mnt_out || !rel_out)
        return -1;
    if (vfs_mutation_lookup_ctx_init(&ctx, path) != 0) {
        vfs_lookup_ctx_clear(&ctx);
        return -1;
    }

    *norm_out = ctx.norm;
    *mnt_out = ctx.mnt;
    *rel_out = ctx.rel;
    ctx.norm = 0;
    vfs_lookup_ctx_clear(&ctx);
    return 0;
}
```

- [ ] **Step 3: Compile after VFS caller refactors**

Run:

```bash
make kernel
```

Expected: command exits 0.

- [ ] **Step 4: Run VFS behavior tests**

Run:

```bash
make check
```

Expected: command exits 0.

- [ ] **Step 5: Inspect VFS diff for semantic drift**

Run:

```bash
git diff -- kernel/fs/vfs.c
```

Expected: repeated cleanup blocks are reduced. The diff keeps explicit handling for root, exact mounts, `/dev`, `/proc`, and `/sys`.

- [ ] **Step 6: Commit the VFS caller cleanup**

Run:

```bash
git add kernel/fs/vfs.c
git commit -m "refactor: reuse vfs lookup context"
```

Expected: commit succeeds and includes only `kernel/fs/vfs.c`.

---

### Task 6: Replace Exact DUFS Byte Loops

**Files:**
- Modify: `kernel/fs/fs.c`
- Test: `kernel/test/test_fs.c`
- Test: full KTEST suite

- [ ] **Step 1: Replace zeroing loops that are exact memset operations**

In `kernel/fs/fs.c`, replace loops like:

```c
for (uint32_t i = 0; i < DUFS_BLOCK_SIZE; i++) z[i] = 0;
```

with:

```c
k_memset(z, 0, DUFS_BLOCK_SIZE);
```

Also replace:

```c
for (uint32_t i = 0; i < sizeof(dufs_inode_t); i++) p[i] = 0;
```

with:

```c
k_memset(p, 0, sizeof(dufs_inode_t));
```

Only apply this to loops where every byte in a buffer is assigned the same value.

- [ ] **Step 2: Replace DUFS read/write data copy loops**

In `fs_read`, replace:

```c
for (uint32_t i = 0; i < chunk; i++)
    buf[total_read + i] = blk[block_off + i];
```

with:

```c
k_memcpy(buf + total_read, blk + block_off, chunk);
```

In `fs_write`, replace:

```c
for (uint32_t i = 0; i < chunk; i++)
    blk[block_off + i] = buf[written + i];
```

with:

```c
k_memcpy(blk + block_off, buf + written, chunk);
```

- [ ] **Step 3: Leave directory-entry padding loops unchanged**

Do not change loops that copy path components into directory entry names and then pad with NUL bytes, such as:

```c
while (i < DUFS_MAX_NAME - 1 && name[i]) {
    entries[e].name[i] = name[i];
    i++;
}
while (i < DUFS_MAX_NAME) entries[e].name[i++] = '\0';
```

Expected: these loops remain visible in the diff because they encode string truncation and padding behavior.

- [ ] **Step 4: Compile after DUFS copy cleanup**

Run:

```bash
make kernel
```

Expected: command exits 0.

- [ ] **Step 5: Run filesystem behavior tests**

Run:

```bash
make check
```

Expected: command exits 0, including DUFS write/readback and truncate zero-fill tests.

- [ ] **Step 6: Inspect DUFS diff**

Run:

```bash
git diff -- kernel/fs/fs.c
```

Expected: only exact byte-copy and memset-equivalent loops are changed. Directory name padding and path component loops remain unchanged.

- [ ] **Step 7: Commit the DUFS copy cleanup**

Run:

```bash
git add kernel/fs/fs.c
git commit -m "refactor: use memory primitives in dufs"
```

Expected: commit succeeds and includes only `kernel/fs/fs.c`.

---

### Task 7: Final Verification And Review

**Files:**
- Inspect: `kernel/proc/syscall.c`
- Inspect: `kernel/fs/vfs.c`
- Inspect: `kernel/fs/fs.c`
- Test: full KTEST suite

- [ ] **Step 1: Run final compile**

Run:

```bash
make kernel
```

Expected: command exits 0.

- [ ] **Step 2: Run final headless test suite**

Run:

```bash
make check
```

Expected: command exits 0. The KTEST summary reports zero failures.

- [ ] **Step 3: Review final file list**

Run:

```bash
git show --stat --oneline HEAD~3..HEAD
git status --short
```

Expected: the recent commits touch only `kernel/proc/syscall.c`, `kernel/fs/vfs.c`, and `kernel/fs/fs.c`. `git status --short` has no unstaged changes.

- [ ] **Step 4: Check for accidental broad rewrites**

Run:

```bash
git diff HEAD~3..HEAD -- kernel/proc/syscall.c kernel/fs/vfs.c kernel/fs/fs.c
```

Expected:

- Syscall changes remove duplicated buffer allocation/free patterns.
- VFS changes keep explicit root, exact mount, `/dev`, `/proc`, and `/sys` behavior.
- DUFS changes only replace exact copy/zero loops.
- No syscall numbers, public prototypes, filesystem operation tables, or return-code policies changed.

- [ ] **Step 5: Prepare final summary**

Write a summary with:

- The three commits created by Tasks 2, 5, and 6.
- `make kernel` result.
- `make check` result.
- Any intentionally skipped refactor candidates, especially `readlink`, `symlinkat`, `open`, or `openat` if they were left unchanged because their behavior was less mechanical.
