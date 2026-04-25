# arm64 ↔ x86 Feature-Parity Roadmap

## Goal

Bring `make ARCH=arm64 …` to feature parity with `make ARCH=x86 …` along three
axes that must all hold:

1. **Capability parity** — every user-visible OS capability supported on x86 is
   also supported on arm64. The mechanism may differ (EMMC instead of ATA,
   NEON instead of SSE, GIC/BCM2836 IRQs instead of 8259), but the capability
   is present and exercised by tests.
2. **Dev-loop parity** — every Make target, debugger workflow, test target,
   and static-analysis pass that exists for x86 also exists and runs for
   arm64. No more `"make ARCH=arm64 $@ is not implemented yet"` stubs.
3. **Hardware parity** — the arm64 build boots on real Raspberry Pi 3
   hardware, not just QEMU `raspi3b`.

This document is a *roadmap only*. It enumerates workstreams, declares
ordering and dependencies, and defines per-workstream exit criteria. Each
workstream is implemented through its own design + plan cycle when picked up.

## Non-goals

- 32-bit ARM (ARMv6/v7, Pi Zero, Pi 1, Pi 2).
- SMP. x86 is also single-CPU today; parity does not change that.
- Adding capabilities that x86 itself does not have (networking,
  full-screen accelerated graphics, etc.).
- Locking the implementation details of late workstreams now. Those
  decisions are made when each workstream is picked up, against current
  reality.

## Multi-board principle

Raspberry Pi 5 is the eventual hardware target. Pi 4 is an intermediate
target of interest. All three Pis share AArch64 as a CPU architecture but
differ substantially in peripheral SoCs (BCM2837 vs BCM2711 vs
BCM2712 + RP1 southbridge). Concretely:

| Layer | Reusable across Pi 3/4/5 | Per-board |
|---|---|---|
| AArch64 CPU code (boot, exceptions, MMU, generic timer) | yes | — |
| ELF64 loader, process model, syscalls, signals, FP/NEON | yes | — |
| VFS, ext3, scheduler, allocators, kernel/gui | yes | — |
| USB HID class drivers | yes | — |
| Peripheral base address, interrupt controller, UART, EMMC, mailbox/FB, USB host controller | — | yes |

To avoid throwing code away when porting to Pi 4 or Pi 5, every workstream
in this roadmap respects a strict split that mirrors the existing x86 tree:

```
kernel/arch/arm64/         # AArch64 generic; reused on Pi 3/4/5
kernel/platform/raspi3b/   # Pi 3 specific; siblings later: raspi4/, raspi5/
```

Per-board code is registered through a single `kernel/platform/platform.h`
interface. Adding Pi 4 or Pi 5 then means *adding* a sibling
`kernel/platform/raspiN/` directory, not modifying `kernel/arch/arm64/`.

## Definition of "supersedes"

This roadmap supersedes the deleted `docs/arm64-port-plan.md` and
`docs/arm64-port-spec.md`. Milestones 1–4 of the old plan have shipped
(boot, MMU, ELF64, shell, busybox, framebuffer, USB keyboard). Milestones 5–8
are subsumed and renumbered as workstreams W1, W2, W3, W4 below.

Stale `(planned, milestone N)` markers in book chapters
(`docs/ch06`, `ch08`, `ch11`, `ch15`, `ch16`, `ch19`, `ch20`, `ch24`,
`ch25`, `ch26`, `ch28`, `ch07`) are intentionally left untouched and will be
swept by a separate doc-cleanup task.

## Workstreams

Format: **W#. Name** — *current* / *gap* / *exit criterion*.

### W0. arm64 arch / platform split

*Foundation. Must land before any new driver work in W1, W2, W3, W4.*

- **Current.** `kernel/arch/arm64/` mixes AArch64-generic code (boot,
  exceptions, MMU, CNTP timer programming, ELF64) with Pi-3-specific
  peripheral code (BCM2835 mini-UART, BCM2836 core-local IRQ controller,
  BCM2835 mailbox + framebuffer, DWC OTG USB host inside `usb_keyboard.c`).
