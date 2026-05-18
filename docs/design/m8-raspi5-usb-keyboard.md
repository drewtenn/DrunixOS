# M8 — Pi 5 USB keyboard (xHCI behind RP1 PCIe)

Date: 2026-05-18
Branch base: `main` @ M7 tip (`d671d93`).
Target hardware: Raspberry Pi 5 (BCM2712 + RP1).

## Goal

Drive a USB keyboard plugged into one of the Pi 5's USB-A ports so the
HDMI console (M7) becomes interactive without a serial cable. Keystrokes
land in `/dev/kbd` via `tty_input_char(0, c)`, the same path raspi3b
already uses. **Keyboard only**: no mouse, no hubs, no hot-plug, no
mass-storage / xHCI device classes beyond HID-boot-keyboard.

Display-only HDMI from M7 becomes display+input. The desktop launch gate
in `arm64_launch_init_or_fallback` requires `chardev_get("fb0") && kbd
&& mouse`, so M8 does NOT yet promote `/bin/desktop` — the shell still
runs over the HDMI console / keyboard combination. Mouse + desktop is
M9.

## Why this is bigger than M7

Pi 5 USB is not the simple DWC2 controller raspi3b uses. The chain on
Pi 5 is:

  Cortex-A76  →  PCIe2 root complex (BCM2712)  →  RP1 chip  →
                                              →  RP1-internal xHCI controllers
                                              →  USB-A ports

Three layers to bring up before a keyboard byte reaches the kernel:

1. **PCIe root complex** — enable the BCM2712 PCIe2 controller, run
   link training, enumerate the RP1 endpoint, map its BARs.
2. **xHCI host controller** — initialize one of RP1's xHCI instances
   (command ring, event ring, device contexts), reset the root port the
   keyboard is plugged into.
3. **USB device + HID class** — enumerate the keyboard (default address
   0 → SET_ADDRESS → GET_DESCRIPTORs → SET_CONFIGURATION), put it in HID
   boot protocol mode, poll the interrupt-in endpoint, parse the 8-byte
   boot reports, push ASCII keys to `tty_input_char(0, c)`.

raspi3b/usb_hci.c (~780 LOC) already implements layer 3 against DWC2.
The HID parsing + USB enumeration shape ports over largely unchanged;
the host-controller substrate is what's new.

## Scope

In scope:

1. PCIe root complex bring-up on BCM2712 enough to read the RP1 endpoint
   config space and map its BARs. Hardcode addresses from `bcm2712.dtsi`
   rather than walking the DT — the BCM2712 has exactly one PCIe2 root
   complex at a known address and RP1 sits at a known function.
2. xHCI host driver: enumerate capability + operational registers,
   allocate DCBAA + command ring + event ring + scratchpad, reset
   controller, enable the port the keyboard is on.
3. Single-device USB enumeration: address 1, get device descriptor, get
   first config descriptor, set config, find HID-boot-keyboard
   interface, claim its interrupt-in endpoint.
