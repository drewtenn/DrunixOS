/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * raspi5/video.c — BCM2712 VideoCore HDMI framebuffer driver.
 *
 * Mailbox MMIO is at 0x10_7c01_3880 (mailbox@7c013880 in
 * bcm2712-rpi-5-b.dtb, translated through the soc@107c000000 ranges
 * block). The address sits inside the L1[65] identity-mapped Device
 * block already established in M6 for the SoC high peripheral window,
 * so the driver dereferences a 64-bit physical pointer directly with
 * the MMU on.
 *
 * The protocol is the legacy BCM2835 channel-8 property interface
 * (compatible = "brcm,bcm2835-mbox" on Pi 5 too). Pi 5 firmware
 * implements it as a documented subset; the SET_PHYSICAL_SIZE,
 * SET_VIRTUAL_SIZE, SET_DEPTH, SET_PIXEL_ORDER, ALLOCATE_BUFFER, and
 * GET_PITCH tags are part of the subset and are all that M7 needs.
 *
 * The driver publishes both the in-kernel fb_text_console (so HDMI
 * shows kernel boot messages) and /dev/fb0 via fbdev_init (so future
 * compositor work can mmap when input lands in M8).
 */

#include "video.h"
#include "../platform.h"
#include "platform.h"
#include "pmm.h"
#include "fb_text_console.h"
#include <stdint.h>

#define RASPI5_MBOX_BASE 0x107c013880ull
#define RASPI5_MBOX_READ 0u
#define RASPI5_MBOX_STATUS 6u
#define RASPI5_MBOX_WRITE 8u
#define RASPI5_MBOX_FULL 0x80000000u
#define RASPI5_MBOX_EMPTY 0x40000000u
#define RASPI5_MBOX_CHANNEL_PROPERTY 8u
#define RASPI5_MBOX_TIMEOUT 10000000u

#define RASPI5_MBOX_REQUEST 0x00000000u
#define RASPI5_MBOX_RESPONSE_SUCCESS 0x80000000u
#define RASPI5_MBOX_TAG_RESPONSE 0x80000000u

#define RASPI5_TAG_ALLOCATE_BUFFER 0x00040001u
#define RASPI5_TAG_GET_PITCH 0x00040008u
#define RASPI5_TAG_SET_PHYSICAL_SIZE 0x00048003u
#define RASPI5_TAG_SET_VIRTUAL_SIZE 0x00048004u
#define RASPI5_TAG_SET_DEPTH 0x00048005u
#define RASPI5_TAG_SET_PIXEL_ORDER 0x00048006u
#define RASPI5_TAG_END 0x00000000u

#define RASPI5_VIDEO_FB_ALIGNMENT 16u
/*
 * VC4 firmware pixel-order values: 0 = BGR (red in low byte), 1 = RGB
 * (blue in low byte). Pi 5 firmware ignores our requested
 * SET_PIXEL_ORDER and returns whichever order the HVS / EDID handshake
 * settled on — empirically 0 (BGR) on the boards we have tested. The
 * driver therefore drives framebuffer_info_from_rgb with channel
 * positions chosen at runtime from the returned value, instead of
 * rejecting the bring-up because firmware disagreed with the request.
 */
#define RASPI5_VIDEO_PIXEL_ORDER_BGR 0u
#define RASPI5_VIDEO_PIXEL_ORDER_RGB 1u
#define RASPI5_VIDEO_BYTES_PER_PIXEL 4u
#define RASPI5_VIDEO_RAM_CEILING 0x80000000ull
#define RASPI5_VIDEO_CONSOLE_CELLS                                             \
	((RASPI5_VIDEO_WIDTH / GUI_FONT_W) * (RASPI5_VIDEO_HEIGHT / GUI_FONT_H))

