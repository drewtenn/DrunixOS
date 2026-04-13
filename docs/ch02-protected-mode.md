\newpage

## Chapter 2 — The GDT and Protected Mode

### What the CPU Needs Before It Can Behave Like a Modern Processor

Chapter 1 ended with GRUB having placed the CPU in protected mode — the 32-bit operating mode where the full 4 GB address space is visible. That sounds like the hard work is done, but protected mode is not a single state. It runs on top of a mandatory data structure, and if that structure is missing or malformed the CPU will fault the moment it tries to execute anything.

The structure is called the **Global Descriptor Table**, abbreviated **GDT**. GRUB installs a minimal one before jumping to our kernel — just enough for it to function while loading the kernel image — but that table belongs to GRUB's memory and is not guaranteed to remain valid. So the very first thing we do in `start_kernel` is build a permanent GDT in kernel memory and load it. This chapter explains what the GDT is, what goes in it, and how we take ownership of the CPU mode.

### Why Protected Mode Requires a Descriptor Table

Before protected mode, the CPU operates in **real mode**, a backwards-compatibility mode modelled on the Intel 8086. Two fundamental defects make real mode unsuitable for any serious operating system.

First, registers are only 16 bits wide. A single register can hold a value between 0 and 65,535, so pointing at anything further away requires combining two registers using awkward segment arithmetic. The effective address is `(segment register × 16) + offset`, limiting the total addressable space to one megabyte.

Second, there is no memory protection whatsoever. Any instruction can read or write any byte of memory — including the BIOS data area, the kernel code, or a neighbour process's private data. There is no mechanism to isolate a buggy or malicious program.

Protected mode fixes both problems: registers are 32 bits wide (enabling a 4 GB address space), and the hardware can enforce access rules. But "the hardware can enforce rules" requires telling the hardware what those rules are. That is exactly what the GDT does — it is the table we hand to the CPU to describe how memory is partitioned and who is allowed to touch what.

### What a GDT Entry Actually Is

A GDT is an array in RAM. Each eight-byte entry is called a **segment descriptor** and describes one region of memory: where it starts, how large it is, and what operations are permitted inside it (read, write, execute) and at what privilege level.

The eight-byte layout has an unfortunate history. The original Intel 80286 used a six-byte format with a 24-bit base address and a 16-bit limit. When the 80386 extended the processor to 32 bits, Intel added the missing bits to the two bytes at the end rather than redesigning the format. The result is that the base address and limit are each split across two non-contiguous regions of the descriptor, and any OS targeting x86 — including ours — must pack and unpack them through this awkward layout:

| Field | Bytes | Meaning |
|-------|-------|---------|
| Limit 0-15 | `0-1` | Low 16 bits of segment limit |
| Base 0-15 | `2-3` | Low 16 bits of base address |
| Base 16-23 | `4` | Base address bits 16-23 |
| Access | `5` | Presence, privilege, and type flags |
| Flags and limit high | `6` | Granularity flags and limit bits 16-19 |
| Base 24-31 | `7` | Base address bits 24-31 |

When code loads a **segment selector** into one of the CPU's segment registers — `CS` for the current code segment, `DS`/`ES`/`FS`/`GS` for data, and `SS` for the stack — the CPU uses that selector as an index into the GDT and reads the corresponding descriptor to determine what the register means. Every memory access is then validated against the permissions in that descriptor.

A selector is a 16-bit value with three sub-fields:

| Bits | Field | Meaning |
|------|-------|---------|
| `0-1` | RPL | Requested privilege level |
| `2` | TI | Global or local table |
| `3-15` | Index | Which descriptor slot to use |

The **RPL** (Requested Privilege Level) field encodes the privilege level at which the memory access is being requested. The **TI** (Table Indicator) bit selects between the global GDT and a per-process **LDT** (Local Descriptor Table); we use only the GDT in this project.

We tell the CPU where the GDT lives in memory through a special register called **GDTR**. We load it with the `lgdt` instruction, which takes a pointer to a six-byte structure giving the table's base address and limit:

| Field | Bytes | Meaning |
|-------|-------|---------|
| Limit | `0-1` | Table size minus one |
| Base address | `2-5` | Physical address of first entry |

The CPU copies these two fields into GDTR when `lgdt` executes.

### The Three Descriptors in the Boot GDT

Our initial GDT has exactly three entries. Each descriptor carries a **DPL** (Descriptor Privilege Level) — a two-bit value that controls which CPU ring level may load the selector. Ring 0 is the most privileged level and is where kernel code runs; ring 3 is where user programs run.

The first entry is required by the CPU specification to be a **null descriptor** — eight zero bytes. Loading a null selector into a segment register is permitted, but using that register to address memory causes the CPU to raise a fault. This acts as a safety net: if a bug leaves a segment register uninitialised, the next memory access crashes loudly and visibly rather than silently reading garbage.

