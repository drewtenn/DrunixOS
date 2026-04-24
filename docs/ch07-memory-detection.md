\newpage

## Chapter 7 — Detecting Memory

### Why We Cannot Assume Anything About RAM

Before we hand out any memory, the kernel must discover which regions of the physical address space are actually RAM and which are reserved for hardware. On a real PC, this is not obvious — and we must find out before we leave real mode.

Chapter 6 left us with SSE enabled and a clean FPU template in hand. Before we can build an allocator, we face a more fundamental problem: the kernel needs to know two things about physical memory before it can hand any of it out — how much there is, and which regions are safe to use. Neither answer can be guessed. The physical address space of a PC is not a clean contiguous block of RAM — it is a patchwork of usable memory, hardware-mapped regions (the VGA buffer, firmware ROM), and legacy holes left over from decades of backward compatibility. Writing to the wrong address can corrupt firmware data, silently fail, or, worst of all, appear to work and then break something unrelated minutes later.

The authoritative source of this information on x86 is a BIOS interface called **E820**, named after the `INT 0x15` function number (`AX = 0xE820`) that retrieves it. E820 returns a list of memory regions, each tagged with a base address, a length, and a type code that identifies whether the region is usable RAM, reserved for hardware, or some other special category.

E820 can only be called from **real mode** — the 16-bit compatibility mode the CPU starts in. Once the CPU has been switched to protected mode (which GRUB does before it hands control to the kernel), the BIOS is gone and E820 can no longer be called. This creates a timing problem: we need E820 data, but we run in protected mode and cannot ask for it.

### How GRUB Solves the Timing Problem

GRUB handles this by calling E820 itself, while it is still in real mode, and embedding the result in the **Multiboot info structure** it passes to the kernel. When GRUB jumps to the kernel's entry point, the `EBX` register holds a pointer to this structure, and one of its fields is a pointer to the memory map that E820 produced.

We receive the Multiboot info pointer as the second argument to `start_kernel` (see Chapter 1) and pass it to the physical memory manager's initialisation function, `pmm_init`. The memory map inside the structure is the foundation the physical memory manager builds its free-page bitmap on top of.

### The Shape of a Multiboot Memory Map

The Multiboot info structure contains two relevant fields for memory detection: `mmap_length` (the byte length of the memory map) and `mmap_addr` (the physical address of the first entry). A flag bit in the `flags` field indicates whether the memory map is actually present — on the rare BIOS that does not support E820, GRUB falls back to a simpler interface and does not produce an mmap.

Each entry in the memory map is a variable-length C struct:

```c
typedef struct {
    uint32_t size;   /* size of this entry, NOT counting this field */
    uint64_t addr;   /* base physical address */
    uint64_t len;    /* length in bytes */
    uint32_t type;   /* 1 = usable RAM, 2 = reserved, ... */
} __attribute__((packed)) multiboot_mmap_entry_t;
```

The `size` field is unusual: it gives the length of the rest of the entry without counting itself. Iterating the map therefore means advancing by `size + 4` bytes each time, not just `size`. This quirk is inherited from the **ACPI** memory-map format that E820 ultimately derives from. ACPI — the **Advanced Configuration and Power Interface** — is an industry standard that defines how platform firmware describes the machine to the operating system and how the OS controls power management, device configuration, and thermal state. The memory-map format used by E820 is one of those firmware-provided tables; GRUB reads the map through the `INT 0x15` interface and copies the entries into the Multiboot info structure unchanged, so we end up seeing ACPI-defined data even though we never talk to ACPI directly.

The `type` field uses the same codes ACPI defines for E820 entries:

| Type | Meaning |
|------|---------|
| 1 | Usable RAM |
| 2 | Reserved — do not use |
| 3 | ACPI reclaimable |
| 4 | ACPI non-volatile storage |
| 5 | Bad memory |

For our purposes, only type 1 is treated as free memory. Everything else is left marked as used, which is the safe choice: even ACPI reclaimable memory holds valid data until we have finished reading the ACPI tables.

### How the Physical Memory Manager Uses the Map

The physical memory manager starts with a conservative default: every page in the bitmap is marked as used. This guarantees that if the memory map is missing or corrupted, we will not accidentally hand out a page that is actually hardware or firmware.

