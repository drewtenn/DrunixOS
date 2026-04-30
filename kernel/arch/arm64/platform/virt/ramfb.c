/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ramfb.c - QEMU ramfb framebuffer bring-up on the arm64 virt machine.
 *
 * Phase 1 M2.5a. After this commit, `make ARCH=arm64 PLATFORM=virt run`
 * (with -device ramfb and a non-headless display) shows a 1024×768×32
 * black framebuffer in the QEMU window, and userspace can mmap /dev/fb0
 * with the right cache attribute. The desktop launch + input lands in
 * M2.5b.
 *
 * Memory model: the framebuffer span is reserved at boot inside
 * platform_ram_layout()->framebuffer_{base,size}. arm64_mmu_init's
 * platform classifier returns PLATFORM_MM_FRAMEBUFFER for those pages,
 * so the kernel linear map gets Normal-NC PTEs at attribute-stamp time
 * (no Normal-WB alias is ever installed). Any user-side mmap of
 * /dev/fb0 picks up the same MAIR slot via the ARCH_MM_MAP_NC path
 * driven by fbdev's mmap_cache_policy hook.
 */

#include "../platform.h"
#include "fwcfg.h"
#include "ramfb.h"
#include "../../dma.h"
#include "../../mm/mmu.h"
#include "fbdev.h"
#include "kprintf.h"
#include "kstring.h"
#include <stdint.h>

/*
 * RAMFBCfg is documented in QEMU's hw/display/ramfb.c. All multi-byte
 * fields are big-endian on the wire regardless of guest endian.
 */
typedef struct {
	uint64_t addr_be64;
	uint32_t fourcc_be32;
	uint32_t flags_be32;
	uint32_t width_be32;
	uint32_t height_be32;
	uint32_t stride_be32;
} __attribute__((packed)) ramfb_cfg_t;

/* DRM_FORMAT_XRGB8888 is fourcc_code('X','R','2','4') = 0x34325258. */
#define RAMFB_FOURCC_XRGB8888 0x34325258u

#define RAMFB_WIDTH  1024u
#define RAMFB_HEIGHT 768u
#define RAMFB_BPP    32u
#define RAMFB_PITCH  (RAMFB_WIDTH * 4u)
#define RAMFB_SIZE   (RAMFB_PITCH * RAMFB_HEIGHT)

static framebuffer_info_t g_ramfb;