The second entry is the **code segment descriptor**. It covers the entire 4 GB address space starting at address zero and is marked executable and readable at ring 0. The flat, full-address-space coverage is an intentional design choice called a **flat memory model**: segmentation is neutralised entirely, and every address the kernel produces is passed through to the hardware unchanged. All memory protection in this project is instead handled by paging (Chapter 8).

The third entry is the **data segment descriptor**. It too covers the full 4 GB range, but is marked as a writable data segment rather than an executable code segment, also at ring 0. We load this selector into `DS`, `SS`, `ES`, `FS`, and `GS`.

| Offset | Selector | DPL | Type | Base         | Limit | Purpose                                   |
|--------|----------|-----|------|--------------|-------|-------------------------------------------|
| `0x00` | `0x00`   | —   | Null | —            | —     | Required trap for uninitialised selectors |
| `0x08` | `0x08`   | 0   | Code | `0x00000000` | 4 GB  | Kernel code segment                       |
| `0x10` | `0x10`   | 0   | Data | `0x00000000` | 4 GB  | Kernel data and stack segment             |

The byte offset of each descriptor within the table *is* its selector value: the null descriptor is at offset 0, the code descriptor at offset 8 (`0x08`), and the data descriptor at offset 16 (`0x10`). These two constants — `0x08` and `0x10` — appear throughout the kernel wherever segment registers are set.

This three-entry table is a temporary scaffold. Chapter 15 describes how we expand it to six entries by adding ring-3 user code and data segments and a **TSS** (Task State Segment, a hardware structure needed for privilege-level transitions). The kernel and data selectors deliberately remain at `0x08` and `0x10` after that expansion, so the interrupt handlers installed in Chapter 4, which hard-code `0x10` when reloading `DS` after an interrupt, continue working without modification.

### Decoding the Permission Bits

Each descriptor carries an **access byte** that the CPU checks on every memory reference. Its eight bits encode:

| Bits | Field | Meaning |
|------|-------|---------|
| `0` | Accessed | Set by the CPU on use |
| `1` | Readable | Read or write permitted |
| `2` | Conforming | Privilege-transition rule |
| `3` | Executable | CPU may fetch instructions |
| `4` | S | Code or data segment |
| `5-6` | DPL | Privilege level |
| `7` | Present | Entry is valid |

The code segment's access byte is `10011010`:

- **Present = 1** — the descriptor is valid.
- **DPL = 00** — ring 0, kernel privilege only.
- **S = 1** — this is a code or data segment (as opposed to a system descriptor like a TSS or gate).
- **Executable = 1** — the CPU may fetch instructions from this segment.
- **Conforming = 0** — the segment does not allow lower-privilege code to call into it without a privilege transition.
- **Readable = 1** — the segment may also be read as data, which is necessary when the compiler places constants adjacent to code in the `.rodata` section.

The data segment's access byte is `10010010` — identical except the executable bit is cleared and the readable bit is set to writable.

### How the Kernel Loads Its Own GDT

The sequence from the moment `start_kernel` begins running through the moment the new GDT is live unfolds in three steps.

First, the kernel fills in the three descriptor entries. Each descriptor's base and limit are packed into the non-contiguous field layout described earlier, so the flat values have to be split across the correct byte positions inside each eight-byte `gdt_entry_t`.

Next, the kernel stores the base address and limit of the completed table into the six-byte format `lgdt` expects.

Finally, the kernel executes `lgdt` to load the new table's address into GDTR. Loading GDTR alone is not enough: the CPU's segment register cache (sometimes called "shadow registers") still holds the descriptor values from GRUB's old GDT. To force the CPU to re-read all the descriptors from the new table, the startup code performs a **far jump** — a jump instruction that explicitly includes a segment selector — using selector `0x08`. A far jump unconditionally reloads `CS` and flushes the instruction pipeline, guaranteeing that all subsequent instruction fetches consult the new GDT. After the jump, the kernel loads `0x10` into every data segment register before returning.

The far jump is the critical step. Without it, `CS` would still reference whatever GRUB's GDT said, and our new GDT's code descriptor would never take effect.

### Where the Machine Is by the End of Chapter 2

At this point in `start_kernel`, before any other subsystem is initialised:

- The CPU is in 32-bit protected mode, and our kernel's own GDT is loaded and active.
- The GDT contains a null descriptor, a ring-0 code descriptor, and a ring-0 data descriptor — all covering the full 4 GB address range.
- `CS` holds `0x08`; `DS`, `SS`, `ES`, `FS`, and `GS` all hold `0x10`.
- Interrupts are still disabled — the **IDT** (Interrupt Descriptor Table, the structure that maps interrupt numbers to handler addresses) has not been installed yet.
- All memory protection is currently handled by segmentation alone, which is effectively a no-op because both descriptors span the entire address space. Paging, which provides real isolation, is not enabled until Chapter 8.

We now own the descriptor table. The CPU is running under our rules, not GRUB's. Chapter 3 covers the kernel entry code in more detail and follows `start_kernel` as it begins initialising each subsystem in turn.