From that safe state, the physical memory manager walks the memory map and, for every entry with type 1, clears the bits for the pages covering that region, making them available for allocation. After all the type-1 entries have been processed, it re-marks several specific regions as reserved again: the kernel image itself (so the allocator does not hand out pages holding executable code), the PMM bitmap (which lives in the kernel's BSS), the VGA/ROM hole, and any other region the kernel uses internally.

The walk advances from one entry to the next by adding `entry->size + 4` to the entry pointer. The `+ 4` is the part that catches everyone the first time, so it is worth spelling out exactly what `size` is counting and what it is not.

The `size` field does not describe the whole entry. It describes everything that comes *after* itself — the `addr`, `len`, and `type` fields, plus any optional trailer the firmware appends. The four bytes that hold `size` are deliberately excluded from the value `size` reports. So if an entry's `size` field reads 20, that entry actually occupies 24 bytes of memory in total: the four-byte `size` header, followed by the 20 bytes of payload that the header is announcing.

Picture one entry laid out in memory:

| Offset | Bytes | Meaning |
|--------|-------|---------|
| `+0` | `4` | `size` header |
| `+4` | `size` bytes | Address, length, type, and any extension fields |
| `+4 + size` | next entry | First byte of the following record |

Walking the map means stepping over the entire entry, header included. From the start of the current entry, the next entry begins `4 + size` bytes later: four bytes to skip past the `size` header itself, plus `size` more bytes to skip past everything the header is counting. Adding only `entry->size` would land four bytes short — inside the current entry's payload — and the loop would happily reinterpret a fragment of the current `addr` field as the next entry's `size`, producing nonsense and almost certainly hanging the kernel on the very next iteration.

Why design the format this way at all? Because it makes the entries self-describing in a way that lets the format grow without breaking older parsers. The memory-map format is deliberately extensible, and some firmware returns entries longer than the 24 bytes the C struct declares, appending ACPI-defined fields such as the 3.0 "extended attributes" word. By placing a length-of-the-rest field at the very front of every entry, ACPI guarantees that any conforming OS — even one written before those extensions existed — can skip cleanly to the next entry by trusting whatever value `size` carries at runtime, instead of hard-coding the layout of the fields it knows about.

This is an easy place to introduce a bug. Writing `sizeof(entry)` looks reasonable but gives four — the size of a pointer on a 32-bit machine — so the walk crawls forward in pointer-sized steps and interprets fragments of the previous entry as the next one. The slightly better-looking `sizeof(*entry)` gives 24, which happens to match QEMU's output exactly and will appear to work on an emulator, but breaks on any real BIOS that returns larger entries. Only `entry->size + 4` is correct in both cases.

If the Multiboot flag for "mmap is present" is clear — meaning GRUB could not produce a memory map — `pmm_init` falls back to assuming that the region from 1 MB to 128 MB is usable RAM. This is the same assumption QEMU's default memory configuration satisfies, so the fallback is enough to boot on any emulator.

### A Realistic Memory Map

On a QEMU machine configured with 128 MB of RAM, the memory map typically looks like this:

| Base | Length | Type | What it is |
|------|--------|------|------------|
| `0x00000000` | `0x0009FC00` | 1 (usable) | Conventional RAM below the 640 KB line |
| `0x0009FC00` | `0x00000400` | 2 (reserved) | Extended BIOS Data Area |
| `0x000F0000` | `0x00010000` | 2 (reserved) | Legacy ROM (Read-Only Memory) region |
| `0x00100000` | `0x07EF0000` | 1 (usable) | Extended RAM from 1 MB to 128 MB |
| `0xFFFC0000` | `0x00040000` | 2 (reserved) | BIOS ROM shadow at the top of the 4 GB space |

The gap between `0x9FC00` and `0x100000` — the roughly 384 KB region containing the BIOS data area, the VGA text buffer, and the ROM shadow — is a legacy hole in the PC address map that cannot be used as general RAM no matter how much physical memory the machine has. We rely on the memory map to know where these holes are, rather than hard-coding assumptions about the PC layout.

### Where the Machine Is by the End of Chapter 7

After `pmm_init` returns, we have a reliable, hardware-provided map of which pages of physical memory are usable. That map is the foundation the rest of the memory management stack in Chapter 8 is built on: the page allocator, the paging system, and the heap all depend on knowing which physical pages are real RAM and which are reserved for something else. With the map in hand, Chapter 8 can turn it into a bitmap of free pages, switch the CPU into paged virtual memory, and layer a heap on top — all of which need the reliable ground truth we have just established.
