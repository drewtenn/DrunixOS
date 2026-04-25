# AArch64 / Raspberry Pi Port — Plan

This document describes the intended plan for porting Drunix from 32-bit
x86 to 64-bit ARM (AArch64) so that it can run on the Raspberry Pi.
It is the strategic companion to `docs/arm64-port-spec.md`, which
specifies the concrete Milestone 1 implementation.

No source files in `kernel/`, `boot/`, `user/`, or `Makefile` are
modified by this commit.  This is a plan-and-spec only.

## Goals

1. Keep the existing 32-bit x86 build fully working throughout the port.
   Every commit on the way to a Pi-capable Drunix must still build and
   run the x86 kernel unchanged.
2. Target the **Raspberry Pi 3 (BCM2837, Cortex-A53, AArch64)** first.
   It is a wide-enough slice of ARM to be useful and is emulated well
   by `qemu-system-aarch64 -M raspi3b`.  Later Pi models and 32-bit
   ARMv6/v7 are out of scope.
3. Use QEMU as the primary development target.  Real-hardware bring-up
   (SD card image, firmware blobs, GPIO mini-UART cable) lands after
   the QEMU target is green.
4. Introduce a clean architecture boundary inside the kernel so that
   higher layers (VFS, ext3, scheduler logic, slab, VMA, GUI rendering,
   syscall dispatcher) are reused with minimal behavioral change
   between x86 and ARM.

## Non-goals

- 32-bit ARM (ARMv6/v7, Pi Zero, Pi 1, Pi 2).  Single 64-bit target
  only.
- Pi 4 / Pi 5 specific peripherals (GIC-400, PCIe, different UART
  layout).  Pi 3 only.
- SMP.  Cores 1–3 are parked at boot and stay parked.
- Rewriting filesystems, GUI, or libc-compatible user runtime.  These
  are mostly portable C and are reused once their prerequisites
  (MMU, block driver, userland ABI, arch-boundary cleanup) are in
  place.

## Why this is a multi-milestone effort

A survey of the codebase (branch `claude/arm-raspberry-pi-port-apKpx`)
shows the x86-ness is concentrated in a predictable set of places:

| Subsystem | Files | x86 assumption |
|---|---|---|
| Boot | `boot/*.asm`, `kernel/arch/x86/boot/kernel-entry.asm`, `kernel/arch/x86/linker.ld` | Real-mode BIOS, Multiboot1, 1 MB load, GRUB2 |
| CPU ctrl | `kernel/arch/x86/{gdt,idt,irq,pit,sse,clock}.*`, `kernel/arch/x86/*.asm` | GDT, IDT, TSS, 8259 PIC, 8253/4 PIT, INT 0x80 |
| Paging | `kernel/arch/x86/mm/paging.c`, `kernel/arch/x86/mm/paging_asm.asm` | CR3, 2-level IA-32 page tables, `invlpg` |
| Port I/O | `kernel/platform/pc/*`, x86 helpers in `kernel/arch/x86/`, and VGA/debugcon paths in `kernel/kernel.c` / `kernel/lib/klog.c` | `in`/`out` instructions, PC device access |
| Context switch | `kernel/arch/x86/proc/switch.asm`, `kernel/arch/x86/proc/process_asm.asm` | `ESP`/`CR3`/`iret` |
| Syscall entry | `kernel/arch/x86/isr.asm` (vector 128), `user/lib/syscall.c` | `int $0x80` |
| User CRT0 | `user/lib/crt0.asm` | 32-bit x86 calling convention |
| ELF loader | `kernel/proc/elf.c` (`e_machine == EM_386`) | Rejects non-i386 |
| Drivers | `kernel/platform/pc/ata.c`, `kernel/platform/pc/keyboard.c`, `kernel/platform/pc/mouse.c`, VGA text path in `kernel/kernel.c` | ATA PIO, PS/2, VGA 0xB8000 |

Roughly 40% of kernel code is x86-tied.  Much of the remaining 60% —
VFS, ext3, procfs/sysfs, scheduler policy, slab, VMA, GUI
compositor/font rendering, syscall dispatcher — is already portable C,
but some paths still assume x86-specific frame layouts, CR3 handling,
or Multiboot-era memory discovery.  Those seams need to be lifted into
an explicit arch boundary before the shared layers can be reused
cleanly on ARM.