- **Gap.** No `kernel/platform/raspi3b/` directory. No
  `kernel/platform/platform.h` interface header. Hardcoded `0x3F000000`
  peripheral base appears in multiple files instead of behind a
  `PLATFORM_PERIPHERAL_BASE` macro.
- **Work.**
  - Create `kernel/platform/raspi3b/` mirroring `kernel/platform/pc/`.
  - Move out of `kernel/arch/arm64/` and into `kernel/platform/raspi3b/`:
    `uart.{c,h}`, `irq.{c,h}` (BCM2836 core-local), `video.{c,h}` (BCM2835
    mailbox + framebuffer), the DWC OTG host-controller portion of
    `usb_keyboard.c`.
  - Stay in `kernel/arch/arm64/`: `boot.S`, `exceptions.S/.c`, `linker.ld`,
    `timer.c` (CNTP system-register programming only — IRQ wiring moves
    to platform), `mm/`, `proc/`, `arch.c`, `arch_layout.h`, `arch.mk`.
  - Introduce `kernel/platform/platform.h` declaring the per-board interface
    every platform must implement: `platform_init()`,
    `platform_uart_putc/getc()`, `platform_irq_init/enable/dispatch()`,
    `platform_framebuffer_acquire()`, `platform_block_register()`,
    `platform_usb_hci_register()`. Keep the surface small; expand only as
    later workstreams need new hooks.
  - Replace hardcoded `0x3F000000` with `PLATFORM_PERIPHERAL_BASE` from
    `kernel/platform/raspi3b/platform.h`.
- **Exit.** `kernel/platform/raspi3b/` exists; `kernel/arch/arm64/`
  contains no BCM-anything; `make ARCH=arm64 check` passes unchanged;
  adding a sibling `kernel/platform/raspi5/` later is purely additive.

### W1. Persistent disk-backed root FS (QEMU + EMMC)

- **Current.** arm64 boots with the embedded `dufs` ramfs as `/`; x86
  mounts ext3 from a real block device.
- **Gap.** No EMMC/SDHCI driver on arm64. No ext3-as-root path on arm64.
- **Design lock-in.** Storage backend is **EMMC/SDHCI**, not virtio-blk
  and not pflash. QEMU `raspi3b` emulates the BCM2835 EMMC controller, so
  one driver works for both QEMU and real Pi 3. Choosing virtio-blk would
  require writing a second block driver in W4 and throwing the first away.
- **Work.**
  - Implement `kernel/platform/raspi3b/emmc.c` against the BCM2835 EMMC
    controller, registered via `platform_block_register()` (W0 dependency).
  - Wire arm64 ext3 mount as the boot rootfs.
  - Switch `make ARCH=arm64 run` default from `dufs` to ext3.
- **Exit.** `make ARCH=arm64 run` boots with ext3 root; `dufs` retired
  as default; `check-filesystem-init` passes against an ext3 root.

### W2. Mouse input (USB HID)

- **Current.** arm64 has a USB HID keyboard via QEMU `usb-kbd`. x86 has
  a PS/2 mouse driver feeding `kernel/gui/desktop.c`.
- **Gap.** No mouse path on arm64.
- **Work.** USB HID mouse driver behind the same input layer the x86 PS/2
  mouse uses. Host controller portion lives in
  `kernel/platform/raspi3b/usb_hci.c` (relocated by W0); HID class driver
  in `kernel/drivers/usb_hid.c` (board-neutral, reused on Pi 4/5).
- **Exit.** arm64 surface receives mouse events; a `check-mouse` test
  (new, mirrored across both arches) passes.

### W3. Desktop / GUI compositor

- **Current.** x86 builds with `kernel/gui/desktop.c` available
  (`NO_DESKTOP=1` is the runtime default but the desktop is wired in).
  arm64 builds without any of `kernel/gui/`.
- **Gap.** Compile arm64 with `kernel/gui/` enabled. Reconcile
  `kernel/gui/framebuffer_multiboot.c` (x86-only) with the framebuffer-
  acquire seam introduced by W0. Verify input plumbing from W2.
