\newpage

## Chapter 12 — Block and Character Device Registries

### The Coupling Problem

Think of the device registry like a phone directory: drivers publish their contact info under a short name, consumers look up the name when they need to reach a device. No compile-time dependency — just a runtime lookup.

Chapter 11 left us with an ATA driver that can read and write sectors. Before this chapter, the filesystem read sectors by calling ATA functions directly, and the syscall layer read keystrokes by calling the keyboard's internal function. These direct dependencies meant the filesystem and syscall layer had compile-time knowledge of both the keyboard driver and the ATA driver. Adding a second disk drive or a USB keyboard would have required modifying the filesystem and syscall code — the wrong place to touch for a hardware change.

The device registries decouple these relationships. A driver publishes a small ops-table under a short name. Consumers look up the device by name and call through the function pointer. Neither side needs to know the other's implementation details at compile time.

### Block Devices

A block device is one that transfers data in fixed-size chunks — on this system, 512-byte sectors. The block device registry provides a central place to publish and look up block devices, and it also carries enough metadata about each entry that higher layers can enumerate the storage topology without reaching into driver internals.

The ops-table for a block device is a pair of function pointers:

```c
typedef struct {
    int (*read_sector)(uint32_t lba, uint8_t *buf);
    int (*write_sector)(uint32_t lba, const uint8_t *buf);
} blkdev_ops_t;
```

Both functions take an **LBA** (Logical Block Address) — the zero-based sector number — and a pointer to a 512-byte buffer. They return 0 on success and non-zero on error.

Alongside the ops-table, each registry entry carries a metadata record:

```c
typedef struct {
    char     name[BLKDEV_NAME_MAX];
    uint32_t kind;
    uint32_t sector_size;
    uint32_t sectors;
    uint32_t readonly;
    uint32_t major;
    uint32_t minor;
    uint32_t parent_index;
    uint32_t start_sector;
    uint32_t partition_number;
} blkdev_info_t;
```

The `kind` field distinguishes a **whole disk** from a **partition**. A whole disk is a device the driver brought up on its own — the primary master ATA drive, for example. A partition is a slice of a whole disk described by a partition table written into that disk's first sector. Both kinds live side by side in the same registry: consumers look them up by name, and each one has its own ops-table pointer.

The registry holds up to sixteen entries. Each slot pairs one `blkdev_info_t` record with one `blkdev_ops_t` pointer. Slots are allocated by linear scan; a slot is free when its name's first byte is zero. Names are up to eleven characters plus a null terminator, which leaves room for sd-style disk and partition names like `sda` and `sda1`.

A typical boot populates the registry like this:

| Slot | Name | Kind | Major / minor | Parent / start LBA |
|------|------|------|---------------|--------------------|
| `0` | `sda` | disk | 8 / 0 | none |
| `1` | `sdb` | disk | 8 / 16 | none |
| `2` | `sda1` | partition | 8 / 1 | `sda` / start taken from MBR |
| `3` | `sdb1` | partition | 8 / 17 | `sdb` / start taken from MBR |

We follow Linux's naming conventions for block devices. The ATA driver registers its master drive as `"sda"` and its slave as `"sdb"`, both with major number 8 (the Linux `sd` major) and minors 0 and 16 respectively. Partitions under each disk take the next minors in the same minor range, so `sda1` has minor 1 and `sdb1` has minor 17. This matches what Linux-aware userland tools expect when they read device numbers out of the registry.

### Registering a Disk

`blkdev_register_disk` claims the next free slot for a whole disk. It records the name, the major and minor numbers, the total sector count, and the ops-table pointer. A disk entry has `kind = BLKDEV_KIND_DISK`, no parent, and a starting LBA of zero.

The ATA driver calls this twice during initialisation: once for the master drive as `sda` and once for the slave as `sdb`. After that, any consumer can resolve either name through `blkdev_get("sda")` and call `read_sector` or `write_sector` without compile-time knowledge of the ATA driver.

### Discovering Partitions From the MBR

The **MBR** (Master Boot Record) is the first 512-byte sector of an IBM-PC disk. It holds a boot loader in its early bytes and a partition table in its tail. The partition table is a fixed array of four 16-byte records describing primary partitions, ending with the two magic bytes `0x55` and `0xAA` that mark a valid MBR.

