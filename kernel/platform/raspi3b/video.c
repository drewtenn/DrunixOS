#include "video.h"
#include "../platform.h"
#include "../../console/fb_text_console.h"
#include "../../arch/arm64/mm/pmm.h"

#if DRUNIX_ARM64_VGA

#define ARM64_MBOX_BASE (PLATFORM_PERIPHERAL_BASE + 0xB880u)
#define ARM64_MBOX_READ 0u
#define ARM64_MBOX_STATUS 6u
#define ARM64_MBOX_WRITE 8u
#define ARM64_MBOX_FULL 0x80000000u
#define ARM64_MBOX_EMPTY 0x40000000u
#define ARM64_MBOX_CHANNEL_PROPERTY 8u
#define ARM64_MBOX_TIMEOUT 10000000u

#define ARM64_MBOX_REQUEST 0x00000000u
#define ARM64_MBOX_RESPONSE_SUCCESS 0x80000000u
#define ARM64_MBOX_TAG_RESPONSE 0x80000000u

#define ARM64_TAG_ALLOCATE_BUFFER 0x00040001u
#define ARM64_TAG_GET_PITCH 0x00040008u
#define ARM64_TAG_SET_PHYSICAL_SIZE 0x00048003u
#define ARM64_TAG_SET_VIRTUAL_SIZE 0x00048004u
#define ARM64_TAG_SET_DEPTH 0x00048005u
#define ARM64_TAG_SET_PIXEL_ORDER 0x00048006u
#define ARM64_TAG_END 0x00000000u

#define ARM64_VIDEO_FB_ALIGNMENT 16u
#define ARM64_VIDEO_PIXEL_ORDER_RGB 1u
#define ARM64_VIDEO_BUS_ADDRESS_MASK 0x3FFFFFFFu
#define ARM64_VIDEO_BYTES_PER_PIXEL 4u
#define ARM64_VIDEO_CONSOLE_CELLS                                              \
	((ARM64_VIDEO_WIDTH / GUI_FONT_W) * (ARM64_VIDEO_HEIGHT / GUI_FONT_H))

enum arm64_video_request_index {
	ARM64_VIDEO_REQ_SIZE = 0,
	ARM64_VIDEO_REQ_CODE = 1,
	ARM64_VIDEO_REQ_PHYSICAL_TAG = 2,
	ARM64_VIDEO_REQ_PHYSICAL_SIZE = 3,
	ARM64_VIDEO_REQ_PHYSICAL_CODE = 4,
	ARM64_VIDEO_REQ_PHYSICAL_WIDTH = 5,
	ARM64_VIDEO_REQ_PHYSICAL_HEIGHT = 6,
	ARM64_VIDEO_REQ_VIRTUAL_TAG = 7,
	ARM64_VIDEO_REQ_VIRTUAL_SIZE = 8,
	ARM64_VIDEO_REQ_VIRTUAL_CODE = 9,
	ARM64_VIDEO_REQ_VIRTUAL_WIDTH = 10,
	ARM64_VIDEO_REQ_VIRTUAL_HEIGHT = 11,
	ARM64_VIDEO_REQ_DEPTH_TAG = 12,
	ARM64_VIDEO_REQ_DEPTH_SIZE = 13,
	ARM64_VIDEO_REQ_DEPTH_CODE = 14,
	ARM64_VIDEO_REQ_DEPTH_VALUE = 15,
	ARM64_VIDEO_REQ_PIXEL_TAG = 16,
	ARM64_VIDEO_REQ_PIXEL_SIZE = 17,
	ARM64_VIDEO_REQ_PIXEL_CODE = 18,
	ARM64_VIDEO_REQ_PIXEL_ORDER = 19,
	ARM64_VIDEO_REQ_ALLOC_TAG = 20,
	ARM64_VIDEO_REQ_ALLOC_SIZE = 21,
	ARM64_VIDEO_REQ_ALLOC_CODE = 22,
	ARM64_VIDEO_REQ_ALLOC_ADDRESS = 23,
	ARM64_VIDEO_REQ_ALLOC_BYTES = 24,
	ARM64_VIDEO_REQ_PITCH_TAG = 25,
	ARM64_VIDEO_REQ_PITCH_SIZE = 26,
	ARM64_VIDEO_REQ_PITCH_CODE = 27,
	ARM64_VIDEO_REQ_PITCH_VALUE = 28,
	ARM64_VIDEO_REQ_END = 29,
	ARM64_VIDEO_REQ_WORDS = 30,
};

