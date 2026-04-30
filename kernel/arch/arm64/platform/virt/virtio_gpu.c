/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_gpu.c - virtio-gpu 2D front-end for the QEMU virt platform.
 *
 * Phase 2 M3.0 of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
 * M3.0 brings up the controlq + cursorq, runs the six-command 2D
 * sequence on a 32x32 BGRA scanout resource, draws a deterministic
 * kernel-side test pattern, and verifies dirty-rect partial flushes.
 * /dev/fb0 stays on ramfb in M3.0; M3.1 swaps the backing.
 *
 * Completion is polled in M3.0. virtio-blk-style IRQ delivery on a
 * GICv3 SPI lands later — codex's Define-phase review explicitly
 * recommended polled-first so protocol semantics are debugged in a
 * deterministic loop before IRQ plumbing piles on its own failure
 * modes.
 */

#include "../platform.h"
#include "../../dma.h"
#include "dma.h"
#include "kprintf.h"
#include "virtio_gpu.h"
#include "virtio_mmio.h"
#include "virtio_queue.h"
#include <stdint.h>

/* Virtio 1.2 §5.7.6.7: command type encodings. Only the 2D commands
 * M3.0 needs are named; the rest are intentionally absent so an
 * accidental reference fails to compile. */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO       0x0100u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     0x0101u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VIRTIO_GPU_CMD_SET_SCANOUT            0x0103u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH         0x0104u

/* Response types we expect from M3.0 commands. */
#define VIRTIO_GPU_RESP_OK_NODATA             0x1100u
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO       0x1101u

/* §5.7.4 device config: VIRTIO_GPU_MAX_SCANOUTS = 16, but 1 is plenty
 * for M3.0. */
#define VIRTIO_GPU_MAX_SCANOUTS_M3_0          1u

/* §5.7.6.7 Format codes (subset). B8G8R8X8_UNORM matches Drunix's
 * existing 32-bpp BGRA framebuffer 1:1 (X = unused alpha byte). */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM      2u

/* Fixed resource id used by M3.0. 0 is reserved as "no resource";
 * M3.0 only ever has one live resource so a static id keeps the
 * lifecycle obvious. */
#define VIRTIO_GPU_M3_0_RESOURCE_ID           1u
#define VIRTIO_GPU_M3_0_SCANOUT_ID            0u

/* M3.0 scanout dimensions: 32x32 BGRA = 4 KiB = exactly one DMA page.
 * The DMA pool is 16 pages and is shared with virtio-blk and
 * virtio-input. M3.1 grows this once a dedicated reservation lands. */
#define VIRTIO_GPU_M3_0_WIDTH                 32u
#define VIRTIO_GPU_M3_0_HEIGHT                32u
#define VIRTIO_GPU_M3_0_BPP                   4u
#define VIRTIO_GPU_M3_0_PITCH \
	(VIRTIO_GPU_M3_0_WIDTH * VIRTIO_GPU_M3_0_BPP)
#define VIRTIO_GPU_M3_0_SCANOUT_BYTES \
	(VIRTIO_GPU_M3_0_PITCH * VIRTIO_GPU_M3_0_HEIGHT)

/* Page budgets — see plan doc for the budget rationale. */
#define VIRTIO_GPU_CONTROLQ_PAGES             2u
#define VIRTIO_GPU_CURSORQ_PAGES              2u
#define VIRTIO_GPU_REQ_PAGES                  1u
#define VIRTIO_GPU_RESP_PAGES                 1u
#define VIRTIO_GPU_SCANOUT_PAGES              1u

/* Bounded poll on the used ring after a controlq submit. Same shape
 * as virtio-blk's POLL_TIMEOUT_ITERS so a stuck device shows up in
 * the boot log instead of hanging silently. Codex's debate-gate review
 * called this out explicitly. */
#define VIRTIO_GPU_POLL_TIMEOUT_ITERS         0x100000u

/* Slot/SPI mapping mirrors virtio-blk. Even though M3.0 polls and
 * doesn't register an SPI handler, we still compute the slot index
 * for diagnostic logging. */
