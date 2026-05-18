# M7 — Pi 5 HDMI framebuffer (bare-metal `/dev/fb0`)

Date: 2026-05-18
Branch base: `main` @ `22d71d2` (M6 tip), plus `7f8c707` (firmware blobs).
Target hardware: Raspberry Pi 5 (BCM2712).

## Goal

Bring up a working HDMI framebuffer on real Pi 5 silicon so the kernel can
publish `/dev/fb0`, the in-kernel `fb_text_console` can render boot messages on
the attached display, and the existing desktop compositor (already proven on
virt + ramfb under M3.x) can paint onto the same surface. **Display only** —
input still comes from the serial console until M8 brings USB HID.

This is the bare-metal counterpart to the M3 series on QEMU virt. After M7,
"Drunix boots a desktop on real silicon" becomes true, modulo input.

## Why this is M7 (vs. the other M6 candidates)

M6's project memory listed five M7 candidates: HDMI fb, USB host (xHCI),
RP1 ethernet, SD write support, push M5/M6 to origin. M7 = HDMI because:

- It mirrors M5/M6's pattern of one big visible "first" per milestone, this
  time the visual one.
- It parallels M3 on virt, so it leverages the existing fbdev / fb_text_console
  / compositor stack rather than designing new infrastructure.
- USB and ethernet both depend on substantial new driver work (xHCI, RP1 PCIe
  enumeration) with no visible payoff on their own; they become M8/M9.
- SD write is incremental rather than a milestone.
- M5/M6 already on `origin/main` as of `22d71d2` (verified at spec time;
  the memory note about a backlog is stale and will be cleared in a
  follow-up memory update).

## Scope

In scope:

1. Port `kernel/arch/arm64/platform/raspi3b/video.c` to a new
   `kernel/arch/arm64/platform/raspi5/video.c`. Same VC4 mailbox property
   protocol (channel 8), same tag list (`SET_PHYSICAL_SIZE`,
   `SET_VIRTUAL_SIZE`, `SET_DEPTH`, `SET_PIXEL_ORDER`, `ALLOCATE_BUFFER`,
   `GET_PITCH`), same XRGB8888 layout. Fixed mode 1024 × 768 × 32 (matches
   raspi3b and virt-ramfb).
2. Map BCM2712 mailbox MMIO. Identify the correct CPU-physical address for
   the VC4 mailbox on BCM2712 (see "Risks" below) and ensure it falls inside
   an existing identity-mapped 1 GiB Device block. Extend `platform_mm.c`'s
   `platform_extra_kernel_blocks()` if needed.
3. Translate the bus address returned by the mailbox to a CPU physical
   address. On Pi 3 this is a mask with `0x3FFF_FFFF` (the VC4 alias of
   uncached SDRAM at `0xC000_0000` bus → `0x0000_0000` phys). On Pi 5 the
   bus-↔-phys relationship is different; document and verify before coding.
4. Identity-map (or reserve and map) the returned framebuffer physical range
   as Normal-WT or Normal-NC memory so kernel + user mmap can reach it.
   Reuse the existing fbdev mmap path (`/dev/fb0`).
5. Replace the framebuffer stubs in `kernel/arch/arm64/platform/raspi5/stubs.c`
   with real `platform_framebuffer_acquire()` and
   `platform_framebuffer_console_write()` that forward to the new video
   driver, mirroring raspi3b.
6. Wire the new `video.o` into `kernel/arch/arm64/arch.mk`'s raspi5 object
   list, alongside the already-included `fb_text_console.arm64.o`.
7. Verify on real Pi 5 hardware: kernel boots, prints to serial AND HDMI in
   parallel, `/bin/shell` runs over serial, `cat /proc/fb` (or equivalent)
   reports the new framebuffer, and a simple userland test (e.g. fill `/dev/fb0`
   with a solid colour) visibly paints the screen.
8. Push the accumulated M5 + M6 + M7 commits to `origin/main` as the M7 closer
   (clears the "local only" backlog noted in the M5/M6 memory entries).

Out of scope (deferred):

- USB HID input / xHCI / interactive desktop on real silicon → M8.
- RP1 ethernet → M9.
- Dirty-rect ioctl wiring for Pi 5 (already present in shared compositor;
  enable once we have input and can drag windows).
- Hardware cursor plane on VC4 (no VC4 KMS equivalent of virtio-gpu's
  cursor surface; revisit if/when we want one).
- EDID parsing, resolution autodetect, multiple display heads. Hardcode
  1024 × 768 × 32 as the v0 mode.
- DMA-accelerated blits, VC4 3D, V3D, h264 hardware codec.

## Architecture

Three units, each small and independently testable:

```
+-----------------------------+
| raspi5/video.c              |   New. Port of raspi3b/video.c.
|  - mailbox_call()           |   Generic VC4 property-channel sender.
|  - build_request()          |   Same tag list as Pi 3.
|  - validate_response()      |   Same checks.
|  - bus_to_phys()            |   NEW. Pi-5-specific translation.
|  - arm64_video_init()       |   Same signature; calls into bus_to_phys.
|  - platform_framebuffer_*() |   Stub replacements.
+-----------------------------+
              |
              v
+-----------------------------+
| raspi5/platform_mm.c        |   Extend platform_extra_kernel_blocks()
|  +mailbox block if needed   |   if BCM2712 mailbox isn't already inside
|  +fb block (or pmm reserve) |   L1[64]/L1[65]. Reserve fb phys via pmm.
+-----------------------------+
              |
              v
+-----------------------------+
| kernel/console/             |   Already arm64-built; no changes.
|   fb_text_console.c         |
+-----------------------------+
              |
              v
+-----------------------------+
| kernel/drivers/fbdev.c      |   Already exists; no changes.
| /dev/fb0                    |   Picks up the platform fb automatically.
+-----------------------------+
```

The fbdev driver and the fb_text_console already work — what's new is purely
the BCM2712 mailbox client plus a few mapping/translation details.

## Risks and unknowns to resolve in c1

1. **VC4 mailbox MMIO base on BCM2712.** On Pi 3 it's
   `PLATFORM_PERIPHERAL_BASE + 0xB880`. On Pi 4 it moved to `0xFE00_B880`. On
   Pi 5 the layout was reworked again: the VC4 lives off the south bridge
   reached through the `soc@107c000000` ranges in the device tree. Verify
   against `bcm2712-rpi-5-b.dtb` (`mailbox@7c013880` is the candidate node)
   before coding. Likely CPU phys: `0x10_7C01_3880`, which falls inside the
   already-mapped `L1[65]` (HIGH window). If true, no new mapping is needed.

2. **Bus → physical translation.** On Pi 3 the mailbox returns a VC4-bus
   address in `0xC000_0000+` and the driver masks with `0x3FFF_FFFF`. On Pi 5,
   per the brcm DT `dma-ranges`, the legacy uncached alias may not be used at
   all — the firmware may return CPU-physical directly, or a "low" bus address
   that maps to phys via a different offset. Plan: print the raw value in c1,
   then derive the translation experimentally on first boot.

3. **Framebuffer physical placement.** VC4 firmware may allocate the fb above
   the kernel's current 2 GiB linear-map ceiling (the M5 limitation called
   out in `docs/ch32-raspi5-bringup.md:65`). If it does, widen the linear map
   (`L1[1]` → `L2` split, or just identity-map an extra block covering the fb
   physical range) before writing pixels. The MVP can request a low fb by
   constraining VC firmware via `config.txt` if firmware-side controls exist;
   otherwise widen the map.

4. **Cache attributes for the framebuffer mapping.** The desktop performance
   roadmap (item 2) calls out that user `mmap(/dev/fb0)` should be
   write-combine on x86. Pi 5 equivalent is `Normal-NC` (Normal non-cacheable)
   or `Device-GRE` for the user mapping. Match what raspi3b's mapping uses;
   don't introduce a new policy in M7.

## Verification

QEMU has no Pi 5 model, so verification is hardware-only:

- `make ARCH=arm64 PLATFORM=raspi5 RASPI5_UART=jstsh pi5-sd.img`
  rebuilds the SD image including the new video.o.
- `sudo dd if=pi5-sd.img of=/dev/rdiskN bs=1m` flashes it.
- Boot the Pi with HDMI + a USB-UART on the debug-probe header.
- Expected serial trace: existing boot output plus `arm64_video_init: fb @
  0xXXXXXXXX, pitch=4096, 1024x768x32`.
- Expected HDMI: kernel boot lines render via `fb_text_console`. The terminal
  cursor lands at the bottom of the screen. After `/bin/shell` starts, a
  userland smoke (`/bin/dd if=/dev/zero of=/dev/fb0 bs=4096 count=768` then
  fill with red via a tiny test program) visibly paints the screen.
- Regression sweep: `make check` (virt 204/204, virt-net-integration 204/204,
  virt-ramfb-fallback 184/184, raspi3b 151/151). The new file is raspi5-only,
  so no virt/raspi3b regressions are expected.

## Commit ladder (planned)

1. `arm64/raspi5: VC4 mailbox base + bus-addr translation (M7 commit 1)` —
   constants + bus_to_phys helper + identity-mapping verification, no driver
   yet. Print the mailbox MMIO and a first probe response to serial; confirm
   layout before writing the rest.
2. `arm64/raspi5: VideoCore HDMI framebuffer driver (M7 commit 2)` — port
   raspi3b/video.c as raspi5/video.c, replace stubs, wire arch.mk.
3. `arm64/raspi5: fb-physical identity map (M7 commit 3)` — only if c1's
   probe shows the fb lands outside existing identity-mapped blocks.
4. `docs/ch32-raspi5: HDMI bring-up section (M7 commit 4)` — append to
   ch32, document mailbox base + bus-phys quirks for the next BCM-class
   board. Update memory snapshot file in a separate session.
5. `arm64/raspi5: HDMI smoke test (M7 commit 5)` — small userland helper
   (e.g. `user/test/fbfill.c`) that opens `/dev/fb0`, fills it, sleeps.
   Built into the raspi5 disk image so the verification step is trivial.

Each commit must build clean for `ARCH=arm64 PLATFORM=raspi5` and not regress
virt/raspi3b. The implementation plan (next step, via writing-plans) will
expand each into ordered steps with intent statements and checkpoints.
