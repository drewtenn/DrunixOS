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

### Where the Machine Is

By the time the M5 milestone completes its bring-up, the Pi 5 reaches the same in-kernel state the Pi 3 chapter ends in: the kernel image is loaded at `0x80000`, the MMU is enabled with identity-mapped peripheral windows, the GIC-400 is forwarding the generic-timer PPI to CPU 0, the PL011 is producing characters on the chosen debug UART, and the console terminal is polling input. No SD root, no display, no network — those land in follow-on milestones. The platform has stopped being a Pi 5 with firmware running on it and started being a Pi 5 with Drunix running on it, which is the qualifying property of every other board in the port.
