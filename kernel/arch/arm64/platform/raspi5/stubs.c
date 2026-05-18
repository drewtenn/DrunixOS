/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * stubs.c - Phase-1 placeholder stubs for the raspi5 platform.
 *
 * MVP scope is a serial shell, so framebuffer and USB hooks are
 * no-ops. M6 adds the real platform_block_register in sdhci.c;
 * future milestones (HDMI, USB keyboard) will replace the remaining
 * stubs with real backends.
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
