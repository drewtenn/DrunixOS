/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_RASPI5_VIDEO_H
#define KERNEL_ARCH_ARM64_PLATFORM_RASPI5_VIDEO_H

/*
 * BCM2712 VideoCore mailbox framebuffer driver. Public surface mirrors
 * raspi3b/video.c so platform_framebuffer_acquire() and the in-kernel
 * fb_text_console wiring look the same on Pi 5 as on Pi 3.
 *
 * The BCM2712 mailbox lives at CPU-physical 0x10_7c01_3880 — a new
 * offset (0x13880, not the historical 0xB880), translated through the
 * soc@107c000000 ranges block in bcm2712.dtsi. The address falls
 * inside the existing L1[65] Device block established in M6, so no new
 * identity-mapped block is required for the mailbox itself.
 *
 * The legacy channel-8 property protocol (SET_PHYSICAL_SIZE,
 * SET_VIRTUAL_SIZE, SET_DEPTH, SET_PIXEL_ORDER, ALLOCATE_BUFFER,
 * GET_PITCH) is still implemented by Pi 5 EEPROM firmware as a
 * documented subset of the historical mailbox property ABI. The driver
 * therefore reuses the same request shape that raspi3b/video.c uses.
 */

#include "framebuffer.h"
#include <stdint.h>

/*
 * Geometry we *request* from the firmware via SET_PHYSICAL_SIZE /
 * SET_VIRTUAL_SIZE / SET_DEPTH. Pi 5 firmware honors the depth but
 * ignores the dimensions — ALLOCATE_BUFFER follows the EDID-detected
 * scanout instead — so these are starting suggestions, not hard
 * bounds. The actual width and height land in
 * framebuffer_info_t->width / ->height after arm64_video_init
 * derives them from the returned pitch and size.
 */
#define RASPI5_VIDEO_WIDTH 1024u
#define RASPI5_VIDEO_HEIGHT 768u
#define RASPI5_VIDEO_DEPTH 32u

/*
 * Upper bounds for the firmware-actual geometry the driver will
 * accept. Caps the boot-time BSS reservation for the fb_text_console
 * cell array. 1920 x 1080 covers every Pi-relevant common monitor;
 * 4K support would lift this and roughly quadruple the cell-array
 * memory (4 KiB cells -> 65 KiB) — left for a later milestone.
 */
#define RASPI5_VIDEO_MAX_WIDTH 1920u
#define RASPI5_VIDEO_MAX_HEIGHT 1080u
#define RASPI5_VIDEO_BYTES_PER_PIXEL 4u

int arm64_video_init(void);
int arm64_video_enabled(void);
framebuffer_info_t *arm64_video_framebuffer(void);
void arm64_video_console_write(const char *buf, uint32_t len);

/*
 * Translate a 32-bit VideoCore bus address (as returned by the
 * mailbox ALLOCATE_BUFFER tag) to a CPU-physical address.
 *
 * On the legacy Pi 3 path the firmware returns addresses in the
 * 0xC000_0000+ uncached SDRAM alias, and the driver masks with
 * 0x3FFF_FFFF to land in low SDRAM. BCM2712's `dma-ranges` is
 * identity for the AXI bus the CPU sees, so the firmware running
 * through dma_alloc_coherent paths should return CPU-physical
 * directly. There is provider disagreement on whether the firmware
 * for the framebuffer tag uses the identity or the legacy alias; the
 * helper handles both shapes so the first boot's serial trace can
 * disambiguate without code changes.
 */
uint64_t raspi5_fb_bus_to_phys(uint32_t bus);

#endif
