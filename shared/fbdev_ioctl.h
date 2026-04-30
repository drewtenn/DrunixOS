/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef SHARED_FBDEV_IOCTL_H
#define SHARED_FBDEV_IOCTL_H

/*
 * Drunix /dev/fb0 ioctl interface (M3.2). The Drunix-specific request
 * block 0xD800-0xD8FF is reserved for fbdev. Numbers in this range
 * MUST NOT collide with the Linux TTY ioctls handled at
 * kernel/proc/syscall/tty.c (those live in the 0x4xxx-0x5xxx range).
 *
 * The argument convention for each ioctl is documented per-request.
 */

#include "desktop_window.h" /* drunix_rect_t */

/*
 * DRUNIX_FBIO_FLUSH_RECT
 *
 *   Tell the kernel that the userspace compositor has updated a
 *   rectangular region of /dev/fb0 and the active framebuffer
 *   provider should propagate that update to the host display.
 *
 *   For the virtio-gpu provider (M3.1+) this enqueues a coalesced
 *   dirty rect that the deferred-flush pump consumes by issuing
 *   TRANSFER_TO_HOST_2D + RESOURCE_FLUSH for the rectangle only —
 *   replacing the M3.1 full-frame TRANSFER on every flush.
 *
 *   For the ramfb fallback provider this ioctl is a no-op (the host
 *   scans guest pages directly; there is nothing for the kernel to
 *   propagate).
 *
 *   Argument: pointer to a `drunix_rect_t` in the calling process's
 *   address space. The rect is in pixel units relative to the
 *   framebuffer's top-left. The kernel validates the rect against
 *   the framebuffer's width/height; an out-of-frame, zero-size, or
 *   negative-coord rect causes the ioctl to return -1 with no state
 *   change.
 *
 *   Returns: 0 on success, -1 on validation failure or unsupported
 *   request.
 */
#define DRUNIX_FBIO_FLUSH_RECT 0xD801u

/*
 * DRUNIX_FBIO_MOVE_CURSOR
 *
 *   M3.3 hardware-cursor position update. Tells the kernel to move
 *   the virtio-gpu cursor plane to (x, y) screen coordinates.
 *
 *   For the virtio-gpu provider this enqueues a VIRTIO_GPU_CMD_MOVE_CURSOR
 *   on cursorq (queue 1). The cursor sprite was uploaded once at
 *   driver init from `shared/cursor_sprite.h`; M3.3 does not support
 *   userspace sprite upload.
 *
 *   For the ramfb fallback provider (or if virtio-gpu cursor init
 *   failed) this ioctl returns -1, signalling the compositor to
 *   keep its software cursor.
 *
 *   Argument: pointer to a `drunix_point_t` in the calling process's
 *   address space. Coordinates are clipped against the framebuffer
 *   dimensions; off-screen coords return -1 with no state change.
 *
 *   Returns: 0 on success, -1 on hardware-cursor unavailable or
 *   invalid coords.
 */
#define DRUNIX_FBIO_MOVE_CURSOR 0xD802u

#endif /* SHARED_FBDEV_IOCTL_H */
