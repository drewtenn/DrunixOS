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
 * SET_VIRTUAL_SIZE, SET_DEPTH, SET_PIXEL_ORDER, SET_VIRTUAL_OFFSET,
 * ALLOCATE_BUFFER, and GET_PITCH tags are part of the subset used here.
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
#include "kstring.h"
#include "hvs.h"
#include "pv.h"
#include "chardev.h"
#include "desktop_window.h"
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
 * Virtual-buffer multiplier: how many visible frames tall the
 * SET_VIRTUAL_SIZE request asks for. The raspi5_video_scroll_pixels
 * fast path pans through this buffer with SET_VIRTUAL_OFFSET, falling
 * back to a full software present when the pan wraps off the end.
 *
 * Pi 5 firmware was observed in M7 to "politely echo" the requested
 * SET_VIRTUAL_SIZE in the response while allocating only the visible
 * frame (fb_size = pitch * visible_height). With PAN_PAGES = 4 the
 * fast path's precondition gate (virtual_height >= visible + GUI_FONT_H)
 * therefore failed on every boot, falling back to full present_all on
 * every scroll — the visible "redraw crawls top-to-bottom" symptom.
 *
 * Dropped to 2 to probe whether the firmware caps virtual at visible
 * unconditionally, or rejects only oversize requests. After the next
 * boot, compare the trace lines:
 *   raspi5 fb: virt_h         (firmware's response echo)
 *   raspi5 fb: virt_actual_h  (fb_size / pitch — actual allocation)
 * If virt_actual_h reports 2160, the firmware honored a 2x request and
 * the pan fast path becomes live. If it stays at 1080, the firmware
 * refuses any virtual > physical and HW pan is unreachable via the
 * mailbox property interface.
 */
#define RASPI5_VIDEO_PAN_PAGES 2u
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
/* RASPI5_VIDEO_BYTES_PER_PIXEL is published in video.h so platform_mm.c
 * can size the M9.3 scanout carve-out from the same constants. */
#define RASPI5_VIDEO_RAM_CEILING 0x80000000ull
/*
 * BSS reservation for the fb_text_console cell grid. Sized for the
 * 1920 x 1080 upper bound (declared in video.h) so any monitor up to
 * that geometry can be driven by a single static buffer; the actual
 * cell count handed to fb_text_console_init at runtime is derived
 * from the firmware-returned width and height after ALLOCATE_BUFFER.
 */
#define RASPI5_VIDEO_MAX_CONSOLE_CELLS                                         \
	((RASPI5_VIDEO_MAX_WIDTH / GUI_FONT_W) *                                   \
	 (RASPI5_VIDEO_MAX_HEIGHT / GUI_FONT_H))

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
static gui_cell_t g_fb_cells[RASPI5_VIDEO_MAX_CONSOLE_CELLS];
static int g_fb_ready;

/*
 * M9.3 scanout buffer — virtual height = N * visible height so the
 * HVS can pan instead of forcing the CPU to memmove on every scroll.
 *
 * The previous one-frame implementation cost 56ms per scroll because
 * memmove of 8 MiB had to fight the HVS DMA for the memory controller
 * (HVS reads ~480 MB/s steady-state at 60Hz × 1920x1080x4) AND drain
 * 8 MiB of dirty cache lines through CVAC+DSB. With an Nx-tall virtual
 * buffer, scroll advances the HVS plane base address by one font row
 * and clears only the new bottom row (~120 KB write + CVAC). Memmove
 * only happens when the offset wraps, and it can be done in one batch.
 *
 * The buffer lives in a dedicated PA carve-out (raspi5 platform_mm.c
 * RASPI5_SCANOUT_CARVE_BASE = 0x04000000, size set by
 * RASPI5_SCANOUT_CARVE_MULTIPLIER). The carve-out:
 *
 *   - sits above ARM64_INIT_STACK_TOP (0x03100000) so it can't
 *     collide with kernel reserved low-PA regions (kernel image,
 *     init image, init stack);
 *   - is identity-mapped Normal-WB Inner-Shareable via the existing
 *     PLATFORM_MM_NORMAL path in arm64_mmu_block_attr (PA < 2 GiB);
 *   - is reserved in PMM by arch_mm_init so kheap, fbdev_init, ELF
 *     loaders, etc. never hand the pages out;
 *   - is sized at link/compile time from the same MAX_WIDTH/HEIGHT/
 *     BYTES_PER_PIXEL constants this driver uses, so the buffer
 *     can't outrun its reservation.
 *
 * The buffer pointer is initialised at arm64_video_init time from
 * platform_ram_layout()->scanout_carve_base. Driver code reads it
 * through g_hvs_scanout_buf so the address is one runtime lookup
 * away if the platform layout ever changes.
 */

static uint8_t *g_hvs_scanout_buf;
static uint64_t g_hvs_scanout_size;

static raspi5_hvs_plane_ref_t g_hvs_plane;

/* Pixel-row offset of the visible window's top within the virtual
 * buffer. Advanced by GUI_FONT_H on each scroll; wraps to 0 after a
 * memmove copies the live visible window back to the top. */
static uint32_t g_hvs_scroll_y;

/* Total virtual height in pixels = sizeof(buffer) / pitch. Computed
 * once at install-time from the actual mode pitch. */
static uint32_t g_hvs_virtual_height_px;

/* Forward decls from kernel/drivers/fbdev.h. raspi3b/video.c keeps the
 * include in a different transitive path; pulling it in here would
 * introduce a wider include surface than the file needs. */
int fbdev_init(const framebuffer_info_t *fb);
void fbdev_set_cache_policy(chardev_cache_policy_t policy);
void fbdev_set_publish_dirty_rect(void (*hook)(drunix_rect_t));

/* Forward decl for the targeted dc cvac hook; definition lives below
 * arm64_video_init so the broader bring-up flow reads top-down. */
static void raspi5_video_dirty_pixels(uint32_t x, uint32_t y, uint32_t w,
                                      uint32_t h);
static int raspi5_video_scroll_pixels(fb_text_console_t *console);
static void raspi5_video_publish_dirty_rect(drunix_rect_t rect);

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

static void raspi5_dc_cvac_lines(const void *start, uint32_t bytes)
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
}

