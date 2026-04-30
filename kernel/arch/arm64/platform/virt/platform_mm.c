/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * platform_mm.c - QEMU virt memory classifier and FDT-driven RAM layout.
 *
 * Phase 1 M2.4b: virt RAM lives at PA 0x40000000-0x80000000 by
 * default; peripherals (GICD, PL011, virtio-mmio, PCIe ECAM) at
 * 0x08000000-0x40000000. The RAM range can be overridden by the
 * FDT /memory node QEMU passes in x0; M2.4a captures that pointer
 * into g_fdt_blob_phys and provides a parser. M2.4b consumes it.
 *
 * 16 MiB headroom comfortably accommodates current BSS (~1.3 MiB:
 * 64 KiB DMA pool + 128 KiB PMM bitmap + 1 MiB PMM refcounts +
 * ~100 KiB statics) plus growth room for Phase 2 subsystems
 * (virtio-net, virtio-gpu). Revisit if a future subsystem materially
 * grows BSS — recompute against actual `size .bss` post-link.
 */

#include "../platform.h"
#include "../../fdt.h"
#include <stdint.h>

#define VIRT_RAM_BASE_DEFAULT 0x40000000ull
#define VIRT_RAM_SIZE_DEFAULT 0x40000000ull

#define VIRT_DEV_BASE 0x08000000ull
#define VIRT_DEV_END VIRT_RAM_BASE_DEFAULT

#define VIRT_HEAP_OFFSET_FROM_KERNEL_END (16u * 0x100000u)
#define VIRT_HEAP_TAIL_RESERVE (16u * 0x100000u)

#define VIRT_PAGE_SIZE 0x1000u

extern char _kernel_end[];

void platform_uart_puts(const char *s);

static platform_ram_layout_t g_layout;
static int g_layout_ready;

static void virt_ram_layout_finalize_defaults(void)
{
	g_layout.ram_base = VIRT_RAM_BASE_DEFAULT;
	g_layout.ram_size = VIRT_RAM_SIZE_DEFAULT;
}

void virt_ram_layout_init(void)
{
	fdt_memory_range_t ranges[FDT_MAX_MEMORY_RANGES];
	uint32_t count = 0;
	uint64_t kernel_end_aligned;
	const void *fdt;

	if (g_layout_ready)
		return;

	virt_ram_layout_finalize_defaults();

	fdt = (const void *)(uintptr_t)g_fdt_blob_phys;
	if (!fdt) {
		platform_uart_puts(
		    "virt: no FDT pointer; using compile-time RAM defaults\n");
	} else if (fdt_validate(fdt) != 0) {
		platform_uart_puts(
		    "virt: FDT header invalid; using compile-time RAM defaults\n");
	} else if (fdt_get_memory(fdt, ranges, FDT_MAX_MEMORY_RANGES, &count) !=
	               0 ||
	           count == 0u) {
		platform_uart_puts("virt: FDT /memory parse failed; using defaults\n");
	} else {
		g_layout.ram_base = ranges[0].base;
		g_layout.ram_size = ranges[0].size;

		/* M2.4b maps RAM as a single 1 GiB L1 block. If the user
		 * passed QEMU more than 1 GiB, warn and truncate; the extra
		 * RAM is unreachable until M3.x adds an L2 split at L1[1]. */
		if (g_layout.ram_size > 0x40000000ull) {
			platform_uart_puts(
			    "virt: RAM > 1 GiB; truncating to 1 GiB for M2.4b\n");
			g_layout.ram_size = 0x40000000ull;
		}
	}

	kernel_end_aligned =
	    ((uint64_t)(uintptr_t)_kernel_end + (uint64_t)VIRT_PAGE_SIZE - 1u) &
	    ~((uint64_t)VIRT_PAGE_SIZE - 1u);
	g_layout.kernel_image_end = kernel_end_aligned;

	g_layout.heap_base = kernel_end_aligned + VIRT_HEAP_OFFSET_FROM_KERNEL_END;
	if (g_layout.ram_base + g_layout.ram_size <
	    g_layout.heap_base + VIRT_HEAP_TAIL_RESERVE) {
		platform_uart_puts(
		    "virt: heap range invalid; falling back to defaults\n");
		virt_ram_layout_finalize_defaults();
		g_layout.kernel_image_end = kernel_end_aligned;
		g_layout.heap_base =
		    kernel_end_aligned + VIRT_HEAP_OFFSET_FROM_KERNEL_END;
	}
	g_layout.heap_size = (g_layout.ram_base + g_layout.ram_size) -
	                     (uint64_t)VIRT_HEAP_TAIL_RESERVE - g_layout.heap_base;

	g_layout_ready = 1;
}

platform_mm_attr_t platform_mm_classify(uint64_t phys)
{
	if (!g_layout_ready)
		virt_ram_layout_init();

	if (phys >= g_layout.ram_base &&
	    phys < g_layout.ram_base + g_layout.ram_size)
		return PLATFORM_MM_NORMAL;
	if (phys >= VIRT_DEV_BASE && phys < VIRT_DEV_END)
		return PLATFORM_MM_DEVICE;
	return PLATFORM_MM_UNMAPPED;
}

const platform_ram_layout_t *platform_ram_layout(void)
{
	if (!g_layout_ready)
		virt_ram_layout_init();
	return &g_layout;
}