## Milestones

### Milestone 1 — "Hello, ARM" in QEMU  *(specified in detail in `docs/arm64-port-spec.md`)*

Boot a standalone AArch64 kernel image under
`qemu-system-aarch64 -M raspi3b`, demote from EL2 to EL1, park
non-primary cores, install a 16-entry exception vector table, bring
up the BCM2835 mini-UART for serial output, enable the ARM Generic
Timer, take periodic IRQs via the BCM2836 core-local interrupt
controller, and print a heartbeat over serial.

Out of scope: MMU, PMM, processes, filesystem, ELF loader, userland,
GUI.  Only the new files under `kernel/arch/arm64/` plus a small but
deliberate `ARCH` split in the build are introduced.  The current build
defaults are strongly i386/GRUB/QEMU-x86-oriented, so even this minimal
ARM target requires explicit build branching while keeping the x86
output unchanged.

Exit criterion: `make ARCH=arm64 run` prints a banner and five
heartbeat ticks in ≤ 10 seconds inside QEMU.  `make` with no `ARCH`
still boots the existing x86 build.

### Milestone 1b — Real Pi 3 hardware

SD card image layout, FAT partition with `bootcode.bin`, `start.elf`,
`fixup.dat`, `config.txt` (`enable_uart=1`, `arm_64bit=1`,
`kernel=kernel8.img`), and GPIO14/15 mini-UART wiring for a USB-serial
adapter.  No kernel code changes beyond whatever the Pi GPU firmware
requires for the mini-UART clock (already handled in Milestone 1).

### Milestone 2 — Build split and architecture boundary

1. Keep x86 CPU/MMU/context/boot code under `kernel/arch/x86/`, including the
   current `boot/`, `mm/`, and `proc/` x86 subtrees.
2. Keep PC-specific hardware support under `kernel/platform/pc/` instead of
   treating ATA, PS/2, and related port-I/O code as generic x86 code.
3. Introduce `kernel/arch/arch.h` as the shared arch API around behavior-level
   seams: CPU interrupt state, timer/clock hooks, console output, IRQ
   registration/dispatch, TLB invalidation, address-space switching, trap
   entry, and user-mode entry.
4. Make `kernel/lib/klog.c` portable by separating timestamp source, console
   sink, and optional debug sink from the current x86/PC-specific `clock.h`,
   `print_string()`, and debugcon path assumptions.

Exit criterion: `make`, `make run`, and `make check` still exercise the
existing x86 kernel unchanged, but the tree and build now support more
than one architecture without relying on x86-only paths.

### Milestone 3 — AArch64 MMU and physical memory bring-up

1. Bring up the AArch64 MMU with a 4 KB granule, 48-bit VA, kernel RAM
   mappings, and a device region for the Pi 3 peripherals at
   `0x3F000000–0x40000000`.  Enable caches.
2. Migrate `kernel/mm/pmm.c` away from its current Multiboot- and
   x86-reservation-specific logic.  Split it into:
   - arch-supplied usable RAM discovery
   - arch-supplied reserved/early-boot exclusions
3. On x86, keep usable RAM discovery backed by the Multiboot memmap.
   On ARM, start with a fixed BCM2837 memory layout and leave DTB
   parsing as a later enhancement.
4. Port `kernel/mm/kheap.c` and `kernel/mm/slab.c` onto the new ARM
   page allocator and mapping API.
5. Fold any remaining shared code that directly reads/writes `cr3` or
   assumes x86 identity-mapped page-table access behind the new arch/MM
   boundary.

Exit criterion: the ARM build boots in QEMU with MMU and caches enabled,
and shared allocators run through the new arch/MM boundary rather than
x86-specific assumptions.

### Milestone 4 — 64-bit process model and user ABI

1. Port context switching and first-entry-to-userland for AArch64:
   save x19–x29, LR, SP, and TTBR0 as needed by the scheduler and
   process launch paths.
2. Define the AArch64 kernel trap-frame shape used by syscall entry,
   IRQ return, scheduler save/restore, signal delivery, and signal
   return.  Do not force the ARM path to emulate the x86 frame layout
   internally.
3. Replace the current ELF assumption with a real ARM64 user-program
   plan: add ELF64 header/program-header support, validate
   `EM_AARCH64`, and handle 64-bit entry and segment addresses.
