/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_RASPI5_HVS_H
#define KERNEL_ARCH_ARM64_PLATFORM_RASPI5_HVS_H

/*
 * BCM2712 Hardware Video Scaler 6 (HVS6) — passive observability for M9.1.
 *
 * The HVS owns scanout composition on Pi 5. The firmware brings it up
 * during early boot and points its primary plane at the framebuffer
 * returned by the mailbox ALLOCATE_BUFFER tag. M9 (see
 * docs/design/m9-raspi5-hvs-plane-driver.md) replaces this firmware-
 * owned scanout with Drunix-owned back/scanout buffers; M9.1 is the
 * read-only first step that proves Drunix can read HVS channel state
 * without disturbing firmware composition.
 *
 * Register layout, channel offsets, and dlist SRAM placement are not
 * documented in any public Broadcom datasheet. The constants here are
 * derived from Linux's drivers/gpu/drm/vc4/ (vc4_regs.h, vc4_hvs.c,
 * vc4_plane.c, vc4_crtc.c) plus arch/arm64/boot/dts/broadcom/bcm2712.dtsi.
 * Per docs/contributing/linux-reference.md no Linux source is copied;
 * the offsets are independently re-stated here as Drunix-native C with
 * citations to the originating files.
 */

#include <stdint.h>

/* HVS MMIO window. From bcm2712.dtsi:
 *   hvs: hvs@7c580000 { reg = <0x10 0x7c580000 0x0 0x1a000>; };
 * Translated to a CPU PA via the soc@107c000000 ranges block, the
 * window lives at 0x10_7c58_0000 with span 0x1a000. The block sits
 * inside the L1[65] high-peripheral identity-map established in M6,
 * so no new MMU TT entries are required to dereference it. */
#define RASPI5_HVS_BASE 0x107c580000ull
#define RASPI5_HVS_SIZE 0x0001a000ull

/* SCALER6_VERSION register. The M9.1 v2 wide-scan trace observed
 *   mmio[0x0000]=0x00002454
 * on Pi 5 hardware; the low byte 0x54 identifies BCM2712 D0 silicon.
 * BCM2712 C0 (older revision) reports 0x53 in the same byte. The
 * channel register layout differs between revisions, so M9.1 v3
 * detects the revision at probe start and selects the channel bases
 * accordingly. */
#define RASPI5_HVS_VERSION_OFFSET 0x000u
#define RASPI5_HVS_VERSION_REV_MASK 0x000000ffu
#define RASPI5_HVS_REV_BCM2712_C0 0x53u
#define RASPI5_HVS_REV_BCM2712_D0 0x54u

#define RASPI5_HVS_CHANNEL_COUNT 3u

/* Per-channel control register blocks differ by silicon revision.
 *   C0 (HVS5-like layout): channels at 0x30, 0x50, 0x70 (stride 0x20)
 *   D0 (HVS6-true layout): channels at 0x100, 0x140, 0x180 (stride 0x40)
 *
 * The D0 bases were observed in M9.1 v2 (mmio[0x0100]=0x877f0437
 * encoded 1920x1080 dimensions in channel 0; mmio[0x0140]=0x827f01df
 * encoded 640x480 in channel 1). The C0 bases are the HVS5-style
 * layout cited by codex during Discover. The probe reads VERSION and
 * picks the right bases at runtime. */
#define RASPI5_HVS_C0_CHANNEL_STRIDE 0x20u
#define RASPI5_HVS_C0_CHANNEL_BASE0 0x030u
#define RASPI5_HVS_D0_CHANNEL_STRIDE 0x40u
#define RASPI5_HVS_D0_CHANNEL_BASE0 0x100u

/* Per-revision within-channel register offsets. The D0 layout shifts
 * LPTRS/DL by +0x04 relative to C0 (consistent with one extra control
 * word inserted at the head of each channel block). M9.1 v3 still
 * does the wide-scan dump unconditionally so unmapped/unknown
 * registers continue to surface in the next trace, but the named
 * reads now use revision-correct offsets. */
#define RASPI5_HVS_C0_DISP_CTRL0_OFFSET 0x00u
#define RASPI5_HVS_C0_DISP_CTRL1_OFFSET 0x04u
#define RASPI5_HVS_C0_DISP_LPTRS_OFFSET 0x0cu
#define RASPI5_HVS_C0_DISP_DL_OFFSET 0x18u

