\newpage

## Chapter 11 — ATA Disk Driver

### Why We Need to Talk to Disks Directly

Up to this point every byte the kernel has touched has been in RAM. The kernel image itself was loaded by GRUB, and once GRUB was finished, the BIOS disk services it used went away — the BIOS is a 16-bit real-mode program, and the CPU has been in 32-bit protected mode since Chapter 2. If we want to read or write persistent data (a filesystem, user programs, configuration files), we have to speak directly to the disk controller.

The disk interface on every x86 PC made before **SATA** (Serial ATA, a newer point-to-point successor that replaced the parallel bus with a serial link) became common is called **ATA** — AT Attachment, named after the IBM PC/AT. The underlying protocol is also sometimes called IDE (Integrated Drive Electronics) or PATA (Parallel ATA). QEMU's default virtual hard drive presents itself as an ATA drive on the standard controller addresses, which makes writing a driver for it the natural next step.

Block storage is a portable concept — every architecture needs a way to read and write persistent sectors, and the block layer interface the filesystem uses looks the same regardless of what controller sits underneath. On a PC the controller is ATA; on ARM-based single-board computers the standard alternative is **SDHCI/EMMC** (SD Host Controller Interface / embedded MultiMediaCard — the controller and embedded-flash standard the Raspberry Pi 3, for example, uses for its boot media).

*On AArch64 (planned, milestone 5): SDHCI/EMMC in place of the PC ATA controller; the block layer interface is unchanged.*

### Two Ways to Move Data: PIO and DMA

There are two distinct strategies for transferring data between an ATA drive and RAM. PIO is like the kernel carrying each word from disk to RAM by hand, one at a time — slow but simple. DMA is like setting up an automatic conveyor belt that moves data while the kernel does other work.

**PIO** (Programmed I/O) is the simpler of the two. The CPU issues a command to the drive, waits for the drive to say "data ready", and then reads or writes the 512 bytes of a sector one word at a time through an I/O port. Every byte passes through the CPU. It is slow — the CPU does nothing else during the transfer — but it requires no configuration of any other hardware.

**DMA** (Direct Memory Access) is the faster strategy. A separate chip called a DMA controller is programmed with a source, a destination, and a length, and it moves data between the drive and RAM without the CPU's involvement. The CPU is free to run other code during the transfer. DMA is mandatory for any serious OS but requires additional setup: the DMA controller has to be configured, the buffer has to be in memory the controller can reach, and an interrupt has to be wired up to notify the CPU when the transfer is complete.

We use **PIO** because simplicity wins at this stage. Every sector is transferred by the CPU one word at a time. Moving to DMA is a future enhancement.

### The ATA Register File

The primary ATA controller is controlled through two blocks of I/O ports. The main block starts at port `0x1F0`; the control block is at port `0x3F6`.

| Port | Read | Write |
|------|------|-------|
| `0x1F0` | Data (16-bit word) | Data (16-bit word) |
| `0x1F1` | Error register | Features register |
| `0x1F2` | Sector count | Sector count |
| `0x1F3` | LBA bits 0–7 | LBA bits 0–7 |
| `0x1F4` | LBA bits 8–15 | LBA bits 8–15 |
| `0x1F5` | LBA bits 16–23 | LBA bits 16–23 |
| `0x1F6` | Drive/head | Drive/head |
| `0x1F7` | Status | Command |
| `0x3F6` | Alternate status | Device control |

The data port (`0x1F0`) is the only one that carries the actual contents of a sector. Every other port carries control information — which sector to read, how many sectors, which drive on the bus, and so on.

The most important register is the **status** register at `0x1F7`. After every command, we read this register in a loop to find out what the drive is doing. Its bits are:

| Bit | Name | Meaning |
|-----|------|---------|
| `0` | error | Last command failed |
| `1` | index | Index pulse bit |
| `2` | fixed | Reserved status bit |
| `3` | data | Data is ready to transfer |
| `4` | seek | Seek operation completed |
| `5` | fault | Drive reported a fault |
| `6` | ready | Controller can accept work |
| `7` | busy | Controller is busy |

We watch four of these flags:

- **BSY** (bit 7) — the controller is busy processing a command. Do not issue anything while this is set.
- **DRDY** (bit 6) — the drive is ready to accept a new command.
- **DRQ** (bit 3) — the drive has a sector buffer ready to be transferred.
- **ERR** (bit 0) — the last command failed. The error register (`0x1F1`) holds details.

### Addressing Sectors With LBA

A hard drive stores data as a sequence of 512-byte **sectors**. The way those sectors are numbered has changed over the years. The original scheme was **CHS** (Cylinder/Head/Sector), which reflected the physical layout of the platters. Modern drives use **LBA** — Logical Block Addressing — which ignores physical geometry and numbers every sector starting from zero.

We use **LBA28**, a variant of LBA that fits the sector number in 28 bits — enough for 128 GB of data, more than sufficient for a 1 MB disk image. The 28 bits are split across four registers: bits 0 through 23 go into `0x1F3`, `0x1F4`, and `0x1F5`, and the top four bits go into the low nibble of the drive/head register at `0x1F6`.

