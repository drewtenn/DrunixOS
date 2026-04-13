\newpage

## Chapter 12 — Block and Character Device Registries

### The Coupling Problem

Chapter 11 left us with an ATA driver that can read and write sectors. Before this chapter, the filesystem read sectors by calling ATA functions directly, and the syscall layer read keystrokes by calling the keyboard's internal function. These direct dependencies meant the filesystem and syscall layer had compile-time knowledge of both the keyboard driver and the ATA driver. Adding a second disk drive or a USB keyboard would have required modifying the filesystem and syscall code — the wrong place to touch for a hardware change.

The device registries decouple these relationships. A driver publishes a small ops-table under a short name. Consumers look up the device by name and call through the function pointer. Neither side needs to know the other's implementation details at compile time.

### Block Devices

A block device is one that transfers data in fixed-size chunks — on this system, 512-byte sectors. The block device registry provides a central place to publish and look up block devices.

The ops-table for a block device is a pair of function pointers:

```c
typedef struct {
    int (*read_sector)(uint32_t lba, uint8_t *buf);
    int (*write_sector)(uint32_t lba, const uint8_t *buf);
} blkdev_ops_t;
```

Both functions take an **LBA** (Logical Block Address) — the zero-based sector number — and a pointer to a 512-byte buffer. They return 0 on success and non-zero on error.

The registry holds up to four entries. Each entry is a name (up to seven characters) paired with a pointer to a `blkdev_ops_t`. The table is static and zero-initialised; a slot is free when its name's first byte is zero.

| Slot | Name | Purpose |
|------|------|---------|
| `0` | `hd0` | Active disk entry |
| `1` | free | Empty slot for a future device |
| `2` | free | Empty slot for a future device |
| `3` | free | Empty slot for a future device |

Registering a block device means finding the first free slot and filling it. Looking one up means doing a linear scan and returning the matching ops-table pointer, or NULL if no device with that name has been registered.

Our ATA driver publishes itself under the name `"hd0"` once initialisation succeeds. After that, the filesystem layer resolves `"hd0"` once during its own startup and caches the resulting ops-table pointer for all subsequent reads.

A file handle in the process open-file table also stores a pointer to the block device ops-table (resolved at `SYS_OPEN` time). When the syscall layer handles a `SYS_READ` on a file descriptor, it calls through the ops-table rather than any named function from the ATA driver.

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

- **Adding a new disk** requires only registering under a new name (e.g., `"hd1"`). No existing source file changes.
- **Swapping the keyboard** for a USB HID (Human Interface Device) requires only registering the new driver under `"stdin"`. The syscall layer is unchanged.
- **Testing** becomes possible without hardware: a test harness could register a mock block device that returns known sector data without touching any disk.

The cost is a single pointer indirection on each read. At the scale of a 512-byte sector read or a keyboard poll, this is completely invisible.

### Where the Machine Is by the End of Chapter 12

We now route all disk I/O through the block device registry and all character I/O through the character device registry. The ATA driver is `"hd0"` in one table; the keyboard is `"stdin"` and `"tty0"` in the other. No code outside each driver reaches its internal functions directly. The coupling between subsystems has been replaced by a narrow, named interface that any future driver can implement.
