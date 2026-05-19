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
#include "video.h"
#include <stdint.h>

#define RASPI5_RAM_BASE_DEFAULT 0x00000000ull
#define RASPI5_RAM_SIZE_DEFAULT 0x10000000ull /* 256 MiB safety floor */

#define RASPI5_PAGE_SIZE 0x1000u
#define RASPI5_HEAP_OFFSET_FROM_KERNEL_END (16u * 0x100000u)
#define RASPI5_HEAP_TAIL_RESERVE (16u * 0x100000u)

/*
 * M9.3 scanout carve-out for the Drunix-owned HVS plane buffer.
 *
 * Location: PA 0x04000000 (64 MiB) — above ARM64_INIT_STACK_TOP
 * (0x03100000), with a 15 MiB margin for the init image / stack
 * regions to grow. Well below the firmware framebuffer at
 * 0x3f400000 and below the VC4 reserve window the firmware
 * carves out of the low 1 GiB.
 *
 * Size: visible-frame-worth × multiplier. 4x visible (=32 MiB at
 * 1920x1080x4) means the HVS plane base advances ~200 row-scrolls
 * before wrap. The carve-out is statically sized for the worst
 * case (RASPI5_VIDEO_MAX_*). Pi 5 has 4-16 GiB total; 32 MiB is
 * a rounding error in the ram_size.
 *
 * Mapped Normal-WB Inner-Shareable via the generic
 * PLATFORM_MM_NORMAL path in arm64_mmu_block_attr — no MMU work
 * here. arch_mm_init's pmm_mark_used keeps kheap and other PMM
 * clients out of the range; the kernel-linear identity map covers
 * it because it sits inside L1[0]/[0..1 GiB).
 *
 * NOT a software framebuffer in the platform_ram_layout sense
 * (framebuffer_base/size still refers to the firmware-allocated
 * Normal-NC plane at 0x3f400000). This is a Drunix-managed
 * scanout the HVS is repointed at after M9.3 install.
 */
#define RASPI5_SCANOUT_CARVE_BASE 0x04000000ull
/*
 * Multiplier was 4 in the M9.3 initial drop. Bumped to 16 here so the
 * scanout buffer is 128 MiB (1920 * 1080 * 4 * 16); the HVS scroll-y
 * wrap fires once every (16 - 1) * 67 = ~1005 scrolls instead of ~200.
 * Carve-out end = 0x04000000 + 0x08000000 = 0x0c000000 (192 MiB) —
 * still well below the firmware fb at 0x3f400000 and inside the
 * kernel-linear identity-mapped 0..2 GiB. Pi 5 has 4-16 GiB total;
 * 128 MiB of PA carved out for the console scrollback is negligible.
 *
 * The proper way to eliminate the wrap stutter entirely is M9.4
 * vblank-paced pre-emptive rotation (copy the live visible content
 * back to offset 0 incrementally as scroll-y approaches the wrap
 * point). Until that lands, a 16x buffer just makes the wrap rare
 * enough that interactive shell use never sees it.
 */
#define RASPI5_SCANOUT_CARVE_MULTIPLIER 16u
#define RASPI5_SCANOUT_CARVE_SIZE                                              \
	((uint64_t)RASPI5_VIDEO_MAX_WIDTH *                                    \
	 (uint64_t)RASPI5_VIDEO_MAX_HEIGHT *                                   \
	 (uint64_t)RASPI5_VIDEO_BYTES_PER_PIXEL *                              \
	 (uint64_t)RASPI5_SCANOUT_CARVE_MULTIPLIER)

extern char _kernel_end[];

static platform_ram_layout_t g_layout;
static int g_layout_ready;