static volatile uint32_t g_arm64_video_request[ARM64_VIDEO_REQ_WORDS]
    __attribute__((aligned(16)));
static framebuffer_info_t g_arm64_video_framebuffer;
static fb_text_console_t g_arm64_video_console;
static gui_cell_t g_arm64_video_cells[ARM64_VIDEO_CONSOLE_CELLS];
static int g_arm64_video_ready;

static volatile uint32_t *arm64_mailbox_regs(void)
{
	return (volatile uint32_t *)(uintptr_t)ARM64_MBOX_BASE;
}

static void arm64_video_memory_barrier(void)
{
	__asm__ volatile("dmb sy" ::: "memory");
}

static int arm64_mailbox_call(volatile uint32_t *request)
{
	volatile uint32_t *mailbox = arm64_mailbox_regs();
	uint32_t address = (uint32_t)(uintptr_t)request;
	uint32_t message;
	uint32_t timeout;

	if ((address & 0xFu) != 0)
		return -1;

	message = (address & ~0xFu) | ARM64_MBOX_CHANNEL_PROPERTY;
	timeout = ARM64_MBOX_TIMEOUT;
	while ((mailbox[ARM64_MBOX_STATUS] & ARM64_MBOX_FULL) != 0u) {
		if (timeout-- == 0u)
			return -1;
	}

	arm64_video_memory_barrier();
	mailbox[ARM64_MBOX_WRITE] = message;
	arm64_video_memory_barrier();

	timeout = ARM64_MBOX_TIMEOUT;
	for (;;) {
		uint32_t response;

		while ((mailbox[ARM64_MBOX_STATUS] & ARM64_MBOX_EMPTY) != 0u) {
			if (timeout-- == 0u)
				return -1;
		}

		response = mailbox[ARM64_MBOX_READ];
		if ((response & 0xFu) == ARM64_MBOX_CHANNEL_PROPERTY &&
		    (response & ~0xFu) == (address & ~0xFu)) {
			arm64_video_memory_barrier();
			return 0;
		}

		if (timeout-- == 0u)
			return -1;
	}
}

