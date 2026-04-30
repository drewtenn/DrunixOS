/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_VIRT_PLATFORM_H
#define KERNEL_PLATFORM_VIRT_PLATFORM_H

/*
 * QEMU "virt" machine memory map (qemu/hw/arm/virt.c, machine version 9.x).
 * Only the constants needed by Phase 1 M0 (PL011 UART) are defined here.
 * GICv3, virtio-mmio bases, etc., land in M1+.
 */

#define PLATFORM_VIRT_RAM_BASE 0x40000000UL
#define PLATFORM_VIRT_PL011_BASE 0x09000000UL

/*
 * `PLATFORM_PERIPHERAL_BASE` is consumed by kernel/arch/arm64/mm/mmu.c.
 * On virt M2.4b+ the value is preserved for compile compatibility but
 * the classifier in platform_mm.c now sources its decisions from
 * platform_ram_layout() rather than this single constant.
 */
#define PLATFORM_PERIPHERAL_BASE 0x08000000UL

/*
 * M2.4b: FDT-driven RAM layout init. Called from platform_init after
 * UART is up; populates the per-platform layout consumed by
 * platform_ram_layout()/platform_mm_classify(). Idempotent on repeat
 * calls.
 */
void virt_ram_layout_init(void);

#endif