static void raspi5_dc_cvac_range(const void *start, uint32_t bytes)
{
	raspi5_dc_cvac_lines(start, bytes);
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
	g_request[RASPI5_VIDEO_REQ_PHYSICAL_WIDTH] = RASPI5_VIDEO_MAX_WIDTH;
	g_request[RASPI5_VIDEO_REQ_PHYSICAL_HEIGHT] = RASPI5_VIDEO_MAX_HEIGHT;

	g_request[RASPI5_VIDEO_REQ_VIRTUAL_TAG] = RASPI5_TAG_SET_VIRTUAL_SIZE;
	g_request[RASPI5_VIDEO_REQ_VIRTUAL_SIZE] = 8u;
	g_request[RASPI5_VIDEO_REQ_VIRTUAL_CODE] = 0u;
	g_request[RASPI5_VIDEO_REQ_VIRTUAL_WIDTH] = RASPI5_VIDEO_MAX_WIDTH;
	g_request[RASPI5_VIDEO_REQ_VIRTUAL_HEIGHT] =
	    RASPI5_VIDEO_MAX_HEIGHT * RASPI5_VIDEO_PAN_PAGES;

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

/*
 * The strict width/height equality check from the M7 c2 driver was
 * removed in M7 polish: Pi 5 firmware reports the requested geometry
 * in the SET_PHYSICAL_SIZE / SET_VIRTUAL_SIZE response fields verbatim
 * (a polite echo), but ALLOCATE_BUFFER follows whatever the EDID
 * handshake settled on — so the response fields are not a reliable
 * source of truth. arm64_video_init now derives the actual width and
 * height from the returned pitch and size, capped at
 * RASPI5_VIDEO_MAX_WIDTH / _HEIGHT. The validator only enforces the
 * structural invariants firmware can't fudge: per-tag response bits
 * set, depth = 32, pixel order in {0, 1}.
 */
static int raspi5_video_validate_mode_response(void)
{
	if (!raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_PHYSICAL_CODE], 8u) ||
	    !raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_VIRTUAL_CODE], 8u) ||
	    !raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_DEPTH_CODE], 4u) ||
	    !raspi5_video_tag_ok(g_request[RASPI5_VIDEO_REQ_PIXEL_CODE], 4u)) {
		platform_uart_puts("raspi5 fb: per-tag response code invalid\n");
		return -1;
	}
	if (g_request[RASPI5_VIDEO_REQ_DEPTH_VALUE] != RASPI5_VIDEO_DEPTH) {
		platform_uart_puts("raspi5 fb: depth not 32; rejecting\n");
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

/*
 * Derive the actual scanout dimensions from the firmware-returned
 * pitch and size, since the SET_*_SIZE response fields are unreliable
 * on Pi 5 (see validate_mode_response comment). width comes from
 * pitch / bytes-per-pixel; height comes from size / pitch.
 *
 * Returns 0 on success and writes *out_width / *out_height. Returns
 * -1 if either dimension would be zero, would exceed the
 * RASPI5_VIDEO_MAX_* caps that bound the static fb_text_console cell
 * array, or if pitch is not a multiple of bytes-per-pixel.
 */
static int raspi5_video_derive_dimensions(uint32_t pitch,
                                          uint32_t fb_size,
                                          uint32_t *out_width,
                                          uint32_t *out_height,
                                          uint32_t *out_virtual_height)
{
	uint32_t width;
	uint32_t visible_height;
	uint32_t virtual_height;

	if (pitch == 0u || fb_size == 0u || !out_virtual_height)
		return -1;
	if ((pitch % RASPI5_VIDEO_BYTES_PER_PIXEL) != 0u)
		return -1;
	width = pitch / RASPI5_VIDEO_BYTES_PER_PIXEL;
	virtual_height = fb_size / pitch;
	visible_height = virtual_height;
	if (virtual_height > RASPI5_VIDEO_MAX_HEIGHT)
		visible_height = RASPI5_VIDEO_MAX_HEIGHT;
	if (width == 0u || visible_height == 0u || virtual_height == 0u)
		return -1;
	if (width > RASPI5_VIDEO_MAX_WIDTH) {
		platform_uart_puts(
		    "raspi5 fb: scanout width exceeds RASPI5_VIDEO_MAX_WIDTH\n");
		return -1;
	}
	*out_width = width;
	*out_height = visible_height;
	*out_virtual_height = virtual_height;
	return 0;
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

	{
		uint32_t actual_width;
		uint32_t actual_height;
		uint32_t virtual_height;
		uint32_t cell_cols;
		uint32_t cell_rows;
		uint32_t cell_count;
		uint8_t red_pos;
		uint8_t blue_pos;

		if (raspi5_video_derive_dimensions(
		        pitch, fb_size, &actual_width, &actual_height, &virtual_height) !=
		    0) {
			platform_uart_puts(
			    "raspi5 fb: cannot derive scanout dimensions from pitch/size\n");
			return -1;
		}
		raspi5_video_trace_u32("raspi5 fb: actual_w", actual_width);
		raspi5_video_trace_u32("raspi5 fb: actual_h", actual_height);
		raspi5_video_trace_u32("raspi5 fb: virt_actual_h", virtual_height);

		fb_phys = raspi5_fb_bus_to_phys(fb_bus);
		raspi5_video_trace_u64("raspi5 fb: phys", fb_phys);

		if (fb_phys + (uint64_t)fb_size > RASPI5_VIDEO_RAM_CEILING) {
			platform_uart_puts(
			    "raspi5 fb: phys above 2 GiB linear-map ceiling; "
			    "rejecting (serial fallback)\n");
			return -1;
		}

		/*
		 * Pi 5 HVS6 byte order is BGRA on the wire regardless of what
		 * SET_PIXEL_ORDER asked for — firmware acks the request but the
		 * actual plane CTL0 (observed via M9.1's dlist dump at boot)
		 * encodes the byte order from EDID negotiation, not from the
		 * mailbox tag. The user-visible symptom of trusting the
		 * mailbox response: fbfill paints red and blue inverted, while
		 * fb_text_console's white-on-black text rendering disguises the
		 * issue because achromatic pixels are byte-order invariant.
		 *
		 * Pi 3 (raspi3b/video.c) hardcodes red_pos=16 / blue_pos=0 for
		 * the same reason: red in bits 23:16 of the 32-bit packed
		 * pixel, blue in 7:0, which on little-endian AArch64 is memory
		 * layout [B, G, R, X]. The HVS6 plane on Pi 5 uses the same
		 * BGRA byte order.
		 *
		 * If a future Pi 5 firmware revision starts honouring
		 * SET_PIXEL_ORDER and a hardware variant ships with RGBA byte
		 * order, the right place to detect that is M9.1's HVS dlist
		 * probe (CTL0's ORDERRGBA bit field), not the mailbox tag
		 * response. Documented and parked.
		 */
		(void)g_request[RASPI5_VIDEO_REQ_PIXEL_ORDER];
		red_pos = 16u;
		blue_pos = 0u;
		if (framebuffer_info_from_rgb((uintptr_t)fb_phys,
		                              pitch,
		                              actual_width,
		                              actual_height,
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

		pmm_mark_used((uint32_t)fb_phys, fb_size);
		raspi5_register_framebuffer(fb_phys, (uint64_t)fb_size);

		/* Cell grid is sized to the firmware-actual scanout, bounded
		 * by RASPI5_VIDEO_MAX_CONSOLE_CELLS (the static BSS reservation
		 * cap). g_fb_cells is large enough for the maximum, so any
		 * actual_width / actual_height that passes derive_dimensions
		 * fits. */
		cell_cols = actual_width / GUI_FONT_W;
		cell_rows = actual_height / GUI_FONT_H;
		cell_count = cell_cols * cell_rows;
		if (cell_count > RASPI5_VIDEO_MAX_CONSOLE_CELLS)
			cell_count = RASPI5_VIDEO_MAX_CONSOLE_CELLS;
		if (fb_text_console_init(&g_fb_console,
		                         &g_fb_info,
		                         g_fb_cells,
		                         cell_count) != 0) {
			platform_uart_puts(
			    "raspi5 fb: fb_text_console_init failed\n");
			return -1;
		}
		fb_text_console_set_dirty_pixels(&g_fb_console,
		                                 raspi5_video_dirty_pixels);
		fb_text_console_set_scroll_pixels(&g_fb_console,
		                                  raspi5_video_scroll_pixels);
		(void)virtual_height;
		/* fb_text_console_init -> fb_text_console_clear already painted
		 * the initial blank screen, but it ran before the hook was
		 * registered, so those writes are still in CPU cache. One full
		 * pass here flushes them so the first bytes the HVS scans out
		 * are the cleared background, not whatever firmware left
		 * behind. After this, every cell update self-flushes via the
		 * hook. */
		raspi5_video_dirty_pixels(0, 0, actual_width, actual_height);
	}

	if (fbdev_init(&g_fb_info) != 0) {
		platform_uart_puts(
		    "raspi5 fb: fbdev_init failed; /dev/fb0 unavailable\n");
		return -1;
	}

	g_fb_ready = 1;
	platform_uart_puts("raspi5 fb: ready (fb_text_console + /dev/fb0)\n");

	/*
	 * M9.1 — passive HVS observability. Read HVS channel state plus a
	 * window of dlist SRAM so the boot trace tells us where firmware
	 * parked HDMI0's primary plane. Read-only; failure is non-fatal —
	 * the mailbox framebuffer continues to drive the console
	 * regardless.
	 */
	{
		raspi5_hvs_probe_state_t hvs_state;
		(void)raspi5_hvs_probe_passive(&hvs_state);
	}

	/*
	 * M9.2 + M9.3 — locate firmware plane and hijack scanout.
	 *
	 * Validate that channel 0's dlist plane has the shape we expect
	 * (matches mailbox-reported dimensions / pitch / fb phys), then:
	 *   1. Copy the firmware's framebuffer contents into our
	 *      Drunix-owned scanout buffer so the screen image is
	 *      preserved across the flip.
	 *   2. CVAC the full buffer so the HVS will see consistent data.
	 *   3. Atomically rewrite the plane's address words to point at
	 *      our buffer. From this point the HVS scans pixels from
	 *      memory the kernel controls.
	 *   4. Repoint g_fb_info's address/phys to our buffer so all
	 *      future draws (fb_text_console put_cell, scroll_pixels
	 *      memmove, /dev/fb0 mmap pointer derivation) target it.
	 *
	 * If the locator's fingerprint check fails, do NOT touch HVS;
	 * stay on the mailbox framebuffer path. The existing per-rect
	 * CVAC hook keeps the firmware scanout coherent in that fallback,
	 * so the console remains usable — just with the original visible
	 * crawl on scroll.
	 *
	 * No new mappings: the carve-out lives in low RAM identity-mapped
	 * as Normal-WB by the existing platform_mm_classify path. The HVS
	 * reads it via DMA; the dirty-pixels CVAC hook keeps that DMA
	 * coherent with CPU writes.
	 */
	if (raspi5_hvs_locate_firmware_plane(g_fb_info.width,
	                                     g_fb_info.height,
	                                     g_fb_info.pitch,
	                                     (uintptr_t)g_fb_info.phys_address,
	                                     &g_hvs_plane) == 0) {
		const platform_ram_layout_t *layout = platform_ram_layout();
		uint64_t fb_bytes;
		uintptr_t new_phys;

		if (!layout || layout->scanout_carve_size == 0u) {
			platform_uart_puts(
			    "raspi5 hvs: platform did not publish a scanout carve-out; "
			    "staying on firmware fb\n");
			goto hvs_install_done;
		}

		g_hvs_scanout_buf = (uint8_t *)(uintptr_t)layout->scanout_carve_base;
		g_hvs_scanout_size = layout->scanout_carve_size;
		raspi5_video_trace_u64("raspi5 hvs: scanout_carve_base",
		                       (uint64_t)(uintptr_t)g_hvs_scanout_buf);
		raspi5_video_trace_u64("raspi5 hvs: scanout_carve_size",
		                       g_hvs_scanout_size);

		fb_bytes = (uint64_t)g_fb_info.pitch * (uint64_t)g_fb_info.height;
		if (fb_bytes > g_hvs_scanout_size) {
			platform_uart_puts(
			    "raspi5 hvs: scanout carve-out smaller than one frame; "
			    "staying on firmware fb\n");
			goto hvs_install_done;
		}

		k_memcpy(g_hvs_scanout_buf,
		         (const void *)(uintptr_t)g_fb_info.address,
		         (uint32_t)fb_bytes);
		raspi5_dc_cvac_lines(g_hvs_scanout_buf, (uint32_t)fb_bytes);
		raspi5_video_dsb();

		new_phys = (uintptr_t)g_hvs_scanout_buf;
		if (raspi5_hvs_flip_plane_address(&g_hvs_plane, new_phys) == 0) {
			g_fb_info.address = new_phys;
			g_fb_info.phys_address = new_phys;
			g_hvs_scroll_y = 0u;
			g_hvs_virtual_height_px =
			    (uint32_t)(g_hvs_scanout_size / g_fb_info.pitch);
			raspi5_video_trace_u32(
			    "raspi5 hvs: virtual_height_px",
			    g_hvs_virtual_height_px);
			/*
			 * The scanout is now in cached normal memory. /dev/fb0
			 * mmap must use the same Normal-WB attribute or
			 * userspace would create a forbidden cacheable / non-
			 * cacheable alias of the same PA. Userspace makes
			 * writes visible via DRUNIX_FBIO_PUBLISH_DIRTY_RECT,
			 * which fbdev forwards to our CVAC hook below.
			 */
			fbdev_set_cache_policy(CHARDEV_CACHE_WB_FLUSH);
			fbdev_set_publish_dirty_rect(
			    raspi5_video_publish_dirty_rect);
			platform_uart_puts(
			    "raspi5 hvs: scanout hijacked to Drunix buffer\n");
		} else {
			platform_uart_puts(
			    "raspi5 hvs: flip_plane_address failed; staying on "
			    "firmware fb\n");
		}
	}
hvs_install_done:
	;

	/*
	 * M9.4b1 — passive PV0 probe. Read-only dump of the PixelValve
	 * driving HDMI0 so the boot trace tells us the actual D0
	 * register layout. M9.4b2 will use this to find INT_EN /
	 * INT_STAT / VFP_START bit positions and register an SPI 101
	 * vblank handler; M9.4b3 will pace HVS plane flips on vblank.
	 * Non-fatal failure path: probe runs after HVS install so a
	 * bad MMIO read here can't break console bring-up.
	 */
	{
		raspi5_pv0_probe_state_t pv_state;
		(void)raspi5_pv0_probe_passive(&pv_state);
	}

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
 * Forward 64-bit-word memcpy with 8x unrolled body. Used by the
 * scroll_pixels wrap path: at wrap time the live visible content gets
 * copied from somewhere near the bottom of the virtual buffer back to
 * offset 0. dst and src don't overlap (caller's invariant; verified
 * by the wrap condition); for the worst case where they touched the
 * forward order [dst < src] would still be correct.
 *
 * Both pointers are 64-bit aligned at the call site because the
 * framebuffer pitch is a multiple of 8 (1920*4 = 7680) and the BSS
 * buffer base is 64-byte aligned. Compiler at -O2 fuses the unrolled
 * uint64_t pair writes into LDP/STP X register pairs — the best a
 * -mgeneral-regs-only build can do without NEON.
 */
static void raspi5_video_fb_memmove_up_raw(uint8_t *dst,
                                           const uint8_t *src,
                                           uint64_t bytes)
{
	uint64_t *d = (uint64_t *)(uintptr_t)dst;
	const uint64_t *s = (const uint64_t *)(uintptr_t)src;
	uint64_t words = bytes / sizeof(uint64_t);
	uint64_t i;

	while (words >= 8u) {
		d[0] = s[0];
		d[1] = s[1];
		d[2] = s[2];
		d[3] = s[3];
		d[4] = s[4];
		d[5] = s[5];
		d[6] = s[6];
		d[7] = s[7];
		d += 8;
		s += 8;
		words -= 8u;
	}
	for (i = 0u; i < words; i++)
		d[i] = s[i];
}

/*
 * Console scroll-up hook. Implements the un-accelerated fbcon scroll
 * pattern: memmove the scanout up by one font row, clear the new
 * bottom row, then CVAC the whole framebuffer. Replaces the old
 * SET_VIRTUAL_OFFSET-based hardware-pan attempt, which Pi 5 firmware
 * silently refuses (proven empirically in M9.1: firmware "polite-
 * echoes" SET_VIRTUAL_SIZE in the mailbox response but
 * ALLOCATE_BUFFER only ever allocates one visible frame, so virtual-
 * offset hardware pan has no buffer to pan within).
 *
 * The previous fallback was fb_text_console_present_all (re-rasterise
 * every glyph cell into the live scanout), which produces the visible
 * top-to-bottom redraw sweep the user reported. Glyph rasterisation
 * is byte-level work spread across the whole frame in source order;
 * the HDMI raster catches every intermediate state. The memmove-and-
 * clear path here writes the framebuffer in tight 64-bit word-aligned
 * batches, then issues a single CVAC sweep — same data volume, much
 * less work per byte, no per-glyph branch overhead.
 *
 * Returns 0 to tell fb_text_console_scroll the scroll has been
 * handled; the present_all fallback is then skipped.
 */
static inline uint64_t raspi5_video_cntvct(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

/*
 * One-shot scroll timing trace. Prints the per-stage cost in raw
 * CNTVCT_EL0 ticks for the first RASPI5_VIDEO_SCROLL_TRACE_BUDGET
 * scrolls so we can see where the cost lives without flooding serial
 * forever. CNTFRQ on Pi 5 is 54 MHz (see early boot trace), so divide
 * the printed tick count by 54 to get microseconds.
 */
#define RASPI5_VIDEO_SCROLL_TRACE_BUDGET 8u
static uint32_t g_scroll_trace_budget = RASPI5_VIDEO_SCROLL_TRACE_BUDGET;

/*
 * HVS plane-base pan scroll.
 *
 * The buffer is virtually 4x the visible height (~32 MiB). The HVS
 * scans a 1080-row window starting at byte offset
 * (g_hvs_scroll_y * pitch) within the buffer. On each scroll:
 *
 *   1. Compute next_y = g_hvs_scroll_y + GUI_FONT_H.
 *   2. If next_y + visible_height > virtual_height:
 *        - WRAP. memcpy the current visible window from
 *          buffer[g_hvs_scroll_y..g_hvs_scroll_y+visible_height-GUI_FONT_H]
 *          back to buffer[0..visible_height-GUI_FONT_H]. CVAC the
 *          moved region. Reset next_y = 0. This is the same one-time
 *          full-frame work the old per-scroll path did; it's now
 *          amortised across ~200 cheap scrolls.
 *   3. Clear the new bottom font row (at next_y + visible_height
 *        - GUI_FONT_H) and CVAC it. ~120 KB write + drain.
 *   4. Update g_fb_info.address to buffer + next_y * pitch so all
 *        future cell drawing lands in the right place.
 *   5. Issue raspi5_hvs_flip_plane_address(new_base_phys). The HVS
 *        picks up the new pointer on next FIFO refill.
 */
/*
 * publish_dirty_rect hook for /dev/fb0 userspace mmap writes. Userspace
 * compositors write into the cached carve-out, then call
 * DRUNIX_FBIO_PUBLISH_DIRTY_RECT which fbdev forwards here. The hook
 * DC CVACs the rect bytes against the *current* g_fb_info.address —
 * which after M9.3 install is `carve_out_base + g_hvs_scroll_y * pitch`
 * (the visible window's top inside the virtual scrollback buffer) — so
 * the HVS DMA engine sees the userspace writes on next FIFO refill.
 *
 * Rect coordinates have already been validated by fbdev_ioctl against
 * the framebuffer width / height; defensive bounds checks here are
 * belt-and-braces.
 */
static void raspi5_video_publish_dirty_rect(drunix_rect_t rect)
{
	if (!g_fb_ready)
		return;
	if (rect.w <= 0 || rect.h <= 0)
		return;
	if ((uint32_t)rect.x >= g_fb_info.width ||
	    (uint32_t)rect.y >= g_fb_info.height)
		return;
	{
		uint32_t x = (uint32_t)rect.x;
		uint32_t y = (uint32_t)rect.y;
		uint32_t w = (uint32_t)rect.w;
		uint32_t h = (uint32_t)rect.h;
		if (x + w > g_fb_info.width)
			w = g_fb_info.width - x;
		if (y + h > g_fb_info.height)
			h = g_fb_info.height - y;
		raspi5_video_dirty_pixels(x, y, w, h);
	}
}

static int raspi5_video_scroll_pixels(fb_text_console_t *console)
{
	uint8_t *buf_base;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint32_t cur_y;
	uint32_t next_y;
	int do_trace;
	int did_wrap = 0;
	uint64_t t0 = 0u, t1 = 0u, t2 = 0u, t3 = 0u;
	uintptr_t new_base_phys;

	/*
	 * Diagnostic entry trace. Fires once on first invocation so we
	 * know scroll_pixels is being reached at all. If serial shows
	 * "raspi5 scroll: entry" but no later "scroll: total_ticks", an
	 * early-return is bailing out — the subsequent trace lines
	 * identify which gate fires.
	 */
	{
		static int s_scroll_entry_traced = 0;
		if (!s_scroll_entry_traced) {
			s_scroll_entry_traced = 1;
			platform_uart_puts("raspi5 scroll: entry first-call\n");
			raspi5_video_trace_u32("raspi5 scroll: g_fb_ready",
			                       (uint32_t)g_fb_ready);
			raspi5_video_trace_u32(
			    "raspi5 scroll: g_hvs_virtual_height_px",
			    g_hvs_virtual_height_px);
			raspi5_video_trace_u32("raspi5 scroll: g_hvs_plane_valid",
			                       (uint32_t)g_hvs_plane.valid);
			raspi5_video_trace_u32("raspi5 scroll: cur_y_at_entry",
			                       g_hvs_scroll_y);
			if (console && console->fb) {
				raspi5_video_trace_u32("raspi5 scroll: console_pitch",
				                       console->fb->pitch);
				raspi5_video_trace_u32("raspi5 scroll: console_height",
				                       console->fb->height);
			} else {
				platform_uart_puts(
				    "raspi5 scroll: console or console->fb NULL\n");
			}
		}
	}

	if (!g_fb_ready || !console || !console->fb)
		return -1;
	if (console->fb->pitch == 0u || console->fb->height < GUI_FONT_H)
		return -1;
	if (g_hvs_virtual_height_px == 0u)
		return -1;
	if (!g_hvs_plane.valid)
		return -1;

	buf_base = g_hvs_scanout_buf;
	if (!buf_base)
		return -1;
	pitch = console->fb->pitch;
	width = console->fb->width;
	height = console->fb->height;
	cur_y = g_hvs_scroll_y;

	do_trace = (g_scroll_trace_budget > 0u) ? 1 : 0;
	if (do_trace)
		t0 = raspi5_video_cntvct();

	next_y = cur_y + (uint32_t)GUI_FONT_H;
	if (next_y + height > g_hvs_virtual_height_px) {
		/*
		 * Wrap: copy the live visible content (minus the row we are
		 * about to scroll off) from buf[cur_y..cur_y+height-GUI_FONT_H]
		 * back to buf[0..height-GUI_FONT_H]. After that the visible
		 * top is at offset 0 and the new bottom row is at offset
		 * height-GUI_FONT_H, which step 3 below will clear.
		 *
		 * Forward memcpy is safe because cur_y >= GUI_FONT_H here and
		 * the destination [0, height-GUI_FONT_H) does not overlap the
		 * source [cur_y, cur_y+height-GUI_FONT_H) — cur_y is at least
		 * height - GUI_FONT_H + 1 by the wrap condition (next_y +
		 * height > virtual_height implies cur_y + GUI_FONT_H + height
		 * > virtual_height, and virtual_height = N * height, so
		 * cur_y > (N-1)*height - GUI_FONT_H, comfortably above
		 * height-GUI_FONT_H for N >= 2).
		 */
		uint64_t copy_bytes =
		    (uint64_t)pitch * (uint64_t)(height - (uint32_t)GUI_FONT_H);
		uint8_t *dst = buf_base;
		const uint8_t *src = buf_base + (uintptr_t)pitch * cur_y;
		raspi5_video_fb_memmove_up_raw(dst, src, copy_bytes);
		next_y = 0u;
		did_wrap = 1;
	}

	if (do_trace)
		t1 = raspi5_video_cntvct();

	{
		uint8_t *new_bottom = buf_base + (uintptr_t)pitch *
		                                     (next_y + height -
		                                      (uint32_t)GUI_FONT_H);
		uint64_t clear_bytes =
		    (uint64_t)pitch * (uint64_t)GUI_FONT_H;
		uint64_t i;
		uint64_t *p = (uint64_t *)(uintptr_t)new_bottom;
		uint64_t words = clear_bytes / sizeof(uint64_t);
		while (words >= 8u) {
			p[0] = 0u; p[1] = 0u; p[2] = 0u; p[3] = 0u;
			p[4] = 0u; p[5] = 0u; p[6] = 0u; p[7] = 0u;
			p += 8;
			words -= 8u;
		}
		for (i = 0u; i < words; i++)
			p[i] = 0u;
	}

	if (do_trace)
		t2 = raspi5_video_cntvct();

	{
		uintptr_t cvac_start;
		uint64_t cvac_bytes;
		if (did_wrap) {
			cvac_start = (uintptr_t)buf_base;
			cvac_bytes = (uint64_t)pitch * (uint64_t)height;
		} else {
			cvac_start = (uintptr_t)buf_base + (uintptr_t)pitch *
			                                       (next_y + height -
			                                        (uint32_t)GUI_FONT_H);
			cvac_bytes = (uint64_t)pitch * (uint64_t)GUI_FONT_H;
		}
		raspi5_dc_cvac_lines((const void *)cvac_start, (uint32_t)cvac_bytes);
		raspi5_video_dsb();
	}

	g_hvs_scroll_y = next_y;
	new_base_phys = (uintptr_t)buf_base + (uintptr_t)pitch * next_y;
	g_fb_info.address = new_base_phys;
	g_fb_info.phys_address = new_base_phys;
	(void)raspi5_hvs_flip_plane_address(&g_hvs_plane, new_base_phys);

	if (do_trace)
		t3 = raspi5_video_cntvct();

	if (do_trace) {
		raspi5_video_trace_u32("raspi5 scroll: wrap",
		                       (uint32_t)did_wrap);
		raspi5_video_trace_u32("raspi5 scroll: copy_ticks",
		                       (uint32_t)(t1 - t0));
		raspi5_video_trace_u32("raspi5 scroll: clear_ticks",
		                       (uint32_t)(t2 - t1));
		raspi5_video_trace_u32("raspi5 scroll: cvac_flip_ticks",
		                       (uint32_t)(t3 - t2));
		raspi5_video_trace_u32("raspi5 scroll: total_ticks",
		                       (uint32_t)(t3 - t0));
		g_scroll_trace_budget--;
	}
	(void)width;
	return 0;
}

/*
 * Targeted cache-flush hook registered with fb_text_console at
 * arm64_video_init time. fb_text_console_present_rect calls this
 * after each modified pixel rect; the rect coordinates are already
 * clipped to fb bounds. We DC CVAC only the affected rows and issue
 * one final barrier after all rows are cleaned. A DSB per scanline
 * makes full-screen scroll visibly crawl from top to bottom.
 */
static void raspi5_video_dirty_pixels(uint32_t x, uint32_t y, uint32_t w,
                                      uint32_t h)
{
	uint32_t row;
	uintptr_t row_base;
	uint32_t row_bytes;

	if (!g_fb_ready || w == 0u || h == 0u)
		return;

	row_bytes = w * RASPI5_VIDEO_BYTES_PER_PIXEL;
	if (x == 0u && row_bytes == g_fb_info.pitch) {
		row_base = (uintptr_t)g_fb_info.address +
		           (uintptr_t)y * (uintptr_t)g_fb_info.pitch;
		raspi5_dc_cvac_lines((const void *)row_base, g_fb_info.pitch * h);
		raspi5_video_dsb();
		return;
	}
	for (row = 0; row < h; row++) {
		row_base = (uintptr_t)g_fb_info.address +
		           (uintptr_t)(y + row) * (uintptr_t)g_fb_info.pitch +
		           (uintptr_t)x * RASPI5_VIDEO_BYTES_PER_PIXEL;
		raspi5_dc_cvac_lines((const void *)row_base, row_bytes);
	}
	raspi5_video_dsb();
}

/*
 * Boot console write path. The kernel-linear identity map for the
 * framebuffer pages was built at boot, before the mailbox call
 * returned the fb_phys range, so those pages are mapped Normal-WB
 * (cacheable) via the generic PLATFORM_MM_NORMAL classification.
 * BCM2712's HVS reads scanout pixels through a non-coherent path,
 * so dirty cache lines must be cleaned to RAM before the display
 * engine can see them. The actual cleaning happens inside
 * raspi5_video_dirty_pixels, which fb_text_console_present_rect
 * calls with the precise pixel rect that was modified — vastly
 * cheaper than the M7 c5 sweep over the full framebuffer.
 */
void arm64_video_console_write(const char *buf, uint32_t len)
{
	if (!g_fb_ready)
		return;
	fb_text_console_write(&g_fb_console, buf, len);
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
