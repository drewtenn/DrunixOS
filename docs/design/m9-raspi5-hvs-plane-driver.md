# M9 — Pi 5 HVS plane driver (fingerprinted plane-hijack)

Date: 2026-05-18
Branch base: `main` @ `01e66b3` (M7 polish + ch33 docs tip; M8 paused at M8.1).
Target hardware: Raspberry Pi 5 (BCM2712, HVS6, PixelValve PV0, HDMI0).
Predecessor: M7 (HDMI framebuffer via firmware mailbox).
Status: planning. No code written; this document is the deliverable.

## Goal

Replace the visible top-to-bottom redraw sweep that occurs every time the Pi 5
console scrolls. The kernel currently writes its single-buffered firmware-allocated
scanout fb directly with the CPU, and the HDMI raster catches each rewritten
scanline in source order, producing a slow visual band that crawls down the screen.

The first viable fix is **Path A\* (fingerprinted plane-hijack)**: own a pair of
back/scanout buffers in cached normal RAM, identify the firmware's active HVS
plane entry at boot, validate it against a structural fingerprint, then rewrite
only its `PTR0` upper-address word and `PTR1` lower-32 word on every present.
The HVS scans out from Drunix-owned memory; firmware-configured PV0 timings and
HDMI PHY state stay untouched.

This is the Pi 5 equivalent of the back-buffer + bulk-blit pattern that
`vc4-kms-v3d` uses on Linux, scoped down to what a hobbyist kernel can actually
land without a public BCM2712 datasheet.

## Why this is M9 (not M8, not the full vc4-kms-v3d equivalent)

The legacy mailbox property interface was proven structurally incapable of
hardware pan scrolling during the M7 polish window: firmware echoes
`SET_VIRTUAL_SIZE` in the response but `ALLOCATE_BUFFER` always returns exactly
one EDID-sized frame (`virt_h=0x870` echoed, `virt_actual_h=0x438` actual,
`size=0x7e9000` = `1920×1080×4`). There is no value the mailbox shim will
accept that gives us a larger virtual buffer to pan within. The only way to get
fast tear-bounded scrolling on Pi 5 is to bypass the mailbox shim and program
the HVS directly.

Two architectural shapes were considered and rejected for M9:

- **Path B — Channel Ownership.** Allocate Drunix's own dlist region in HVS SRAM
  (after `HVS_BOOTLOADER_DLIST_END = 32` words, per Linux convention), build a
  Drunix-native plane dlist, point `SCALER6_DISP0_LPTRS` at our dlist head,
  reset and re-enable `SCALER6_DISP0_CTRL0`. Architecturally cleaner: Drunix
  defines the world rather than parsing firmware-private state. Gemini argued
  for this in the Define phase; **deferred to M10** because the LPTRS rewrite
  is the highest-risk single moment in the bring-up (a malformed dlist black-screens
  the channel until power cycle), and because the productive payoff (multi-plane
  composition, hardware cursor support) is M10-class scope.

- **Path C — Full HVS + PV + HDMI ownership.** The complete `vc4-kms-v3d`
  equivalent. Roughly the size of Linux's `drivers/gpu/drm/vc4/` (~10K LOC).
  Not in scope for any single Drunix milestone; out of M-budget.

M8 (USB keyboard) remains paused at M8.1 with the PCIe-to-RP1 link in a
non-responsive state. M9 is HDMI-pipeline-only and does not touch PCIe; M8
can resume independently when there's appetite for further PCIe-reset work.

## What "Path A\*" means precisely

`*` is the fingerprint guard. Concretely:

1. Map the BCM2712 HVS MMIO window `[0x107c580000, 0x107c59a000)` (size
   `0x1a000`) as Device-nGnRnE. Already inside the existing L1[65] high-peripheral
   identity-map block from M6, so no new TT entries.
2. Read `SCALER6_DISP0_CTRL0` (channel 0 control), `SCALER6_DISP0_LPTRS`
   (active dlist head pointer), and `SCALER6_DISP0_DL` (latched active dlist
   pointer). Locate the firmware's primary plane entry in HVS dlist SRAM
   (mapped at HVS base + `0x4000`).
3. Parse the candidate plane's dlist words and assert all of:
   - Header (`SCALER6_CTL0`) declares format = XRGB8888 (32 bpp, alpha-ignored).
   - Position word (`SCALER6_POS0`) = `(0, 0)`.
   - Scaled-size word (`SCALER6_POS1`) = `(1920, 1080)` (or whatever EDID
     resolution the mailbox reported; not hard-coded).
   - Source-size word (`SCALER6_POS2`) = `(1920, 1080)`.
   - `PTR0`+`PTR1` together encode a 40-bit address matching the mailbox-reported
     `fb_phys = 0x3f400000`.
   - Pitch word matches `pitch = 0x1e00`.
   - Dlist terminates with a sane end marker within the bootloader-reserved range.
4. If any check fails → log `raspi5 hvs: firmware plane fingerprint mismatch
   (reason)`, leave HVS untouched, stay on the mailbox framebuffer. No further
   HVS writes that boot.
5. If all checks pass → save the original `PTR0`/`PTR1` words for restore,
   allocate two cached Pi 5 scanout buffers (`pitch × height × 2` total),
   render the existing console contents into the first one, CVAC-clean the
   touched range, then atomically rewrite the active plane's `PTR0`/`PTR1`
   to point at the new buffer. From this point forward, present is a
   `memmove`/`memcpy` into the back buffer + a CVAC + an `PTR0`/`PTR1`
   rewrite at the next vblank.

