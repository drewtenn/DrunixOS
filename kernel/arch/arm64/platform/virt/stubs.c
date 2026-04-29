/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * stubs.c - Minimal Phase-1 M0 stubs for the virt platform.
 *
 * IRQ, framebuffer, USB, and block-device hooks are required by the
 * platform.h interface but not yet implemented for virt. M0's success
 * criterion is "boot and print"; the kernel halts in arm64_console_loop()
 * before any of these would be exercised. M1 replaces the IRQ stubs with a
 * real GICv3 driver; M2/M3 replace the rest with virtio-mmio backends.
 */

#include "../platform.h"
#include <stdint.h>

void platform_irq_init(void)
{
}

void platform_irq_register(uint32_t irq, platform_irq_handler_fn fn)
{
	(void)irq;
	(void)fn;
}

int platform_irq_dispatch(void)
{
	return 0;
}

void platform_irq_enable(void)
{
	/* Leave interrupts masked until GICv3 lands in M1. */
}

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

int platform_block_register(void)
{
	return -1;
}

int platform_usb_hci_register(void)
{
	return -1;
}

void platform_usb_hci_poll(void)
{
}
