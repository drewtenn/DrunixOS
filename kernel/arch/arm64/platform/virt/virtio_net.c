/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_net.c - virtio-net driver for the arm64 virt machine.
 *
 * M4 commit 1: probe-only. Enumerate virtio-mmio slots for DeviceID 1,
 * record the first match's MMIO base, transport version, and slot,
 * then leave the device un-driven. Subsequent M4 commits add feature
 * negotiation (commit 2), DMA-pool ring allocation (commit 3),
 * IRQ-driven RX refill (commit 4), TX submission (commit 5), and the
 * /dev/net0 raw-frame chardev (commit 6).
 *
 * No DMA discipline applies in this commit because no descriptors or
 * packet buffers are allocated yet. When commits 3+ allocate from
 * virt_dma_alloc the rules in docs/contributing/aarch64-dma.md apply.
 *
 * The probe sequence is deliberately read-only — it reads MagicValue,
 * DeviceID, and Version. It does NOT write to VIRTIO_MMIO_STATUS, so
 * the device is left in whatever state firmware / QEMU put it in.
 * Commit 2 owns the reset + feature handshake.
 */

#include "../platform.h"
#include "virtio_mmio.h"
#include "virtio_net.h"
#include "kprintf.h"
#include <stdint.h>

/* QEMU virt machine maps virtio-mmio slot N at base 0x0A000000 + N*0x200,
 * for N in 0..31. The same constants drive virtio_mmio_enumerate() and
 * virtio_input.c — kept local here so the probe is self-contained until
 * later commits move shared scan helpers into virtio_mmio.{c,h}. */
#define VIRTIO_NET_MMIO_BASE_ADDR       0x0A000000UL
#define VIRTIO_NET_MMIO_STRIDE_BYTES    0x200UL
#define VIRTIO_NET_MMIO_MAX_SLOTS       32u
#define VIRTIO_NET_MMIO_MAGIC           0x74726976u

static struct {
	int found;
	uintptr_t base;
	uint32_t slot;
	uint32_t version;
} g_state;

static uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

int arm64_virt_virtio_net_init(void)
{
	char line[96];

	g_state.found = 0;
	g_state.base = 0;
	g_state.slot = 0;
	g_state.version = 0;

	for (uint32_t slot = 0; slot < VIRTIO_NET_MMIO_MAX_SLOTS; slot++) {
		uintptr_t base = VIRTIO_NET_MMIO_BASE_ADDR +
		                 slot * VIRTIO_NET_MMIO_STRIDE_BYTES;
		uint32_t magic;
		uint32_t device_id;
		uint32_t version;

		magic = mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE);
		if (magic != VIRTIO_NET_MMIO_MAGIC)
			continue;

		device_id = mmio_read32(base + VIRTIO_MMIO_DEVICE_ID);
		if (device_id != VIRTIO_DEV_ID_NET)
			continue;

		version = mmio_read32(base + VIRTIO_MMIO_VERSION);

		g_state.found = 1;
		g_state.base = base;
		g_state.slot = slot;
		g_state.version = version;

		k_snprintf(line, sizeof(line),
		           "virtio-net: device at slot %u "
		           "(base 0x%X, version %u)\n",
		           (unsigned int)slot,
		           (unsigned int)base,
		           (unsigned int)version);
		platform_uart_puts(line);
		return 1;
	}

	platform_uart_puts("virtio-net: no device present\n");
	return 0;
}

int arm64_virt_virtio_net_device_found(void)
{
	return g_state.found;
}

uintptr_t arm64_virt_virtio_net_mmio_base(void)
{
	return g_state.base;
}

uint32_t arm64_virt_virtio_net_slot(void)
{
	return g_state.slot;
}

uint32_t arm64_virt_virtio_net_version(void)
{
	return g_state.version;
}