static void arm64_video_build_request(void)
{
	g_arm64_video_request[ARM64_VIDEO_REQ_SIZE] =
	    ARM64_VIDEO_REQ_WORDS * sizeof(uint32_t);
	g_arm64_video_request[ARM64_VIDEO_REQ_CODE] = ARM64_MBOX_REQUEST;

	g_arm64_video_request[ARM64_VIDEO_REQ_PHYSICAL_TAG] =
	    ARM64_TAG_SET_PHYSICAL_SIZE;
	g_arm64_video_request[ARM64_VIDEO_REQ_PHYSICAL_SIZE] = 8u;
	g_arm64_video_request[ARM64_VIDEO_REQ_PHYSICAL_CODE] = 0u;
	g_arm64_video_request[ARM64_VIDEO_REQ_PHYSICAL_WIDTH] = ARM64_VIDEO_WIDTH;
	g_arm64_video_request[ARM64_VIDEO_REQ_PHYSICAL_HEIGHT] = ARM64_VIDEO_HEIGHT;

	g_arm64_video_request[ARM64_VIDEO_REQ_VIRTUAL_TAG] =
	    ARM64_TAG_SET_VIRTUAL_SIZE;
	g_arm64_video_request[ARM64_VIDEO_REQ_VIRTUAL_SIZE] = 8u;
	g_arm64_video_request[ARM64_VIDEO_REQ_VIRTUAL_CODE] = 0u;
	g_arm64_video_request[ARM64_VIDEO_REQ_VIRTUAL_WIDTH] = ARM64_VIDEO_WIDTH;
	g_arm64_video_request[ARM64_VIDEO_REQ_VIRTUAL_HEIGHT] = ARM64_VIDEO_HEIGHT;

	g_arm64_video_request[ARM64_VIDEO_REQ_DEPTH_TAG] = ARM64_TAG_SET_DEPTH;
	g_arm64_video_request[ARM64_VIDEO_REQ_DEPTH_SIZE] = 4u;
	g_arm64_video_request[ARM64_VIDEO_REQ_DEPTH_CODE] = 0u;
	g_arm64_video_request[ARM64_VIDEO_REQ_DEPTH_VALUE] = ARM64_VIDEO_DEPTH;

	g_arm64_video_request[ARM64_VIDEO_REQ_PIXEL_TAG] =
	    ARM64_TAG_SET_PIXEL_ORDER;
	g_arm64_video_request[ARM64_VIDEO_REQ_PIXEL_SIZE] = 4u;
	g_arm64_video_request[ARM64_VIDEO_REQ_PIXEL_CODE] = 0u;
	g_arm64_video_request[ARM64_VIDEO_REQ_PIXEL_ORDER] =
	    ARM64_VIDEO_PIXEL_ORDER_RGB;

	g_arm64_video_request[ARM64_VIDEO_REQ_ALLOC_TAG] =
	    ARM64_TAG_ALLOCATE_BUFFER;
	g_arm64_video_request[ARM64_VIDEO_REQ_ALLOC_SIZE] = 8u;
	g_arm64_video_request[ARM64_VIDEO_REQ_ALLOC_CODE] = 0u;
	g_arm64_video_request[ARM64_VIDEO_REQ_ALLOC_ADDRESS] =
	    ARM64_VIDEO_FB_ALIGNMENT;
	g_arm64_video_request[ARM64_VIDEO_REQ_ALLOC_BYTES] = 0u;

	g_arm64_video_request[ARM64_VIDEO_REQ_PITCH_TAG] = ARM64_TAG_GET_PITCH;
	g_arm64_video_request[ARM64_VIDEO_REQ_PITCH_SIZE] = 4u;
	g_arm64_video_request[ARM64_VIDEO_REQ_PITCH_CODE] = 0u;
	g_arm64_video_request[ARM64_VIDEO_REQ_PITCH_VALUE] = 0u;

	g_arm64_video_request[ARM64_VIDEO_REQ_END] = ARM64_TAG_END;
}

static int arm64_video_tag_ok(uint32_t request_code, uint32_t value_size)
{
	return (request_code & ARM64_MBOX_TAG_RESPONSE) != 0u &&
	       (request_code & ~ARM64_MBOX_TAG_RESPONSE) >= value_size;
}

static int arm64_video_validate_mode_response(void)
{
	if (!arm64_video_tag_ok(
	        g_arm64_video_request[ARM64_VIDEO_REQ_PHYSICAL_CODE], 8u) ||
	    !arm64_video_tag_ok(g_arm64_video_request[ARM64_VIDEO_REQ_VIRTUAL_CODE],
	                        8u) ||
	    !arm64_video_tag_ok(g_arm64_video_request[ARM64_VIDEO_REQ_DEPTH_CODE],
	                        4u) ||
	    !arm64_video_tag_ok(g_arm64_video_request[ARM64_VIDEO_REQ_PIXEL_CODE],
	                        4u))
		return -1;

	if (g_arm64_video_request[ARM64_VIDEO_REQ_PHYSICAL_WIDTH] !=
	        ARM64_VIDEO_WIDTH ||
	    g_arm64_video_request[ARM64_VIDEO_REQ_PHYSICAL_HEIGHT] !=
	        ARM64_VIDEO_HEIGHT)
		return -1;
	if (g_arm64_video_request[ARM64_VIDEO_REQ_VIRTUAL_WIDTH] !=
	        ARM64_VIDEO_WIDTH ||
	    g_arm64_video_request[ARM64_VIDEO_REQ_VIRTUAL_HEIGHT] !=
	        ARM64_VIDEO_HEIGHT)
		return -1;
	if (g_arm64_video_request[ARM64_VIDEO_REQ_DEPTH_VALUE] != ARM64_VIDEO_DEPTH)
		return -1;
	if (g_arm64_video_request[ARM64_VIDEO_REQ_PIXEL_ORDER] !=
	    ARM64_VIDEO_PIXEL_ORDER_RGB)
		return -1;

	return 0;
}