enum raspi5_video_request_index {
	RASPI5_VIDEO_REQ_SIZE = 0,
	RASPI5_VIDEO_REQ_CODE = 1,
	RASPI5_VIDEO_REQ_PHYSICAL_TAG = 2,
	RASPI5_VIDEO_REQ_PHYSICAL_SIZE = 3,
	RASPI5_VIDEO_REQ_PHYSICAL_CODE = 4,
	RASPI5_VIDEO_REQ_PHYSICAL_WIDTH = 5,
	RASPI5_VIDEO_REQ_PHYSICAL_HEIGHT = 6,
	RASPI5_VIDEO_REQ_VIRTUAL_TAG = 7,
	RASPI5_VIDEO_REQ_VIRTUAL_SIZE = 8,
	RASPI5_VIDEO_REQ_VIRTUAL_CODE = 9,
	RASPI5_VIDEO_REQ_VIRTUAL_WIDTH = 10,
	RASPI5_VIDEO_REQ_VIRTUAL_HEIGHT = 11,
	RASPI5_VIDEO_REQ_DEPTH_TAG = 12,
	RASPI5_VIDEO_REQ_DEPTH_SIZE = 13,
	RASPI5_VIDEO_REQ_DEPTH_CODE = 14,
	RASPI5_VIDEO_REQ_DEPTH_VALUE = 15,
	RASPI5_VIDEO_REQ_PIXEL_TAG = 16,
	RASPI5_VIDEO_REQ_PIXEL_SIZE = 17,
	RASPI5_VIDEO_REQ_PIXEL_CODE = 18,
	RASPI5_VIDEO_REQ_PIXEL_ORDER = 19,
	RASPI5_VIDEO_REQ_ALLOC_TAG = 20,
	RASPI5_VIDEO_REQ_ALLOC_SIZE = 21,
	RASPI5_VIDEO_REQ_ALLOC_CODE = 22,
	RASPI5_VIDEO_REQ_ALLOC_ADDRESS = 23,
	RASPI5_VIDEO_REQ_ALLOC_BYTES = 24,
	RASPI5_VIDEO_REQ_PITCH_TAG = 25,
	RASPI5_VIDEO_REQ_PITCH_SIZE = 26,
	RASPI5_VIDEO_REQ_PITCH_CODE = 27,
	RASPI5_VIDEO_REQ_PITCH_VALUE = 28,
	RASPI5_VIDEO_REQ_END = 29,
	RASPI5_VIDEO_REQ_WORDS = 30,
};

/* Property request lives in BSS so it sits in low RAM (well below
 * the 4 GiB ceiling the 32-bit mailbox interface can address) and
 * the 16-byte alignment requirement is satisfied at link time. */
static volatile uint32_t g_request[RASPI5_VIDEO_REQ_WORDS]
    __attribute__((aligned(16)));
static framebuffer_info_t g_fb_info;
static fb_text_console_t g_fb_console;
static gui_cell_t g_fb_cells[RASPI5_VIDEO_CONSOLE_CELLS];
static int g_fb_ready;

/* Forward decl from kernel/drivers/fbdev.h. raspi3b/video.c keeps the
 * include in a different transitive path; pulling it in here would
 * introduce a wider include surface than the file needs. */
int fbdev_init(const framebuffer_info_t *fb);

/*
 * Translate a 32-bit VC4 bus address returned by the mailbox
 * ALLOCATE_BUFFER tag to a CPU-physical address. BCM2712 declares
 * identity dma-ranges for the AXI bus the CPU sees, so for the
 * firmware paths that go through dma_alloc_coherent this is a
 * pass-through. The Pi 3 legacy 0xc0000000 alias path is NOT
 * applied here because on Pi 5 a returned 0xc0000000 could equally
 * be a legitimate identity-mapped 3 GiB SDRAM address; masking it
 * would translate to phys 0 and clobber the kernel image. Caller
 * (arm64_video_init) clamps the resolved phys against the 2 GiB
 * linear-map ceiling, which catches both the >= 2 GiB identity
 * case and any high-bus surprise.
 */
uint64_t raspi5_fb_bus_to_phys(uint32_t bus)
{
	return (uint64_t)bus;
}

static volatile uint32_t *raspi5_mailbox_regs(void)
{
	return (volatile uint32_t *)(uintptr_t)RASPI5_MBOX_BASE;
}

