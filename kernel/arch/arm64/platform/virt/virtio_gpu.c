/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_gpu.c - virtio-gpu 2D front-end for the QEMU virt platform.
 *
 * Phase 2 of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md. M3.0
 * brought up the controlq + cursorq and ran the six-command 2D
 * sequence on a 32x32 BGRA scratch resource. M3.1 retires the
 * scratch resource and drives the 1024x768 BGRA framebuffer that
 * /dev/fb0 publishes — virtio-gpu now owns the chardev when its init
 * succeeds, with ramfb falling back when virtio-gpu is absent.
 *
 * Scanout backing: the existing 8 MiB reservation in
 * platform_ram_layout()->framebuffer_base/size, reused from ramfb
 * (M2.5a). Mapped Normal-NC by the existing PLATFORM_MM_FRAMEBUFFER
 * classifier; the kernel-alias remap walks the page tables once
 * during init to drop any leftover Normal-WB block coverage. The
 * /dev/fb0 mmap policy stays CHARDEV_CACHE_NC (no userspace change).
 *
 * Completion is polled. The polled controlq wait runs in process
 * context; the timer-tick path will set a request flag in M3.1
 * Commit 4, with the deferred pump running TRANSFER+FLUSH from
 * arm64_sync_handler() on syscall return. virtio-blk-style IRQ
 * delivery on a GICv3 SPI is a later milestone.
 */

#include "../platform.h"
#include "../../dma.h"
#include "../../mm/mmu.h"
#include "dma.h"
#include "fbdev.h"
#include "kprintf.h"
#include "kstring.h"
#include "virtio_gpu.h"
#include "virtio_mmio.h"
#include "virtio_queue.h"
#include <stdint.h>

/* Virtio 1.2 §5.7.6.7: command type encodings. Only the 2D commands
 * M3.0 needs are named; the rest are intentionally absent so an
 * accidental reference fails to compile. */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104u

/* Response types we expect from M3.0 commands. */
#define VIRTIO_GPU_RESP_OK_NODATA 0x1100u
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101u

/* §5.7.4 device config: VIRTIO_GPU_MAX_SCANOUTS = 16, but 1 is plenty
 * for the v1.2 path. */
#define VIRTIO_GPU_MAX_SCANOUTS 1u

/* §5.7.6.7 Format codes (subset). B8G8R8X8_UNORM matches Drunix's
 * existing 32-bpp BGRA framebuffer 1:1 (X = unused alpha byte). */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2u

/* Fixed resource id and scanout id. 0 is reserved as "no resource";
 * the driver only ever has one live resource so a static id keeps the
 * lifecycle obvious. */
#define VIRTIO_GPU_RESOURCE_ID 1u
#define VIRTIO_GPU_SCANOUT_ID 0u

/* Scanout dimensions. M3.1 keeps the same 1024x768 BGRA size that
 * ramfb (M2.5a) used so the userspace compositor (user/apps/desktop)
 * doesn't need to recompile. The 8 MiB framebuffer reservation
 * (platform_mm.c VIRT_RAMFB_BYTES) covers up to ~1448x1448 BGRA;
 * growing to display-info-reported dimensions (1280x800 or larger)
 * is a separate milestone gated on the compositor handling dynamic
 * geometry.
 */
#define VIRTIO_GPU_WIDTH 1024u
#define VIRTIO_GPU_HEIGHT 768u
#define VIRTIO_GPU_BPP 4u
#define VIRTIO_GPU_PITCH (VIRTIO_GPU_WIDTH * VIRTIO_GPU_BPP)
#define VIRTIO_GPU_SCANOUT_BYTES (VIRTIO_GPU_PITCH * VIRTIO_GPU_HEIGHT)

/* Page budgets — see docs/design/m3.1-virtio-gpu-fbdev-swap.md.
 * The scanout buffer no longer comes from the DMA pool; it lives in
 * the platform_ram_layout()->framebuffer_base/size reservation. */
#define VIRTIO_GPU_CONTROLQ_PAGES 2u
#define VIRTIO_GPU_CURSORQ_PAGES 2u
#define VIRTIO_GPU_REQ_PAGES 1u
#define VIRTIO_GPU_RESP_PAGES 1u

/* Bounded poll on the used ring after a controlq submit. Same shape
 * as virtio-blk's POLL_TIMEOUT_ITERS so a stuck device shows up in
 * the boot log instead of hanging silently. Codex's debate-gate review
 * called this out explicitly. */
