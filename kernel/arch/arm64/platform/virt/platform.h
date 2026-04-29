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
 * `PLATFORM_PERIPHERAL_BASE` is consumed by kernel/arch/arm64/mm/mmu.c.
 * On raspi3b ("addr >= peripheral_base implies MMIO") works because
 * peripherals live above RAM. On QEMU virt the layout is inverted —
 * device MMIO (GICD 0x08000000, PL011 0x09000000, virtio-mmio
 * 0x0A000000, PCIe ECAM 0x10000000) sits *below* RAM (which starts at
 * 0x40000000) — so the raspi3b predicate does not work.
 *
 * M0 and M1 do not call arch_mm_init at all, so the constant below is
 * a placeholder kept only so mmu.c compiles for the virt build. It is
 * NOT a correct device/RAM split. M2 will replace this with an
 * FDT-driven memory-map walk that handles the multi-region layout.
 */
#define PLATFORM_PERIPHERAL_BASE     0x08000000UL

#endif