static void raspi5_video_dsb(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Clean a [start, end) virtual range to the Point of Coherency. The
 * VC4 mailbox transport reads CPU-side request buffers from physical
 * RAM through a non-coherent path; a dsb alone only drains the CPU
 * store buffer into L1 / L2. CVAC walks each cache line and forces
 * dirty data out to system memory so the VPU and HVS see the bytes
 * the kernel just wrote. Range is rounded outward to cache-line
 * alignment; the BCM2712 Cortex-A76 D-cache line is 64 bytes.
 */
#define RASPI5_DCACHE_LINE 64u

static void raspi5_dc_cvac_range(const void *start, uint32_t bytes)
{
	uintptr_t addr = (uintptr_t)start & ~(uintptr_t)(RASPI5_DCACHE_LINE - 1u);
	uintptr_t end = (uintptr_t)start + bytes;
	uintptr_t end_aligned =
	    (end + (uintptr_t)RASPI5_DCACHE_LINE - 1u) &
	    ~(uintptr_t)(RASPI5_DCACHE_LINE - 1u);

	while (addr < end_aligned) {
		__asm__ volatile("dc cvac, %0" ::"r"(addr) : "memory");
		addr += RASPI5_DCACHE_LINE;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Clean + invalidate a range to PoC. Used after the mailbox returns
 * so the CPU sees the firmware-written response bytes instead of any
 * cached pre-call shadow.
 */
static void raspi5_dc_civac_range(const void *start, uint32_t bytes)
{
	uintptr_t addr = (uintptr_t)start & ~(uintptr_t)(RASPI5_DCACHE_LINE - 1u);
	uintptr_t end = (uintptr_t)start + bytes;
	uintptr_t end_aligned =
	    (end + (uintptr_t)RASPI5_DCACHE_LINE - 1u) &
	    ~(uintptr_t)(RASPI5_DCACHE_LINE - 1u);

	while (addr < end_aligned) {
		__asm__ volatile("dc civac, %0" ::"r"(addr) : "memory");
		addr += RASPI5_DCACHE_LINE;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Plain hex32 line: "label=0xXXXXXXXX\n". Used for the compact bring-up
 * trace. Avoids pulling kprintf into the video.o to keep the failure
 * paths simple (kprintf depends on a working terminal). */
static void raspi5_video_trace_u32(const char *label, uint32_t v)
{
	static const char hexd[] = "0123456789abcdef";
	char buf[12];
	int i;

	platform_uart_puts(label);
	platform_uart_puts("=0x");
	for (i = 0; i < 8; i++)
		buf[i] = hexd[(v >> ((7 - i) * 4)) & 0xfu];
	buf[8] = '\n';
	buf[9] = '\0';
	platform_uart_puts(buf);
}

static void raspi5_video_trace_u64(const char *label, uint64_t v)
{
	static const char hexd[] = "0123456789abcdef";
	char buf[20];
	int i;

	platform_uart_puts(label);
	platform_uart_puts("=0x");
	for (i = 0; i < 16; i++)
		buf[i] = hexd[(v >> ((15 - i) * 4)) & 0xfu];
	buf[16] = '\n';
	buf[17] = '\0';
	platform_uart_puts(buf);
}

static int raspi5_mailbox_call(volatile uint32_t *request)
{
	volatile uint32_t *mailbox = raspi5_mailbox_regs();
	uint32_t address = (uint32_t)(uintptr_t)request;
	uint32_t message;
	uint32_t timeout;

	if ((address & 0xfu) != 0u) {
		platform_uart_puts("raspi5 fb: request not 16-byte aligned\n");
		return -1;
	}

	message = (address & ~0xfu) | RASPI5_MBOX_CHANNEL_PROPERTY;

	/* Flush the request buffer out of the CPU caches before signalling
	 * the VPU; the property channel reads bytes from physical RAM
	 * through a non-coherent path. */
	raspi5_dc_cvac_range((const void *)request,
	                     (uint32_t)(RASPI5_VIDEO_REQ_WORDS * sizeof(uint32_t)));

	timeout = RASPI5_MBOX_TIMEOUT;
	while ((mailbox[RASPI5_MBOX_STATUS] & RASPI5_MBOX_FULL) != 0u) {
		if (timeout-- == 0u) {
			platform_uart_puts("raspi5 fb: mailbox FULL timeout\n");
			return -1;
		}
	}

	raspi5_video_dsb();
	mailbox[RASPI5_MBOX_WRITE] = message;
	raspi5_video_dsb();

	timeout = RASPI5_MBOX_TIMEOUT;
	for (;;) {
		uint32_t response;

		while ((mailbox[RASPI5_MBOX_STATUS] & RASPI5_MBOX_EMPTY) != 0u) {
			if (timeout-- == 0u) {
				platform_uart_puts(
				    "raspi5 fb: mailbox EMPTY timeout\n");
				return -1;
			}
		}

		response = mailbox[RASPI5_MBOX_READ];
		if ((response & 0xfu) == RASPI5_MBOX_CHANNEL_PROPERTY &&
		    (response & ~0xfu) == (address & ~0xfu)) {
			raspi5_video_dsb();
			/* Invalidate any stale cached copy of the request
			 * buffer so the per-tag response codes the VPU just
			 * wrote are the ones the caller reads. */
			raspi5_dc_civac_range(
			    (const void *)request,
			    (uint32_t)(RASPI5_VIDEO_REQ_WORDS *
			               sizeof(uint32_t)));
			return 0;
		}

		if (timeout-- == 0u) {
			platform_uart_puts(
			    "raspi5 fb: mailbox response mismatch timeout\n");
			return -1;
		}
	}
}

static void raspi5_video_build_request(void)
{
	g_request[RASPI5_VIDEO_REQ_SIZE] =
	    RASPI5_VIDEO_REQ_WORDS * sizeof(uint32_t);
	g_request[RASPI5_VIDEO_REQ_CODE] = RASPI5_MBOX_REQUEST;

	g_request[RASPI5_VIDEO_REQ_PHYSICAL_TAG] = RASPI5_TAG_SET_PHYSICAL_SIZE;
	g_request[RASPI5_VIDEO_REQ_PHYSICAL_SIZE] = 8u;
	g_request[RASPI5_VIDEO_REQ_PHYSICAL_CODE] = 0u;
	g_request[RASPI5_VIDEO_REQ_PHYSICAL_WIDTH] = RASPI5_VIDEO_WIDTH;
	g_request[RASPI5_VIDEO_REQ_PHYSICAL_HEIGHT] = RASPI5_VIDEO_HEIGHT;

	g_request[RASPI5_VIDEO_REQ_VIRTUAL_TAG] = RASPI5_TAG_SET_VIRTUAL_SIZE;
	g_request[RASPI5_VIDEO_REQ_VIRTUAL_SIZE] = 8u;
	g_request[RASPI5_VIDEO_REQ_VIRTUAL_CODE] = 0u;
	g_request[RASPI5_VIDEO_REQ_VIRTUAL_WIDTH] = RASPI5_VIDEO_WIDTH;
	g_request[RASPI5_VIDEO_REQ_VIRTUAL_HEIGHT] = RASPI5_VIDEO_HEIGHT;

	g_request[RASPI5_VIDEO_REQ_DEPTH_TAG] = RASPI5_TAG_SET_DEPTH;
	g_request[RASPI5_VIDEO_REQ_DEPTH_SIZE] = 4u;
	g_request[RASPI5_VIDEO_REQ_DEPTH_CODE] = 0u;
	g_request[RASPI5_VIDEO_REQ_DEPTH_VALUE] = RASPI5_VIDEO_DEPTH;

	g_request[RASPI5_VIDEO_REQ_PIXEL_TAG] = RASPI5_TAG_SET_PIXEL_ORDER;
	g_request[RASPI5_VIDEO_REQ_PIXEL_SIZE] = 4u;
	g_request[RASPI5_VIDEO_REQ_PIXEL_CODE] = 0u;
	g_request[RASPI5_VIDEO_REQ_PIXEL_ORDER] = RASPI5_VIDEO_PIXEL_ORDER_RGB;

	g_request[RASPI5_VIDEO_REQ_ALLOC_TAG] = RASPI5_TAG_ALLOCATE_BUFFER;
	g_request[RASPI5_VIDEO_REQ_ALLOC_SIZE] = 8u;
	g_request[RASPI5_VIDEO_REQ_ALLOC_CODE] = 0u;
	g_request[RASPI5_VIDEO_REQ_ALLOC_ADDRESS] = RASPI5_VIDEO_FB_ALIGNMENT;
	g_request[RASPI5_VIDEO_REQ_ALLOC_BYTES] = 0u;

	g_request[RASPI5_VIDEO_REQ_PITCH_TAG] = RASPI5_TAG_GET_PITCH;
	g_request[RASPI5_VIDEO_REQ_PITCH_SIZE] = 4u;
	g_request[RASPI5_VIDEO_REQ_PITCH_CODE] = 0u;
	g_request[RASPI5_VIDEO_REQ_PITCH_VALUE] = 0u;

	g_request[RASPI5_VIDEO_REQ_END] = RASPI5_TAG_END;
}

static int raspi5_video_tag_ok(uint32_t request_code, uint32_t value_size)
{
	return (request_code & RASPI5_MBOX_TAG_RESPONSE) != 0u &&
	       (request_code & ~RASPI5_MBOX_TAG_RESPONSE) >= value_size;
}

/*
 * Dump the per-tag response codes and values to serial. Called
 * unconditionally so we can diagnose firmware that ignored the
 * requested geometry (Pi 5 firmware is known to do this when an
 * EDID-detected resolution conflicts with the SET_PHYSICAL_SIZE
 * request).
 */
static void raspi5_video_trace_mode_response(void)
{
	raspi5_video_trace_u32("raspi5 fb: phys_code",
	                       g_request[RASPI5_VIDEO_REQ_PHYSICAL_CODE]);
	raspi5_video_trace_u32("raspi5 fb: phys_w",
	                       g_request[RASPI5_VIDEO_REQ_PHYSICAL_WIDTH]);
	raspi5_video_trace_u32("raspi5 fb: phys_h",
	                       g_request[RASPI5_VIDEO_REQ_PHYSICAL_HEIGHT]);
	raspi5_video_trace_u32("raspi5 fb: virt_code",
	                       g_request[RASPI5_VIDEO_REQ_VIRTUAL_CODE]);
	raspi5_video_trace_u32("raspi5 fb: virt_w",
	                       g_request[RASPI5_VIDEO_REQ_VIRTUAL_WIDTH]);
	raspi5_video_trace_u32("raspi5 fb: virt_h",
	                       g_request[RASPI5_VIDEO_REQ_VIRTUAL_HEIGHT]);
	raspi5_video_trace_u32("raspi5 fb: depth_code",
	                       g_request[RASPI5_VIDEO_REQ_DEPTH_CODE]);
	raspi5_video_trace_u32("raspi5 fb: depth",
	                       g_request[RASPI5_VIDEO_REQ_DEPTH_VALUE]);
	raspi5_video_trace_u32("raspi5 fb: pixel_code",
	                       g_request[RASPI5_VIDEO_REQ_PIXEL_CODE]);
	raspi5_video_trace_u32("raspi5 fb: pixel_order",
	                       g_request[RASPI5_VIDEO_REQ_PIXEL_ORDER]);
}

static int raspi5_video_validate_mode_response(void)
{
	if (!raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_PHYSICAL_CODE], 8u) ||
	    !raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_VIRTUAL_CODE], 8u) ||
	    !raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_DEPTH_CODE], 4u) ||
	    !raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_PIXEL_CODE], 4u)) {
		platform_uart_puts("raspi5 fb: per-tag response code invalid\n");
		return -1;
	}

	if (g_request[RASPI5_VIDEO_REQ_PHYSICAL_WIDTH] != RASPI5_VIDEO_WIDTH ||
	    g_request[RASPI5_VIDEO_REQ_PHYSICAL_HEIGHT] != RASPI5_VIDEO_HEIGHT) {
		platform_uart_puts(
		    "raspi5 fb: physical dimensions differ from requested\n");
		return -1;
	}
	if (g_request[RASPI5_VIDEO_REQ_VIRTUAL_WIDTH] != RASPI5_VIDEO_WIDTH ||
	    g_request[RASPI5_VIDEO_REQ_VIRTUAL_HEIGHT] != RASPI5_VIDEO_HEIGHT) {
		platform_uart_puts(
		    "raspi5 fb: virtual dimensions differ from requested\n");
		return -1;
	}
	if (g_request[RASPI5_VIDEO_REQ_DEPTH_VALUE] != RASPI5_VIDEO_DEPTH) {
		platform_uart_puts("raspi5 fb: depth differs from requested\n");
		return -1;
	}
	if (g_request[RASPI5_VIDEO_REQ_PIXEL_ORDER] != RASPI5_VIDEO_PIXEL_ORDER_RGB &&
	    g_request[RASPI5_VIDEO_REQ_PIXEL_ORDER] != RASPI5_VIDEO_PIXEL_ORDER_BGR) {
		platform_uart_puts(
		    "raspi5 fb: pixel order outside {0,1}; rejecting\n");
		return -1;
	}
	return 0;
}

