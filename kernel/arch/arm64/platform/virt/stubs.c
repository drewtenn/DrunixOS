/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * stubs.c - Phase-1 placeholder stubs for the virt platform.
 *
 * The IRQ surface moved to irq.c (GICv3) in M1.
 * platform_block_register() moved to virtio_blk.c in M2.3.
 * Framebuffer and USB hooks remain stubbed; the framebuffer hook is
 * filled in by M2.5 (ramfb scanout) and Phase 2 (virtio-gpu); the USB
 * surface is not in Phase 1 scope on virt.
 */

#include "../platform.h"
#include <stdint.h>

int platform_framebuffer_acquire(framebuffer_info_t **out)
{
	(void)out;
	return -1;
}

void platform_framebuffer_console_write(const char *buf, uint32_t len)
{
	(void)buf;
	(void)len;
}

int platform_usb_hci_register(void)
{
	return -1;
}

void platform_usb_hci_poll(void)
{
}