#define RASPI5_HVS_D0_DISP_CTRL0_OFFSET 0x00u
#define RASPI5_HVS_D0_DISP_CTRL1_OFFSET 0x04u
#define RASPI5_HVS_D0_DISP_LPTRS_OFFSET 0x10u
#define RASPI5_HVS_D0_DISP_DL_OFFSET 0x1cu

/* DLIST SRAM. Linux vc4_hvs.c maps the dlist region at
 *   hvs->regs + SCALER5_DLIST_START
 * where SCALER5_DLIST_START = 0x4000 on gen >= 5 (Pi 4 and Pi 5).
 * The dlist is on-die SRAM; reads from offsets outside this window
 * are reserved and may abort or return undefined data. M9.1 bounds
 * all dlist reads to [DLIST_OFFSET, DLIST_OFFSET + DLIST_SIZE). */
#define RASPI5_HVS_DLIST_OFFSET 0x4000u
#define RASPI5_HVS_DLIST_SIZE (RASPI5_HVS_SIZE - RASPI5_HVS_DLIST_OFFSET)
#define RASPI5_HVS_DLIST_WORD_COUNT (RASPI5_HVS_DLIST_SIZE / 4u)

/* The first 32 dlist words are reserved for the bootloader's display
 * list head per Linux's HVS_BOOTLOADER_DLIST_END convention. Drunix
 * follows the same reservation when M9.2+ allocates its own dlist
 * region. M9.1 just records that the firmware's LPTRS lands inside
 * this window. */
#define RASPI5_HVS_BOOTLOADER_DLIST_END 32u

/* DISP_CTRL0 bit positions. Linux vc4_regs.h:
 *   SCALER_DISPCTRLX_ENABLE bit 31 — channel running
 * Other bits encode line width and FIFO occupancy; M9.1 only reads
 * the raw word and traces it. M9.2+ may parse further. */
#define RASPI5_HVS_DISP_CTRL0_ENABLE_BIT (1u << 31)

/* DISP_LPTRS encodes the active dlist HEAD pointer (word offset
 * within HVS dlist SRAM) in the low 12 bits on HVS6 (Linux mask, not
 * the 16 bits I incorrectly assumed in v1/v2). DISP_DL holds the
 * LATCHED active head — the value the channel is currently scanning
 * out from, which may differ from LPTRS during an in-flight async-flip
 * handoff. */
#define RASPI5_HVS_DISP_LPTRS_HEAD_MASK 0x00000fffu
#define RASPI5_HVS_DISP_DL_HEAD_MASK 0x00000fffu

/* Runtime-detected HVS layout. Populated by raspi5_hvs_probe_passive()
 * after reading SCALER6_VERSION; carries the per-revision channel
 * base offsets and within-channel register offsets so the probe loop
 * doesn't have to compile-time pick. */
typedef struct {
	uint8_t revision;
	uint8_t known;
	uint32_t channel_base[RASPI5_HVS_CHANNEL_COUNT];
	uint32_t disp_ctrl0_offset;
	uint32_t disp_ctrl1_offset;
	uint32_t disp_lptrs_offset;
	uint32_t disp_dl_offset;
} raspi5_hvs_layout_t;

/* Result struct populated by raspi5_hvs_probe_passive(). Fields are
 * intentionally raw register values rather than parsed semantics —
 * M9.1 is observation, not interpretation. M9.2 introduces a
 * separate raspi5_hvs_plane_ref_t with parsed fields and validation
 * outcomes. */
typedef struct {
	raspi5_hvs_layout_t layout;
	uint32_t version;
	uint32_t channel_ctrl0[RASPI5_HVS_CHANNEL_COUNT];
	uint32_t channel_ctrl1[RASPI5_HVS_CHANNEL_COUNT];
	uint32_t channel_lptrs[RASPI5_HVS_CHANNEL_COUNT];
	uint32_t channel_dl[RASPI5_HVS_CHANNEL_COUNT];
	uint8_t channel_enabled[RASPI5_HVS_CHANNEL_COUNT];
	uint8_t active_channel;       /* index of first enabled channel, or 0xff */
	uint8_t channel_enabled_count;
} raspi5_hvs_probe_state_t;

