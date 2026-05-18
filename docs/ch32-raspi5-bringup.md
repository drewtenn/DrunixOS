\newpage

## Chapter 32 — Raspberry Pi 5 Platform Notes

This appendix is a companion to ch31. It collects the platform-specific address detail and bring-up contract for the Raspberry Pi 5 (BCM2712 SoC, four Cortex-A76 cores) — the second piece of native hardware the AArch64 port can target, alongside the BCM2837-based Pi 3 from ch31 and the QEMU `virt` machine used throughout ch01–ch05. The runtime flow itself does not change: boot.S still demotes from EL2 to EL1, ch04's exception vector table still installs at `VBAR_EL1`, and the ARM Generic Timer from ch05 still drives the scheduler tick. What changes is the peripheral map, the interrupt controller, and the page-table reach.

### What's different from the Pi 3

The Pi 5 is closer to a workstation-class arm64 board than a microcontroller. Four Cortex-A76 cores share a 2 MB L3 cache through the DynamIQ Shared Unit, and most peripherals — UART, GPIO, I²C, SPI, ethernet — have moved from the SoC itself to a separate I/O controller called RP1, reached over a PCIe Gen 2 ×4 link. The BCM2712 retains a small set of legacy peripherals on its internal AXI bus: the GIC-400 interrupt controller, the System Timer, a debug UART, and a handful of low-level support blocks. Everything else lives in RP1.

The practical consequence for OS bring-up is twofold. First, the peripheral memory map climbs above 4 GB: the BCM2712 SoC window starts at physical `0x10_4000_0000`, and the PCIe outbound window — through which the kernel reaches RP1's registers — starts at physical `0x1f_0000_0000`. Both sit beyond the 32-bit identity-mapped range the existing AArch64 port used for the Pi 3 and the QEMU virt machine, so the MMU layer from ch07 has to be widened before either window can be touched. Second, the interrupt controller is a GIC-400 — a GICv2 implementation reached entirely through MMIO — rather than the BCM2836 core-local block on the Pi 3 or the GICv3 system-register interface on QEMU virt. The Pi 5 needs a new driver that wakes the GIC by writing to its MMIO bases instead of system registers.

### MMU widening: VA_BITS 32 → 39

The Pi 3 and QEMU virt both fit inside a 4 GB virtual address space (`VA_BITS = 32`, `T0SZ = 32` in `TCR_EL1`), and the kernel identity-maps the low 2 GB of physical addresses through `g_kernel_l1[0]` (a 1 GB level-2 table covering 0–1 GB in 2 MB blocks) and `g_kernel_l1[1]` (a single 1 GB level-1 block covering 1–2 GB). To reach the Pi 5 peripheral windows the M5 milestone widens the address space to 39 bits — `T0SZ = 25` — which keeps the same three-level page table shape (`L1 → L2 → L3` with 4 KB granule, per ARM ARM D5.2.3) but unlocks indexing into the previously unused `g_kernel_l1[2]` through `g_kernel_l1[511]`. The Pi 3 and virt entries at `L1[0]` and `L1[1]` are unaffected by the widening; the page table just has more reachable slots.

The other half of the MMU change is `TCR_EL1.IPS`, the field that bounds the output physical address size for stage-1 translation. The default `IPS = 0` only encodes 32-bit physical addresses, which would refuse Pi 5's high-address descriptors with an address-size fault. The Pi 5 build sets `IPS = 2` to allow 40-bit PAs, matching the BCM2712's actual physical address width. QEMU's emulated Cortex-A53 reports `PARange = 1` (36-bit) and rejects `IPS = 2` as constrained-unpredictable, so the IPS bump is platform-conditional: only the `PLATFORM=raspi5` build raises it.

### The peripheral windows

The Pi 5 build installs two extra level-1 block descriptors through the new `platform_extra_kernel_blocks()` hook the MMU layer consults at boot:

| Slot | Virtual base | Physical base | Size | Attribute | What lives here |
|------|--------------|---------------|------|-----------|-----------------|
| `L1[65]`  | `0x10_4000_0000` | `0x10_4000_0000` | 1 GB | Device-nGnRnE | BCM2712 SoC peripherals: `uart10`, GIC-400 |
| `L1[124]` | `0x1f_0000_0000` | `0x1f_0000_0000` | 1 GB | Device-nGnRnE | PCIe2 outbound window: RP1 peripherals |

Both blocks are identity-mapped — virtual equals physical — so the driver code can read the live device tree's `reg` properties at face value and dereference them directly. There is no `ioremap` step. The attribute is Device-nGnRnE rather than Normal Cacheable, so accesses are not gathered, not reordered, and not held in any cache. That is the right default for MMIO and matches what the Pi 3 chapter uses for its peripheral window at `0x3F000000`.