#define VIRTIO_GPU_POLL_TIMEOUT_ITERS 0x100000u

/* Slot/SPI mapping mirrors virtio-blk. Even though M3.0 polls and
 * doesn't register an SPI handler, we still compute the slot index
 * for diagnostic logging. */
#define VIRTIO_GPU_MMIO_BASE_ADDR 0x0A000000UL
#define VIRTIO_GPU_MMIO_STRIDE_BYTES 0x200u
#define VIRTIO_GPU_MMIO_SPI_BASE 16u

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
	} pmodes[16]; /* VIRTIO_GPU_MAX_SCANOUTS spec value */
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
static int g_device_found;
static uintptr_t g_dev_base;

static virtq_t g_controlq;
static virtq_t g_cursorq;

static void *g_controlq_backing;
static void *g_cursorq_backing;
static union virtio_gpu_req_page *g_req;
static union virtio_gpu_resp_page *g_resp;

/* Scanout buffer in the framebuffer reservation. virt_to_phys is an
 * identity mapping on this platform, so g_scanout (kernel virtual)
 * equals g_scanout_phys (guest physical) numerically. The driver
 * keeps both for clarity at the protocol boundary. */
static uint8_t *g_scanout;
static uint64_t g_scanout_phys;

static uint32_t g_display_width = VIRTIO_GPU_WIDTH;
static uint32_t g_display_height = VIRTIO_GPU_HEIGHT;

/* framebuffer_info_t handed to fbdev_init() once the protocol path
 * is alive. Lives in BSS so its address is stable across the lifetime
 * of the kernel image. */
static framebuffer_info_t g_fbdev_info;

static uint32_t g_dma_pages_held;

/* M3.1 Commit 4 — deferred flush state. arm64_timer_tick is IRQ
 * context; it sets g_flush_needed = 1 and returns. arm64_sync_handler
 * (process context, after every syscall) calls the pump, which clears
 * the flag and runs the actual controlq commands. */
static volatile uint32_t g_flush_needed;
static uint32_t g_flush_failures;
static uint32_t g_flush_runs;
static int g_flush_disabled;

#define VIRTIO_GPU_FLUSH_FAIL_THRESHOLD 60u

/* M3.2 — coalesced dirty-rect state. publish_dirty_rect (called from
 * fbdev ioctl in process context, but the timer-tick fallback in
 * Commit 4 will also publish from IRQ context) unions rects into
 * g_pending_dirty. The pump consumes the union under a brief
 * IRQ-mask critical section so IRQ-context publishes can't tear the
 * consume; the actual TRANSFER+FLUSH runs with IRQs enabled. */
static struct {
	drunix_rect_t rect;
	int valid;
} g_pending_dirty;

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

	/*
	 * Validate the entire response header, not just hdr.type. A
	 * well-behaved QEMU echoes the request's flags / fence_id / ctx_id
	 * unchanged when no fence was requested, and writes exactly
	 * resp_len bytes. Any deviation means either the device is
	 * misbehaving or a future change introduced a fence path without
	 * teaching this validator about it; either way, fail loud.
	 * (Codex M3.0 delivery review #3.)
	 */
	if (g_resp->hdr.type != expected_type) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: cmd 0x%X resp type 0x%X (want 0x%X)\n",
		           (unsigned int)cmd_type,
		           (unsigned int)g_resp->hdr.type,
		           (unsigned int)expected_type);
		platform_uart_puts(line);
		goto bad_resp;
	}

	if (g_resp->hdr.flags != 0u) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: cmd 0x%X resp flags 0x%X (want 0)\n",
		           (unsigned int)cmd_type,
		           (unsigned int)g_resp->hdr.flags);
		platform_uart_puts(line);
		goto bad_resp;
	}
	if (g_resp->hdr.fence_id != 0u || g_resp->hdr.ctx_id != 0u) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: cmd 0x%X resp fence_id/ctx_id nonzero\n",
		           (unsigned int)cmd_type);
		platform_uart_puts(line);
		goto bad_resp;
	}
	if (completed_len != resp_len) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-gpu: cmd 0x%X short resp (%u of %u)\n",
		           (unsigned int)cmd_type,
		           (unsigned int)completed_len,
		           (unsigned int)resp_len);
		platform_uart_puts(line);
		goto bad_resp;
	}

	virtq_free_chain(&g_controlq, head);
	return 0;