#define VIRTIO_GPU_MMIO_BASE_ADDR             0x0A000000UL
#define VIRTIO_GPU_MMIO_STRIDE_BYTES          0x200u
#define VIRTIO_GPU_MMIO_SPI_BASE              16u

/*
 * Wire-level structs from the Virtio 1.2 spec. Naturally aligned, so
 * no `packed` attribute — leaving it off keeps GCC's alignment warnings
 * useful if a future edit introduces padding.
 */

struct virtio_gpu_ctrl_hdr {
	uint32_t type;
	uint32_t flags;
	uint64_t fence_id;
	uint32_t ctx_id;
	uint32_t padding;
};

struct virtio_gpu_rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct virtio_gpu_resp_display_info {
	struct virtio_gpu_ctrl_hdr hdr;
	struct {
		struct virtio_gpu_rect r;
		uint32_t enabled;
		uint32_t flags;
	} pmodes[16];  /* VIRTIO_GPU_MAX_SCANOUTS spec value */
};

struct virtio_gpu_resource_create_2d {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t resource_id;
	uint32_t format;
	uint32_t width;
	uint32_t height;
};

struct virtio_gpu_resource_attach_backing {
	struct virtio_gpu_ctrl_hdr hdr;
	uint32_t resource_id;
	uint32_t nr_entries;
};

struct virtio_gpu_mem_entry {
	uint64_t addr;
	uint32_t length;
	uint32_t padding;
};

struct virtio_gpu_set_scanout {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	uint32_t scanout_id;
	uint32_t resource_id;
};

struct virtio_gpu_transfer_to_host_2d {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	uint64_t offset;
	uint32_t resource_id;
	uint32_t padding;
};

struct virtio_gpu_resource_flush {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	uint32_t resource_id;
	uint32_t padding;
};

/*
 * Single request and response page. M3.0 sends one command at a time
 * on the controlq, so the largest command (resource_attach_backing
 * with one mem_entry, or resource_create_2d) easily fits within one
 * page; same with the largest response (display_info, ~408 bytes).
 */
union virtio_gpu_req_page {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_resource_create_2d create_2d;
	struct virtio_gpu_set_scanout set_scanout;
	struct virtio_gpu_transfer_to_host_2d xfer;
	struct virtio_gpu_resource_flush flush;
	struct {
		struct virtio_gpu_resource_attach_backing hdr;
		struct virtio_gpu_mem_entry entry;
	} attach;
	uint8_t bytes[VIRT_DMA_PAGE_SIZE];
};

union virtio_gpu_resp_page {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_resp_display_info display_info;
	uint8_t bytes[VIRT_DMA_PAGE_SIZE];
};

/*
 * Wire-layout sanity checks. The Virtio 1.2 spec pins these sizes for
 * legacy little-endian transports; if a future edit introduces padding
 * or reorders fields, the build breaks loudly here rather than silently
 * shipping a wrong-sized command on the controlq. RESOURCE_ATTACH_BACKING
 * uses an inline single-entry layout in the request page; this assert
 * confirms the entry sits immediately after the 24-byte header.
 */
_Static_assert(sizeof(struct virtio_gpu_ctrl_hdr) == 24,
               "virtio-gpu ctrl_hdr must be 24 bytes (Virtio 1.2 §5.7.6.7)");
_Static_assert(sizeof(struct virtio_gpu_rect) == 16,
               "virtio-gpu rect must be 16 bytes (4 × u32)");
_Static_assert(sizeof(struct virtio_gpu_mem_entry) == 16,
               "virtio-gpu mem_entry must be 16 bytes (u64 + u32 + pad)");
_Static_assert(sizeof(struct virtio_gpu_resource_create_2d) == 40,
               "virtio-gpu resource_create_2d must be 40 bytes "
               "(hdr 24 + 4×u32)");
_Static_assert(sizeof(struct virtio_gpu_set_scanout) == 48,
               "virtio-gpu set_scanout must be 48 bytes "
               "(hdr 24 + rect 16 + 2×u32)");
