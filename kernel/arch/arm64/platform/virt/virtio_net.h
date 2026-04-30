/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_NET_H
#define KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_NET_H

#include <stdint.h>

/*
 * M4 commits 1 + 2: virtio-net device enumeration, legacy-transport
 * handshake, and MAC discovery.
 *
 * Commit 1 scans virtio-mmio slots for DeviceID 1 (virtio-net) and
 * records the first matching device's MMIO base, transport version,
 * and slot.
 *
 * Commit 2 extends init to drive the legacy (version 1) handshake:
 * reset → ACKNOWLEDGE → DRIVER → offer F_MAC → FEATURES_OK readback.
 * Modern transport (version 2) and additional feature bits (F_STATUS,
 * F_CTRL_VQ, etc.) are deferred to later commits per the M4 plan at
 * docs/superpowers/plans/2026-04-30-drunix-m4-virtio-net-raw-frames.md.
 *
 * Init returns 1 when a device was found and the handshake completed,
 * 0 when no virtio-net device is present (clean no-op skip), or -1 on
 * a hard failure (e.g. device refused F_MAC, FEATURES_OK didn't stick).
 * The KTEST harness skip-passes when no device is advertised.
 */
int arm64_virt_virtio_net_init(void);

/*
 * Query helpers exposed for KTEST. All return zero / false / NULL-
 * equivalent when no device has been enumerated. `_features_ok()`
 * reports whether the handshake completed; `_mac()` returns a pointer
 * to the cached 6-byte MAC (zero-filled before negotiation succeeds).
 */
int arm64_virt_virtio_net_device_found(void);
uintptr_t arm64_virt_virtio_net_mmio_base(void);
uint32_t arm64_virt_virtio_net_slot(void);
uint32_t arm64_virt_virtio_net_version(void);
int arm64_virt_virtio_net_features_ok(void);
const uint8_t *arm64_virt_virtio_net_mac(void);

/*
 * Commit 3 accessors. `_rings_ready()` reports whether the queue
 * backing + packet buffer pools were successfully allocated and the
 * RX/TX queues configured at the MMIO level (DRIVER_OK is NOT yet
 * asserted in commit 3 — that is commit 4's responsibility).
 *
 * `_buffer_count()` returns the per-queue buffer count
 * (VIRTIO_NET_RING_BUFFERS, currently 16).
 *
 * `_rx_buffer_phys(i)` / `_tx_buffer_phys(i)` return the device-
 * visible physical address of the i-th packet buffer for KTEST
 * verification that every buffer translates to a non-zero phys
 * (silent DMA-to-zero canary). Returns 0 if i is out of range.
 *
 * `_rx_queue_phys()` / `_tx_queue_phys()` return the queue backing's
 * physical base, or 0 if rings are not yet ready.
 */
int arm64_virt_virtio_net_rings_ready(void);
uint32_t arm64_virt_virtio_net_buffer_count(void);
uint64_t arm64_virt_virtio_net_rx_buffer_phys(uint32_t i);
uint64_t arm64_virt_virtio_net_tx_buffer_phys(uint32_t i);
uint64_t arm64_virt_virtio_net_rx_queue_phys(void);
uint64_t arm64_virt_virtio_net_tx_queue_phys(void);

#endif