/*
 * Read every channel's control/status registers and trace the result
 * over serial. If at least one channel reports CTRL0.ENABLE set, also
 * dump the first RASPI5_HVS_PROBE_DLIST_TRACE_WORDS words of dlist
 * SRAM starting from that channel's LPTRS head, so the trace makes
 * the firmware's primary plane visible without requiring any HVS
 * writes.
 *
 * Returns 0 on success and populates *state. Returns -1 only if state
 * is NULL or if no HVS register can be read sensibly; failure is
 * non-fatal at the M9.1 layer — caller continues with the mailbox
 * framebuffer regardless.
 *
 * No HVS writes occur in this function. No assumption about which
 * channel HDMI0 lives on is hard-coded; the probe records what it
 * finds.
 */
int raspi5_hvs_probe_passive(raspi5_hvs_probe_state_t *state);

/*
 * Reference to the firmware's primary plane after M9.2 validation.
 * Populated by raspi5_hvs_locate_firmware_plane(); consumed by
 * raspi5_hvs_flip_plane_address(). Fields are absolute word offsets
 * within HVS dlist SRAM (0..0x6800 word range).
 *
 * The address words encode a 64-bit physical address split as:
 *   address[63:32] in dlist word `addr_hi_word_offset`
 *   address[31:0]  in dlist word `addr_lo_word_offset`
 *
 * The control word at `ctl0_dlist_word_offset` and the volatile
 * status word at `ptr0_dlist_word_offset` are recorded only for
 * documentation; the flip path must never write the ptr0 word
 * because HVS6 updates it dynamically per scanout frame (verified
 * across three boots in M9.1 v3 traces).
 */
typedef struct {
	uint8_t valid;
	uint8_t channel;
	uint32_t plane_head_word_offset;
	uint32_t ctl0_dlist_word_offset;
	uint32_t ptr0_dlist_word_offset;
	uint32_t addr_hi_word_offset;
	uint32_t addr_lo_word_offset;
	uint32_t pitch_dlist_word_offset;
	uint32_t saved_ctl0;
	uint32_t saved_dims;
	uint32_t saved_addr_hi;
	uint32_t saved_addr_lo;
	uint32_t saved_pitch;
} raspi5_hvs_plane_ref_t;

/*
 * Locate and structurally validate the firmware's primary plane on
 * the HVS channel currently scanning HDMI0. Reads channel 0's DL
 * register (latched active head), walks the named-word fingerprint
 * at dlist[head .. head+8], and verifies:
 *   - dimensions word == ((height-1) << 16) | (width-1)
 *   - address-high word == 0 (sub-4GB scanout assumption)
 *   - address-low word == expected_fb_phys
 *   - pitch word == expected_pitch
 *   - end-of-plane marker == 0xb0b0b0b0
 *
 * Returns 0 on a clean fingerprint match. -1 on any mismatch (caller
 * stays on the mailbox framebuffer, never touches HVS). Trace
 * messages identify which check failed.
 */
int raspi5_hvs_locate_firmware_plane(uint32_t expected_width,
                                     uint32_t expected_height,
                                     uint32_t expected_pitch,
                                     uintptr_t expected_fb_phys,
                                     raspi5_hvs_plane_ref_t *out);

/*
 * Atomically (within HVS6 async-flip semantics) rewrite the plane's
 * address words to point scanout at new_phys. Writes only the
 * address words — never CTL0, dimensions, pitch, or the volatile
 * ptr0 word. plane->valid must be non-zero (locator returned 0).
 * Returns 0 on success, -1 on invalid args.
 *
 * The HVS picks up the new address on next FIFO refill, which in
 * practice happens within microseconds. M9.4 will add a PV0 vblank
 * IRQ that defers the write to the vertical front porch for
 * tear-free behavior; M9.3 accepts a single-frame tear window in
 * exchange for not needing IRQ infrastructure yet.
 */
int raspi5_hvs_flip_plane_address(const raspi5_hvs_plane_ref_t *plane,
                                  uintptr_t new_phys);

#endif /* KERNEL_ARCH_ARM64_PLATFORM_RASPI5_HVS_H */