The fingerprint is the entire Drunix-vs-firmware contract. If the RPi firmware
team ships a release that uses a different plane format, adds a logo overlay,
or changes dlist word ordering, Drunix detects this on the next flash and
falls back to the mailbox path; nothing breaks. The known-bad case is
"firmware emits a plane with the same shape as we expect but living somewhere
else in the dlist"; we mitigate by walking the dlist starting from the
`LPTRS`-named head, bounding the walk to HVS SRAM extents, and refusing to
proceed on any out-of-range pointer.

## Phase breakdown

Each phase ships independently. Each phase boots a working kernel + console
(via mailbox fallback if HVS path isn't yet live). Recovery from any failure
is "power cycle"; no firmware reflash should ever be required.

### M9.1 — Passive HVS observability

**Goal.** Read-only HVS state dump so every Pi 5 boot reports the firmware's
HVS channel layout without touching display behavior.

**Code changes (estimated 220–320 LOC):**
- `kernel/arch/arm64/platform/raspi5/hvs.h` — HVS MMIO base + register
  offsets (`SCALER6_DISP0_CTRL0/1/LPTRS/DL/STATUS`, dlist SRAM base, channel-
  span constants), with provenance comments citing the relevant Linux files
  (`drivers/gpu/drm/vc4/vc4_regs.h`, `vc4_hvs.c`) without copying source.
- `kernel/arch/arm64/platform/raspi5/hvs.c` — `raspi5_hvs_probe_passive()`
  that reads the channel control + LPTRS + DL registers and traces them on
  serial. Bounded reads only; refuses any address outside the HVS window.
- `kernel/arch/arm64/platform/raspi5/video.c` — invoke `raspi5_hvs_probe_passive()`
  after the existing mailbox path succeeds.
- `kernel/arch/arm64/platform/raspi5/arch.mk` — add `hvs.o`.

**Test.** Flash `pi5-sd.img`. Serial trace shows HVS channel control word,
LPTRS, DL, and the first ~16 dlist words around the active head. HDMI
console still works via existing mailbox framebuffer. `make check` for virt
and raspi3b is unchanged.

**Risk.** Bad MMIO read causes a kernel exception. Mitigation: the M6
identity-map already covers HVS MMIO; probe runs *after* the mailbox path
has already proven the high-peripheral block reads work; only reads occur,
no writes.

### M9.2 — Firmware dlist locator + fingerprint validator

**Goal.** Reliably identify the firmware's active primary plane and prove
its structural fingerprint matches the mailbox framebuffer before any
mutation is allowed.

**Code changes (estimated 250–400 LOC):**
- `hvs.c` — bounded dlist walker that follows `LPTRS` head, parses
  one plane entry, terminates on out-of-range pointer or word-count
  overflow.
- `hvs.h` — new `raspi5_hvs_plane_ref_t` struct (dlist offset, word
  indices, saved `PTR0`/`PTR1`, parsed format, position, dimensions, pitch).
- `hvs.c` — fingerprint matcher comparing parsed plane against expected
  shape derived at runtime from `g_fb_info` (geometry, pitch, format).
- `video.c` — invoke locator after probe; record result; no behavior change.

**Test.** Flash `pi5-sd.img`. Serial trace either reports
`raspi5 hvs: firmware plane found dlist_off=0xN pitch=0x1e00 phys=0x3f400000`
or `raspi5 hvs: firmware plane fingerprint mismatch (reason)`. HDMI
console behavior is unchanged either way.

**Risk.** Parser follows a bogus dlist pointer and reads garbage or loops.
Mitigation: bound all offsets to HVS SRAM `[0x4000, 0x1a000)`, cap walk
length at 64 words per plane and 256 words total, fail closed on the first
out-of-range deref or unrecognized opcode. Never write.

### M9.3 — Guarded one-shot plane-hijack flip

**Goal.** Prove Drunix can point the firmware plane at a Drunix-owned scanout
buffer and recover the original scanout state on demand.

**Code changes (estimated 220–360 LOC):**
- `video.c` — `raspi5_video_alloc_scanout()` that allocates one
  page-aligned, framebuffer-sized buffer via the existing PMM, mapped
  Normal-WB.
- `video.c` — render the current console image into the new buffer
  (memcpy from the firmware scanout) then `DC CVAC` over the affected range.
- `hvs.c` — `raspi5_hvs_flip_active_plane_phys()` that validates the target
  buffer phys range, writes the new `PTR0`/`PTR1`, and `DSB`s.
- `hvs.c` — `raspi5_hvs_restore_active_plane()` writes back the saved
  `PTR0`/`PTR1`. Used by the test harness; not on the boot path.
- Gate the flip behind a compile-time flag `DRUNIX_RASPI5_HVS_FLIP=1` so
  default builds remain mailbox-only.

**Test.** Build with `DRUNIX_RASPI5_HVS_FLIP=1`. Flash. Serial shows one
flip attempt; HDMI now displays Drunix's copy of the console (visually
identical, but coming from a different buffer). After 10 seconds, invoke
restore from a kernel task; HDMI returns to firmware-allocated scanout.
Power cycle if HDMI goes black at any point.

**Risk.** `PTR0`/`PTR1` point at wrong address or wrong pixel layout →
HDMI scans out garbage or goes black. Mitigation: flip target must pass
the same fingerprint as the source (same dimensions, same pitch, same
format); validate alignment to 16-byte HVS DMA constraint; gate off by
default so a regression doesn't ship to anyone running the default build.