- **Note.** `kernel/arch/arm64/video.c` already uses the BCM2835 mailbox
  property interface at `0x3F00B880`; W3 inherits that path through W0's
  relocation, so framebuffer-acquire is throw-away-free.
- **Exit.** `make ARCH=arm64 NO_DESKTOP=0 run` brings up the desktop;
  existing desktop smoke tests run on both arches.

### W4. Real Pi 3 hardware boot

- **Current.** arm64 produces `kernel8.img` for QEMU `-kernel` loading.
  No SD-card image, no firmware-blob bundle, no real-hardware verification.
- **Gap.** SD-card image build target with `bootcode.bin`, `start.elf`,
  `fixup.dat`, `config.txt` (`enable_uart=1`, `arm_64bit=1`,
  `kernel=kernel8.img`); documented USB-serial wiring on GPIO14/15;
  one-time real-hardware bring-up procedure verified.
- **Note.** EMMC driver and mailbox framebuffer are already in
  `kernel/platform/raspi3b/` by the time W4 runs (W1, W3), so W4 is
  primarily packaging.
- **Exit.** `make ARCH=arm64 sd-image` produces a flashable image;
  written hardware-bring-up procedure verified once on real Pi 3 hardware.
- **Per-platform scope.** Pi 4 (`kernel/platform/raspi4/`) and Pi 5
  (`kernel/platform/raspi5/`) are explicit future siblings, tracked outside
  this roadmap.

### W5. NEON / FP enablement

- **Current.** arm64 kernel built with `-mgeneral-regs-only`; user FP
  unverified. x86 has SSE enabled with kernel save/restore.
- **Gap.** NEON/FP context save/restore in the arm64 context-switch path
  (`kernel/arch/arm64/proc/`). Drop `-mgeneral-regs-only` where user FP is
  needed. Equivalent of `kernel/arch/x86/sse.{c,asm}` for AArch64.
- **Sequencing note.** Do this *before* W3, not after. If desktop renderer
  paths (font blend, alpha, scaling) are written assuming integer-only,
  enabling FP later forces a rewrite.
- **Exit.** A simple float-using user program runs correctly on arm64;
  a `check-fp-context-switch` test (new) passes.

### W6. Debug workflow

- **Current.** x86 supports `debug`, `debug-user`, `debug-fresh`,
  `test-halt`, `test-threadtest` (QEMU `-s -S` + `i386-elf-gdb`). arm64
  prints `"make ARCH=arm64 $@ is not implemented yet"` and exits 2.
- **Gap.** `aarch64-elf-gdb` integration with QEMU `-s -S` for `raspi3b`;
  symbol-loading conventions matching x86; debug-user variant for userland
  processes; `test-halt` and `test-threadtest` ported.
- **Exit.** Every x86 debug Make target has an arm64 equivalent that works.

### W7. Cross-arch static analysis

- **Current.** `compile-commands`, `format-check`, `cppcheck`,
  `sparse-check`, `clang-tidy-include-check`, `scan` are all hardcoded
  `$(MAKE) ARCH=x86 $@`.
- **Gap.** Parameterize each on `ARCH`; ensure each tool can ingest
  AArch64 sources (sparse machine model, cppcheck platform definitions,
  clang triple).
- **Exit.** `make ARCH=arm64 scan` succeeds; both arches' scans run in
  whatever pre-commit / CI gate already exists for x86.

### W8. ext3 cross-platform tests

- **Current.** `validate-ext3-linux`, `test-ext3-linux-compat`,
  `test-ext3-host-write-interop` are x86-only.
- **Gap.** Run these against the arm64 build. The tests check disk-image
  bit-equivalence with Linux's `e2fsck`/`debugfs`, which is arch-neutral
  on the *image* side — they need the kernel actually writing ext3 from
  arm64. Direct dependency on W1.
- **Exit.** All three test targets pass for `ARCH=arm64`.

## Sequencing