Each record gives a partition type, a starting LBA, and a sector count. A zero type means "this slot is empty". Types `0x05` and `0x0F` mean "extended partition, look elsewhere" — we skip those because extended partitions require walking a chain of partition tables that we do not support yet.

`blkdev_scan_mbr(disk_index)` runs once per whole disk at boot. It reads sector 0 through the disk's own ops-table, checks the magic bytes, and for every non-empty primary record it registers a partition entry.

Partition names are formed by concatenating the disk name and the slot number: slot 1 under `sda` becomes `sda1`, slot 2 becomes `sda2`, and so on.

A partition entry has `kind = BLKDEV_KIND_PART`, its `parent_index` pointing at the disk's slot, and a `start_sector` equal to the partition's starting LBA within the disk.

### Partition-Backed Ops

A partition is a view into part of its parent disk. Reading sector 0 of `sda1` must read sector `start_sector` of `sda`, not sector 0 of `sda`. The registry handles this with a per-slot set of partition trampolines that translate an incoming LBA by adding the partition's start sector and then dispatch to the parent disk's ops.

Each partition slot gets its own static pair of trampoline functions — one for reads and one for writes. The trampoline looks up its own info record, resolves the parent's ops pointer, adds `start_sector` to the caller's LBA, and calls through to the parent. Because the ops-table pointer is per-slot, a registry consumer can open `sda1` and never know or care that the bytes ultimately come from `sda`. A bounds check inside the trampoline keeps a stray read on `sda1` from spilling over into the next partition.

### Enumeration

Higher layers need to present the storage topology to user space, so the registry also exposes a small enumeration API. `blkdev_count()` returns the number of occupied slots, `blkdev_info_at(index, out)` copies the `blkdev_info_t` for the *i*-th occupied slot in the table, and `blkdev_find_index(name)` resolves a name back to its slot index.

Two consumers use these helpers today. The synthetic `/sys/block` tree reads the registry and builds a directory per disk, a subdirectory per partition, and small text files that expose each device's size, major/minor numbers, and partition start. The userland `lsblk` utility parses that tree and prints it as a Linux-style table. Neither of them has compile-time knowledge of how many disks exist, which is exactly the property the registry was introduced to provide.

### Character Devices

A character device is one that transfers data one byte at a time, typically an interactive input or output stream. The character device registry uses the same pattern as the block device registry but with a simpler ops-table:

```c
typedef struct {
    char (*read_char)(void);   /* returns 0 if no data is available */
    void (*write_char)(char c); /* may be NULL for input-only devices */
} chardev_ops_t;
```

The registry holds up to four entries and is queried the same way as the block-device table: publish a name/ops pair at startup, then resolve that name later when some other subsystem wants terminal-style byte I/O.

Our keyboard driver publishes the same character source under two names:

- `"stdin"` — used by `SYS_READ` when the calling process reads from file descriptor 0.
- `"tty0"` — the conventional name for the first terminal device, available for any future code that wants to write to or read from the system console.

When the syscall dispatcher handles `SYS_READ` on file descriptor 0, it resolves the `"stdin"` ops-table and polls `read_char()` in a tight loop until it returns a non-zero byte. Each iteration emits a `pause` instruction — an x86 hint that marks this as a deliberate spin-wait, reducing power consumption and avoiding a false memory-ordering hazard on hyperthreaded designs. A future improvement would transition the process to a blocking sleep state so the CPU is freed entirely while waiting; the current implementation is correct but wastes cycles during the wait.

### Why Function Pointers Instead of Compile-Time Includes

Both registries use runtime function pointers rather than compile-time `#include` dependencies. The tradeoff is worth it:

- **Adding a new disk** requires only registering under a new name. No existing source file changes.
- **Swapping the keyboard** for a USB HID (Human Interface Device) requires only registering the new driver under `"stdin"`. The syscall layer is unchanged.
- **Discovering partitions** at boot is a matter of reading one sector per disk and calling the same registration path the ATA driver used for the disk itself.
- **Testing** becomes possible without hardware: a test harness could register a mock block device that returns known sector data without touching any disk.

The cost is a single pointer indirection on each read. At the scale of a 512-byte sector read or a keyboard poll, this is completely invisible.

### Where the Machine Is by the End of Chapter 12

Every device in the kernel is now reached through a name and an ops-table — disks and partitions through the block registry, the keyboard through the character registry — so any future driver can plug in without touching the filesystem, the syscall layer, or each other.
