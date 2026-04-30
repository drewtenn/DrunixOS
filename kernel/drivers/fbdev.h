/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRIVERS_FBDEV_H
#define DRIVERS_FBDEV_H

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
