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