### M9.4 — PV0 vblank IRQ + flip queue

**Goal.** Move HVS pointer updates into a vblank-driven path so flips are
synchronised to scanout and double-buffering becomes correct.

**Code changes (estimated 280–480 LOC):**
- `kernel/arch/arm64/platform/raspi5/pv.h`, `pv.c` — PixelValve PV0
  MMIO offsets (`PV_INT_EN`, `PV_INT_STAT`, with `PV_INT_VFP_START` bit
  defined per Linux `vc4_regs.h`), enable/ack helpers, monotonic vblank
  counter.
- `kernel/arch/arm64/platform/raspi5/irq.c` — register GIC SPI 101 for
  HDMI0 vblank using the existing GIC-400 dispatch path. Existing IRQ
  framework already handles SPI registration; this is a new client of
  it, not new infrastructure.
- `hvs.c` — single-slot pending-flip queue. The IRQ handler is tiny:
  ack PV0, snapshot pending phys, write `PTR0`/`PTR1`, increment counters.
  Self-disable after N consecutive bad-status frames (Drunix-conservative
  safety valve).
- `video.c` — replace the one-shot flip from M9.3 with a vblank-queued
  flip path. Default build still mailbox-only until M9.5.

**Test.** Build with `DRUNIX_RASPI5_HVS_FLIP=1` + `DRUNIX_RASPI5_HVS_VBLANK=1`.
Flash. Serial trace shows vblank counter ticking at ~60 Hz (or whatever
EDID mode the firmware negotiated), one flip completes from IRQ context,
no IRQ storm, no kernel hang. HDMI shows the Drunix-rendered console.

**Risk.** IRQ handler doesn't ack PV0 correctly → IRQ storm → CPU pegged
in IRQ context, kernel hangs. Mitigation: handler reads-then-clears
`PV_INT_STAT`, returns immediately if status word doesn't show `VFP_START`,
hard-disables the IRQ source after `IRQ_STORM_THRESHOLD = 16` consecutive
unexpected entries. Serial fallback always works because UART IRQ priority
is independent.

### M9.5 — Production fbdev integration

**Goal.** Default Pi 5 build uses HVS-flipping presentation; the visible
top-to-bottom scroll crawl disappears; mailbox fallback survives any
failure.

**Code changes (estimated 350–600 LOC):**
- `video.c` — at boot, attempt mailbox path first (unchanged); on success,
  attempt HVS probe + fingerprint + scanout alloc + vblank IRQ. If all
  succeed, switch the `fb_text_console`'s dirty/scroll hooks to write into
  the Drunix back buffer and queue an HVS flip on present. If any step
  fails, stay on the mailbox path.
- `kernel/gui/framebuffer.{c,h}` — use the existing
  `framebuffer_attach_back_buffer()` (already in tree, see
  `kernel/gui/framebuffer.h:724`) for the CPU render target. Add a Pi 5
  scanout-buffer field to a new platform-private struct, not to the
  generic `framebuffer_info_t`, so virt and raspi3b are not perturbed.
- `kernel/console/fb_text_console.c` — minor: the existing `scroll_pixels`
  hook already lets the platform supply a fast scroll. Hook it up so
  scrolling renders into the back buffer and triggers a flip rather than
  writing the live scanout.
- `kernel/drivers/fbdev.c` — Pi 5 path returns the back-buffer phys for
  `/dev/fb0` mmap (so userspace draws into the cached CPU buffer); the
  fbdev close/sync path triggers a flip. Other platforms (virt with ramfb,
  raspi3b with VC4 fb, virt with virtio-gpu) unchanged — they continue
  returning their own scanout phys with their existing cache attributes.
- `kernel/arch/arm64/platform/raspi5/platform_mm.c` — Normal-WB mapping
  for the Drunix-owned scanout/back buffers (new), independent of the
  Device-nGnRnE mapping the firmware scanout already has.

**Test.** Default build (no gate flags). Flash. Boot to shell over HDMI +
serial. Scroll a long file: no visible top-to-bottom crawl, scrolling
matches userland output rate without a draw lag. Run a 5-minute stress
(`yes | head -c 1G | head -c 200000`) — no panic, no IRQ storm, vblank
counter and flip counter stay synchronized. Regression sweep: virt
204/204, virt-net-integration 204/204, virt-ramfb-fallback 184/184,
raspi3b 151/151 (all unchanged from M4/M5/M6 baselines). Force-fail the
fingerprint check (e.g. by setting a fake mailbox geometry) and confirm
boot still reaches a shell over the mailbox-fallback path.

**Risk.** Mailbox + HVS double-allocation runs out of low RAM (the
firmware scanout at `0x3f400000` is ~8 MB; two Drunix back/scanout buffers
add another ~16 MB). Pi 5 has plenty of RAM but the kernel's identity-
mapped low region is small. Mitigation: allocate from PMM after the boot
stack reservation; if alloc fails, log and stay on mailbox path. Cache
incoherency between CPU writes and HVS DMA reads: explicit `DC CVAC` on
the touched byte range before queueing the flip; this is the choke point
where the WB cache attribute pays off.

## Integration decisions (resolved during Define)

**(a) Placement.** The new HVS code lives **alongside**
`kernel/arch/arm64/platform/raspi5/video.c`, not replacing it. New files:
`hvs.{c,h}` and `pv.{c,h}` under `kernel/arch/arm64/platform/raspi5/`.
`video.c` remains the Pi 5 display coordinator that decides whether the
mailbox path or the HVS path is in use, and where the public surface
(`/dev/fb0` registration, `fb_text_console` init) is wired.

