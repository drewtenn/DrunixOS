/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * raspi5/video.c — BCM2712 VideoCore HDMI framebuffer driver.
 *
 * M7 commit 1: constants, defensive bus-to-phys helper, and a stubbed
 * arm64_video_init() that returns -1. The mailbox transaction and
 * fbdev wiring land in commit 2.
 *
 * Mailbox base 0x10_7c01_3880 falls inside L1[65] (mapped Device in
 * M6 via raspi5/platform_mm.c). The request buffer lives in BSS and
 * is 16-byte aligned per the mailbox property interface contract.
 */

#include "video.h"
#include <stdint.h>

#define RASPI5_MBOX_BASE 0x107c013880ull
#define RASPI5_MBOX_READ 0u
#define RASPI5_MBOX_STATUS 6u
#define RASPI5_MBOX_WRITE 8u
#define RASPI5_MBOX_FULL 0x80000000u
#define RASPI5_MBOX_EMPTY 0x40000000u
#define RASPI5_MBOX_CHANNEL_PROPERTY 8u

#define RASPI5_VIDEO_BUS_ALIAS_MASK 0xc0000000u
#define RASPI5_VIDEO_BUS_ALIAS_OFFSET 0x3fffffffu
#define RASPI5_VIDEO_RAM_CEILING 0x80000000ull

static framebuffer_info_t g_fb_info;
static int g_fb_ready;

uint64_t raspi5_fb_bus_to_phys(uint32_t bus)
{
	if ((bus & RASPI5_VIDEO_BUS_ALIAS_MASK) == RASPI5_VIDEO_BUS_ALIAS_MASK)
		return (uint64_t)(bus & RASPI5_VIDEO_BUS_ALIAS_OFFSET);
	return (uint64_t)bus;
}

int arm64_video_init(void)
{
	/* Commit 2 lands the real mailbox transaction. */
	return -1;
}

int arm64_video_enabled(void)
{
	return g_fb_ready;
}

framebuffer_info_t *arm64_video_framebuffer(void)
{
	if (!g_fb_ready)
		return 0;
	return &g_fb_info;
}

void arm64_video_console_write(const char *buf, uint32_t len)
{
	(void)buf;
	(void)len;
}
