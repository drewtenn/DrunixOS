/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_RASPI5_PV_H
#define KERNEL_PLATFORM_RASPI5_PV_H

#include <stdint.h>

/*
 * BCM2712 PixelValve PV0 (HDMI0 timing source) — M9.4b1 passive
 * observability.
 *
 * The HVS6 composes pixels and hands them to one of the BCM2712's
 * five PixelValves; PV0 drives HDMI0 on Pi 5. M9.4b plans to use
 * PV0's vertical-front-porch IRQ as the vblank source for tear-free
 * HVS plane flips, on GIC SPI 101 per bcm2712.dtsi convention.
 *
 * Step 1 (this header) is read-only observability of PV0's MMIO
 * window so we can confirm the base address, dump current state,
 * and identify the PV_INT_EN / PV_INT_STAT / PV_STAT register
 * positions on D0 silicon. Same approach M9.1 took with HVS6: dump
 * first, mutate later.
 *
 * MMIO base per Linux bcm2712.dtsi: pixelvalve0@7c400000 →
 * CPU-physical 0x10_7c40_0000 after translation through the
 * soc@107c000000 ranges block. The address sits inside the L1[65]
 * Device identity-map block M6 established for the SoC high
 * peripheral window, so no new MMU plumbing is required.
 *
 * Per Linux vc4_regs.h, the PV register block is fairly small —
 * RASPI5_PV_WINDOW_SIZE caps the probe scan at 0x100 bytes which
 * covers every documented register Linux uses through gen6.
 */

#define RASPI5_PV0_BASE 0x107c400000ull
#define RASPI5_PV_WINDOW_SIZE 0x100u

/*
 * Probe-state struct populated by raspi5_pv0_probe_passive(). Raw
 * register values, no interpretation — M9.4b2 will parse these.
 */
typedef struct {
	uint32_t control;
	uint32_t v_control;
	uint32_t horza;
	uint32_t horzb;
	uint32_t verta;
	uint32_t vertb;
	uint32_t int_en;
	uint32_t int_stat;
	uint32_t stat;
} raspi5_pv0_probe_state_t;

/*
 * Read every 32-bit word in the first RASPI5_PV_WINDOW_SIZE bytes of
 * PV0 MMIO and emit a non-zero-only serial trace, mirroring M9.1's
 * "raspi5 hvs:" wide-scan style. Returns 0 on success and populates
 * *state if non-NULL. The state struct's field layout follows the
 * Pi 4 / HVS5 documented offsets; on Pi 5 / HVS6 the same offsets
 * may have shifted, which is exactly what the wide-scan output
 * tells us.
 *
 * Read-only. No PV0 register writes occur. Safe to call after
 * arm64_video_init's mailbox path succeeds — the firmware has by
 * then brought the HVS + PV0 + HDMI pipeline up, so reads see live
 * state. Returns -1 only if state is NULL or the trace helper
 * cannot run; failure is non-fatal at the caller.
 */
int raspi5_pv0_probe_passive(raspi5_pv0_probe_state_t *state);

#endif