static int arm64_video_framebuffer_size_ok(uint32_t pitch, uint32_t fb_size)
{
	uint64_t row_offset;
	uint64_t row_bytes;
	uint64_t required_size;

	row_offset = (uint64_t)(ARM64_VIDEO_HEIGHT - 1u) * pitch;
	row_bytes = (uint64_t)ARM64_VIDEO_WIDTH * ARM64_VIDEO_BYTES_PER_PIXEL;
	required_size = row_offset + row_bytes;
	if (required_size < row_offset || required_size > (uint64_t)UINT32_MAX)
		return 0;

	return (uint64_t)fb_size >= required_size;
}

int arm64_video_init(void)
{
	uint32_t fb_address;
	uint32_t fb_size;
	uint32_t pitch;

	if (g_arm64_video_ready)
		return 0;

	arm64_video_build_request();
	if (arm64_mailbox_call(g_arm64_video_request) != 0)
		return -1;
	if (g_arm64_video_request[ARM64_VIDEO_REQ_CODE] !=
	    ARM64_MBOX_RESPONSE_SUCCESS)
		return -1;
	if (arm64_video_validate_mode_response() != 0)
		return -1;
	if (!arm64_video_tag_ok(g_arm64_video_request[ARM64_VIDEO_REQ_ALLOC_CODE],
	                        8u) ||
	    !arm64_video_tag_ok(g_arm64_video_request[ARM64_VIDEO_REQ_PITCH_CODE],
	                        4u))
		return -1;

	fb_address = g_arm64_video_request[ARM64_VIDEO_REQ_ALLOC_ADDRESS] &
	             ARM64_VIDEO_BUS_ADDRESS_MASK;
	fb_size = g_arm64_video_request[ARM64_VIDEO_REQ_ALLOC_BYTES];
	pitch = g_arm64_video_request[ARM64_VIDEO_REQ_PITCH_VALUE];
	if (fb_address == 0u || fb_size == 0u || pitch == 0u)
		return -1;
	if (!arm64_video_framebuffer_size_ok(pitch, fb_size))
		return -1;

	/*
	 * Pixel order RGB on the Pi/QEMU mailbox path is XRGB8888: red in bits
	 * 23:16, green in 15:8, and blue in 7:0 on little-endian AArch64.
	 */
	if (framebuffer_info_from_rgb((uintptr_t)fb_address,
	                              pitch,
	                              ARM64_VIDEO_WIDTH,
	                              ARM64_VIDEO_HEIGHT,
	                              ARM64_VIDEO_DEPTH,
	                              16u,
	                              8u,
	                              8u,
	                              8u,
	                              0u,
	                              8u,
	                              &g_arm64_video_framebuffer) != 0)
		return -1;

	pmm_mark_used(fb_address, fb_size);

	if (fb_text_console_init(&g_arm64_video_console,
	                         &g_arm64_video_framebuffer,
	                         g_arm64_video_cells,
	                         ARM64_VIDEO_CONSOLE_CELLS) != 0)
		return -1;

	g_arm64_video_ready = 1;
	return 0;
}

int arm64_video_enabled(void)
{
	return g_arm64_video_ready;
}

framebuffer_info_t *arm64_video_framebuffer(void)
{
	if (!arm64_video_enabled())
		return 0;
	return &g_arm64_video_framebuffer;
}

void arm64_video_console_write(const char *buf, uint32_t len)
{
	if (!arm64_video_enabled())
		return;
	fb_text_console_write(&g_arm64_video_console, buf, len);
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

#else

int arm64_video_init(void)
{
	return -1;
}

int arm64_video_enabled(void)
{
	return 0;
}

framebuffer_info_t *arm64_video_framebuffer(void)
{
	return 0;
}

void arm64_video_console_write(const char *buf, uint32_t len)
{
	(void)buf;
	(void)len;
}

int platform_framebuffer_acquire(framebuffer_info_t **out)
{
	if (out)
		*out = 0;
	return -1;
}

#endif

void platform_framebuffer_console_write(const char *buf, uint32_t len)
{
	arm64_video_console_write(buf, len);
}
