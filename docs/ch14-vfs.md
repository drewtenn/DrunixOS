\newpage

## Chapter 14 — The Virtual Filesystem Switch

### Why the Kernel Uses a VFS Layer

Chapter 13 left us with DUFS v3 fully operational and an inode number standing in as our stable handle for any open file. The code that calls filesystem operations does not call DUFS functions directly. Instead it talks to a narrow indirection layer. That keeps the ELF loader, `start_kernel`, and the rest of the kernel decoupled from DUFS-specific details, and it allows the namespace to be assembled from more than one backend.

The call stack for any file operation flows downward through a fixed set of layers. Each layer knows about exactly the one below it:

![](diagrams/ch14-diag01.svg)

We currently mount DUFS at `/`, a synthetic device filesystem at `/dev`, and a synthetic process-information tree at `/proc`. Path resolution therefore walks a small mount tree rooted at `/`, always choosing the deepest mounted prefix.

### The Ops-Table

The VFS interface is defined as:

```c
typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t link_count;
    uint32_t mtime;
} vfs_stat_t;

typedef struct {
    int (*init)(void);
    int (*open)(const char *path, uint32_t *inode_out, uint32_t *size_out);
    int (*getdents)(const char *path, char *buf, uint32_t bufsz);
    int (*create)(const char *path);
    int (*unlink)(const char *path);
    int (*mkdir)(const char *name);
    int (*rmdir)(const char *name);
    int (*rename)(const char *oldpath, const char *newpath);
    int (*stat)(const char *path, vfs_stat_t *st);
} fs_ops_t;
```

The `mtime` field is a filesystem-provided Unix timestamp in UTC seconds. DUFS fills it from the kernel wall clock.

The important point is that the VFS traffic is inode-oriented, not sector-oriented:

- `open` returns an inode number and a byte size.
- `create` returns the inode number of the created or truncated file.
- `stat` reports inode metadata.
- The VFS never exposes a starting LBA to its callers.

That matches the current DUFS implementation in Chapter 13.

### Registration and Mounting

The registry is still a tiny fixed array, but the namespace now has two separate tables:

```c
#define VFS_MAX_FS     4
#define VFS_MAX_MOUNTS 8
```

Registering a backend copies the filesystem name into the first free registry slot and stores the ops-table pointer beside it. Mounting then resolves the mount's parent directory through the existing namespace, looks up the backend by name, runs its `init()` hook, and records the resulting mount point in the mount table.

Two backends are special: `"devfs"` and `"procfs"` are synthetic and are handled directly by the VFS layer rather than through registered ops-tables. `devfs` contributes nodes like `stdin` and `tty0` under `/dev`; `procfs` synthesises live process and module state under `/proc`.

At boot the sequence is:

1. register DUFS as an available backend,
2. enable interrupts so ATA I/O is safe,
3. mount DUFS at `/`,
4. mount `devfs` at `/dev`,
5. mount `procfs` at `/proc`.

DUFS therefore becomes the root mount only after interrupts are live and ATA reads can complete safely. The synthetic `/dev` and `/proc` subtrees are then layered on top of that root namespace.

### The Public API

The public API is deliberately small:

- `vfs_resolve(path, &node)`
- `vfs_open(path, &inode, &size)`
- `vfs_getdents(path, buf, bufsz)`
- `vfs_create(path)`
- `vfs_unlink(path)`
- `vfs_mkdir(name)`
- `vfs_rmdir(name)`
- `vfs_rename(oldpath, newpath)`
- `vfs_stat(path, st)`
- `vfs_mount(path, name)`

Path resolution is the core service: it normalises the path, finds the deepest matching mount, and reports whether the result is a regular file, a directory, a character device, a TTY node, or a synthetic procfs file. `SYS_OPEN` uses that richer result to install either a DUFS file descriptor, a device-backed one, or a synthetic procfs-backed one.

Mutation helpers first identify the mount that owns the target path. They reject mount points themselves as mutation targets, and renames reject cross-mount moves. Optional operations such as `mkdir`, `rmdir`, `rename`, and `stat` are still checked for `NULL` before the function pointer is called.

### DUFS Through the VFS

The DUFS registration installs this table:

```c
static const fs_ops_t dufs_ops = {
    .init     = fs_init,
    .open     = fs_open,
    .getdents = fs_list,
    .create   = fs_create,
    .unlink   = fs_unlink,
    .mkdir    = fs_mkdir,
    .rmdir    = fs_rmdir,
    .rename   = fs_rename,
    .stat     = fs_stat,
};
```

So the live kernel supports more than lookup and directory listing. Through VFS, user space can create files, delete them, create and remove directories, rename or move entries within a mount, and fetch metadata.

Directory enumeration is where the mount tree shows most clearly. `vfs_getdents` asks the owning backend for the directory's entries, filters out any names shadowed by child mounts, and then appends those child mount points as synthetic directory entries. Listing `/` therefore shows `dev/` and `proc/` even though those names come from the mount table rather than from DUFS itself.

`procfs` also shows why the VFS returns richer node types than just "file or directory". `/proc/<pid>/status`, `/proc/<pid>/vmstat`, `/proc/<pid>/fault`, `/proc/<pid>/maps`, `/proc/<pid>/fd/<n>`, `/proc/modules`, and `/proc/kmsg` are openable files, but they do not have DUFS inode numbers. The syscall layer therefore treats them as synthetic read-only files whose contents are rendered on demand from scheduler, page-table, file-descriptor, module-loader, and kernel-log state.

At the top level, `/proc` enumerates one numeric directory per live PID plus the synthetic `modules` and `kmsg` files. Each `/proc/<pid>/` directory contains `status`, `vmstat`, `fault`, `maps`, and an `fd/` subdirectory. `vmstat` and `fault` provide compact summaries, while `maps` remains the detailed virtual-memory layout view; all three are generated from the same process-memory forensics model, so they stay internally consistent. The VFS does not cache any of that output as on-disk metadata; every open, stat, getdents, and read call asks `procfs` to re-render the current kernel state. That is why `/proc/<pid>/fd/` naturally reflects descriptor duplication, pipes, TTY bindings, and even previously opened procfs files without any separate update path, and why `/proc/kmsg` always reflects the kernel's current retained log buffer rather than a stale snapshot written to disk.

### What the VFS Does Not Do

This VFS is intentionally much smaller than Linux's:

- no per-file descriptor objects inside the VFS
- no **dentry** (directory entry cache — Linux's in-memory index of recently looked-up path components) or inode cache beyond whatever the concrete filesystem provides
- no unmount operation
- no symlinks, bind mounts, or mount options
- no cross-mount rename or move

It is still enough to keep the rest of the kernel from depending on DUFS's on-disk layout.

### Where the Machine Is by the End of Chapter 14

Our file-facing code is now structured around a stable interface instead of a concrete filesystem implementation. `start_kernel`, the ELF loader, and the module loader all talk to `vfs_*` helpers and receive inode-oriented or node-oriented answers. DUFS provides the root filesystem at `/`, while the VFS layer itself synthesises `/dev` and `/proc`; all three are reached through the same path-resolution machinery rather than through backend-specific call sites.
