# Linux-Style Lsblk Design

Date: 2026-04-18

## Context

Drunix currently exposes two ATA disks through a small block-device registry.
The registry supports lookup by names such as `hd0` and `hd1`, but it does not
enumerate devices, describe device size, model partitions, or expose block
metadata to userland. The default ext3 root image and the DUFS secondary image
are raw filesystem images rather than partitioned disks.

Linux users normally reach for `lsblk` to list block devices, partitions, and
mountpoints. `lsblk -f` adds filesystem-oriented columns such as filesystem
type, label, UUID, and mountpoint. Drunix should mimic that shape closely enough
that the command feels familiar and leaves room for future Linux-compatible
storage tools.

## Goals

- Add Linux-style public block names: `sda`, `sda1`, `sdb`, and `sdb1`.
- Convert normal `disk.img` and `dufs.img` builds into Master Boot Record (MBR)
  disk images with one primary partition each.
- Mount the default ext3 root filesystem from `sda1`.
- Mount the DUFS secondary filesystem from `sdb1` at `/dufs` during ext3-root
  boots.
- Expose block devices under `/dev`, including `/dev/sda`, `/dev/sda1`,
  `/dev/sdb`, and `/dev/sdb1`.
- Add a minimal `/sys/block` tree that userland can inspect.
- Update `/proc/mounts` so it reports the mounted block devices and synthetic
  filesystems accurately.
- Add a userland `lsblk` command with Linux-like default output and `-f`
  filesystem output.

## Non-Goals

- Full Linux sysfs compatibility.
- GPT partition tables, extended MBR partitions, logical partitions, removable
  media, hotplug, udev, or persistent `/dev/disk/by-*` links.
- Full raw block-device read/write support from userland in the first pass.
- A complete util-linux option set for `lsblk`.
- DUFS labels or UUIDs unless DUFS gains those fields separately.

## Chosen Approach

The selected approach is a minimal Linux-like `sysfs` surface instead of a
Drunix-only procfs file or a syscall-only API.

The kernel remains the source of truth for block topology. ATA registers whole
disk devices. A partition scanner registers partition children. A synthetic
`sysfs` mount exposes stable text files under `/sys/block`, and `lsblk` reads
those files plus `/proc/mounts` to format its tables.

This gives Drunix a familiar discovery path without committing to all of
Linux's storage stack at once. It also keeps the metadata visible to shell users
and future tools such as `blkid`, `mount`, and partition inspectors.

## Block Layer

Extend the block-device registry from name lookup into an enumerable table of
devices. Each record should describe:

- public name, such as `sda` or `sda1`,
- device kind, either `disk` or `part`,
- sector size,
- sector count,
- readonly flag,
- Linux-style major and minor numbers,
- parent index for partitions,
- start sector for partitions,
- read and write operations.

ATA should register the primary master as `sda` and the primary slave as `sdb`.
The registry should scan each disk's first sector for an MBR signature. For each
valid primary partition, it should register a child partition such as `sda1`.
The first implementation only needs primary partition entries. Unsupported or
empty partition slots are skipped.

Partition devices wrap their parent disk. A read or write to partition LBA `N`
becomes a parent operation at `partition_start + N`, and the wrapper rejects
operations that would exceed the partition's sector count.

Whole disks should still appear even if no valid MBR exists. That keeps the
system debuggable when a disk is blank or corrupt.

## Disk Images

Normal image builds should produce real MBR disk images.

`disk.img` contains:

- LBA 0: MBR with one primary partition entry.
- `sda1`: ext3 by default, or DUFS when built with `ROOT_FS=dufs`.

`dufs.img` contains:

- LBA 0: MBR with one primary partition entry.
- `sdb1`: DUFS.

The existing filesystem builders can continue to produce filesystem payloads,
but the Makefile or helper scripts must wrap those payloads at a partition
offset and write the matching MBR. Host-side ext3 validation must inspect the
partition payload rather than treating the entire disk image as ext3.

## Mount Flow

Boot storage setup should follow this order:

1. ATA registers `sda` and `sdb`.
2. The block layer scans MBRs and registers `sda1` and `sdb1`.
3. Filesystem drivers register their VFS backends.
4. The root filesystem mounts from `sda1`.
5. During ext3-root boots, DUFS mounts from `sdb1` at `/dufs`.
6. Synthetic filesystems mount at `/dev`, `/proc`, and `/sys`.

The `ROOT_FS=dufs` mode should mount DUFS from `sda1` as `/`. Secondary DUFS
scratch behavior should follow the current boot policy: do not mount `/dufs`
when DUFS is already the root filesystem. Normal ext3-root boots must expose
`sdb1` at `/dufs`.

## `/dev`

Extend devfs with block-device nodes:

- `/dev/sda`
- `/dev/sda1`
- `/dev/sdb`
- `/dev/sdb1`

The nodes should resolve as block devices even if userland cannot yet open them
for arbitrary sector I/O. This keeps path naming Linux-like and gives later
tools a real namespace to build on.

## `/sys/block`

Add a synthetic `sysfs` backend and mount it at `/sys`. The first tree is small
and block-focused:

```text
/sys/block/sda/
/sys/block/sda/size
/sys/block/sda/dev
/sys/block/sda/type
/sys/block/sda/sda1/
/sys/block/sda/sda1/size
/sys/block/sda/sda1/dev
/sys/block/sda/sda1/type
/sys/block/sda/sda1/partition
/sys/block/sda/sda1/start
```

Repeat the same shape for `sdb` and `sdb1`.

`size` should report 512-byte sectors, matching Linux sysfs block-device
convention. `dev` should report `major:minor`. `type` should report `disk` or
`part`. `partition` should report the partition number for partition devices.
`start` should report the start sector relative to the parent disk.

The sysfs implementation should be explicit about its narrow scope. It should
not imply that unrelated Linux sysfs paths exist.

## `/proc/mounts`

`/proc/mounts` should describe the real mounted sources rather than hard-coded
rows. In a normal ext3-root boot it should include:

```text
/dev/sda1 / ext3 rw 0 0
/dev/sdb1 /dufs dufs rw 0 0
devfs /dev devfs rw,nosuid 0 0
proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0
sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0
```

`lsblk` should use this file only for mountpoint association. Device topology
still comes from `/sys/block`.

## Filesystem Detection

`lsblk -f` needs filesystem type and optional UUID data.

The first implementation can detect filesystems by reading the device:

- ext3: read the ext-family superblock and recognize the ext magic; read UUID
  from the superblock.
- DUFS: read the DUFS superblock and recognize the DUFS magic.

If probing fails or the filesystem is unknown, `FSTYPE`, `LABEL`, and `UUID`
remain blank. DUFS leaves `LABEL` and `UUID` blank until the on-disk format
grows those concepts.

## `lsblk`

Add a userland `lsblk` program and include it in the disk image.

Plain `lsblk` should print:

```text
NAME    MAJ:MIN RM  SIZE RO TYPE MOUNTPOINTS
sda       8:0    0   50M  0 disk
`-sda1    8:1    0   49M  0 part /
sdb       8:16   0   50M  0 disk
`-sdb1    8:17   0   49M  0 part /dufs
```

`lsblk -f` should print:

```text
NAME    FSTYPE LABEL UUID                             MOUNTPOINTS
sda
`-sda1  ext3         4452554e-4958-4558-3352-4f4f54303031 /
sdb
`-sdb1  dufs                                           /dufs
```

The exact spacing may vary, but columns should be stable, readable, and
Linux-like. The initial supported options are:

- `lsblk`
- `lsblk -f`
- `lsblk --help`

Unsupported options should print usage and exit nonzero.

## Error Handling

If `/sys/block` is missing, `lsblk` should print a clear error and exit
nonzero.

If `/proc/mounts` is missing, `lsblk` should still list devices and leave
mountpoints blank.

If a disk has an invalid MBR, the disk remains visible as a whole-disk device
with no partition children.

If a partition entry points outside the parent disk, the scanner should skip
that partition and log the reason.

If root mounting from `sda1` fails, the boot should fail loudly as it does today
for root mount failures. Silent fallback to raw whole-disk mounting would hide
partition bugs in the default boot path.

## Testing

Kernel tests should cover:

- block-device enumeration,
- MBR parsing for valid, empty, invalid-signature, and out-of-range entries,
- partition read/write offset translation and bounds checks,
- sysfs path resolution, directory listing, file sizes, and reads,
- devfs block-node resolution,
- root and DUFS mount selection by partition name.

Userland or integration tests should verify:

- `lsblk` lists `sda`, `sda1`, `sdb`, and `sdb1`,
- `lsblk` shows `/` for `sda1` and `/dufs` for `sdb1` in normal ext3-root boots,
- `lsblk -f` shows `ext3` for `sda1`,
- `lsblk -f` shows `dufs` for `sdb1`,
- unsupported options fail with usage,
- host ext3 validation succeeds against the partition payload.

The main boot verification should continue to use the existing headless test
flow where possible, with focused host-side image checks for MBR layout and
partition offsets.

## Scope Guardrails

Keep this work focused on block topology, partitioned images, the `/sys/block`
surface, `/dev` block names, filesystem mounting by partition device, and the
first `lsblk` utility.

Do not introduce GPT, dynamic device management, full sysfs, or a general raw
block I/O syscall unless implementation reveals that a smaller internal helper
cannot support `lsblk` and mount flow correctly.