static inline uint32_t bswap32(uint32_t v)
{
	return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
	       ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static inline uint32_t cpu_to_be32(uint32_t v)
{
	return bswap32(v);
}

static inline uint64_t cpu_to_be64(uint64_t v)
{
	return ((uint64_t)bswap32((uint32_t)v) << 32) |
	       (uint64_t)bswap32((uint32_t)(v >> 32));
}

int arm64_virt_ramfb_init(void)
{
	const platform_ram_layout_t *l = platform_ram_layout();
	uint64_t fb_base = l->framebuffer_base;
	uint64_t fb_size = l->framebuffer_size;
	uint16_t selector = 0;
	uint32_t entry_size = 0;
	ramfb_cfg_t cfg;
	char line[96];

	if (fb_size == 0) {
		platform_uart_puts(
		    "ramfb: no framebuffer reservation in layout; skipping\n");
		return 0;
	}
	if (fb_size < RAMFB_SIZE) {
		platform_uart_puts(
		    "ramfb: reservation smaller than 1024x768x32; skipping\n");
		return 0;
	}

	if (!fwcfg_present()) {
		platform_uart_puts(
		    "ramfb: fw_cfg not present; /dev/fb0 unavailable\n");
		return 0;
	}

	if (fwcfg_find_file("etc/ramfb", &selector, &entry_size) != 0) {
		platform_uart_puts(
		    "ramfb: etc/ramfb not present; /dev/fb0 unavailable\n");
		return 0;
	}
	if (entry_size != sizeof(ramfb_cfg_t)) {
		k_snprintf(line,
		           sizeof(line),
		           "ramfb: etc/ramfb size %u != %u; skipping\n",
		           (unsigned int)entry_size,
		           (unsigned int)sizeof(ramfb_cfg_t));
		platform_uart_puts(line);
		return 0;
	}

	/*
	 * Drop the Normal-WB Inner-Shareable kernel alias for the FB
	 * span. The classifier already returns PLATFORM_MM_FRAMEBUFFER
	 * for these pages; the remap walks the kernel page tables and
	 * stamps Normal-NC leaves where the L1[1] 1 GiB block previously
	 * covered them with Normal-WB.
	 */
	if (arm64_mmu_kernel_remap_range(fb_base, fb_size) != 0) {
		platform_uart_puts(
		    "ramfb: kernel-alias remap failed; refusing to publish\n");
		return -1;
	}

	/*
	 * Evict any cache lines that may have been speculatively prefetched
	 * via the prior Normal-WB block mapping. Subsequent accesses through
	 * the new Normal-NC mapping bypass the cache, so leftover lines are
	 * harmless in steady state — but invalidating here is cheap and
	 * removes the only realistic window where guest CPU and host
	 * scan-out could disagree on the FB contents.
	 */
	arm64_dma_cache_invalidate((void *)(uintptr_t)fb_base,
	                           (uint32_t)fb_size);

	/* Zero the framebuffer once the kernel mapping is Normal-NC, so
	 * the host-side scan-out reads zeros (black) rather than whatever
	 * boot left behind. */
	k_memset((void *)(uintptr_t)fb_base, 0, RAMFB_SIZE);

	/* Build and submit the RAMFBCfg. */
	cfg.addr_be64 = cpu_to_be64(fb_base);
	cfg.fourcc_be32 = cpu_to_be32(RAMFB_FOURCC_XRGB8888);
	cfg.flags_be32 = 0;
	cfg.width_be32 = cpu_to_be32(RAMFB_WIDTH);
	cfg.height_be32 = cpu_to_be32(RAMFB_HEIGHT);
	cfg.stride_be32 = cpu_to_be32(RAMFB_PITCH);

	if (fwcfg_dma_write(selector, &cfg, sizeof(cfg)) != 0) {
		platform_uart_puts(
		    "ramfb: fwcfg DMA write failed; refusing to publish\n");
		return -1;
	}

	/*
	 * Build the framebuffer_info_t for the chardev layer. Both
	 * phys_address and address are the same (identity-mapped via
	 * g_kernel_l1[1]); the fbdev assertion at fbdev.c:82 only
	 * requires they be non-zero.
	 *
	 * Color masks: XRGB8888 → R[23:16], G[15:8], B[7:0], X[31:24].
	 */
	g_ramfb.phys_address = (uintptr_t)fb_base;
	g_ramfb.address = (uintptr_t)fb_base;
	g_ramfb.pitch = RAMFB_PITCH;
	g_ramfb.width = RAMFB_WIDTH;
	g_ramfb.height = RAMFB_HEIGHT;
	g_ramfb.bpp = RAMFB_BPP;
	g_ramfb.red_pos = 16;
	g_ramfb.red_size = 8;
	g_ramfb.green_pos = 8;
	g_ramfb.green_size = 8;
	g_ramfb.blue_pos = 0;
	g_ramfb.blue_size = 8;
	g_ramfb.cell_cols = 0;
	g_ramfb.cell_rows = 0;
	g_ramfb.back_address = 0;
	g_ramfb.back_pitch = 0;
	g_ramfb.cursor.x = 0;
	g_ramfb.cursor.y = 0;
	g_ramfb.cursor.fg = 0;
	g_ramfb.cursor.shadow = 0;
	g_ramfb.cursor.visible = 0;

	if (fbdev_init(&g_ramfb) != 0) {
		platform_uart_puts(
		    "ramfb: fbdev_init refused; /dev/fb0 unavailable\n");
		return -1;
	}

	k_snprintf(line,
	           sizeof(line),
	           "ramfb: %ux%ux%u @ phys 0x%lx published as /dev/fb0\n",
	           (unsigned int)RAMFB_WIDTH,
	           (unsigned int)RAMFB_HEIGHT,
	           (unsigned int)RAMFB_BPP,
	           (unsigned long)fb_base);
	platform_uart_puts(line);
	return 0;
}