_Static_assert(sizeof(struct virtio_gpu_transfer_to_host_2d) == 56,
               "virtio-gpu transfer_to_host_2d must be 56 bytes "
               "(hdr 24 + rect 16 + u64 + u32 + pad)");
_Static_assert(sizeof(struct virtio_gpu_resource_flush) == 48,
               "virtio-gpu resource_flush must be 48 bytes "
               "(hdr 24 + rect 16 + u32 + pad)");

/* Driver state. Single-CPU + single-driver-thread per virtio-blk. */
static int g_initialized;
static int g_ready;
static uintptr_t g_dev_base;

static virtq_t g_controlq;
static virtq_t g_cursorq;

static void *g_controlq_backing;
static void *g_cursorq_backing;
static union virtio_gpu_req_page *g_req;
static union virtio_gpu_resp_page *g_resp;
static uint8_t *g_scanout;

static uint32_t g_display_width = VIRTIO_GPU_M3_0_WIDTH;
static uint32_t g_display_height = VIRTIO_GPU_M3_0_HEIGHT;

static uint32_t g_dma_pages_held;

static uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static void mmio_write32(uintptr_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static void status_or(uintptr_t base, uint32_t bit)
{
	uint32_t v = mmio_read32(base + VIRTIO_MMIO_STATUS);
	mmio_write32(base + VIRTIO_MMIO_STATUS, v | bit);
}

/*
 * Issue a controlq command of `req_len` bytes that expects a response
 * of `resp_len` bytes. Returns 0 on success and the response is in
 * g_resp. -1 on any failure; logs to UART.
 *
 * Flow: alloc 2-descriptor chain (req read-only, resp write-only) →
 * fill descriptors → cache-clean req → submit → kick → poll used →
 * cache-invalidate resp → validate response type matches expected_type.
 */
static int virtio_gpu_submit_cmd(uint32_t req_len,
                                  uint32_t resp_len,
                                  uint32_t expected_type)
{
	uint16_t head;
	uint16_t req_idx;
	uint16_t resp_idx;
	uint16_t completed;
	uint32_t completed_len = 0;
	uint32_t poll;
	char line[96];
	uint32_t cmd_type = g_req->hdr.type;

	g_req->hdr.flags = 0;
	g_req->hdr.fence_id = 0;
	g_req->hdr.ctx_id = 0;
	g_req->hdr.padding = 0;

	/* Zero the response so a stale value can't masquerade as success. */
	g_resp->hdr.type = 0;
	g_resp->hdr.flags = 0;
	g_resp->hdr.fence_id = 0;
	g_resp->hdr.ctx_id = 0;

	arm64_dma_cache_clean(g_req, req_len);
	arm64_dma_cache_clean(g_resp, resp_len);

	head = virtq_alloc_chain(&g_controlq, 2u);
	if (head == 0xFFFFu) {
		platform_uart_puts(
		    "virtio-gpu: controlq alloc_chain failed (queue full?)\n");
		return -1;
	}
	req_idx = head;
	resp_idx = g_controlq.desc[req_idx].next;

	g_controlq.desc[req_idx].addr = virt_virt_to_phys(g_req);
	g_controlq.desc[req_idx].len = req_len;
	g_controlq.desc[req_idx].flags = VIRTQ_DESC_F_NEXT;
	g_controlq.desc[req_idx].next = resp_idx;

	g_controlq.desc[resp_idx].addr = virt_virt_to_phys(g_resp);
	g_controlq.desc[resp_idx].len = resp_len;
	g_controlq.desc[resp_idx].flags = VIRTQ_DESC_F_WRITE;
	g_controlq.desc[resp_idx].next = 0u;

	virtq_submit(&g_controlq, head);
	mmio_write32(g_dev_base + VIRTIO_MMIO_QUEUE_NOTIFY, 0u);

	for (poll = 0; poll < VIRTIO_GPU_POLL_TIMEOUT_ITERS; poll++) {
		completed = virtq_drain_one(&g_controlq, &completed_len);
		if (completed != 0xFFFFu)
			break;
	}

	if (poll == VIRTIO_GPU_POLL_TIMEOUT_ITERS) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: timeout polling for cmd 0x%X\n",
		           (unsigned int)cmd_type);
		platform_uart_puts(line);
		virtq_free_chain(&g_controlq, head);
		return -1;
	}

	if (completed != head) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: completion id mismatch (got %u, want %u)\n",
		           (unsigned int)completed,
		           (unsigned int)head);
		platform_uart_puts(line);
		virtq_free_chain(&g_controlq, head);
		return -1;
	}

	arm64_dma_rmb();
	arm64_dma_cache_invalidate(g_resp, resp_len);

	if (g_resp->hdr.type != expected_type) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: cmd 0x%X resp type 0x%X (want 0x%X)\n",
		           (unsigned int)cmd_type,
		           (unsigned int)g_resp->hdr.type,
		           (unsigned int)expected_type);
		platform_uart_puts(line);
		virtq_free_chain(&g_controlq, head);
		return -1;
	}

	virtq_free_chain(&g_controlq, head);
	return 0;
}

