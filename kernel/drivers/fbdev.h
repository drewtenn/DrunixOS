/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DRIVERS_FBDEV_H
#define DRIVERS_FBDEV_H

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
