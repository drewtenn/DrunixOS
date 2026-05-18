/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * fbfill - M7 smoke test: open /dev/fb0, mmap it, paint solid colors.
 *
 * Reads framebuffer geometry from /dev/fb0info (width, height, pitch,
 * bpp, channel positions), mmaps /dev/fb0, and walks four solid full-
 * screen fills (red, green, blue, white) with a one-second pause
 * between them. Used for end-to-end verification on real Pi 5
 * hardware: serial launches it; HDMI shows the colors.
 *
 * Honors the reported pitch (not assumed width * bpp/8) so a firmware
 * that returns a padded scanline doesn't produce a diagonal-tear
 * pattern, which is the easy way to catch a wrong-pitch driver bug.
 */

#include "stdio.h"
#include "syscall.h"
#include "unistd.h"
#include <stdint.h>

#define FBFILL_PATH      "/dev/fb0"
#define FBFILL_INFO_PATH "/dev/fb0info"

/* Mirrors kernel/drivers/fbdev.h's fbdev_info_t layout. The header
 * isn't on user's include path; the binary protocol is part of the
 * /dev/fb0info contract and is documented in fbdev.h. */
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

static int read_fb_info(fbdev_info_t *info)
{
	int fd;
	int total = 0;
	int n;

	fd = sys_open(FBFILL_INFO_PATH);
	if (fd < 0) {
		printf("fbfill: cannot open %s\n", FBFILL_INFO_PATH);
		return -1;
	}
	while (total < (int)sizeof(*info)) {
		n = sys_read(fd, (char *)info + total, (int)sizeof(*info) - total);
		if (n <= 0)
			break;
		total += n;
	}
	sys_close(fd);
	if (total != (int)sizeof(*info)) {
		printf("fbfill: short read from %s (%d bytes)\n",
		       FBFILL_INFO_PATH,
		       total);
		return -1;
	}
	if (info->width == 0u || info->height == 0u || info->pitch == 0u ||
	    info->bpp != 32u) {
		printf("fbfill: invalid geometry w=%u h=%u pitch=%u bpp=%u\n",
		       info->width,
		       info->height,
		       info->pitch,
		       info->bpp);
		return -1;
	}
	return 0;
}

static void fill_color(uint8_t *fb,
                       const fbdev_info_t *info,
                       uint32_t pixel)
{
	uint32_t y;
	uint32_t x;
	uint32_t *row;

	for (y = 0; y < info->height; y++) {
		row = (uint32_t *)(fb + (uint64_t)y * info->pitch);
		for (x = 0; x < info->width; x++)
			row[x] = pixel;
	}
}

static uint32_t pack_pixel(const fbdev_info_t *info,
                           uint8_t r,
                           uint8_t g,
                           uint8_t b)
{
	uint32_t pixel = 0u;

	pixel |= ((uint32_t)r) << info->red_pos;
	pixel |= ((uint32_t)g) << info->green_pos;
	pixel |= ((uint32_t)b) << info->blue_pos;
	return pixel;
}

int main(void)
{
	fbdev_info_t info;
	int fbfd;
	void *map;
	uint8_t *fb;
	uint32_t bytes;
	int i;
	struct {
		const char *name;
		uint8_t r, g, b;
	} bands[] = {
	    {"red",   0xff, 0x00, 0x00},
	    {"green", 0x00, 0xff, 0x00},
	    {"blue",  0x00, 0x00, 0xff},
	    {"white", 0xff, 0xff, 0xff},
	};

	if (read_fb_info(&info) != 0)
		return 1;

	printf("fbfill: %ux%u pitch=%u bpp=%u\n",
	       info.width,
	       info.height,
	       info.pitch,
	       info.bpp);

	bytes = info.pitch * info.height;
	fbfd = sys_open_flags(FBFILL_PATH, 2 /* O_RDWR */, 0);
	if (fbfd < 0) {
		printf("fbfill: cannot open %s\n", FBFILL_PATH);
		return 2;
	}

	map = sys_mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (map == MAP_FAILED) {
		printf("fbfill: mmap failed (%u bytes)\n", bytes);
		sys_close(fbfd);
		return 3;
	}
	fb = (uint8_t *)map;

	for (i = 0; i < (int)(sizeof(bands) / sizeof(bands[0])); i++) {
		uint32_t pixel =
		    pack_pixel(&info, bands[i].r, bands[i].g, bands[i].b);

		printf("fbfill: %s (0x%x)\n", bands[i].name, pixel);
		fill_color(fb, &info, pixel);
		sleep(1);
	}

	sys_munmap(map, bytes);
	sys_close(fbfd);
	printf("fbfill: done\n");
	return 0;
}
