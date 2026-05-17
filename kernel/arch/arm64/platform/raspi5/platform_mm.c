/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * platform_mm.c - Raspberry Pi 5 RAM classifier and FDT-driven layout.
 *
 * RAM on Pi 5 starts at PA 0x0. The kernel image lives at 0x80000
 * (linker.raspi5.ld), boot.S parks the EL1 stack just below it.
 * Peripherals live above the 4 GiB boundary and are reached via
 * the two 1 GiB identity-mapped Device blocks returned from
 * platform_extra_kernel_blocks().
 *
 *   L1[65]  -> 0x10_4000_0000 (SoC: uart10, GIC-400)
 *   L1[124] -> 0x1f_0000_0000 (PCIe outbound: RP1 UART0)
 */

#include "../platform.h"
#include "../../fdt.h"
#include "../../arch_layout.h"
#include "platform.h"
#include <stdint.h>

#define RASPI5_RAM_BASE_DEFAULT 0x00000000ull
#define RASPI5_RAM_SIZE_DEFAULT 0x10000000ull /* 256 MiB safety floor */

#define RASPI5_PAGE_SIZE 0x1000u
#define RASPI5_HEAP_OFFSET_FROM_KERNEL_END (16u * 0x100000u)
#define RASPI5_HEAP_TAIL_RESERVE (16u * 0x100000u)

extern char _kernel_end[];

static platform_ram_layout_t g_layout;
static int g_layout_ready;

static const platform_kernel_block_t g_extra_blocks[] = {
    {
        .virt = PLATFORM_RASPI5_SOC_WINDOW_BASE,
        .phys = PLATFORM_RASPI5_SOC_WINDOW_BASE,
        .attr = PLATFORM_MM_DEVICE,
    },
    {
        .virt = PLATFORM_RASPI5_PCIE_WINDOW_BASE,
        .phys = PLATFORM_RASPI5_PCIE_WINDOW_BASE,
        .attr = PLATFORM_MM_DEVICE,
    },
};

static void raspi5_layout_finalize_defaults(void)
{
	g_layout.ram_base = RASPI5_RAM_BASE_DEFAULT;
	g_layout.ram_size = RASPI5_RAM_SIZE_DEFAULT;
}

void raspi5_ram_layout_init(void)
{
	fdt_memory_range_t ranges[FDT_MAX_MEMORY_RANGES];
	uint32_t count = 0;
	uint64_t kernel_end_aligned;
	const void *fdt;

	if (g_layout_ready)
		return;

	raspi5_layout_finalize_defaults();

	fdt = (const void *)(uintptr_t)g_fdt_blob_phys;
	if (!fdt) {
		platform_uart_puts(
		    "raspi5: no FDT pointer; using 256 MiB RAM default\n");
	} else if (fdt_validate(fdt) != 0) {
		platform_uart_puts(
		    "raspi5: FDT header invalid; using 256 MiB RAM default\n");
	} else if (fdt_get_memory(fdt, ranges, FDT_MAX_MEMORY_RANGES, &count) != 0 ||
	           count == 0u) {
		platform_uart_puts("raspi5: FDT /memory parse failed; using default\n");
	} else {
		g_layout.ram_base = ranges[0].base;
		g_layout.ram_size = ranges[0].size;

		/* Identity-map of low RAM uses L1[0] (2 MiB L2 blocks across
		 * the first 1 GiB) and L1[1] (a 1 GiB block). Anything beyond
		 * 2 GiB is currently unreachable by the kernel's heap walker
		 * until a later milestone splits L1[1]; truncate to keep the
		 * promise of the existing kernel address space. */
		if (g_layout.ram_size > 0x80000000ull) {
			platform_uart_puts(
			    "raspi5: RAM > 2 GiB; truncating for kernel-linear-map limit\n");
			g_layout.ram_size = 0x80000000ull;
		}
	}

	kernel_end_aligned =
	    ((uint64_t)(uintptr_t)_kernel_end + (uint64_t)RASPI5_PAGE_SIZE - 1u) &
	    ~((uint64_t)RASPI5_PAGE_SIZE - 1u);
	g_layout.kernel_image_end = kernel_end_aligned;

	g_layout.heap_base =
	    kernel_end_aligned + RASPI5_HEAP_OFFSET_FROM_KERNEL_END;
	if (g_layout.ram_base + g_layout.ram_size <
	    g_layout.heap_base + RASPI5_HEAP_TAIL_RESERVE) {
		platform_uart_puts("raspi5: heap range invalid; using defaults\n");
		raspi5_layout_finalize_defaults();
		g_layout.kernel_image_end = kernel_end_aligned;
		g_layout.heap_base =
		    kernel_end_aligned + RASPI5_HEAP_OFFSET_FROM_KERNEL_END;
	}
	g_layout.heap_size = (g_layout.ram_base + g_layout.ram_size) -
	                     (uint64_t)RASPI5_HEAP_TAIL_RESERVE -
	                     g_layout.heap_base;

	g_layout.framebuffer_base = 0;
	g_layout.framebuffer_size = 0;
	g_layout_ready = 1;
}

platform_mm_attr_t platform_mm_classify(uint64_t phys)
{
	if (!g_layout_ready)
		raspi5_ram_layout_init();

	if (phys >= g_layout.ram_base &&
	    phys < g_layout.ram_base + g_layout.ram_size)
		return PLATFORM_MM_NORMAL;
	if (phys >= PLATFORM_RASPI5_SOC_WINDOW_BASE &&
	    phys < PLATFORM_RASPI5_SOC_WINDOW_BASE + PLATFORM_RASPI5_SOC_WINDOW_SIZE)
		return PLATFORM_MM_DEVICE;
	if (phys >= PLATFORM_RASPI5_PCIE_WINDOW_BASE &&
	    phys <
	        PLATFORM_RASPI5_PCIE_WINDOW_BASE + PLATFORM_RASPI5_PCIE_WINDOW_SIZE)
		return PLATFORM_MM_DEVICE;
	return PLATFORM_MM_UNMAPPED;
}

const platform_ram_layout_t *platform_ram_layout(void)
{
	if (!g_layout_ready)
		raspi5_ram_layout_init();
	return &g_layout;
}

uint32_t platform_extra_kernel_blocks(const platform_kernel_block_t **out)
{
	if (out)
		*out = g_extra_blocks;
	return (uint32_t)(sizeof(g_extra_blocks) / sizeof(g_extra_blocks[0]));
}