Drawn out as a bit field, an LBA28 value is chopped like this:

| Bits | Field | Stored in |
|------|-------|-----------|
| `0-7` | Low byte | Low address byte |
| `8-15` | Middle byte | Middle address byte |
| `16-23` | High byte | High address byte |
| `24-27` | Top nibble | Drive-select byte |

The drive/head register at `0x1F6` packs the top LBA nibble together with the drive-select bits:

| Bits | Field | Meaning |
|------|-------|---------|
| `0-3` | Top sector bits | Sector bits 24 through 27 |
| `4` | Drive | Choose master or slave |
| `5` | Fixed 1 | Required constant bit |
| `6` | Address mode | Use linear sector addressing |
| `7` | Fixed 1 | Required constant bit |

Bits 7 and 5 are obsolete holdovers that historically must be set to 1. A typical drive/head value for the DUFS disk is `0xE0` ORed with the top four bits of the LBA.

### Waiting for the Drive

Because PIO mode does not use interrupts (we do not wire IRQ14 into the IDT), every wait is a polling loop. The driver spins on the status register until either the drive is ready or an iteration counter runs out (a crude timeout to catch dead drives). The busy flag must clear before any command is issued — sending a command while BSY is set is a protocol violation. After a read or write command is issued, the driver waits for the data-request bit so it knows the drive's sector buffer is ready.

### The 400-Nanosecond Delay

The ATA specification requires a short delay after certain operations to give the drive's internal state machine time to update the status register. The canonical way to implement this delay is to read the alternate status register (`0x3F6`) four times. Each I/O port read takes roughly one hundred nanoseconds on real hardware, so four reads give the required 400-nanosecond pause. Reading the alternate status register, as opposed to the main status register at `0x1F7`, has the additional benefit of not clearing any pending interrupt from the drive.

### Initialising the Controller

Initialisation starts by performing a **software reset** on the primary bus. The driver sets the **SRST** (Software ReSeT) bit in the device-control register, clears it again, and performs the 400 ns delay. After the reset, it waits for BSY to clear and reads the main status register. On a real machine, a value of `0xFF` means the bus is floating — there is no drive attached. Anything else indicates a drive is present, and the driver continues to wait for the DRDY bit before declaring the drive ready.

### Reading a Sector

Reading one sector is a sequence of seven steps, in order:

1. Wait for BSY to be clear.
2. Write the sector count (`1`) to `0x1F2`.
3. Write the low, middle, and high bytes of the LBA to `0x1F3`, `0x1F4`, and `0x1F5`.
4. Write the drive select byte (LBA mode, master drive, top four bits of the LBA) to `0x1F6`.
5. Write the READ SECTORS command (`0x20`) to the command register at `0x1F7`.
6. Wait 400 nanoseconds, then wait for DRQ to be set.
7. Read 256 16-bit words from the data port at `0x1F0` into the caller's buffer.

The seventh step is where the actual data crosses from the drive into RAM. Rather than reading one word at a time in a C loop, we use a single x86 instruction called `rep insw` ("repeat input string word"), which reads 16-bit words from an I/O port into memory under the control of the ECX register. We call this through a tiny inline-assembly wrapper that passes the port number, the buffer address, and the count of 256. One instruction copies the entire sector.

### Writing a Sector

Writing a sector follows the same skeleton, but the command byte is WRITE SECTORS (`0x30`) and the data flow reverses — we use `rep outsw` to write 256 words from the buffer to the data port. There is one extra step at the end: we send a **cache flush** command (`0xE7`). Modern drives have write caches that hold data in volatile memory until they decide to commit it. Without an explicit flush, a power loss could leave the supposedly-written data unrecorded. The flush command tells the drive to wait until everything in the cache has reached the physical medium before reporting completion.

### Wiring It Into the Kernel

The ATA controller is brought up during kernel startup after the heap is set up but before the full interrupt path is live. On boot, we print one of three messages through the klog layer introduced in Chapter 9: a drive was found and is ready, no drive was detected, or an error occurred during initialisation. This gives immediate visible feedback that the controller is responding.

Once the driver is initialised, it wraps each drive's sector read/write routines in a small ops-table and publishes them to the block device registry under the names `"sda"` for the master drive and `"sdb"` for the slave drive. Chapter 12 describes that registry in detail. After that, the filesystem layer and the syscall layer find each drive by name rather than calling ATA-specific code directly.

### Where the Machine Is by the End of Chapter 11

By the end of this chapter, the kernel can read and write arbitrary 512-byte sectors without any filesystem knowledge. That raw capability is exactly what a filesystem needs — the filesystem imposes structure on top of the raw sectors, turning "sector 22" into "the file called `hello.txt`". Chapter 13 takes that step. For now, the driver is intentionally minimal: one sector at a time, polling only, master drive on the primary channel, no interrupt handling. It is not efficient, but it is correct and easy to reason about.