static int raspi5_video_framebuffer_size_ok(uint32_t pitch, uint32_t fb_size)
{
	uint64_t row_offset =
	    (uint64_t)(RASPI5_VIDEO_HEIGHT - 1u) * (uint64_t)pitch;
	uint64_t row_bytes =
	    (uint64_t)RASPI5_VIDEO_WIDTH * (uint64_t)RASPI5_VIDEO_BYTES_PER_PIXEL;
	uint64_t required = row_offset + row_bytes;

	if (required < row_offset || required > (uint64_t)UINT32_MAX)
		return 0;
	return (uint64_t)fb_size >= required;
}

int arm64_video_init(void)
{
	uint32_t fb_bus;
	uint32_t fb_size;
	uint32_t pitch;
	uint64_t fb_phys;

	if (g_fb_ready)
		return 0;

	platform_uart_puts("raspi5 fb: bringup start\n");
	raspi5_video_trace_u64("raspi5 fb: mbox_base", RASPI5_MBOX_BASE);

	raspi5_video_build_request();
	if (raspi5_mailbox_call(g_request) != 0)
		return -1;
	raspi5_video_trace_u32("raspi5 fb: resp_code",
	                       g_request[RASPI5_VIDEO_REQ_CODE]);
	raspi5_video_trace_mode_response();
	if (g_request[RASPI5_VIDEO_REQ_CODE] != RASPI5_MBOX_RESPONSE_SUCCESS)
		return -1;
	if (raspi5_video_validate_mode_response() != 0) {
		platform_uart_puts("raspi5 fb: mode response mismatch\n");
		return -1;
	}
	if (!raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_ALLOC_CODE], 8u) ||
	    !raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_PITCH_CODE], 4u)) {
		platform_uart_puts(
		    "raspi5 fb: alloc/pitch tag response invalid\n");
		return -1;
	}

	fb_bus = g_request[RASPI5_VIDEO_REQ_ALLOC_ADDRESS];
	fb_size = g_request[RASPI5_VIDEO_REQ_ALLOC_BYTES];
	pitch = g_request[RASPI5_VIDEO_REQ_PITCH_VALUE];
	raspi5_video_trace_u32("raspi5 fb: bus_raw", fb_bus);
	raspi5_video_trace_u32("raspi5 fb: size", fb_size);
	raspi5_video_trace_u32("raspi5 fb: pitch", pitch);

	if (fb_bus == 0u || fb_size == 0u || pitch == 0u) {
		platform_uart_puts(
		    "raspi5 fb: firmware returned zero bus/size/pitch\n");
		return -1;
	}
	if (!raspi5_video_framebuffer_size_ok(pitch, fb_size)) {
		platform_uart_puts("raspi5 fb: pitch/size geometry invalid\n");
		return -1;
	}

	fb_phys = raspi5_fb_bus_to_phys(fb_bus);
	raspi5_video_trace_u64("raspi5 fb: phys", fb_phys);

	if (fb_phys + (uint64_t)fb_size > RASPI5_VIDEO_RAM_CEILING) {
		platform_uart_puts(
		    "raspi5 fb: phys above 2 GiB linear-map ceiling; "
		    "rejecting (serial fallback)\n");
		return -1;
	}

	/*
	 * Channel bit-positions on the Pi mailbox path are little-endian
	 * relative to the 32-bit pixel word:
	 *   pixel_order = 1 (RGB) → XRGB8888 → red bits 23:16, green 15:8,
	 *                  blue 7:0  → byte order in memory [B, G, R, X]
	 *   pixel_order = 0 (BGR) → XBGR8888 → red bits 7:0,  green 15:8,
	 *                  blue 23:16 → byte order in memory [R, G, B, X]
	 * Pi 5 firmware empirically returns BGR regardless of what we
	 * request via SET_PIXEL_ORDER. Pi 3 returns RGB. Both paths land
	 * here.
	 */
	{
		uint8_t red_pos;
		uint8_t blue_pos;

		if (g_request[RASPI5_VIDEO_REQ_PIXEL_ORDER] ==
		    RASPI5_VIDEO_PIXEL_ORDER_RGB) {
			red_pos = 16u;
			blue_pos = 0u;
		} else {
			red_pos = 0u;
			blue_pos = 16u;
		}
		if (framebuffer_info_from_rgb((uintptr_t)fb_phys,
		                              pitch,
		                              RASPI5_VIDEO_WIDTH,
		                              RASPI5_VIDEO_HEIGHT,
		                              RASPI5_VIDEO_DEPTH,
		                              red_pos,
		                              8u,
		                              8u,
		                              8u,
		                              blue_pos,
		                              8u,
		                              &g_fb_info) != 0) {
			platform_uart_puts(
			    "raspi5 fb: framebuffer_info_from_rgb failed\n");
			return -1;
		}
	}

	pmm_mark_used((uint32_t)fb_phys, fb_size);
	raspi5_register_framebuffer(fb_phys, (uint64_t)fb_size);

	if (fb_text_console_init(&g_fb_console,
	                         &g_fb_info,
	                         g_fb_cells,
	                         RASPI5_VIDEO_CONSOLE_CELLS) != 0) {
		platform_uart_puts("raspi5 fb: fb_text_console_init failed\n");
		return -1;
	}

	if (fbdev_init(&g_fb_info) != 0) {
		platform_uart_puts(
		    "raspi5 fb: fbdev_init failed; /dev/fb0 unavailable\n");
		return -1;
	}

	g_fb_ready = 1;
	platform_uart_puts("raspi5 fb: ready (fb_text_console + /dev/fb0)\n");
	return 0;
}

