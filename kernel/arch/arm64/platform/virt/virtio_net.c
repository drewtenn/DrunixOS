/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_net.c - virtio-net driver for the arm64 virt machine.
 *
 * M4 commit 1: probe-only enumeration (read-only scan).
 * M4 commit 2: feature negotiation + MAC discovery, legacy transport
 * (version 1) only. The driver writes STATUS through ACKNOWLEDGE,
 * DRIVER, FEATURES_OK, offers VIRTIO_NET_F_MAC (page 0 bit 5) only,
 * and reads the 6-byte MAC from config space as six byte loads.
 *
 * Commit 2 deliberately defers:
 *   - VIRTIO_NET_F_STATUS (page 1 bit 16). Adds the first SEL=1
 *     code path; not worth the complexity until link-status
 *     consumption matters.
 *   - Modern transport (MMIO version 2). Modern uses a different
 *     queue setup register layout (QUEUE_DESC/DRIVER/DEVICE) and
 *     would compound the handshake commit into a transport layer.
 *     A future commit adds version-2 support behind a feature flag.
 *
 * Subsequent M4 commits add DMA-pool ring allocation (commit 3),
 * IRQ-driven RX refill (commit 4), TX submission (commit 5), and the
 * /dev/net0 raw-frame chardev (commit 6).
 *
 * DMA discipline (per docs/contributing/aarch64-dma.md) applies once
 * commit 3 introduces descriptor and buffer DMA. Commit 2 only writes
 * MMIO registers and reads config space, so the DMA helpers are not
 * yet engaged.
 */

#include "../platform.h"
#include "virtio_mmio.h"
#include "virtio_net.h"
#include "kprintf.h"
#include "kstring.h"
#include <stdint.h>

/* QEMU virt machine maps virtio-mmio slot N at base 0x0A000000 + N*0x200,
 * for N in 0..31. The same constants drive virtio_mmio_enumerate() and
 * virtio_input.c — kept local here so the probe is self-contained until
 * later commits move shared scan helpers into virtio_mmio.{c,h}. */
#define VIRTIO_NET_MMIO_BASE_ADDR       0x0A000000UL
#define VIRTIO_NET_MMIO_STRIDE_BYTES    0x200UL
#define VIRTIO_NET_MMIO_MAX_SLOTS       32u
#define VIRTIO_NET_MMIO_MAGIC           0x74726976u
#define VIRTIO_NET_MMIO_VERSION_LEGACY  1u

/* Virtio 1.x §5.1.3 — virtio-net feature bits. */
#define VIRTIO_NET_F_MAC                5u   /* page 0 */

/* virtio-net config space layout (Virtio 1.x §5.1.4). MAC at offsets
 * 0..5; status at 6..7 only when VIRTIO_NET_F_STATUS is negotiated. */
#define VIRTIO_NET_CFG_MAC_OFFSET       0u
#define VIRTIO_NET_MAC_BYTES            6u

static struct {
	int found;
	int features_ok;
	uintptr_t base;
	uint32_t slot;
	uint32_t version;
	uint8_t mac[VIRTIO_NET_MAC_BYTES];
} g_state;

static uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static void mmio_write32(uintptr_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static uint8_t mmio_read8(uintptr_t addr)
{
	return *(volatile uint8_t *)addr;
}

static void status_or(uintptr_t base, uint32_t bit)
{
	uint32_t v = mmio_read32(base + VIRTIO_MMIO_STATUS);
	mmio_write32(base + VIRTIO_MMIO_STATUS, v | bit);
}

/*
 * Drive the legacy virtio handshake on `base` and offer F_MAC only.
 * Returns 0 on success (FEATURES_OK retained), -1 on failure (caller
 * leaves g_state.found = 0).
 */
static int virtio_net_negotiate(uintptr_t base)
{
	uint32_t status;
	uint32_t device_features;

	/* Reset → ACK → DRIVER. Mirrors virtio_input.c init order. */
	mmio_write32(base + VIRTIO_MMIO_STATUS, 0u);
	status_or(base, VIRTIO_STATUS_ACKNOWLEDGE);
	status_or(base, VIRTIO_STATUS_DRIVER);

	/* Read device features on page 0 only. F_MAC lives there; we
	 * deliberately do not select page 1 in commit 2. */
	mmio_write32(base + VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0u);
	device_features = mmio_read32(base + VIRTIO_MMIO_DEVICE_FEATURES);

	/* Refuse to drive a device that does not advertise F_MAC. The
	 * KTEST harness's virtio-net-device always provides F_MAC; if
	 * the device doesn't, treat as a hard failure (g_state stays
	 * un-found and the FAILED bit is set). */
	if ((device_features & (1u << VIRTIO_NET_F_MAC)) == 0u) {
		platform_uart_puts(
		    "virtio-net: device does not offer F_MAC; aborting\n");
		status_or(base, VIRTIO_STATUS_FAILED);
		return -1;
	}

	/* Offer only F_MAC on page 0; do not touch page 1 (F_STATUS,
	 * F_MQ, F_CTRL_VLAN, etc. are deferred). */
	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0u);
	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES,
	             1u << VIRTIO_NET_F_MAC);

	status_or(base, VIRTIO_STATUS_FEATURES_OK);
	status = mmio_read32(base + VIRTIO_MMIO_STATUS);
	if ((status & VIRTIO_STATUS_FEATURES_OK) == 0u) {
		platform_uart_puts(
		    "virtio-net: device rejected feature subset; aborting\n");
		status_or(base, VIRTIO_STATUS_FAILED);
		return -1;
	}

	return 0;
}

/*
 * Read the MAC address from config space as six byte loads. Linux
 * kernel virtio_net.c does the same (per byte) because the config
 * region is byte-addressable and the host may not honor wider loads
 * cleanly. A packed `uint64_t` cast or word-pair read can corrupt
 * the order on legacy mmio.
 */
static void virtio_net_read_mac(uintptr_t base, uint8_t *out)
{
	for (uint32_t i = 0; i < VIRTIO_NET_MAC_BYTES; i++)
		out[i] = mmio_read8(base + VIRTIO_MMIO_CONFIG +
		                    VIRTIO_NET_CFG_MAC_OFFSET + i);
}

int arm64_virt_virtio_net_init(void)
{
	char line[96];

	g_state.found = 0;
	g_state.features_ok = 0;
	g_state.base = 0;
	g_state.slot = 0;
	g_state.version = 0;
	k_memset(g_state.mac, 0, sizeof(g_state.mac));

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

		/* Commit 2: legacy transport only. Modern (v2) deferred. */
		if (version != VIRTIO_NET_MMIO_VERSION_LEGACY) {
			k_snprintf(line, sizeof(line),
			           "virtio-net: slot %u rejects non-legacy "
			           "transport (version %u)\n",
			           (unsigned int)slot,
			           (unsigned int)version);
			platform_uart_puts(line);
			/* Leave the device alone (no STATUS writes); keep
			 * scanning in case another slot offers v1. */
			continue;
		}

		/* Record probe state before negotiation so a negotiation
		 * failure leaves diagnostic information. */
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

		if (virtio_net_negotiate(base) != 0) {
			/* Hard failure during handshake; STATUS_FAILED is
			 * already set. Keep `found` so KTEST can report what
			 * we saw, but features_ok stays 0. */
			return -1;
		}

		virtio_net_read_mac(base, g_state.mac);
		g_state.features_ok = 1;

		k_snprintf(line, sizeof(line),
		           "virtio-net: features_ok mac="
		           "%02x:%02x:%02x:%02x:%02x:%02x\n",
		           g_state.mac[0], g_state.mac[1], g_state.mac[2],
		           g_state.mac[3], g_state.mac[4], g_state.mac[5]);
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

int arm64_virt_virtio_net_features_ok(void)
{
	return g_state.features_ok;
}

const uint8_t *arm64_virt_virtio_net_mac(void)
{
	return g_state.mac;
}