static const platform_kernel_block_t g_extra_blocks[] = {
    /* L1[64]: 0x10_0000_0000 .. 0x10_4000_0000 — lower half of the SoC
     * peripheral range. The BCM2712 SDIO1 SDHCI host registers at
     * 0x10_00ff_f000 sit here. Added in M6 for SD card root mount. */
    {
        .virt = PLATFORM_RASPI5_SOC_LOW_BASE,
        .phys = PLATFORM_RASPI5_SOC_LOW_BASE,
        .attr = PLATFORM_MM_DEVICE,
    },
    /* L1[65]: 0x10_4000_0000 .. 0x10_8000_0000 — upper half of the SoC
     * peripheral range. uart10 and GIC-400 are here. */
    {
        .virt = PLATFORM_RASPI5_SOC_WINDOW_BASE,
        .phys = PLATFORM_RASPI5_SOC_WINDOW_BASE,
        .attr = PLATFORM_MM_DEVICE,
    },
    /* L1[124]: 0x1f_0000_0000 .. 0x1f_4000_0000 — PCIe2 outbound window.
     * RP1 UART0 lives here. */
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
	g_layout.scanout_carve_base = RASPI5_SCANOUT_CARVE_BASE;
	g_layout.scanout_carve_size = RASPI5_SCANOUT_CARVE_SIZE;
	g_layout_ready = 1;

	/* Trace the resolved layout. Kept in the boot path under the
	 * project's "diagnostic logging stays" rule; the alternative
	 * (silent layout) cost us a round-trip in M7 when the VC4
	 * framebuffer landed inside a chunk of physically-present
	 * SDRAM that the FDT's first range did not advertise. */
	{
		static const char hexd[] = "0123456789abcdef";
		char buf[20];
		uint64_t v;
		int i;
		const uint64_t labels[3] = {
		    g_layout.ram_base, g_layout.ram_size, g_layout.heap_size};
		const char *names[3] = {
		    "raspi5: ram_base=0x",
		    "raspi5: ram_size=0x",
		    "raspi5: heap_size=0x"};
		int j;
		for (j = 0; j < 3; j++) {
			v = labels[j];
			platform_uart_puts(names[j]);
			for (i = 0; i < 16; i++)
				buf[i] = hexd[(v >> ((15 - i) * 4)) & 0xfu];
			buf[16] = '\n';
			buf[17] = '\0';
			platform_uart_puts(buf);
		}
	}
}

void raspi5_register_framebuffer(uint64_t phys, uint64_t size)
{
	if (!g_layout_ready)
		raspi5_ram_layout_init();
	if (size == 0u)
		return;
	g_layout.framebuffer_base = phys;
	g_layout.framebuffer_size = size;
}

platform_mm_attr_t platform_mm_classify(uint64_t phys)
{
	if (!g_layout_ready)
		raspi5_ram_layout_init();

	/* The framebuffer span lives inside RAM but needs Normal-NC, not
	 * the default Normal-WB. Checked before the generic RAM range
	 * because PLATFORM_MM_FRAMEBUFFER is the narrower predicate. */
	if (g_layout.framebuffer_size != 0u &&
	    phys >= g_layout.framebuffer_base &&
	    phys < g_layout.framebuffer_base + g_layout.framebuffer_size)
		return PLATFORM_MM_FRAMEBUFFER;
	/*
	 * BCM2712 hardware contract: every Pi 5 SKU has at least 2 GiB of
	 * SDRAM at PA 0..0x80000000. The FDT's first /memory range can
	 * carve chunks out (VC4 firmware reservation, CMA, locked-down
	 * boot region) but those are advisory to OS-level allocators;
	 * the CPU can still read/write the bytes. The MMU classification
	 * therefore covers the full 2 GiB unconditionally so any
	 * firmware-handed pointer in that range (e.g. the mailbox-
	 * returned framebuffer address) lands inside a present L2 leaf.
	 * The PMM still uses g_layout.ram_size for its free-list bounds,
	 * so the carve-outs are respected at allocation time. M7 hit
	 * the bug version of this: FDT ranges[0] ended before the VC4
	 * framebuffer at 0x3f400000 and the L2 sweep skipped that block,
	 * causing a level-2 translation fault on first pixel write.
	 */
	if (phys < 0x80000000ull)
		return PLATFORM_MM_NORMAL;
	if (phys >= PLATFORM_RASPI5_SOC_LOW_BASE &&
	    phys < PLATFORM_RASPI5_SOC_LOW_BASE + PLATFORM_RASPI5_SOC_LOW_SIZE)
		return PLATFORM_MM_DEVICE;
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