static int virtio_gpu_get_display_info(void)
{
	uint32_t enabled;
	uint32_t width;
	uint32_t height;
	char line[96];

	g_req->hdr.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
	if (virtio_gpu_submit_cmd(sizeof(struct virtio_gpu_ctrl_hdr),
	                           sizeof(struct virtio_gpu_resp_display_info),
	                           VIRTIO_GPU_RESP_OK_DISPLAY_INFO) != 0)
		return -1;

	enabled = g_resp->display_info.pmodes[0].enabled;
	width = g_resp->display_info.pmodes[0].r.width;
	height = g_resp->display_info.pmodes[0].r.height;

	if (!enabled || width == 0 || height == 0) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: scanout 0 not enabled (en=%u %ux%u)\n",
		           (unsigned int)enabled,
		           (unsigned int)width,
		           (unsigned int)height);
		platform_uart_puts(line);
		return -1;
	}

	g_display_width = width;
	g_display_height = height;

	k_snprintf(line,
	           sizeof(line),
	           "virtio-gpu: scanout 0 enabled, host display %ux%u\n",
	           (unsigned int)width,
	           (unsigned int)height);
	platform_uart_puts(line);
	return 0;
}

static int virtio_gpu_resource_create_2d(void)
{
	g_req->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
	g_req->create_2d.resource_id = VIRTIO_GPU_M3_0_RESOURCE_ID;
	g_req->create_2d.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
	g_req->create_2d.width = VIRTIO_GPU_M3_0_WIDTH;
	g_req->create_2d.height = VIRTIO_GPU_M3_0_HEIGHT;

	return virtio_gpu_submit_cmd(sizeof(struct virtio_gpu_resource_create_2d),
	                              sizeof(struct virtio_gpu_ctrl_hdr),
	                              VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_attach_backing(void)
{
	g_req->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
	g_req->attach.hdr.resource_id = VIRTIO_GPU_M3_0_RESOURCE_ID;
	g_req->attach.hdr.nr_entries = 1u;
	g_req->attach.entry.addr = virt_virt_to_phys(g_scanout);
	g_req->attach.entry.length = VIRTIO_GPU_M3_0_SCANOUT_BYTES;
	g_req->attach.entry.padding = 0;

	return virtio_gpu_submit_cmd(sizeof(g_req->attach),
	                              sizeof(struct virtio_gpu_ctrl_hdr),
	                              VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_set_scanout(uint32_t width, uint32_t height)
{
	g_req->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
	g_req->set_scanout.r.x = 0;
	g_req->set_scanout.r.y = 0;
	g_req->set_scanout.r.width = width;
	g_req->set_scanout.r.height = height;
	g_req->set_scanout.scanout_id = VIRTIO_GPU_M3_0_SCANOUT_ID;
	g_req->set_scanout.resource_id = VIRTIO_GPU_M3_0_RESOURCE_ID;

	return virtio_gpu_submit_cmd(sizeof(struct virtio_gpu_set_scanout),
	                              sizeof(struct virtio_gpu_ctrl_hdr),
	                              VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_transfer_to_host(uint32_t x,
                                        uint32_t y,
                                        uint32_t width,
                                        uint32_t height)
{
	uint64_t offset = ((uint64_t)y * (uint64_t)VIRTIO_GPU_M3_0_PITCH) +
	                  (uint64_t)x * (uint64_t)VIRTIO_GPU_M3_0_BPP;

	g_req->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
	g_req->xfer.r.x = x;
	g_req->xfer.r.y = y;
	g_req->xfer.r.width = width;
	g_req->xfer.r.height = height;
	g_req->xfer.offset = offset;
	g_req->xfer.resource_id = VIRTIO_GPU_M3_0_RESOURCE_ID;
	g_req->xfer.padding = 0;

	/* Make sure the CPU's writes to g_scanout are visible to the device
	 * before TRANSFER_TO_HOST_2D processes them. arm64_dma_cache_clean
	 * issues `dc cvac` + `dsb ish` per the M2.4b cache-discipline rules
	 * shared with virtio-blk. */
	arm64_dma_cache_clean(g_scanout, VIRTIO_GPU_M3_0_SCANOUT_BYTES);

	return virtio_gpu_submit_cmd(sizeof(struct virtio_gpu_transfer_to_host_2d),
	                              sizeof(struct virtio_gpu_ctrl_hdr),
	                              VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_resource_flush(uint32_t x,
                                      uint32_t y,
                                      uint32_t width,
                                      uint32_t height)
{
	g_req->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
	g_req->flush.r.x = x;
	g_req->flush.r.y = y;
	g_req->flush.r.width = width;
	g_req->flush.r.height = height;
	g_req->flush.resource_id = VIRTIO_GPU_M3_0_RESOURCE_ID;
	g_req->flush.padding = 0;

	return virtio_gpu_submit_cmd(sizeof(struct virtio_gpu_resource_flush),
	                              sizeof(struct virtio_gpu_ctrl_hdr),
	                              VIRTIO_GPU_RESP_OK_NODATA);
}

/*
 * Draw a deterministic 32x32 BGRA test pattern into g_scanout. The
 * pattern is four 16x16 quadrants in distinct primary colors so a
 * checksum gate in KTEST has a non-degenerate value to compare.
 *
 * Layout: top-left red, top-right green, bottom-left blue, bottom-right
 * white. BGRA pixel layout: byte 0 = blue, 1 = green, 2 = red, 3 = X.
 */
static void virtio_gpu_draw_test_pattern(void)
{
	uint32_t row;
	uint32_t col;
	uint32_t *pixels = (uint32_t *)g_scanout;

	for (row = 0; row < VIRTIO_GPU_M3_0_HEIGHT; row++) {
		for (col = 0; col < VIRTIO_GPU_M3_0_WIDTH; col++) {
			uint32_t bgra;
			int top = row < VIRTIO_GPU_M3_0_HEIGHT / 2u;
			int left = col < VIRTIO_GPU_M3_0_WIDTH / 2u;

			if (top && left)
				bgra = 0x000000FFu;  /* red */
			else if (top && !left)
				bgra = 0x0000FF00u;  /* green */
			else if (!top && left)
				bgra = 0x00FF0000u;  /* blue */
			else
				bgra = 0x00FFFFFFu;  /* white */

			pixels[row * VIRTIO_GPU_M3_0_WIDTH + col] = bgra;
		}
	}
}

/*
 * Allocate the driver's working buffers. Each successful allocation
 * increments g_dma_pages_held so the cleanup path in init failure can
 * be asserted leak-free by KTEST. virtq_init lays out desc/avail/used
 * inside the queue backing region; the caller owns that backing.
 */
static int virtio_gpu_alloc_buffers(void)
{
	g_controlq_backing = virt_dma_alloc(VIRTIO_GPU_CONTROLQ_PAGES);
	if (!g_controlq_backing) {
		platform_uart_puts("virtio-gpu: controlq backing alloc failed\n");
		return -1;
	}
	g_dma_pages_held += VIRTIO_GPU_CONTROLQ_PAGES;

	g_cursorq_backing = virt_dma_alloc(VIRTIO_GPU_CURSORQ_PAGES);
	if (!g_cursorq_backing) {
		platform_uart_puts("virtio-gpu: cursorq backing alloc failed\n");
		return -1;
	}
	g_dma_pages_held += VIRTIO_GPU_CURSORQ_PAGES;

	g_req = virt_dma_alloc(VIRTIO_GPU_REQ_PAGES);
	if (!g_req) {
		platform_uart_puts("virtio-gpu: req page alloc failed\n");
		return -1;
	}
	g_dma_pages_held += VIRTIO_GPU_REQ_PAGES;

	g_resp = virt_dma_alloc(VIRTIO_GPU_RESP_PAGES);
	if (!g_resp) {
		platform_uart_puts("virtio-gpu: resp page alloc failed\n");
		return -1;
	}
	g_dma_pages_held += VIRTIO_GPU_RESP_PAGES;

	g_scanout = virt_dma_alloc(VIRTIO_GPU_SCANOUT_PAGES);
	if (!g_scanout) {
		platform_uart_puts("virtio-gpu: scanout buffer alloc failed\n");
		return -1;
	}
	g_dma_pages_held += VIRTIO_GPU_SCANOUT_PAGES;

	if (virtq_init(&g_controlq,
	                g_controlq_backing,
	                VIRTIO_GPU_CONTROLQ_PAGES * VIRT_DMA_PAGE_SIZE) != 0) {
		platform_uart_puts("virtio-gpu: controlq virtq_init failed\n");
		return -1;
	}

	if (virtq_init(&g_cursorq,
	                g_cursorq_backing,
	                VIRTIO_GPU_CURSORQ_PAGES * VIRT_DMA_PAGE_SIZE) != 0) {
		platform_uart_puts("virtio-gpu: cursorq virtq_init failed\n");
		return -1;
	}

	return 0;
}

static void virtio_gpu_free_buffers(void)
{
	if (g_scanout) {
		virt_dma_free(g_scanout, VIRTIO_GPU_SCANOUT_PAGES);
		g_dma_pages_held -= VIRTIO_GPU_SCANOUT_PAGES;
		g_scanout = 0;
	}
	if (g_resp) {
		virt_dma_free(g_resp, VIRTIO_GPU_RESP_PAGES);
		g_dma_pages_held -= VIRTIO_GPU_RESP_PAGES;
		g_resp = 0;
	}
	if (g_req) {
		virt_dma_free(g_req, VIRTIO_GPU_REQ_PAGES);
		g_dma_pages_held -= VIRTIO_GPU_REQ_PAGES;
		g_req = 0;
	}
	if (g_cursorq_backing) {
		virt_dma_free(g_cursorq_backing, VIRTIO_GPU_CURSORQ_PAGES);
		g_dma_pages_held -= VIRTIO_GPU_CURSORQ_PAGES;
		g_cursorq_backing = 0;
	}
	if (g_controlq_backing) {
		virt_dma_free(g_controlq_backing, VIRTIO_GPU_CONTROLQ_PAGES);
		g_dma_pages_held -= VIRTIO_GPU_CONTROLQ_PAGES;
		g_controlq_backing = 0;
	}
}

/*
 * Configure one virtqueue on the legacy virtio-mmio transport. `qsel`
 * is the queue index (0 = controlq, 1 = cursorq); `q` is the in-memory
 * virtq_t already populated by virtq_init.
 *
 * Returns 0 on success or -1 if the device's QueueNumMax is below
 * VIRTQ_SIZE.
 */
static int virtio_gpu_configure_queue(uint32_t qsel, virtq_t *q)
{
	uint32_t qmax;
	char line[96];

	mmio_write32(g_dev_base + VIRTIO_MMIO_QUEUE_SEL, qsel);
	qmax = mmio_read32(g_dev_base + VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (qmax < VIRTQ_SIZE) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: q%u QueueNumMax=%u below VIRTQ_SIZE\n",
		           (unsigned int)qsel,
		           (unsigned int)qmax);
		platform_uart_puts(line);
		return -1;
	}
	mmio_write32(g_dev_base + VIRTIO_MMIO_QUEUE_NUM, VIRTQ_SIZE);
	mmio_write32(g_dev_base + VIRTIO_MMIO_QUEUE_ALIGN, VIRT_DMA_PAGE_SIZE);
	mmio_write32(g_dev_base + VIRTIO_MMIO_QUEUE_PFN,
	             (uint32_t)(q->base_phys >> 12));
	return 0;
}

int arm64_virt_virtio_gpu_init(void)
{
	uintptr_t base;
	uint32_t version;
	uint32_t slot;
	uint32_t spi;
	char line[96];

	if (g_initialized)
		return 0;

	if (!virtio_mmio_find(VIRTIO_DEV_ID_GPU, &base, &version)) {
		platform_uart_puts(
		    "virtio-gpu: no device found on bus (skipping M3.0)\n");
		return -1;
	}

	if (version != 1u) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: unsupported transport version %u (legacy only)\n",
		           (unsigned int)version);
		platform_uart_puts(line);
		return -1;
	}

	g_dev_base = base;
	slot = (uint32_t)((base - VIRTIO_GPU_MMIO_BASE_ADDR) /
	                   VIRTIO_GPU_MMIO_STRIDE_BYTES);
	spi = VIRTIO_GPU_MMIO_SPI_BASE + slot;
	(void)spi;  /* M3.0 polls; SPI computed for diagnostic completeness. */

	k_snprintf(line,
	           sizeof(line),
	           "virtio-gpu: device @ 0x%X (slot %u, polled mode)\n",
	           (unsigned int)base,
	           (unsigned int)slot);
	platform_uart_puts(line);

	if (virtio_gpu_alloc_buffers() != 0) {
		virtio_gpu_free_buffers();
		return -1;
	}

	/* Per Virtio 1.0 §3.1.1: reset → ack → driver → feature negotiation
	 * (we accept zero features, explicitly rejecting VIRGL/EDID/UUID/BLOB
	 * /CONTEXT_INIT per the M3.0 plan) → features-ok → queue setup →
	 * driver-ok. The same sequence virtio-blk uses, with two queues
	 * configured before DRIVER_OK. */
	mmio_write32(base + VIRTIO_MMIO_STATUS, 0u);
	status_or(base, VIRTIO_STATUS_ACKNOWLEDGE);
	status_or(base, VIRTIO_STATUS_DRIVER);
	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0u);
	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES, 0u);
	status_or(base, VIRTIO_STATUS_FEATURES_OK);

	if ((mmio_read32(base + VIRTIO_MMIO_STATUS) &
	     VIRTIO_STATUS_FEATURES_OK) == 0u) {
		platform_uart_puts(
		    "virtio-gpu: device rejected feature negotiation\n");
		status_or(base, VIRTIO_STATUS_FAILED);
		virtio_gpu_free_buffers();
		return -1;
	}

	mmio_write32(base + VIRTIO_MMIO_GUEST_PAGE_SIZE, VIRT_DMA_PAGE_SIZE);

	if (virtio_gpu_configure_queue(0u, &g_controlq) != 0)
		goto out_cleanup;
	if (virtio_gpu_configure_queue(1u, &g_cursorq) != 0)
		goto out_cleanup;

	status_or(base, VIRTIO_STATUS_DRIVER_OK);

	/*
	 * Six-command 2D bring-up sequence. Any failure unwinds via the
	 * same cleanup label as the queue-setup failures: reset the
	 * device, free buffers, leave g_initialized clear so a later
	 * caller can retry. M3.1's /dev/fb0 swap will key off
	 * arm64_virt_virtio_gpu_ready().
	 */
	if (virtio_gpu_get_display_info() != 0)
		goto out_cleanup;
	if (virtio_gpu_resource_create_2d() != 0)
		goto out_cleanup;
	if (virtio_gpu_attach_backing() != 0)
		goto out_cleanup;
	if (virtio_gpu_set_scanout(VIRTIO_GPU_M3_0_WIDTH,
	                            VIRTIO_GPU_M3_0_HEIGHT) != 0)
		goto out_cleanup;

	virtio_gpu_draw_test_pattern();

	if (virtio_gpu_transfer_to_host(0,
	                                 0,
	                                 VIRTIO_GPU_M3_0_WIDTH,
	                                 VIRTIO_GPU_M3_0_HEIGHT) != 0)
		goto out_cleanup;
	if (virtio_gpu_resource_flush(0,
	                               0,
	                               VIRTIO_GPU_M3_0_WIDTH,
	                               VIRTIO_GPU_M3_0_HEIGHT) != 0)
		goto out_cleanup;

	/* All six commands succeeded — only now mark the driver init'd
	 * and ready. Setting g_initialized earlier would let a future
	 * init call short-circuit with success after a partial failure. */
	g_initialized = 1;
	g_ready = 1;
	platform_uart_puts(
	    "virtio-gpu: M3.0 sequence complete (32x32 BGRA test pattern flushed)\n");
	return 0;

out_cleanup:
	/* Reset the device to detach it from the buffers we are about to
	 * free, clear DRIVER_OK if it was set, and surface the failure to
	 * the host via VIRTIO_STATUS_FAILED. Per the spec, writing zero to
	 * STATUS triggers a device reset; the FAILED bit is then a no-op
	 * but we set it for symmetry with the feature-negotiation path. */
	mmio_write32(base + VIRTIO_MMIO_STATUS, 0u);
	status_or(base, VIRTIO_STATUS_FAILED);
	virtio_gpu_free_buffers();
	g_dev_base = 0;
	return -1;
}

int arm64_virt_virtio_gpu_ready(void)
{
	return g_ready;
}

int arm64_virt_virtio_gpu_query_display(uint32_t *out_width,
                                         uint32_t *out_height)
{
	if (!g_ready)
		return -1;
	if (out_width)
		*out_width = g_display_width;
	if (out_height)
		*out_height = g_display_height;
	return 0;
}

int arm64_virt_virtio_gpu_partial_flush_smoke(void)
{
	uint32_t *pixels;
	uint32_t row;
	uint32_t col;

	if (!g_ready)
		return -1;

	/* Overwrite a 16x16 patch in the bottom-right quadrant with a
	 * known sentinel color so a later checksum can confirm the path
	 * actually mutated the buffer end-to-end. */
	pixels = (uint32_t *)g_scanout;
	for (row = VIRTIO_GPU_M3_0_HEIGHT / 2u;
	     row < VIRTIO_GPU_M3_0_HEIGHT;
	     row++) {
		for (col = VIRTIO_GPU_M3_0_WIDTH / 2u;
		     col < VIRTIO_GPU_M3_0_WIDTH;
		     col++) {
			pixels[row * VIRTIO_GPU_M3_0_WIDTH + col] = 0x00010203u;
		}
	}

	if (virtio_gpu_transfer_to_host(VIRTIO_GPU_M3_0_WIDTH / 2u,
	                                 VIRTIO_GPU_M3_0_HEIGHT / 2u,
	                                 VIRTIO_GPU_M3_0_WIDTH / 2u,
	                                 VIRTIO_GPU_M3_0_HEIGHT / 2u) != 0)
		return -1;
	if (virtio_gpu_resource_flush(VIRTIO_GPU_M3_0_WIDTH / 2u,
	                               VIRTIO_GPU_M3_0_HEIGHT / 2u,
	                               VIRTIO_GPU_M3_0_WIDTH / 2u,
	                               VIRTIO_GPU_M3_0_HEIGHT / 2u) != 0)
		return -1;

	return 0;
}

uint32_t arm64_virt_virtio_gpu_checksum_pattern(void)
{
	uint32_t sum = 0;
	uint32_t i;
	uint32_t *pixels;

	if (!g_scanout)
		return 0;

	pixels = (uint32_t *)g_scanout;
	for (i = 0; i < (VIRTIO_GPU_M3_0_SCANOUT_BYTES / 4u); i++)
		sum = (sum * 1664525u) + pixels[i] + 1013904223u;
	return sum;
}

uint32_t arm64_virt_virtio_gpu_dma_pages_held(void)
{
	return g_dma_pages_held;
}
