/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * fbdev.c — /dev/fb0 character device.
 *
 * Translates user-space mmap() requests against /dev/fb0 into the
 * framebuffer's physical aperture so the syscall layer can install
 * direct page-table entries.  Read/write byte ops are intentionally
 * absent: the framebuffer is meaningfully accessed only through mmap.
 */

#include "fbdev.h"
#include "chardev.h"
#include "kstring.h"
#include "proc/sched.h"
#include "proc/uaccess.h"
#include "desktop_window.h"
#include "fbdev_ioctl.h"
#include "syscall/syscall_linux.h"
#include <stdint.h>

#define PAGE_SIZE 0x1000u

static const framebuffer_info_t *fbdev_target;
static fbdev_info_t fbdev_info_cache;

static int fbdev_mmap_phys(uint32_t offset,
                           uint32_t length,
                           uint32_t prot,
                           uint64_t *phys_out)
{
	uint32_t fb_size;
	uint32_t end;

	if (!fbdev_target || !phys_out)
		return -1;
	if (length == 0 || (offset & (PAGE_SIZE - 1u)) != 0)
		return -1;
	if (prot & ~(uint32_t)(LINUX_PROT_READ | LINUX_PROT_WRITE))
		return -1;

	fb_size = fbdev_target->pitch * fbdev_target->height;
	if (fb_size == 0)
		return -1;
	if (offset >= fb_size)
		return -1;
	end = offset + length;
	if (end < offset || end > fb_size)
		return -1;

	*phys_out = (uint64_t)(uintptr_t)fbdev_target->phys_address + offset;
	return 0;
}

static chardev_cache_policy_t fbdev_cache_policy(uint32_t offset, uint32_t length)
{
	(void)offset;
	(void)length;
	/*
	 * M2.5a: framebuffer pages must be mapped Normal-NC on arm64
	 * (so QEMU ramfb reads see fresh writes without explicit dcache
	 * cleans) and PAT slot 4 / WC on x86 (so user mappings inherit
	 * the WC policy the kernel already configures for its identity
	 * map). CHARDEV_CACHE_NC selects both.
	 */
	return CHARDEV_CACHE_NC;
}

/*
 * Active framebuffer provider's "publish dirty rect" hook. Set by
 * the provider after it calls fbdev_init successfully. NULL on the
 * ramfb path (the host scans guest pages directly; nothing to
 * publish). When non-NULL, fbdev_ioctl forwards a validated
 * drunix_rect_t here.
 */
static void (*g_publish_dirty_rect)(drunix_rect_t rect);

void fbdev_set_publish_dirty_rect(void (*hook)(drunix_rect_t))
{
	g_publish_dirty_rect = hook;
}

/*
 * M3.3 hardware-cursor hook. Set by the provider only after the
 * cursor sprite has been uploaded AND the initial UPDATE_CURSOR
 * succeeded. NULL means no hardware cursor — fbdev_ioctl returns
 * -1 for DRUNIX_FBIO_MOVE_CURSOR, signalling userspace to keep its
 * software cursor.
 */
static void (*g_move_cursor)(drunix_point_t pt);

void fbdev_set_move_cursor(void (*hook)(drunix_point_t))
{
	g_move_cursor = hook;
}

static int fbdev_ioctl(uint32_t request, uintptr_t user_arg)
{
	process_t *cur;
	drunix_rect_t rect;

	if (!fbdev_target)
		return -1;

	switch (request) {
	case DRUNIX_FBIO_FLUSH_RECT: {
		if (user_arg == 0)
			return -1;
		cur = sched_current();
		if (!cur)
			return -1;
		if (uaccess_copy_from_user(
		        cur, &rect, (uint32_t)user_arg, sizeof(rect)) != 0)
			return -1;

		/* Validate: non-empty, non-negative, within fb bounds. */
		if (rect.w <= 0 || rect.h <= 0 || rect.x < 0 || rect.y < 0)
			return -1;
		if ((uint32_t)rect.x >= fbdev_target->width ||
		    (uint32_t)rect.y >= fbdev_target->height)
			return -1;
		if ((uint32_t)rect.w > fbdev_target->width - (uint32_t)rect.x ||
		    (uint32_t)rect.h > fbdev_target->height - (uint32_t)rect.y)
			return -1;

		if (g_publish_dirty_rect)
			g_publish_dirty_rect(rect);
		/* If no provider hook registered (e.g. ramfb fallback path),
		 * the ioctl still succeeds — userspace doesn't need to know
		 * the request was a no-op for the active provider. */
		return 0;
	}
	case DRUNIX_FBIO_MOVE_CURSOR: {
		drunix_point_t pt;

		if (user_arg == 0)
			return -1;
		/* No hardware cursor on this provider → -1 tells the
		 * compositor to keep its software cursor (M3.3 design). */
		if (!g_move_cursor)
			return -1;
		cur = sched_current();
		if (!cur)
			return -1;
		if (uaccess_copy_from_user(
		        cur, &pt, (uint32_t)user_arg, sizeof(pt)) != 0)
			return -1;

		/* Validate: non-negative, within fb bounds. */
		if (pt.x < 0 || pt.y < 0)
			return -1;
		if ((uint32_t)pt.x >= fbdev_target->width ||
		    (uint32_t)pt.y >= fbdev_target->height)
			return -1;

		g_move_cursor(pt);
		return 0;
	}
	default:
		return -1;
	}
}

static int fbdev_info_read(uint32_t offset, uint8_t *buf, uint32_t count)
{
	uint32_t total = (uint32_t)sizeof(fbdev_info_t);
	uint32_t remaining;
	uint32_t to_copy;

	if (!fbdev_target)
		return -1;
	if (offset >= total)
		return 0;
	remaining = total - offset;
	to_copy = count < remaining ? count : remaining;
	k_memcpy(buf, (const uint8_t *)&fbdev_info_cache + offset, to_copy);
	return (int)to_copy;
}

static const chardev_ops_t fbdev_ops = {
    .read_char = 0,
    .write_char = 0,
    .read = 0,
    .mmap_phys = fbdev_mmap_phys,
    .mmap_cache_policy = fbdev_cache_policy,
    .ioctl = fbdev_ioctl,
};

static const chardev_ops_t fbdev_info_ops = {
    .read_char = 0,
    .write_char = 0,
    .read = fbdev_info_read,
    .mmap_phys = 0,
};

int fbdev_init(const framebuffer_info_t *fb)
{
	if (!fb || fb->phys_address == 0 || fb->address == 0 || fb->pitch == 0 ||
	    fb->height == 0)
		return -1;

	fbdev_target = fb;

	fbdev_info_cache.width = fb->width;
	fbdev_info_cache.height = fb->height;
	fbdev_info_cache.pitch = fb->pitch;
	fbdev_info_cache.bpp = fb->bpp;
	fbdev_info_cache.red_pos = fb->red_pos;
	fbdev_info_cache.red_size = fb->red_size;
	fbdev_info_cache.green_pos = fb->green_pos;
	fbdev_info_cache.green_size = fb->green_size;
	fbdev_info_cache.blue_pos = fb->blue_pos;
	fbdev_info_cache.blue_size = fb->blue_size;
	fbdev_info_cache.pad[0] = 0;
	fbdev_info_cache.pad[1] = 0;

	if (chardev_register("fb0", &fbdev_ops) != 0)
		return -1;
	return chardev_register("fb0info", &fbdev_info_ops);
}