```
W0 (arch/platform split)  ── prerequisite for W1, W2, W3, W4
                           
W6 (debug workflow)        ┐
                           ├── independent; can start in parallel with W0
W7 (static analysis)       ┘

W0 ── W1 (EMMC + ext3) ─┬── W8 (ext3 cross-platform tests)
                        │
                        └── W4 (Pi 3 SD image, EMMC reused)

W5 (NEON/FP) ── independent of W0; do before W3

W0 ── W2 (USB HID mouse) ── W3 (desktop)

W3 + W1 ── W4 (Pi 3 SD image; capstone)
```

**Recommended order:**

1. **W0** (arch/platform split — foundation; must land first).
2. **W6 + W7** (low-risk dev-loop work; can parallelize with W0 since they
   touch different subsystems).
3. **W1** (EMMC + ext3 root; foundation for W4 and W8).
4. **W8** (free after W1 — gating change only).
5. **W5** (NEON/FP; independent but must land before W3).
6. **W2 → W3** (mouse before desktop; sequential).
7. **W4** (Pi 3 SD image; capstone).

## Risks

1. **EMMC under QEMU vs real HW.** QEMU `raspi3b`'s BCM2835 EMMC emulation
   may diverge from real silicon on init sequencing, DMA, or timing.
   Surprises surface only when W4 runs on hardware. *Mitigation:* get an
   early Pi-3 hardware smoke test in W1's plan as soon as the QEMU path
   is green; do not wait until W4.

2. **Lazy FP vs eager save/restore in W5.** Eager save on every context
   switch is correct but expensive. Lazy FP (trap on first FP use, then
   save) is the usual pattern but adds complexity to the trap path.
   *Mitigation:* start eager, optimize later only if measurable.

3. **`kernel/gui/framebuffer_multiboot.c` refactor in W3.** Lifting
   framebuffer-acquire into an arch / platform seam touches x86 paths.
   Risk of regressing the x86 desktop. *Mitigation:* arch-boundary
   phases already established a refactor pattern; follow it.

4. **Static-analysis tooling AArch64 support in W7.** Sparse needs an
   `aarch64` machine model; cppcheck needs platform definitions;
   clang-tidy needs the right triple. Some warnings may have no AArch64
   analogue. *Mitigation:* per-arch suppression files; treat first-time
   noise as expected.

5. **Stale `(planned, milestone N)` chapter markers.** Many book chapters
   reference the old milestone numbering, and many point to work that
   has already landed. *Mitigation:* tracked separately as a doc-sweep
   task; not a blocker for any workstream above.

6. **Workstream-spec drift.** This roadmap is enumeration-only; per-
   workstream specs are written when picked up. Discoveries during W1
   may reshape W4's SD-image scope. *Mitigation:* this is by design —
   re-spec at hand-off, do not lock W4's details a year ahead.

7. **W0 underscoping.** The temptation in W0 is to invent a complete
   board abstraction up front. Resist this. The `platform.h` surface
   should grow only as later workstreams need new hooks. Anything Pi 3
   only ever needs internally stays private to
   `kernel/platform/raspi3b/`.

## Forward path to Pi 4 and Pi 5

After this roadmap completes, adding Pi 4 or Pi 5 is a per-platform
effort:

- `kernel/platform/raspi4/` or `kernel/platform/raspi5/` is added as a
  sibling to `kernel/platform/raspi3b/`.
- Per-board drivers are written for the new SoC's UART (PL011), interrupt
  controller (GIC-400), EMMC controller, USB host (xHCI on Pi 4 via
  VL805/PCIe; via RP1 on Pi 5), and mailbox/framebuffer.
- All AArch64-generic code under `kernel/arch/arm64/` is reused unchanged.
- All board-neutral drivers (USB HID class, ext3, VFS, scheduler,
  compositor) are reused unchanged.
- A new `make ARCH=arm64 PLATFORM=raspi4 sd-image` target produces the
  per-board image.

QEMU support for Pi 4 is currently experimental and Pi 5 is newer than
QEMU's BCM2712 support, so Pi 3 remains the best dev target until QEMU
catches up — even for code that will eventually run on Pi 5.