4. Add an AArch64 `user/lib/crt0.S` and `user/lib/syscall.c` variant
   that uses `svc #0`, with arguments in `x0..x5` and the syscall
   number in `x8`.
5. Define the ARM64 initial user stack contract (`argc`, `argv`,
   `envp`, auxv) and the signal trampoline / `sigreturn` path instead
   of carrying over the current i386-only startup and signal
   assumptions.
6. Treat TLS explicitly: either defer full ARM64 TLS parity until after
   the first user-process boot or add a minimal ARM64-compatible design
   here if a near-term dependency appears.

Exit criterion: a simple ARM64 user program loads, enters userspace,
issues syscalls, and exits cleanly under QEMU.

### Milestone 5 — Storage and filesystem

SDHCI/EMMC driver for the Pi 3.  Plugs into the existing
`kernel/blk/bcache.c` + `kernel/drivers/blkdev.c` layer.
`kernel/fs/ext3/*` and the VFS remain shared once the MMU/process
prerequisites above are complete.

### Milestone 6 — VGA-style framebuffer console

VideoCore IV mailbox property interface (`0x3F00B880`) to allocate a
framebuffer on the QEMU/Pi ARM64 target.  The ARM build reuses the
existing 8x16 font renderer and VGA attribute palette to mirror the
serial console into a visible framebuffer text grid.

This is not a literal PC VGA device and does not add PS/2, USB HID,
mouse, or desktop support.  Input remains serial-backed.

Exit criterion: `make ARCH=arm64 build` succeeds and
`python3 tools/test_arm64_vga_console.py` captures a nonblank framebuffer
screendump containing the ARM64 console boot.

### Milestone 7 — ARM64 keyboard input

Add a real ARM64 keyboard path before attempting the framebuffer desktop.
For QEMU this should start with a narrow, testable input device choice such
as USB HID keyboard support on the emulated Pi platform or a documented QEMU
keyboard device that can feed the existing TTY/input layer without importing
PC PS/2 assumptions.

Exit criterion: an ARM64 graphical boot can accept keyboard input without
using the serial console, route printable characters into the active console
or TTY path, and preserve `make ARCH=arm64 check` as a headless serial test.

### Milestone 8 — Framebuffer desktop

Reuse the generic desktop/compositor once ARM64 has a real input device path
and the process/display ownership model matches the x86 desktop path closely
enough to avoid ARM64-specific desktop forks.  The rendering and widget code
in `kernel/gui/` is mostly portable C, but presentation, interrupt masking,
and address-space assumptions must stay behind the architecture boundary.

## Risks

- **klog coupling.**  `kernel/lib/klog.c` depends on `clock.h`,
  `sched.h`, `print_string()` from `kernel.c`, and the QEMU
  debugcon port.  Milestone 1 therefore does **not** reuse klog;
  the ARM build prints via a direct UART path.  klog is made
  portable in Milestone 2 alongside the arch boundary.
- **64-bit ABI creep.**  The current process launcher, signal delivery
  path, user CRT0, and ELF loader are i386-shaped.  Milestone 4 must
  treat ELF64, trap-frame layout, and signal return as first-class ABI
  work, not as a small syscall-entry swap.
- **PMM assumptions.**  `kernel/mm/pmm.c` currently mixes physical RAM
  discovery with x86-specific reserved ranges, page-table placement,
  and VGA-era exclusions.  Milestone 3 must untangle those concerns
  before shared allocators can be trusted on ARM.
- **QEMU vs hardware drift.**  `-M raspi3b` does not model the Pi
  GPU boot sequence, cache behavior, or exact mini-UART clocking.
  Milestone 1b will uncover small issues (baud rate, caches, start
  EL).  Flagged separately, not in Milestone 1.
- **Scope creep.**  The natural temptation is to try to get to a
  shell on day one.  Milestone 1 is deliberately tiny; every
  subsequent milestone also has an explicit small exit criterion.

## Relationship to the existing book

The book chapters `ch01`–`ch30` describe the x86 kernel as-is.  They
stay authoritative for the x86 target.  A new `ch31-aarch64-bringup.md`
lands alongside the Milestone 1 implementation and covers the AArch64
boot flow, EL demotion, mini-UART, exception vectors, and the Generic
Timer.  Follow-on milestones add chapters in the normal way per
`docs/contributing/docs.md`.
