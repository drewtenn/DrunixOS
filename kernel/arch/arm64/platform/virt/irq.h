/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_VIRT_IRQ_H
#define KERNEL_PLATFORM_VIRT_IRQ_H

#include "../platform.h"
#include <stdint.h>

/*
 * Register a handler for a GICv3 SPI (shared peripheral interrupt).
 * `spi` is the SPI number — INTID 32 + spi. M2.2 uses this for the
 * virtio-mmio slot interrupts (slot N → SPI 16+N on QEMU virt).
 * Returns 0 on success, -1 if the handler table is full.
 */
int virt_irq_register_spi(uint32_t spi, platform_irq_handler_fn fn);

/*
 * Enable an SPI in the GICv3 distributor: assign Group 1 NS, set
 * priority, route to CPU 0, set-enable. Caller registers the handler
 * via virt_irq_register_spi() before this so the dispatch path can
 * find it on the first IRQ.
 */
void virt_irq_enable_spi(uint32_t spi, uint8_t priority);

#endif
