/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_NET_H
#define KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_NET_H

#include <stdint.h>

/*
 * M4 commit 1: virtio-net device enumeration only.
 *
 * Scans virtio-mmio slots for DeviceID 1 (virtio-net) on the QEMU virt
 * machine and records the first matching device's MMIO base, transport
 * version, and slot. The device is otherwise left un-driven; feature
 * negotiation, MAC discovery, queue allocation, IRQ wiring, and the
 * /dev/net0 chardev land in subsequent M4 commits per the milestone
 * plan at docs/superpowers/plans/2026-04-30-drunix-m4-virtio-net-raw-
 * frames.md.
 *
 * Returns 1 if a device was found and recorded, 0 if no virtio-net
 * device is present (a clean no-op skip), or -1 on a hard failure that
 * should be logged. The KTEST harness skip-passes when no device is
 * advertised.
 */
int arm64_virt_virtio_net_init(void);

/*
 * Query helpers exposed for KTEST. All return zero / false when no
 * device has been enumerated.
 */
int arm64_virt_virtio_net_device_found(void);
uintptr_t arm64_virt_virtio_net_mmio_base(void);
uint32_t arm64_virt_virtio_net_slot(void);
uint32_t arm64_virt_virtio_net_version(void);

#endif
