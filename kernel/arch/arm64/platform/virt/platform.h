/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_VIRT_PLATFORM_H
#define KERNEL_PLATFORM_VIRT_PLATFORM_H

/*
 * QEMU "virt" machine memory map (qemu/hw/arm/virt.c, machine version 9.x).
 * Only the constants needed by Phase 1 M0 (PL011 UART) are defined here.
 * GICv3, virtio-mmio bases, etc., land in M1+.
 */

#define PLATFORM_VIRT_RAM_BASE       0x40000000UL
#define PLATFORM_VIRT_PL011_BASE     0x09000000UL

/*
 * `PLATFORM_PERIPHERAL_BASE` is consumed by kernel/arch/arm64/mm/mmu.c to
 * decide whether a physical address belongs to MMIO or RAM. On the virt
 * machine, all device MMIO sits below 0x40000000 (PL011 at 0x09000000,
 * GICv3 distributor at 0x08000000, virtio-mmio at 0x0A000000–0x0A000200,
 * PCIe at 0x10000000). RAM starts at 0x40000000. Setting peripheral base
 * to 0x08000000 makes the same "addr >= peripheral_base => MMIO"
 * predicate the raspi3b path uses correct on virt. M0 skips MMU init
 * entirely so this constant is currently only consulted by code that does
 * not run; keeping it accurate makes M1 enabling MMU painless.
 */
#define PLATFORM_PERIPHERAL_BASE     0x08000000UL

#endif
