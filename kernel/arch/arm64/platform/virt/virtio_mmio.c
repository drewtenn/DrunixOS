/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_mmio.c - virtio-mmio bus enumeration for the QEMU virt machine.
 *
 * Phase 1 M2.0 of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
 * This is enumeration only — read MagicValue / Version / DeviceID for
 * each of the 32 virtio-mmio slots QEMU exposes on `-M virt`, and
 * report what we find. Virtqueue setup, feature negotiation, and the
 * actual virtio-blk / virtio-gpu / virtio-codec / virtio-sound front-
 * end drivers land in M2.1+.
 *
 * QEMU virt memory map (qemu/hw/arm/virt.c, machine version 9.x):
 *   virtio-mmio slot N base = 0x0A000000 + N * 0x200, for N in 0..31.
 * Each slot is 512 bytes, but only the first 0x100 carries the legacy/
 * modern register layout. SPI numbering for virtio interrupts is 16+N
 * once we wire the GIC up to dispatch them in M2.x.
 */

#include "../platform.h"
#include "virtio_mmio.h"
#include "kprintf.h"
#include <stdint.h>

#define VIRTIO_MMIO_BASE         0x0A000000UL
#define VIRTIO_MMIO_STRIDE       0x200UL
#define VIRTIO_MMIO_SLOTS        32u

/* Modern layout (Virtio 1.x). Legacy (v1) shares the first three. */
#define VIRTIO_MMIO_MAGIC_VALUE  0x000u  /* "virt" — 0x74726976 */
#define VIRTIO_MMIO_VERSION      0x004u  /* 1 = legacy, 2 = modern */
#define VIRTIO_MMIO_DEVICE_ID    0x008u  /* 0 = absent, see VIRTIO_DEV_ID_* */
#define VIRTIO_MMIO_VENDOR_ID    0x00Cu

#define VIRTIO_MAGIC             0x74726976u  /* 'v''i''r''t' little-endian */

static uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static const char *device_id_name(uint32_t id)
{
	switch (id) {
	case VIRTIO_DEV_ID_INVALID: return "invalid";
	case VIRTIO_DEV_ID_NET:     return "net";
	case VIRTIO_DEV_ID_BLOCK:   return "block";
	case VIRTIO_DEV_ID_CONSOLE: return "console";
	case VIRTIO_DEV_ID_GPU:     return "gpu";
	case VIRTIO_DEV_ID_INPUT:   return "input";
	case VIRTIO_DEV_ID_SOUND:   return "sound";
	default:                    return "unknown";
	}
}

uint32_t virtio_mmio_enumerate(void)
{
	uint32_t populated = 0;
	char line[96];

	platform_uart_puts("virtio-mmio: scanning 32 slots @ 0x0A000000\n");

	for (uint32_t slot = 0; slot < VIRTIO_MMIO_SLOTS; slot++) {
		uintptr_t base = VIRTIO_MMIO_BASE + slot * VIRTIO_MMIO_STRIDE;
		uint32_t magic = mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE);
		uint32_t version;
		uint32_t device;
		uint32_t vendor;

		if (magic != VIRTIO_MAGIC)
			continue;

		version = mmio_read32(base + VIRTIO_MMIO_VERSION);
		device = mmio_read32(base + VIRTIO_MMIO_DEVICE_ID);
		vendor = mmio_read32(base + VIRTIO_MMIO_VENDOR_ID);

		if (device == VIRTIO_DEV_ID_INVALID)
			continue;

		populated++;
		k_snprintf(line,
		           sizeof(line),
		           "  slot %u @ 0x%X: ver=%u device=%u (%s) vendor=0x%X\n",
		           (unsigned int)slot,
		           (unsigned int)base,
		           (unsigned int)version,
		           (unsigned int)device,
		           device_id_name(device),
		           (unsigned int)vendor);
		platform_uart_puts(line);
	}

	k_snprintf(line,
	           sizeof(line),
	           "virtio-mmio: %u populated slot%s\n",
	           (unsigned int)populated,
	           populated == 1u ? "" : "s");
	platform_uart_puts(line);

	return populated;
}
