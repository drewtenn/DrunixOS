/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * platform_mm.c - raspi3b memory classifier and RAM layout.
 *
 * Phase 1 M2.4b: preserves the existing M2.3 behavior. raspi3b RAM
 * is at PA 0x00000000-0x40000000 (1 GiB BCM2837); peripherals are
 * mapped at 0x3F000000-0x40000000 ("PERIPHERAL_BASE..end-of-RAM").
 * Heap is placed at the static [ARCH_HEAP_START, ARCH_HEAP_END)
 * window inside RAM that has been used since v0.
 */

#include "../platform.h"
#include "platform.h"
#include "../../arch_layout.h"
#include <stdint.h>

platform_mm_attr_t platform_mm_classify(uint64_t phys)
{
	/* The 0x80000000 upper bound matches the L1[1]-as-Device 1 GiB block
	 * the existing mmu builder maps for raspi3b (see arm64_mmu_build_
	 * kernel_tables in mm/mmu.c). The BCM2837 has no useful MMIO above
	 * 0x80000000; UNMAPPED keeps stale references from silently
	 * translating. Tighten only if mmu.c's L1[1] mapping shrinks. */
	if (phys < PLATFORM_PERIPHERAL_BASE)
		return PLATFORM_MM_NORMAL;
	if (phys < 0x80000000ull)
		return PLATFORM_MM_DEVICE;
	return PLATFORM_MM_UNMAPPED;
}

static const platform_ram_layout_t g_layout = {
    .ram_base = 0x00000000ull,
    .ram_size = 0x40000000ull,
    .heap_base = (uint64_t)ARCH_HEAP_START,
    .heap_size = (uint64_t)(ARCH_HEAP_END - ARCH_HEAP_START),
    .kernel_image_end = 0x01800000ull,
};

const platform_ram_layout_t *platform_ram_layout(void)
{
	return &g_layout;
}