4. HID boot keyboard handler: schedule a periodic interrupt-in transfer
   for 8-byte reports, parse modifiers + key array, translate HID usage
   IDs to ASCII (reuse raspi3b/usb_hci.c's table), debounce repeats
   based on the report-array delta, push to `tty_input_char(0, c)`.
5. Wire `platform_usb_hci_register()` and `platform_usb_hci_poll()` in
   `raspi5/stubs.c` to the new driver. start_kernel.c on raspi5 already
   has the call site, gated by `DRUNIX_ARM64_VGA` (which is on).
6. Hardware verification on the owner's Pi 5: plug in a USB keyboard,
   type at the HDMI console, see characters land in the shell.

Out of scope (M9+):

- USB mouse (deferred to M9 alongside the desktop-launch promotion).
- USB hubs / hot-plug / multiple devices.
- Other HID classes (gamepads, multitouch, anything beyond boot kbd).
- Mass storage, video, audio, network over USB.
- MSI-X / MSI interrupts. Polling is fine for one keyboard's
  ~125 Hz interrupt-in rate.
- USB suspend / resume / power management.
- Linking against the raspi3b DWC2 driver (Pi 5 RP1 is xHCI; the
  controllers are unrelated at the register level).

## Architecture

Three new units, each isolated and independently testable:

```
+-----------------------------+
| raspi5/pcie.c               |   New. BCM2712 PCIe2 root complex bring-up.
|  - bring up link            |   Enumerate RP1 endpoint at fn(0,0,0).
|  - read RP1 config space    |   Map xHCI BAR to a CPU-physical
|  - return BAR mappings      |   address inside the existing
|                             |   PLATFORM_RASPI5_PCIE_WINDOW_BASE
|                             |   identity-mapped L1[124] block.
+-----------------------------+
              |
              v
+-----------------------------+
| raspi5/xhci.c               |   New. USB 3.0 xHCI host driver. Bare
|  - capreg / opreg            |   minimum: command ring (16 entries),
|  - DCBAA + scratchpad        |   event ring (64 entries, polled),
|  - command + event rings     |   one device slot, one transfer ring
|  - port reset                |   per endpoint. No MSI; no DMA pool.
|  - submit transfer / poll    |   Uses the kernel heap for DMA-capable
|                             |   allocations (heap is in low SDRAM
|                             |   identity-mapped Normal-WB; cache
|                             |   maintenance per transfer).
+-----------------------------+
              |
              v
+-----------------------------+
| raspi5/usb_kbd.c            |   New. Single-keyboard glue and HID
|  - enumerate one device      |   boot-protocol parsing. Reuses the
|  - SET_PROTOCOL boot         |   usage->ASCII table layout from
|  - parse 8-byte HID report   |   raspi3b/usb_hci.c (copy with the
|  - tty_input_char(0, c)      |   same comment lineage; the raspi3b
|                             |   file does not need to move).
+-----------------------------+
              |
              v
+-----------------------------+
| kernel/drivers/tty.c        |   No changes. Same path raspi3b uses.
| /dev/kbd / shell stdin      |
+-----------------------------+
```

The three files map roughly to the three layers above. Splitting them
keeps the xHCI register-soup out of the HID class code and makes M9's
"add USB mouse" trivial (just add a second usb_mouse.c that talks to
the same xhci.c).

## Sub-milestones and commit ladder (planned)

Sub-milestones are sequential and each lands on `main` before the next
starts. This keeps commits reviewable and lets the user pause between
sub-milestones if M8 turns out to take longer than expected.

### M8.1 — PCIe enumeration of RP1

| # | Commit |
|---|---|
| 1 | `arm64/raspi5: PCIe2 root complex MMIO base + link bring-up (M8.1 commit 1)` |
| 2 | `arm64/raspi5: RP1 endpoint config-space read + BAR map (M8.1 commit 2)` |
| 3 | `docs/ch32-raspi5: PCIe + RP1 bring-up section (M8.1 commit 3)` |

**Verification gate:** serial trace prints RP1 vendor ID, device ID, and
the xHCI BAR physical addresses; matches the bcm2712.dtsi expectations.

### M8.2 — xHCI host bring-up

| # | Commit |
|---|---|
| 4 | `arm64/raspi5: xHCI capability + operational reg read (M8.2 commit 1)` |
| 5 | `arm64/raspi5: xHCI command ring + event ring init (M8.2 commit 2)` |
| 6 | `arm64/raspi5: xHCI controller reset + run (M8.2 commit 3)` |
| 7 | `arm64/raspi5: xHCI root-port reset + USB device-attached event (M8.2 commit 4)` |

**Verification gate:** plugging a USB device of any kind into a Pi 5
USB-A port produces an `xHCI: port N device attached` serial line.

### M8.3 — USB enumeration + HID keyboard

| # | Commit |
|---|---|
| 8  | `arm64/raspi5: USB device enumeration (set address, get desc) (M8.3 commit 1)` |
| 9  | `arm64/raspi5: HID boot keyboard interrupt-in transfer (M8.3 commit 2)` |
| 10 | `arm64/raspi5: HID usage->ASCII + tty_input_char wiring (M8.3 commit 3)` |
| 11 | `arm64/raspi5: platform_usb_hci_register/poll wiring (M8.3 commit 4)` |
| 12 | `docs+memory: M8 USB keyboard ship note (M8.3 commit 5)` |

**Acceptance:** keyboard plugged into a Pi 5 USB-A port, characters
typed land at the shell prompt on the HDMI console without a serial
cable connected.

Expected total: 12 commits. Range 10-15 depending on how many xHCI
quirks the real hardware surfaces.

## Risks and unknowns to resolve in M8.1

1. **PCIe link training timing on BCM2712.** The pcie2 controller may
   need vendor-specific reset / PERST / 100ms-then-poll sequences. The
   Linux brcm-stb-pcie driver is the reference; that driver is ~2000
   LOC and very Linux-flavored. The MVP will hardcode delays and skip
   any error recovery (link-up or fail, no retries).

2. **RP1 enumeration shape.** RP1 may appear as a single endpoint with
   multiple BARs (one per xHCI controller, plus GPIO, eMMC, etc.), or
   as a multi-function device. The bcm2712-rpi-5-b.dtb DT node tells us
   exactly. Likely fn(0,0,0) with vendor 0x1de4 (Raspberry Pi).

3. **xHCI BAR placement in the PCIe outbound window.** The driver
   needs the BAR mapped into the existing L1[124] PCIe identity block
   (`0x1f_0000_0000 .. 0x1f_4000_0000`, established for RP1 UART0). If
   firmware places the xHCI BAR elsewhere, we widen the block or add an
   extra one — same pattern M6 used for L1[64].

4. **DMA addressing.** xHCI rings and device contexts must live in
   physical memory the controller can DMA to. BCM2712 has identity
   dma-ranges for the AXI bus the CPU sees (M7 discover phase already
   verified this for the VC4 mailbox), so kernel-heap allocations
   should be directly usable by the xHCI controller. Cache maintenance
   on rings is mandatory — same dc cvac / dc civac pattern M7 c5 used
   for the VC4 mailbox.

5. **Port routing.** Pi 5 has 4 USB-A ports (2 USB 3.0, 2 USB 2.0). The
   MVP will only enable port 1 of the first xHCI controller it finds;
   the user plugs the keyboard there. If the keyboard ends up on the
   USB 2.0 ports they may be served by a different xHCI controller —
   spec says we accept either.

## Verification

No QEMU model for Pi 5 USB; hardware-only.

After M8.1: serial line `raspi5 pcie: RP1 vendor=0x1de4 device=0x????
bar0=0x...`. After M8.2: `raspi5 xhci: port N device attached`. After
M8.3: typing on a USB keyboard at the HDMI console produces text at the
shell prompt; serial cable is no longer needed.

Regression baselines must hold: virt 204/204, virt-net-integration
204/204, virt-ramfb-fallback 184/184, raspi3b 151/151. None of the M8
files are linked into virt or raspi3b builds, so regression risk is
low.

## Out-of-scope explicitly recorded

- USB mouse → M9 (alongside the desktop-launch promotion).
- USB hub support → no immediate plan; user can plug the keyboard
  directly into the Pi.
- Hot-plug → no immediate plan; the keyboard must be present at boot.
- MSI / MSI-X → no plan; polling at ~125 Hz is sufficient and avoids
  RP1 + BCM2712 interrupt-routing complexity.
- USB 3.0 SuperSpeed device negotiation → most modern keyboards work
  at USB 2.0 / Full-Speed; the driver targets the highest speed the
  port + device negotiate but does not require SuperSpeed.
- HID report descriptor parsing → boot protocol is fixed 8-byte
  format; report descriptors are skipped (SET_PROTOCOL = BOOT).