**(b) Mailbox path retained as fallback.** Yes. Two reasons: (i) it is
the only Pi 5 HDMI path that's currently proven to work, so it's the
correct cold-boot fall-back when any HVS bring-up step fails; (ii) the
console must be visible before HVS instrumentation is safe, which means
the mailbox framebuffer needs to be live for at least the M9.1/M9.2
probe-only phases regardless of how M9.5 lands.

**(c) Cache attribute for Drunix-owned Pi 5 back/scanout buffers.**
`Normal-WB` (write-back cached) with explicit `DC CVAC` on the modified
byte range before each flip. Justification: the CPU writes the entire
visible content of every dirty rect, so cache locality matters for
performance. Linux uses this pattern for vc4 CMA scanout. **QEMU virt
ramfb continues to use Normal-NC unchanged**; the cache-attribute decision
is per-platform, not global.

**(d) `framebuffer_info_t.back_buffer` use.** The existing back-buffer
field (`kernel/gui/framebuffer.h:724`) becomes the CPU render target on
Pi 5 as on other platforms. The HVS-specific "currently-scanned phys"
lives in a new Pi 5 platform-private struct in `hvs.c`, not in the
generic `framebuffer_info_t`. This keeps platform specifics out of the
shared GUI layer.

## Acceptance criteria

M9 ships when all of the following hold simultaneously on `main`:

1. Default Pi 5 build (no gate flags) boots to the shell over HDMI and
   serial, with M9.5's production flip path active. Serial trace shows
   `raspi5 hvs: firmware plane found ...`, `pv0 vblank IRQ registered
   on SPI 101`, and one or more flip-success log lines before login.
2. **The top-to-bottom redraw sweep is gone.** Continuous shell scrolling
   matches the userland output rate; no visible band of in-progress
   redraw marches down the screen. Confirmed by visual inspection
   against a high-FPS phone camera capture.
3. No kernel panic, no IRQ storm, no console hang during a 5-minute
   sustained-scroll stress (`cat /dev/zero | head -c <large>` piped to
   a hex dump, or equivalent).