bad_resp:
	virtq_free_chain(&g_controlq, head);
	return -1;
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
	g_req->create_2d.resource_id = VIRTIO_GPU_RESOURCE_ID;
	g_req->create_2d.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
	g_req->create_2d.width = VIRTIO_GPU_WIDTH;
	g_req->create_2d.height = VIRTIO_GPU_HEIGHT;

	return virtio_gpu_submit_cmd(sizeof(struct virtio_gpu_resource_create_2d),
	                             sizeof(struct virtio_gpu_ctrl_hdr),
	                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_attach_backing(void)
{
	g_req->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
	g_req->attach.hdr.resource_id = VIRTIO_GPU_RESOURCE_ID;
	g_req->attach.hdr.nr_entries = 1u;
	/* The scanout buffer lives in the platform_ram_layout()
	 * framebuffer reservation, NOT the virt_dma_alloc pool.
	 * virt_virt_to_phys() only knows how to translate DMA-pool
	 * pointers — handing it g_scanout returns 0, which would point
	 * RESOURCE_ATTACH_BACKING at physical address 0 and silently
	 * black-screen the display. The reservation is identity-mapped
	 * (kernel virtual == guest physical), so g_scanout_phys is the
	 * direct address. */
	g_req->attach.entry.addr = g_scanout_phys;
	g_req->attach.entry.length = VIRTIO_GPU_SCANOUT_BYTES;
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
	g_req->set_scanout.scanout_id = VIRTIO_GPU_SCANOUT_ID;
	g_req->set_scanout.resource_id = VIRTIO_GPU_RESOURCE_ID;

	return virtio_gpu_submit_cmd(sizeof(struct virtio_gpu_set_scanout),
	                             sizeof(struct virtio_gpu_ctrl_hdr),
	                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_transfer_to_host(uint32_t x,
                                       uint32_t y,
                                       uint32_t width,
                                       uint32_t height)
{
	uint64_t offset = ((uint64_t)y * (uint64_t)VIRTIO_GPU_PITCH) +
	                  (uint64_t)x * (uint64_t)VIRTIO_GPU_BPP;

	g_req->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
	g_req->xfer.r.x = x;
	g_req->xfer.r.y = y;
	g_req->xfer.r.width = width;
	g_req->xfer.r.height = height;
	g_req->xfer.offset = offset;
	g_req->xfer.resource_id = VIRTIO_GPU_RESOURCE_ID;
	g_req->xfer.padding = 0;

	/* Make sure the CPU's writes to g_scanout are visible to the device
	 * before TRANSFER_TO_HOST_2D processes them. arm64_dma_cache_clean
	 * issues `dc cvac` + `dsb ish` per the M2.4b cache-discipline rules
	 * shared with virtio-blk. */
	arm64_dma_cache_clean(g_scanout, VIRTIO_GPU_SCANOUT_BYTES);

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
	g_req->flush.resource_id = VIRTIO_GPU_RESOURCE_ID;
	g_req->flush.padding = 0;

	return virtio_gpu_submit_cmd(sizeof(struct virtio_gpu_resource_flush),
	                             sizeof(struct virtio_gpu_ctrl_hdr),
	                             VIRTIO_GPU_RESP_OK_NODATA);
}

/*
 * Zero the scanout buffer. The compositor (user/apps/desktop) draws
 * over this within a few frames; a black framebuffer at boot is the
 * least-confusing starting state. M3.0's color-quadrant test pattern
 * was useful when the resource was a small scratch buffer outside
 * /dev/fb0; with M3.1 the scanout IS the displayed framebuffer, so
 * any kernel-side art would just be a brief flicker before the
 * desktop overwrites it.
 */
static void virtio_gpu_zero_scanout(void)
{
	k_memset(g_scanout, 0, VIRTIO_GPU_SCANOUT_BYTES);
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
	/* Scanout backing lives in the framebuffer reservation (owned by
	 * platform_ram_layout()), not the DMA pool. The reservation
	 * persists across init failures; just drop the pointer so a
	 * future init call recomputes it. */
	g_scanout = 0;
	g_scanout_phys = 0;

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
	const platform_ram_layout_t *layout;
	uint64_t fb_base;
	uint64_t fb_size;
	char line[128];

	if (g_initialized)
		return 0;

	if (!virtio_mmio_find(VIRTIO_DEV_ID_GPU, &base, &version)) {
		platform_uart_puts(
		    "virtio-gpu: no device found on bus (skipping)\n");
		return -1;
	}
	g_device_found = 1;

	if (version != 1u) {
		k_snprintf(
		    line,
		    sizeof(line),
		    "virtio-gpu: unsupported transport version %u (legacy only)\n",
		    (unsigned int)version);
		platform_uart_puts(line);
		return -1;
	}

	/* M3.1: scanout backing comes from the framebuffer reservation
	 * carved by platform_mm_init (M2.5a). If the reservation is
	 * absent (RAM too small) or smaller than what the configured
	 * resolution needs, fall through to ramfb fallback. */
	layout = platform_ram_layout();
	fb_base = layout->framebuffer_base;
	fb_size = layout->framebuffer_size;
	if (fb_size < VIRTIO_GPU_SCANOUT_BYTES || fb_base == 0) {
		platform_uart_puts(
		    "virtio-gpu: framebuffer reservation too small or absent; skipping\n");
		return -1;
	}

	g_dev_base = base;
	slot = (uint32_t)((base - VIRTIO_GPU_MMIO_BASE_ADDR) /
	                  VIRTIO_GPU_MMIO_STRIDE_BYTES);
	spi = VIRTIO_GPU_MMIO_SPI_BASE + slot;
	(void)spi; /* polled mode; SPI computed for diagnostic completeness. */

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

	/* Drop the Normal-WB Inner-Shareable kernel alias for the FB
	 * span. The platform classifier already returns
	 * PLATFORM_MM_FRAMEBUFFER for these pages; the remap walks the
	 * kernel page tables and stamps Normal-NC leaves where the L1[1]
	 * 1 GiB block previously covered them with Normal-WB. Idempotent
	 * — if ramfb already remapped (it won't, since we run first now,
	 * but the contract should survive future reordering), this is a
	 * no-op. */
	if (arm64_mmu_kernel_remap_range(fb_base, fb_size) != 0) {
		platform_uart_puts(
		    "virtio-gpu: kernel-alias remap failed; refusing to publish\n");
		virtio_gpu_free_buffers();
		return -1;
	}

	g_scanout_phys = fb_base;
	g_scanout = (uint8_t *)(uintptr_t)fb_base;
	virtio_gpu_zero_scanout();

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

	if ((mmio_read32(base + VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) ==
	    0u) {
		platform_uart_puts("virtio-gpu: device rejected feature negotiation\n");
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
	if (virtio_gpu_set_scanout(VIRTIO_GPU_WIDTH, VIRTIO_GPU_HEIGHT) !=
	    0)
		goto out_cleanup;

	if (virtio_gpu_transfer_to_host(
	        0, 0, VIRTIO_GPU_WIDTH, VIRTIO_GPU_HEIGHT) != 0)
		goto out_cleanup;
	if (virtio_gpu_resource_flush(
	        0, 0, VIRTIO_GPU_WIDTH, VIRTIO_GPU_HEIGHT) != 0)
		goto out_cleanup;

	/* Build the framebuffer_info_t and publish /dev/fb0. fbdev_init
	 * registers the chardevs once; ramfb's skip-on-fb0-already-
	 * registered check (M3.1 Commit 2) keeps the fallback provider
	 * out of the way when this path wins. */
	g_fbdev_info.phys_address = (uintptr_t)g_scanout_phys;
	g_fbdev_info.address = (uintptr_t)g_scanout_phys;
	g_fbdev_info.pitch = VIRTIO_GPU_PITCH;
	g_fbdev_info.width = VIRTIO_GPU_WIDTH;
	g_fbdev_info.height = VIRTIO_GPU_HEIGHT;
	g_fbdev_info.bpp = VIRTIO_GPU_BPP * 8u;
	g_fbdev_info.red_pos = 16;
	g_fbdev_info.red_size = 8;
	g_fbdev_info.green_pos = 8;
	g_fbdev_info.green_size = 8;
	g_fbdev_info.blue_pos = 0;
	g_fbdev_info.blue_size = 8;
	g_fbdev_info.cell_cols = 0;
	g_fbdev_info.cell_rows = 0;
	g_fbdev_info.back_address = 0;
	g_fbdev_info.back_pitch = 0;
	g_fbdev_info.cursor.x = 0;
	g_fbdev_info.cursor.y = 0;
	g_fbdev_info.cursor.fg = 0;
	g_fbdev_info.cursor.shadow = 0;
	g_fbdev_info.cursor.visible = 0;

	if (fbdev_init(&g_fbdev_info) != 0) {
		platform_uart_puts(
		    "virtio-gpu: fbdev_init refused; refusing to publish\n");
		goto out_cleanup;
	}

	/* M3.2: register the dirty-rect publish hook so userspace ioctls
	 * route through to virtio-gpu's per-rect pump path. The ramfb
	 * fallback registers no hook (publish_dirty_rect = NULL) since
	 * the host scans guest pages directly there. */
	fbdev_set_publish_dirty_rect(arm64_virt_virtio_gpu_publish_dirty_rect);

	/* All steps succeeded — only now mark the driver init'd and
	 * ready. Setting g_initialized earlier would let a future init
	 * call short-circuit with success after a partial failure. */
	g_initialized = 1;
	g_ready = 1;
	k_snprintf(line,
	           sizeof(line),
	           "virtio-gpu: %ux%ux%u @ phys 0x%lx published as /dev/fb0\n",
	           (unsigned int)VIRTIO_GPU_WIDTH,
	           (unsigned int)VIRTIO_GPU_HEIGHT,
	           (unsigned int)(VIRTIO_GPU_BPP * 8u),
	           (unsigned long)g_scanout_phys);
	platform_uart_puts(line);
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

int arm64_virt_virtio_gpu_device_found(void)
{
	return g_device_found;
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
	const uint32_t patch_w = 64u;
	const uint32_t patch_h = 64u;
	const uint32_t patch_x = VIRTIO_GPU_WIDTH / 4u;
	const uint32_t patch_y = VIRTIO_GPU_HEIGHT / 4u;

	if (!g_ready)
		return -1;

	/* Overwrite a small fixed 64x64 patch with a known sentinel so the
	 * checksum can confirm the partial-flush path actually mutated the
	 * buffer end-to-end. Smaller than the surrounding display so the
	 * KTEST cost stays bounded regardless of resolution. */
	pixels = (uint32_t *)g_scanout;
	for (row = patch_y; row < patch_y + patch_h; row++) {
		for (col = patch_x; col < patch_x + patch_w; col++)
			pixels[row * VIRTIO_GPU_WIDTH + col] = 0x00010203u;
	}

	if (virtio_gpu_transfer_to_host(patch_x, patch_y, patch_w, patch_h) != 0)
		return -1;
	if (virtio_gpu_resource_flush(patch_x, patch_y, patch_w, patch_h) != 0)
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
	for (i = 0; i < (VIRTIO_GPU_SCANOUT_BYTES / 4u); i++)
		sum = (sum * 1664525u) + pixels[i] + 1013904223u;
	return sum;
}

uint32_t arm64_virt_virtio_gpu_dma_pages_held(void)
{
	return g_dma_pages_held;
}

/*
 * IRQ-safe: just set the flag. No submission, no polling. The
 * timer tick calls this; the actual TRANSFER+FLUSH runs from
 * arm64_virt_virtio_gpu_pump_flush() in process context.
 */
void arm64_virt_virtio_gpu_request_flush(void)
{
	if (!g_ready || g_flush_disabled)
		return;
	g_flush_needed = 1u;
}

/*
 * Process-context pump. If a flush is pending, run a full-frame
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. On repeated failure beyond
 * VIRTIO_GPU_FLUSH_FAIL_THRESHOLD, mark the display permanently
 * disabled and stop attempting flushes (avoids log spam under a
 * dead device). The request side respects the disabled flag too,
 * so subsequent ticks no-op.
 *
 * Must NOT run from IRQ context: virtio_gpu_submit_cmd polls the
 * controlq used ring with a bounded busy wait, which is appropriate
 * in process context but ugly in an interrupt handler.
 */
/*
 * Per-rect TRANSFER+FLUSH. Cleans the contiguous row-span covering
 * the rect (rect.y * pitch for rect.h * pitch bytes) — over-cleans
 * horizontally for non-full-width rects, but stays correct and
 * mostly academic on the Normal-NC mapping.
 */
static int virtio_gpu_pump_rect(drunix_rect_t rect)
{
	uint64_t row_offset = (uint64_t)rect.y * (uint64_t)VIRTIO_GPU_PITCH;
	uint64_t row_bytes = (uint64_t)rect.h * (uint64_t)VIRTIO_GPU_PITCH;

	arm64_dma_cache_clean(g_scanout + row_offset, (uint32_t)row_bytes);

	if (virtio_gpu_transfer_to_host((uint32_t)rect.x, (uint32_t)rect.y,
	                                (uint32_t)rect.w,
	                                (uint32_t)rect.h) != 0)
		return -1;
	if (virtio_gpu_resource_flush((uint32_t)rect.x, (uint32_t)rect.y,
	                              (uint32_t)rect.w,
	                              (uint32_t)rect.h) != 0)
		return -1;
	return 0;
}

/*
 * Atomically take the pending dirty rect (consume + reset) under an
 * IRQ-mask critical section. Called by the pump in process context;
 * an IRQ-context publish_dirty_rect (e.g. fallback from timer tick
 * in Commit 4) cannot interleave with the copy + valid-clear pair.
 */
static int virtio_gpu_take_pending_rect(drunix_rect_t *out_rect)
{
	uint64_t saved_daif;
	int had_rect;

	__asm__ volatile("mrs %0, daif" : "=r"(saved_daif));
	__asm__ volatile("msr daifset, #2");

	had_rect = g_pending_dirty.valid;
	if (had_rect) {
		*out_rect = g_pending_dirty.rect;
		g_pending_dirty.valid = 0;
		g_pending_dirty.rect = drunix_rect_make(0, 0, 0, 0);
	}

	__asm__ volatile("msr daif, %0" : : "r"(saved_daif) : "memory");
	return had_rect;
}

void arm64_virt_virtio_gpu_publish_dirty_rect(drunix_rect_t rect)
{
	uint64_t saved_daif;

	if (!g_ready || g_flush_disabled || !drunix_rect_valid(rect))
		return;

	__asm__ volatile("mrs %0, daif" : "=r"(saved_daif));
	__asm__ volatile("msr daifset, #2");

	if (g_pending_dirty.valid)
		g_pending_dirty.rect =
		    drunix_rect_union(g_pending_dirty.rect, rect);
	else
		g_pending_dirty.rect = rect;
	g_pending_dirty.valid = 1;
	g_flush_needed = 1u;

	__asm__ volatile("msr daif, %0" : : "r"(saved_daif) : "memory");
}

void arm64_virt_virtio_gpu_pump_flush(void)
{
	drunix_rect_t rect;
	int had_pending;
	int rc;

	if (!g_ready || g_flush_disabled || !g_flush_needed)
		return;

	g_flush_needed = 0u;

	/*
	 * M3.2: prefer the per-rect path when a publish has set the
	 * pending dirty rect. Otherwise (M3.1 callers that still set
	 * g_flush_needed without publishing a rect) fall back to a
	 * full-frame TRANSFER. Commit 4 makes the full-frame path
	 * conditional on the no-ioctl-recent fallback timer.
	 */
	had_pending = virtio_gpu_take_pending_rect(&rect);
	if (had_pending) {
		rc = virtio_gpu_pump_rect(rect);
	} else {
		rect = drunix_rect_make(0, 0, (int)VIRTIO_GPU_WIDTH,
		                        (int)VIRTIO_GPU_HEIGHT);
		rc = virtio_gpu_pump_rect(rect);
	}

	if (rc != 0) {
		g_flush_failures++;
		if (g_flush_failures >= VIRTIO_GPU_FLUSH_FAIL_THRESHOLD) {
			g_flush_disabled = 1;
			platform_uart_puts(
			    "virtio-gpu: display permanently disabled after "
			    "repeated flush failures\n");
		}
		return;
	}
	g_flush_failures = 0u;
	g_flush_runs++;

	/* Diagnostic: log the first pump-driven flush so the boot log
	 * confirms the deferred-flush path is alive. */
	if (g_flush_runs == 1u)
		platform_uart_puts(
		    "virtio-gpu: first pump-driven flush succeeded\n");
	else if (g_flush_runs == 60u)
		platform_uart_puts(
		    "virtio-gpu: pump steady-state (60 flushes)\n");
}

uint32_t arm64_virt_virtio_gpu_pump_runs(void)
{
	return g_flush_runs;
}