### GIC-400: the GICv2 MMIO interface

The BCM2712's interrupt controller is a GIC-400 distributor at physical `0x10_7fff_9000` and a CPU interface at `0x10_7fff_a000`. Unlike the GICv3 on QEMU virt — which exposes a system-register interface accessed through `ICC_*_EL1` — the GIC-400 is reached entirely through MMIO. The MVP driver writes the distributor's `GICD_CTLR` to enable forwarding, sets `GICD_IPRIORITYR` and `GICD_ISENABLER` for the generic-timer PPI, then enables the CPU interface by writing `GICC_PMR`, `GICC_BPR`, and `GICC_CTLR`. Interrupt acknowledge is a read of `GICC_IAR`; end-of-interrupt is a write back to `GICC_EOIR`.

The Pi 5 driver opens with a small safety check that the rest of the port does not need. The Cortex-A76 supports the GICv3 system-register interface as well as legacy GICv2 MMIO, and which one is active depends on what EL3 firmware left in `ICC_SRE_EL2` and `ICC_SRE_EL1` before handing control to the kernel. Pi 5 firmware (Raspberry Pi's TF-A build) keeps the system-register interface disabled and leaves the GIC-400 in MMIO mode — the live device tree declaring `compatible = "arm,gic-400"` is the signal — but a misconfigured boot could leave the system-register interface enabled, in which case MMIO writes to `GICC_*` are silently ignored and IRQs never dispatch. The driver reads `ICC_SRE_EL1` at init, and if its SRE bit is set it prints a banner explaining what happened and halts in `WFE` rather than continuing in a state where the timer interrupt cannot fire.

### Generic-timer PPI

The ARM Generic Timer uses **PPI 14**, the non-secure physical timer interrupt, which the GIC numbers as **INTID 30** (PPI 14 + the 16-slot PPI base). This is the same INTID the QEMU virt GICv3 driver wires for `CNTP_EL1`, so the kernel-side handler in `kernel/arch/arm64/timer.c` is unchanged — only the path through the GIC differs. PPIs (private peripheral interrupts) are banked per-CPU, so the driver enables INTID 30 by writing to `GICD_ISENABLER0` without setting any target register; `GICD_ITARGETSR` is read-only for IDs 0–31.

### The two debug serials

The Pi 5 board exposes two unrelated debug UART paths, and which one a build talks to is a deployment choice rather than a software decision. Both speak PL011, so the register-level driver is identical; only the MMIO base address differs.

The default path is **RP1 UART0**, which sits inside the RP1 I/O controller and reaches the outside world through GPIO 14 (TX) and GPIO 15 (RX) on the 40-pin GPIO header. This is the path Pi 5 firmware uses when `config.txt` contains `enable_uart=1` — the same conventions every previous Pi has used. A standard USB-to-serial adapter connected to header pins 8, 10, and a ground pin is the typical debug rig. Because RP1 lives behind PCIe, the kernel reaches it through the `L1[124]` outbound-window mapping; the live device tree describes the translation as RP1's internal `0xc0_4003_0000` → PCIe `0x00_0003_0000` → CPU `0x1f_0003_0000`, and the MVP driver hardcodes that final CPU address.

The alternative is **uart10**, an SoC-internal PL011 at physical `0x10_7d00_1000` that drives the Pi 5's dedicated debug JST-SH connector — the small three-pin socket near the SoC intended for the Raspberry Pi Debug Probe. Builds opt into this path by passing `RASPI5_UART=jstsh` to `make`, which flips `PLATFORM_RASPI5_UART_BASE` to point at uart10. Everything else stays the same; the MMU mapping at `L1[65]` covers both peripherals.

Both paths assume Pi 5 firmware has already programmed the PL011 for 115200 8N1 by the time the kernel takes over (`uart_2ndstage=1` in `config.txt` makes the firmware itself print over the same line, which doubles as a sanity check). The MVP driver therefore makes `uart_init` a no-op: defensive re-initialization computes IBRD/FBRD divisors from an assumed clock frequency, and an assumed clock that doesn't match firmware silently destroys the only diagnostic channel. The cost of trusting firmware is one assumption; the benefit is that the first banner byte appears whether the host has the standard 40-pin adapter or the Debug Probe.

### Cortex-A76 codegen: keeping `-mcpu=cortex-a53`

The Pi 5 build keeps the same `-mcpu=cortex-a53` compiler flag the rest of the AArch64 port uses. A53 binaries run natively on A76, and the M5 milestone treats `-mcpu=cortex-a76` as a deferred optimization rather than a requirement. Switching to A76 codegen would enable the LSE atomic extensions (`stadd`, `ldset`, …) and slightly better instruction scheduling, but it also requires verifying that none of the existing locking and memory-ordering paths drift into territory the A53-based virt and raspi3b CI haven't exercised. The A76 is an aggressive out-of-order core that surfaces missing `dsb`/`dmb` barriers an A53 happily papers over; until the port has hardware-driven regression coverage, the safer default is the same codegen the existing tests already validate.

### Pi 5 fixed memory layout

The Pi 5 build expects a flattened device tree pointer in `x0` per the Linux AArch64 boot protocol (ch01), just like the QEMU virt build. The RAM range comes from the FDT's `/memory` node and varies with the board variant (4 GB, 8 GB, or 16 GB); the other ranges are fixed. A 256 MB safety floor backs the FDT parser if the blob fails validation, so the kernel still boots far enough to print a diagnostic banner from an early hardware-test scenario.

| Region | Start | End | Notes |
|--------|-------|-----|-------|
| Usable RAM | `0x0000_0000` | up to `0x3_FFFF_FFFF` (16 GB) | From FDT `/memory`; truncated to 2 GB for the current kernel linear-map reach |
| Kernel image | `0x0008_0000` | — | Load address per `config.txt kernel=kernel8.img` |
| SoC peripheral window | `0x10_4000_0000` | `0x10_8000_0000` | GIC-400, uart10, System Timer |
| PCIe2 outbound window | `0x1f_0000_0000` | `0x1f_4000_0000` | RP1 (UART0, GPIO, I²C, SPI, ethernet) |

The 2 GB kernel-linear-map ceiling is a current limitation of the M5 milestone, not a property of the hardware. Splitting `L1[1]` into a `L2` table to reach more than 2 GB of RAM is a follow-on; the MVP serial-shell scope does not need it.

### Boot artifact and SD card layout

Pi 5 firmware loads `kernel8.img` from the boot partition at physical `0x80000` when `config.txt` declares `arm_64bit=1 kernel=kernel8.img`. The MVP build produces both `kernel-arm64.elf` (for `gdb` and disassembly) and `kernel8.img` (the flat binary the firmware actually loads). The minimal `config.txt` for a Pi 5 SD card is four lines: `arm_64bit=1`, `kernel=kernel8.img`, `enable_uart=1`, and `uart_2ndstage=1`. The firmware blobs themselves — `bootcode.bin`, `start4.elf`, `fixup4.dat`, and the matching `bcm2712-rpi-5-b.dtb` — are fetched from the Raspberry Pi firmware project and copied onto the FAT32 boot partition alongside `kernel8.img`. The build does not bundle these binaries because they are vendor-controlled and shift with firmware updates.

### Boot entry contract

The Pi 5 boot entry contract differs from the Pi 3's in a few places worth knowing:

- All four cores enter AArch64 state at **EL2**, with PC at `0x80000` — the same as Pi 3.
- The firmware passes an FDT pointer in `x0` per the Linux boot protocol, so the kernel can discover RAM, peripherals, and the debug-UART path from `/chosen/stdout-path` rather than relying on the fixed map. The MVP uses the FDT only for `/memory`; address hardcoding handles the rest.
- Caches and MMU are off; no stack pointer is established.
- The GIC-400 starts in MMIO mode with `ICC_SRE_EL1.SRE = 0`. Firmware does not enable the GICv3 system-register interface.

Boot.S handles the EL2 → EL1 demotion, core parking, and BSS zero exactly as it does for the Pi 3 and QEMU virt; the only Pi 5-specific behaviour is the new MMU geometry, which `arm64_mmu_init` picks up by reading `ARM64_MMU_VA_BITS` and the `platform_extra_kernel_blocks()` hook.

### SD root and init launch (M6)

The follow-on M6 milestone adds the BCM2712 SDHCI block driver (`kernel/arch/arm64/platform/raspi5/sdhci.c`), a new identity-mapped L1[64] block covering the lower SoC peripheral range where the SD host registers live (`0x10_00ff_f000`), and a small `DRUNIX_ROOT_DEVICE` plumbing change so the raspi5 build mounts `/dev/sda2` instead of `/dev/sda1`. The driver is a port of `raspi3b/emmc.c` — both controllers implement the SD Host Controller Simplified Spec v3.00; only the MMIO base differs.

The Pi-5-specific safety step in the new driver, added because Pi 5 firmware may leave the controller in UHS-I 1.8V signaling mode after loading `kernel8.img`, is the reset sequence that forces the host pads back to 3.3V (`HOST_CONTROL_2.S18EN = 0`) and power-cycles the SD card via SDHCI Power Control before any commands. Without it, CMD0 drops the card to 3.3V identification state but the host pads stay at 1.8V, and the ACMD41 init loop silently times out.

The Pi 5 SD card carries two MBR partitions: FAT32 at `sda1` for the firmware boot artifacts (Pi firmware can only load `kernel8.img` from FAT32), and ext3 at `sda2` for the Drunix root. The disk.fs ext3 image is the same one virt and raspi3b use, written to `sda2` directly with `dd`.

### HDMI framebuffer (M7)

M7 adds an HDMI framebuffer driver at `kernel/arch/arm64/platform/raspi5/video.c`, replaces the framebuffer stubs in `raspi5/stubs.c` with the real backend, and wires `start_kernel.c`'s raspi5 branch to call `platform_framebuffer_acquire()` between `kheap_init()` and `arch_irq_init()`. The kernel still launches `/bin/shell` (not `/bin/desktop`) because the desktop-launch gate in `arm64_launch_init_or_fallback` also requires `/dev/kbd` and `/dev/mouse`, which arrive in M8 with USB HID. HDMI in M7 mirrors the in-kernel boot console; it does not yet drive a user-space compositor.

The driver is a near-port of `raspi3b/video.c` with three BCM2712-specific changes worth recording for the next BCM-class bring-up.

First, the mailbox MMIO offset moved. The historical BCM2835/2837 mailbox lives at peripheral offset `0xB880`; BCM2712 keeps the same `brcm,bcm2835-mbox` register layout but at offset `0x13880`. Translated through the `soc@107c000000` `ranges` block in `bcm2712.dtsi`, that is CPU-physical `0x10_7C01_3880`. The address sits inside the L1[65] identity-mapped Device block M6 already established for the SoC high peripheral window, so no new MMU plumbing is needed.

Second, the bus-to-physical translation used by `ALLOCATE_BUFFER` differs from the Pi 3 path. The Pi 3 driver masks the returned bus address with `0x3FFF_FFFF`, stripping the `0xC000_0000` legacy VC4 uncached SDRAM alias. BCM2712's `axi` bus declares identity `dma-ranges` for the lower 64 GiB, so for firmware paths that go through `dma_alloc_coherent` the returned value is CPU-physical directly. The raspi5 helper, `raspi5_fb_bus_to_phys`, accommodates both: if the top two bits are set it applies the legacy mask, otherwise it returns the value unchanged. The compact serial trace prints the raw bus address every boot so we can collapse the helper to whichever branch the Pi 5 firmware actually exercises once we have hardware traces.

Third, the kernel currently identity-maps only the first 2 GiB of RAM on raspi5 (`raspi5_ram_layout_init` truncates `ram_size` accordingly). If firmware allocates the framebuffer at or above `0x80000000`, the driver rejects the bring-up with a `phys above 2 GiB linear-map ceiling; rejecting (serial fallback)` line and returns failure; the boot continues to the serial shell. The L1[1]→L2 split that would let the full RAM range carry framebuffer mappings is deferred to a later milestone; firmware empirically places small framebuffers low so the rejection path should rarely fire in practice.

`config.txt` on the FAT32 boot partition needs a small set of belt-and-braces knobs to keep HDMI behavior reproducible across cold boots: `framebuffer_width=1024`, `framebuffer_height=768`, `framebuffer_depth=32`, `framebuffer_ignore_alpha=1`, `hdmi_force_hotplug=1`, and `max_framebuffers=1`. `framebuffer_ignore_alpha` is the documented mitigation for the Pi 5 quirk where a 32-bpp fb reads back as black with the alpha lane left at 0; XRGB8888 needs that lane explicitly opaque. `hdmi_force_hotplug` keeps HDMI alive if the monitor handshake is slow.

The `raspi3b/video.c` driver only registered the in-kernel `fb_text_console`; raspi5/video.c additionally calls `fbdev_init()` so `/dev/fb0` and `/dev/fb0info` are published as character devices. `user/apps/fbfill` is a tiny smoke test that reads `/dev/fb0info`, `mmap()`s `/dev/fb0`, and walks four solid full-screen colors (red, green, blue, white) using the firmware-reported pitch. Running it from the serial shell on real hardware verifies the end-to-end path without needing input.

### Where the Machine Is

By the time the M5 milestone completes its bring-up, the Pi 5 reaches the same in-kernel state the Pi 3 chapter ends in: the kernel image is loaded at `0x80000`, the MMU is enabled with identity-mapped peripheral windows, the GIC-400 is forwarding the generic-timer PPI to CPU 0, the PL011 is producing characters on the chosen debug UART, and the console terminal is polling input. By the time M6 lands, the kernel additionally mounts the ext3 root from the SD card and execs `/bin/shell` — the platform reaches a real userspace shell prompt over serial, the same way virt and raspi3b do today. M7 turns on HDMI: the kernel boot log now mirrors to an attached display, `/dev/fb0` is registered, and `bin/fbfill` paints solid colors to prove the path end to end. Interactive input on that display still comes in a later milestone.