int arm64_video_enabled(void)
{
	return g_fb_ready;
}

framebuffer_info_t *arm64_video_framebuffer(void)
{
	if (!g_fb_ready)
		return 0;
	return &g_fb_info;
}

/*
 * Boot console write path. The kernel-linear identity map for the
 * framebuffer pages was built at boot, before the mailbox call
 * returned the fb_phys range, so those pages are still mapped
 * Normal-WB (cacheable) via the generic PLATFORM_MM_NORMAL
 * classification — the PLATFORM_MM_FRAMEBUFFER attribute only
 * applies to fresh L1 / L2 splits taken after the fb range was
 * registered with the layout. BCM2712's HVS reads scanout pixels
 * from physical RAM through a non-coherent path, so after each
 * fb_text_console_write the dirty cache lines must be cleaned to
 * the Point of Coherency or the display will show stale (or
 * blank) pixels until natural eviction pushes them out.
 *
 * A full clean over the framebuffer span (3 MiB at 1024x768x32)
 * costs ~50,000 dc cvac instructions per write — fine for the
 * slow boot-text rate, but the compositor flow in M8 will want
 * either a true Normal-NC remap (Option A from the M7 design
 * synthesis) or a per-rect dirty hint to bound this cost.
 */
void arm64_video_console_write(const char *buf, uint32_t len)
{
	uint32_t fb_bytes;

	if (!g_fb_ready)
		return;
	fb_text_console_write(&g_fb_console, buf, len);
	fb_bytes = g_fb_info.pitch * g_fb_info.height;
	raspi5_dc_cvac_range((const void *)g_fb_info.address, fb_bytes);
}

int platform_framebuffer_acquire(framebuffer_info_t **out)
{
	int rc;

	if (!out)
		return -1;

	rc = arm64_video_init();
	if (rc != 0)
		return rc;

	*out = arm64_video_framebuffer();
	return *out ? 0 : -1;
}

void platform_framebuffer_console_write(const char *buf, uint32_t len)
{
	arm64_video_console_write(buf, len);
}
