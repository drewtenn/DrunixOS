/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRIVERS_FBDEV_H
#define DRIVERS_FBDEV_H

#include "chardev.h"
#include "desktop_window.h"
#include "framebuffer.h"
#include <stdint.h>

/*
 * Framebuffer character device.
 *
 * Publishes /dev/fb0 as a chardev whose mmap op exposes the framebuffer's
 * physical aperture to user space.  Also publishes /dev/fb0info, a small
 * read-only stream that returns a fixed binary geometry header (see
 * fbdev_info_t below) so a user-space compositor can discover the
 * framebuffer's width, height, pitch, and pixel layout at runtime
 * instead of hard-coding them.
 *
 * The supplied framebuffer pointer must outlive the kernel.  Returns 0 on
 * success and -1 if the chardev registry is full or the framebuffer is
 * malformed.
 */
int fbdev_init(const framebuffer_info_t *fb);

/*
 * Register the active framebuffer provider's "publish dirty rect"
 * hook (M3.2). The provider sets this after fbdev_init() succeeds —
 * for virtio-gpu, the hook unions the rect into the driver's pending
 * dirty state so the next pump_flush issues a per-rect TRANSFER.
 *
 * NULL hook (or no setter call) means the provider has no concept of
 * dirty-rect propagation — DRUNIX_FBIO_FLUSH_RECT then succeeds as
 * a no-op (this matches the ramfb fallback, where the host scans
 * guest pages directly).
 */
void fbdev_set_publish_dirty_rect(void (*hook)(drunix_rect_t));

/*
 * Register the active framebuffer provider's "move hardware cursor"
 * hook (M3.3). The provider sets this after fbdev_init() AND its
 * hardware cursor sprite is uploaded successfully — for virtio-gpu,
 * the hook submits MOVE_CURSOR on cursorq. NULL hook means no
 * hardware cursor; DRUNIX_FBIO_MOVE_CURSOR then returns -1 so the
 * compositor keeps drawing the cursor in software.
 */
void fbdev_set_move_cursor(void (*hook)(drunix_point_t));

/*
 * Override the cache attribute used for userspace mmap of /dev/fb0.
 * Default is CHARDEV_CACHE_NC (correct for QEMU ramfb and the legacy
 * Pi 3 / pre-M9 Pi 5 firmware-allocated framebuffers, all of which
 * the kernel maps Normal-NC). Providers whose scanout buffer is in
 * kernel-cacheable RAM — specifically raspi5 with the M9.3 HVS
 * carve-out — must call this with CHARDEV_CACHE_WB_FLUSH after
 * fbdev_init succeeds so the userspace alias matches the kernel
 * Normal-WB mapping. ARM forbids cacheable / non-cacheable aliases
 * of the same PA; this setter is how a provider declares its actual
 * kernel-side attribute to userspace.
 *
 * Provider is also expected to register a publish_dirty_rect hook
 * that DC CVACs the dirty range so userspace writes become visible
 * to the scanout DMA engine on the next vblank.
 */
void fbdev_set_cache_policy(chardev_cache_policy_t policy);

/*
 * Binary layout of /dev/fb0info.  The compositor reads sizeof(fbdev_info_t)
 * bytes and uses the values directly.
 */
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bpp;
	uint8_t red_pos;
	uint8_t red_size;
	uint8_t green_pos;
	uint8_t green_size;
	uint8_t blue_pos;
	uint8_t blue_size;
	uint8_t pad[2];
} fbdev_info_t;

#endif /* DRIVERS_FBDEV_H */
