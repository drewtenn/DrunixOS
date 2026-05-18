/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * stubs.c - Phase-1 placeholder stubs for the raspi5 platform.
 *
 * M7 commit 2 replaces the framebuffer stubs with real backends in
 * raspi5/video.c; the USB host stubs remain because M7 is display-
 * only and USB HID is M8. Once M8 lands these can move too.
 */

#include "../platform.h"
#include <stdint.h>

int platform_usb_hci_register(void)
{
	return -1;
}

void platform_usb_hci_poll(void)
{
}