4. **Forced-failure path verified.** Deliberately fail the fingerprint
   check (a build flag that lies about expected geometry, or a serial
   command if that's cheaper). Boot still reaches a usable shell on
   the mailbox fallback. Serial trace shows the failure reason.
5. Power cycle restores firmware HDMI behavior without any SD reflash,
   from any M9.x state.
6. **No regressions on other platforms:** `make check` for
   `ARCH=arm64 PLATFORM=virt` (204/204), `virt-net-integration` (204/204),
   `virt-ramfb-fallback` (184/184), `raspi3b` (151/151), plus the x86
   suite. The HVS/PV files are gated by `PLATFORM=raspi5` in `arch.mk`,
   so cross-platform impact is structural rather than tested.
7. No LinuxKPI source-shim introduced. Implementation is Drunix-native
   C; Linux source cited only in provenance comments on register-offset
   constants.

## Open questions to settle before M9.1 lands

These were called out during Discover but did not resolve in the Define
debate. Resolve them on hardware (likely as part of M9.1's serial trace):

1. **Is HDMI0 always PV0 + HVS channel 0?** Linux's `bcm2712.dtsi`
   maps it that way and the Define-phase consensus assumed it, but the
   M9.1 probe should *verify* by reading the channel-control words for
   all three HVS channels and reporting which is enabled. If HDMI0 lives
   on a different channel, the channel index in `hvs.c` is parameterised
   at probe time, not hard-coded.
2. **Is the mailbox-allocated `0x3f400000` framebuffer inside an
   HVS-scannable PA range?** It's below the 4 GB ceiling that the
   32-bit mailbox interface enforces, but that doesn't automatically
   mean the HVS DMA path can reach it. M9.1 probes whether HVS scanout
   is currently active and confirms the channel's `PTR0`/`PTR1` point
   into the firmware-allocated range.
3. **Does the firmware install any overlay planes?** A second plane
   (logo, watermark, debug overlay) above the primary would mean the
   primary-plane fingerprint is not the topmost plane, and the visible
   "top-to-bottom redraw sweep" Drunix is trying to fix is the
   *primary* plane's update, not the only one. M9.1 walks the entire
   dlist and reports plane count + per-plane format.
4. **GIC SPI 101 vs Pi 5 DT.** Linux's DT for `bcm2712-rpi-5-b.dtb`
   maps PV0 vblank to SPI 101, but the FDT Drunix is parsing during
   boot should be the authoritative source. M9.4 should read the
   `pixelvalve0` node's `interrupts` property from the FDT rather
   than hard-coding 101.
5. **HVS channel reset semantics.** Linux pulses
   `SCALER6_DISPX_CTRL0_RESET` during init. Drunix never needs to
   pulse this in Path A\* — but if M9.3's restore path is invoked
   after a failed flip and the channel is wedged, a soft reset
   sequence may be needed. Document the sequence even if M9 never
   executes it.

## Cross-references

- Discover synthesis (multi-provider): `~/.claude-octopus/results/63d0c46a-a499-4e04-aa3c-a00ebf9a29b1/probe-synthesis-1779150120.md`
  (and per-provider files in the same directory).
- Define phase outputs:
  - Codex (Path A advocate): `/tmp/octo-embrace-hvs/define-codex-output.md`
  - Gemini (Path B advocate): `/tmp/octo-embrace-hvs/define-gemini-output.md`
- Debate gate transcripts (both providers converged on A\*):
  - `/tmp/octo-embrace-hvs/debate-codex.md`
  - `/tmp/octo-embrace-hvs/debate-gemini.md`
- Empirical proof of mailbox-shim's structural refusal:
  `boot trace 2026-05-18 with PAN_PAGES=2`:
  `virt_h=0x870` echoed, `size=0x7e9000`, `virt_actual_h=0x438`.

Linux source files cited for register provenance (read-only, no copying):
- `drivers/gpu/drm/vc4/vc4_hvs.c` — HVS dlist allocation, channel init.
- `drivers/gpu/drm/vc4/vc4_plane.c` — async-flip rewriting `PTR0`/`PTR1` of
  the active dlist, the model Path A\* follows.
- `drivers/gpu/drm/vc4/vc4_regs.h` — `SCALER6_*` register offsets and
  bitfield definitions.
- `drivers/gpu/drm/vc4/vc4_crtc.c` — PV0 vblank IRQ handling via
  `PV_INT_VFP_START`.
- `arch/arm64/boot/dts/broadcom/bcm2712.dtsi` — HVS / PV0 / HDMI0 IRQ map.

## v2 revisions from multi-provider Deliver review (2026-05-18)

Two reviewers (codex, gemini) examined the v1 plan and converged on one **fatal
correctness issue** plus a set of robustness gaps. Revisions below; each phase
above must be read with these amendments applied.

**FATAL (must fix before M9.3 ships):** `PTR0` is a packed control word, not
just the upper-address bits. On HVS6 it encodes both `[39:32]` of the 40-bit
physical address *and* the Unified Pre-Fetcher (UPM) configuration
(`SCALER6_PTR0_UPM_BASE`, `SCALER6_PTR0_UPM_HANDLE`) and a pointer-ID field
that the firmware sets up to match its specific buffer placement and AXI QoS.
The async-flip path in `vc4_plane_async_set_fb()` does a read-modify-write
preserving the non-address fields. **M9.3's flip helper must be RMW, masking
only the address bits.** Any blind `PTR0` write will clobber UPM, scanout
stops fetching, screen goes black. Same for `PTR2` if the plane has a Cr/Cb
plane (it doesn't for XRGB8888, but the principle stands).

The complete dlist word layout for an XRGB8888 plane on HVS6 is (per Linux
`vc4_plane.c`): `CTL0`, `POS0`, optional `CTL2` (when blending/CSC active),
optional `POS1` (only when not unity-scaled), `POS2`, `POS3` (context word),
then pointer words `PTR0`/`PTR1` (and `PTR2`/`PTR3` for multi-plane formats).
The v1 plan's "CTL0, POS0, POS1, POS2, PTR0, PTR1, PTR2" enumeration was
incomplete. M9.2's parser must handle the optional words and the unity-scale
shortcut.

**Fingerprint robustness — additional checks required in M9.2:**
- `ORDERRGBA` field in `CTL0` must be explicitly verified, not assumed. The
  firmware returned `pixel_order=0` (BGR) when asked for RGB; the HVS plane
  may be configured for BGR byte order, which Drunix's render path must match.
- `SCALER6_CTL0_UNITY` must be checked. Unity-scaled planes omit `POS1`, so
  a parser that hard-expects `POS1` falsely rejects the real primary plane.
- `CTL2` must not declare unexpected blend, alpha, or CSC modes. If
  firmware has configured these, Drunix's back buffer drawn at the same
  format will look wrong.
- HVS dlist supports branching (`SCALER6_CTL0_BRANCH` opcode). The walker
  must detect cycles and bound jump offsets, not just walk linearly.
- Firmware may be double-buffering (animated splash). The mailbox-reported
  `fb_phys` may point at the firmware's *back* buffer while `PTR0`/`PTR1`
  in the active dlist point at the *front*. The fingerprint must accept
  this case: parsed PTR address need not equal mailbox `fb_phys` — it
  needs to be a *valid* fb-sized region with matching pitch/geometry.
- Firmware may install a transparent overlay at `(0, 0)` with primary-plane
  dimensions (low-voltage warning, debug HUD, etc.). The locator must
  identify *the lowest-z plane*, not just *the first plane at (0, 0)*. Use
  `SCALER6_DISP0_DL` (latched active dlist offset) as the authoritative
  starting point, walk it through any branches, and pick the plane whose
  position/dimensions match the *visible scanout area* — not necessarily
  the first hit.

**Underrun / HVS QoS — new risk to track in M9.3 onward:** The firmware
configures HVS arbiter priority and UPM fetch QoS for the specific physical
location of its allocated framebuffer (`0x3f400000` in low RAM, AXI port X,
known latency window). Drunix's PMM-allocated buffer may live somewhere
with different AXI latency characteristics; the HVS may underrun (FIFO
empty mid-scanline) even if the address itself is valid. M9.1/M9.3
**must serial-log `SCALER6_DISPX_STATUS_EMPTY` and the EUFLOW / fetcher /
AXI error counters every second** during the gated-flip phase, so an
underrun shows up as a measurable counter increment rather than just
visual corruption.

**Cache attribute — slowness risk:** Cleaning an 8 MB framebuffer with
line-by-line `DC CVAC` is not free. On Cortex-A76 at 64-byte lines that's
131 072 cleans per full-frame present. M9.3 should benchmark this and
M9.5's design should narrow the CVAC range to the dirty rect (which
`fb_text_console`'s scroll path already knows), not the whole frame.
`framebuffer_attach_back_buffer()` (correct location:
`kernel/gui/framebuffer.h:72`, not 724 as v1 stated) already has a
dirty-rect concept that maps to this.

**Contiguous allocation — missing prereq:** The PMM currently exposes
`pmm_alloc_page()` (4 KB pages). HVS DMA requires the entire framebuffer
in physically contiguous memory; an 8 MB scanout buffer needs 2048
contiguous 4 KB pages. This **does not exist** in the current PMM. Two
options for M9.3, pick during M9.1:
- Boot-time CMA-style carve-out: reserve a contiguous block in
  `arch_mm_init` before `pmm_init` populates the free list. Smaller code,
  fixed-size reservation, must be sized for both back+scanout (~16 MB
  total at 1920×1080×4×2).
- Buddy-style large-page allocator: extend the PMM to support
  contiguous multi-page allocations with a max order. More code, more
  flexible, useful beyond M9.

Recommendation: boot-time carve-out for M9. Buddy allocator is its own
project.

**Missing subsystem — GIC SPI registration:** `kernel/arch/arm64/platform/raspi5/irq.c`
currently only dispatches the timer SGI and the comment explicitly states
"No SPI plumbing." M9.4 needs to add SPI registration to the GIC-400
driver before PV0 vblank can be wired. **Make this a M9.4 prerequisite
sub-step** (M9.4a = GIC SPI plumbing; M9.4b = PV0 vblank client of that
plumbing), not bundled into the PV0 work.

**fbdev cache attribute — userland surface change:** The current
`kernel/drivers/fbdev.c` returns `CHARDEV_CACHE_NC` for `/dev/fb0` mmap.
Switching Pi 5 to a Normal-WB back buffer means userland mmap behavior
changes for Pi 5 specifically. **This is a userland-visible change**
and belongs in its own phase. Split M9.5 into:
- **M9.5a — kernel-internal text console only.** `fb_text_console` writes
  into the Pi 5 back buffer, CVAC-cleans the dirty rect, queues an HVS
  flip via M9.4. `/dev/fb0` userland mmap behavior **unchanged** — keep
  returning the firmware scanout phys with NC mapping. Userspace `fbfill`
  still works, just doesn't get the back-buffer optimisation.
- **M9.5b — userland `/dev/fb0` migration.** Pi 5's `/dev/fb0` mmap
  returns the back buffer with WB attribute; an explicit
  `fbdev_present()` syscall or auto-sync on `msync` triggers an HVS
  flip. This is desktop-compositor scope and may slip to M10.

Apparent M9.5 LOC of 350-600 becomes M9.5a ~200 + M9.5b ~250-400.

**M9.3 honesty — temporary tearing:** Until M9.4 lands, M9.3's flips happen
asynchronously to scanout. The HVS picks up the new PTR0/PTR1 mid-frame
(non-vblank-latched on gen6). **Document this as a temporary visual
regression** so M9.3 isn't mistaken for the final solution: horizontal
tear during scroll instead of the top-to-bottom crawl. Tear is faster
than crawl, but isn't tear-free; tear-free is M9.4's payoff.

**Optional M9 simplification (gemini's suggestion, not adopted but
recorded):** Drop M9.4 from M9 entirely. Live with the horizontal tear
from M9.3's pre-vblank flips. Saves the GIC SPI plumbing + PV0 IRQ work
(~400 LOC) and removes IRQ-storm risk surface. Tear-free becomes M10.
**Rationale for not adopting:** the IRQ infrastructure from M9.4 is
load-bearing for any future input/event-pacing work (HVS cursor animation,
vsync-paced animation), so it's worth landing in M9 even if the immediate
payoff is just "no tear" rather than "much faster than crawl."

**Acceptance criteria — add measurable counters:** the v1 acceptance
relied on "visible inspection" via phone camera. Add serial-trace
counters so success is also numerically verifiable: present requests,
completed flips, dropped flips (back-pressure), HVS underruns,
bad-status frames, PV0 vblank tick rate. M9-complete also requires
underrun counter == 0 over the 5-minute stress.

**Revised phase summary:**

| Phase   | LOC est.   | Notes                                                                  |
|---------|------------|------------------------------------------------------------------------|
| M9.1    | 220-320    | Unchanged. Pick allocator strategy here.                               |
| M9.2    | 250-400    | Strengthened fingerprint (ORDERRGBA, UNITY, branches, lowest-z plane). |
| M9.3    | 280-420    | PTR0 RMW preserving UPM. Underrun counters. Marked as tear-visible.    |
| M9.4a   | ~150       | NEW: GIC SPI registration in `kernel/arch/arm64/platform/raspi5/irq.c`.|
| M9.4b   | 250-400    | PV0 vblank client of M9.4a.                                            |
| M9.5a   | ~200       | Kernel `fb_text_console` only. `/dev/fb0` mmap unchanged.              |
| M9.5b   | 250-400    | Userland `/dev/fb0` WB migration. May slip to M10.                     |

Estimated revised M9 (without 9.5b): 1350-1890 LOC. With 9.5b: 1600-2290 LOC.

The v1 commit-ladder section above should be re-read as: commits 1-5 map
to M9.1, M9.2, M9.3, M9.4 (which is now two commits 4a+4b), and M9.5a.
M9.5b becomes commit 7. The docs commit is now 8.

**Cross-references for the revisions:**
- Codex review: `/tmp/octo-embrace-hvs/deliver-codex.md`
- Gemini review: `/tmp/octo-embrace-hvs/deliver-gemini.md`

## v3 — empirical findings from M9.1 on Pi 5 D0 hardware (2026-05-18)

M9.1 v3 was iterated three times on real Pi 5 silicon. The first two boot
traces falsified the v1 plan's HVS5-derived register layout assumptions; the
third confirmed a clean BCM2712 D0 picture and discovered the firmware's
plane shape directly. These findings are load-bearing for M9.2 and M9.3
and amend the corresponding sections above.

**Silicon identity.** `SCALER6_VERSION` lives at HVS MMIO offset `0x000`.
Low byte is the chip revision: `0x54` = BCM2712 D0 (current shipping Pi 5),
`0x53` = BCM2712 C0 (older revision). Detected at probe start; per-channel
read layout chosen accordingly. Unknown revision → skip per-channel reads,
keep the wide-scan + dlist-scan dumps as the source of truth.

**HVS6 D0 layout (different from the HVS5/codex-Discover assumption).**

| Item | D0 value | C0 value (HVS5 carryover) |
|---|---|---|
| Channel 0 base | `+0x100` | `+0x030` |
| Channel 1 base | `+0x140` | `+0x050` |
| Channel 2 base | `+0x180` | `+0x070` |
| Channel stride | `0x40` | `0x20` |
| `DISP_CTRL0` within channel | `+0x00` | `+0x00` |
| `DISP_CTRL1` within channel | `+0x04` | `+0x04` |
| `DISP_LPTRS` within channel | `+0x10` | `+0x0c` |
| `DISP_DL` within channel | `+0x1c` | `+0x18` |
| LPTRS/DL head pointer mask | `0x0fff` (12 bits) | `0x0fff` |

The v1 plan stated channel registers spanned `+0x30..+0x4c` and the head
mask was 16 bits. Both were wrong for HVS6. The v3 hvs.c encodes both
revisions and selects at runtime.

**Active scanout topology.** On the boot trace:

| Channel | `CTRL0` | Enabled | LPTRS / DL | Role |
|---|---|---|---|---|
| 0 | `0x877f0437` | yes (bit 31) | `0x20` | HDMI0, firmware framebuffer, 1920×1080 |
| 1 | `0x827f01df` | yes (bit 31) | `0` | HDMI1/composite, 640×480, no dlist (parked) |
| 2 | `0x00000000` | no | `0` | unused |

The `CTRL0` low 24 bits encode the visible mode as `(H-1) << 16 | (W-1)`:
`0x437=1079`, `0x77f=1919` → 1920×1080. The bit-31 ENABLE indicator is on
`CTRL0` for D0 (not on `CTRL1` as the v2 probe initially hypothesised);
the v3 probe checks both bits and treats either as "enabled" so the trace
stays unambiguous regardless.

**Firmware plane shape (the load-bearing finding).** Channel 0's
LPTRS=DL=`0x20` says the active plane head is at dlist word 32, i.e. byte
offset `0x4080` in HVS dlist SRAM. The plane is 8 words, terminated by
the HVS scratch marker `0xb0b0b0b0`:

| dlist word | offset | Observed value | Role |
|---|---|---|---|
| 0 | `0x4080` | `0x600cc007` | **CTL0** — pixel format = 7 (XRGB8888), control flags |
| 1 | `0x4084` | `0x00000000` | POS0 = (0, 0) |
| 2 | `0x4088` | `0x0000fff0` | CTL2 / bootloader scratch padding |
| 3 | `0x408c` | `0x0437077f` | **dimensions** = (H-1)<<16 \| (W-1) = 1920×1080 |
| 4 | `0x4090` | `0x80000???` | **PTR0** — *VOLATILE*, do not write (see below) |
| 5 | `0x4094` | `0x00000000` | **address[63:32]** (= 0 for sub-4GB) |
| 6 | `0x4098` | `0x3f400000` | **address[31:0]** = mailbox fb_phys |
| 7 | `0x409c` | `0x00001e00` | pitch = 7680 bytes (matches mailbox) |
| 8 | `0x40a0` | `0xb0b0b0b0` | HVS end-of-plane scratch marker |

**Critical correction to the deliver review's PTR0-clobber finding.** The
deliver review predicted that `PTR0` would pack the buffer's upper-address
bits with UPM configuration, requiring a read-modify-write that preserves
UPM. On D0 HVS6 the layout we observe is different: **the buffer address
is in words 5 and 6 (separate from `PTR0`)**, and `PTR0` (word 4) carries
something the firmware-and-hardware update *together* between every read
— across three boots dl[4] varied from `0x80000376` to `0x8000025a` to
`0x80000100` while the rest of the plane stayed identical. dl[4] is
read-only-effectively from the kernel's perspective; it's either a
per-frame status (line counter, fetch progress) or a hardware-updated
control word.

The implication: **M9.3 must never write dl[4]** but does not need an
RMW dance either. The flip helper writes only dl[5] (high 32) and dl[6]
(low 32) of the new buffer's physical address. The current Drunix PMM
allocates sub-4GB buffers, so dl[5] stays 0 in steady state and only dl[6]
changes per flip.

This collapses M9.3 from "address-bit RMW preserving UPM in PTR0" (~280–420
LOC for the careful masking + edge cases) to "two MMIO word writes plus a
DSB barrier" (~30–50 LOC for the helper, the rest of the M9.3 LOC budget
goes to the contiguous allocator and the gated-flag plumbing). Net
M9.3 estimate revised down to ~180–240 LOC.

**Fingerprint shape concretely known.** M9.2's validator is now a fixed
check: read 9 specific words at dlist `[0x4080 .. 0x40a0]` and assert:

```
dl[0] & 0xffff00ff == 0x600c0007    (CTL0 — fixed-format part of control word)
dl[3]              == 0x0437077f    (dimensions — derived from mailbox geometry)
dl[5]              == 0x00000000    (sub-4GB only on current firmware)
dl[6]              == 0x3f400000    (matches mailbox fb_phys)
dl[7]              == 0x00001e00    (matches mailbox pitch)
dl[8]              == 0xb0b0b0b0    (HVS end-of-plane marker)
```

dl[3], dl[6], dl[7] are computed from `g_fb_info` at validation time rather
than hard-coded so a different EDID-negotiated mode still validates.
dl[2] is left unchecked (its `0x0000fff0` value looks like firmware-private
scratch, not a stable contract). dl[1] and dl[4] are explicitly *not*
checked: dl[1] is always 0 here but could be non-zero on different firmware
versions, and dl[4] is volatile.

If the validator finds the plane shape matches, M9.2 records:
`raspi5_hvs_plane_ref_t = { dlist_offset=0x4080, addr_lo_word=6,
addr_hi_word=5, saved_addr_lo=0x3f400000, saved_addr_hi=0x00000000 }`.
M9.3 reads these fields directly; no further dlist parsing at flip time.

**Other useful trace artifacts (not load-bearing for M9 but recorded
for future milestones):**

- `mmio[0x0118]` (ch0) and `mmio[0x0158]` (ch1) vary across boots
  (~`0xb1xxxx` ↔ `0xb6xxxx`). Likely line counter / fetch progress.
  Snapshotting these per-vblank in M9.4 will give the underrun /
  EUFLOW counters the deliver review asked for.
- `mmio[0x4000..0x402c]` is a horizontal scaler filter coefficient
  ROM, mirrored. Not in the dlist's path of execution on this firmware
  (CTL0 indicates unity-scaled).
- `mmio[0x0194..0x01a8]` shows channel 2 has partly-written registers
  despite `CTRL0=0`. Power-domain or arbiter state; not relevant to M9.

These findings amend rather than replace the v2 plan; the phasing and
acceptance criteria from v1+v2 still stand, just with the LOC estimate
reductions and the PTR0-clobber concern resolved by direct silicon
observation.

## What this plan does *not* commit to

- **Hardware cursor plane** (which would need a second HVS plane composed
  on top of the primary). Not in M9; that's M10 along with the channel
  ownership shape.
- **HDMI mode-set** (changing resolution or refresh). The firmware's
  EDID-negotiated mode stays. Drunix never touches the HDMI PHY or PV
  timing registers in M9.
- **Page-flip on every present.** The async-flip path is *available* once
  M9.4 lands, but the M9.5 default may be lower-frequency (flip on every
  vblank, not on every dirty rect) to keep IRQ load proportional to the
  refresh rate rather than the application's draw rate. Tune in M9.5.
- **GPU acceleration (V3D)**. Out of scope. M9 is composition pipeline
  ownership at the HVS layer, not 3D rendering.
- **Suspend/resume**. Drunix has no suspend path yet.

## Commit ladder (planned)

Each commit must build clean for `ARCH=arm64 PLATFORM={raspi5,virt,raspi3b}`
and not regress the existing test suites. Each commit corresponds to a
phase above.

1. `arm64/raspi5: HVS6 passive observability (M9.1 commit 1)` — register
   constants + `hvs.h/c` skeleton + serial-only probe wired from `video.c`.
2. `arm64/raspi5: firmware dlist locator + fingerprint validator (M9.2)` —
   bounded walker, result struct, fail-closed semantics. Still read-only.
3. `arm64/raspi5: guarded one-shot HVS plane flip (M9.3)` — gated by
   `DRUNIX_RASPI5_HVS_FLIP`. PMM-allocated scanout buffer + flip helper +
   restore helper. Default build behavior unchanged.
4. `arm64/raspi5: PV0 vblank IRQ + flip queue (M9.4)` — `pv.{c,h}`,
   GIC SPI registration, IRQ handler, single-slot pending flip,
   self-disable on storm. Gated build.
5. `arm64/raspi5: HVS production path + fbdev integration (M9.5)` —
   default build switches to HVS path on success; mailbox stays as
   fallback; `fb_text_console` scrolls into the back buffer; `/dev/fb0`
   userspace mapping returns the back buffer.
6. `docs/ch33: HVS plane driver section (M9 commit 6)` — append to
   the stand-up chapter so an operator who flashes a new SD card knows
   how to interpret the HVS bring-up trace lines.

The implementation plan (next step, via the `superpowers:writing-plans`
flow) will expand each commit into ordered intent-checkpoint steps.

## Where this leaves the platform after M9

- Pi 5 HDMI console scrolls without the visible top-to-bottom redraw.
- The kernel owns its scanout buffers in cached RAM; userspace `/dev/fb0`
  drawing remains the same surface the desktop compositor already paints
  to on virt/ramfb.
- Vblank IRQ is wired and counted; future work can use it for input/event
  pacing, cursor animation, etc.
- The HVS channel itself is still firmware-configured: PV0 timings, HDMI
  PHY, dlist head, plane format/position/dimensions. Drunix only owns the
  `PTR0`/`PTR1` words of the firmware's primary plane.
- M10 path is clear: take ownership of the dlist (Path B) when there's
  appetite for a hardware cursor or multi-plane composition. The HVS
  observability and PV0 vblank infrastructure from M9.1/M9.4 transfer
  directly; only the dlist construction and `SCALER6_DISP0_LPTRS`
  rewrite are new.
