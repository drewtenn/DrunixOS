/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_blk.c - Smallest virtio-blk driver that does one read.
 *
 * Phase 1 M2.1 of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
 * Brings up the first virtio-blk device the bus enumeration found,
 * sets up one virtqueue (queue 0 = requestq), and issues a single
 * 512-byte read of sector 0. Polls QueueNotify-driven completion
 * via the used ring; no interrupts in M2.1. Pretty-prints the first
 * 16 bytes of the result so we can prove the round trip end-to-end.
 *
 * M2.2 will: hook Drunix's blkdev registry, add write support,
 * IRQ-driven completion via SPI and the GICv3 path from M1, and
 * scatter/gather across multiple sectors. M2.3 mounts the resulting
 * /dev/sda1 as the Drunix root filesystem.
 */

#include "../platform.h"
#include "irq.h"
#include "virtio_blk.h"
#include "virtio_mmio.h"
#include "virtio_queue.h"
#include "kprintf.h"
#include <stdint.h>

#define VIRTIO_BLK_T_IN     0u
#define VIRTIO_BLK_T_OUT    1u

#define VIRTIO_BLK_S_OK     0u
#define VIRTIO_BLK_S_IOERR  1u
#define VIRTIO_BLK_S_UNSUPP 2u

/* virtio-blk config space (Virtio 1.0 §5.2.4). */
#define VIRTIO_BLK_CFG_CAPACITY_LO  0x000u
#define VIRTIO_BLK_CFG_CAPACITY_HI  0x004u

#define POLL_TIMEOUT_ITERS 0x10000000u

/* QEMU virt machine maps virtio-mmio slot N to SPI (16 + N), per
 * hw/arm/virt.c's VIRT_MMIO_IRQ + slot index. */
#define VIRTIO_MMIO_SPI_BASE      16u
#define VIRTIO_MMIO_BASE_ADDR     0x0A000000UL
#define VIRTIO_MMIO_STRIDE_BYTES  0x200u

struct virtio_blk_req_hdr {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
} __attribute__((packed));

/*
 * Static, page-aligned backing for the virtqueue and the I/O buffers.
 * MMU is off in M2.1, so guest physical == kernel virtual; the queue's
 * `addr` fields can take the address of these statics directly.
 */
static __attribute__((aligned(4096))) uint8_t g_queue_pages[8192];
static __attribute__((aligned(64))) struct virtio_blk_req_hdr g_req_hdr;
static __attribute__((aligned(512))) uint8_t g_data_buf[512];
static __attribute__((aligned(64))) volatile uint8_t g_status;

static virtq_t g_queue;
static uintptr_t g_dev_base;
static volatile uint32_t g_completion_pending;

static void virtio_blk_irq_handler(void)
{
	uint32_t isr = *(volatile uint32_t *)(g_dev_base +
	                                       VIRTIO_MMIO_INTERRUPT_STATUS);

	/* InterruptStatus bit 0 = used buffer notification, bit 1 = config
	 * change. Ack whichever bits we saw so the device deasserts the
	 * line; the actual completion drain happens after WFI returns. */
	*(volatile uint32_t *)(g_dev_base + VIRTIO_MMIO_INTERRUPT_ACK) = isr;
	g_completion_pending = 1u;
}

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

static void hex_dump_16(const uint8_t *buf)
{
	char line[64];
	char *p = line;
	static const char hex[] = "0123456789ABCDEF";

	for (int i = 0; i < 16; i++) {
		*p++ = hex[(buf[i] >> 4) & 0xF];
		*p++ = hex[buf[i] & 0xF];
		*p++ = (i == 15) ? '\n' : ' ';
	}
	*p = '\0';
	platform_uart_puts(line);
}

int virtio_blk_smoke(void)
{
	uintptr_t base;
	uint32_t version;
	uint32_t cap_lo;
	uint32_t cap_hi;
	uint32_t qmax;
	uint16_t head;
	uint16_t hdr_idx;
	uint16_t data_idx;
	uint16_t status_idx;
	uint16_t completed;
	uint32_t completed_len = 0;
	uint32_t poll;
	char line[96];

	if (!virtio_mmio_find(VIRTIO_DEV_ID_BLOCK, &base, &version)) {
		platform_uart_puts("virtio-blk: no device found on bus\n");
		return -1;
	}

	if (version != 1u) {
		platform_uart_puts(
		    "virtio-blk: M2.1 only supports legacy (v1) transport\n");
		return -1;
	}

	cap_lo = mmio_read32(base + VIRTIO_MMIO_CONFIG +
	                     VIRTIO_BLK_CFG_CAPACITY_LO);
	cap_hi = mmio_read32(base + VIRTIO_MMIO_CONFIG +
	                     VIRTIO_BLK_CFG_CAPACITY_HI);
	k_snprintf(line,
	           sizeof(line),
	           "virtio-blk: device @ 0x%X, capacity=%u sectors (hi=0x%X)\n",
	           (unsigned int)base,
	           (unsigned int)cap_lo,
	           (unsigned int)cap_hi);
	platform_uart_puts(line);

	/* Per Virtio 1.0 §3.1.1: reset, ack, driver, feature negotiation,
	 * features-ok, queue setup, driver-ok. M2.1 negotiates no
	 * features beyond the base virtio-blk read/write ops. */
	mmio_write32(base + VIRTIO_MMIO_STATUS, 0u);
	status_or(base, VIRTIO_STATUS_ACKNOWLEDGE);
	status_or(base, VIRTIO_STATUS_DRIVER);
	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0u);
	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES, 0u);
	status_or(base, VIRTIO_STATUS_FEATURES_OK);

	/* Legacy queue setup. Page size = the unit QEMU multiplies the
	 * QueuePFN by to get the physical address (Virtio 1.0 §4.2.4.1). */
	mmio_write32(base + VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096u);
	mmio_write32(base + VIRTIO_MMIO_QUEUE_SEL, 0u);

	qmax = mmio_read32(base + VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (qmax < VIRTQ_SIZE) {
		platform_uart_puts(
		    "virtio-blk: device QueueNumMax below VIRTQ_SIZE\n");
		return -1;
	}
	mmio_write32(base + VIRTIO_MMIO_QUEUE_NUM, VIRTQ_SIZE);
	mmio_write32(base + VIRTIO_MMIO_QUEUE_ALIGN, 4096u);

	if (virtq_init(&g_queue, g_queue_pages, sizeof(g_queue_pages)) != 0) {
		platform_uart_puts("virtio-blk: virtq_init failed\n");
		return -1;
	}

	/* QueuePFN is in 4 KiB units. */
	mmio_write32(base + VIRTIO_MMIO_QUEUE_PFN,
	             (uint32_t)(g_queue.base_phys >> 12));

	/* Route this device's SPI through the GICv3 to our handler.
	 * QEMU virt: slot N → SPI (16 + N); slot index = (base - 0xA000000)
	 * / 0x200. Register and enable before driver-OK so the device
	 * cannot fire an IRQ that goes unhandled. */
	g_dev_base = base;
	{
		uint32_t slot = (uint32_t)((base - VIRTIO_MMIO_BASE_ADDR) /
		                            VIRTIO_MMIO_STRIDE_BYTES);
		uint32_t spi = VIRTIO_MMIO_SPI_BASE + slot;

		if (virt_irq_register_spi(spi, virtio_blk_irq_handler) != 0) {
			platform_uart_puts(
			    "virtio-blk: SPI handler registration failed\n");
			return -1;
		}
		virt_irq_enable_spi(spi, 0xA0u);
	}

	status_or(base, VIRTIO_STATUS_DRIVER_OK);

	/* Build the read request: header (RO), data buffer (WR), status
	 * byte (WR). */
	g_req_hdr.type = VIRTIO_BLK_T_IN;
	g_req_hdr.reserved = 0u;
	g_req_hdr.sector = 0u;
	g_status = 0xFFu; /* sentinel; device writes 0/1/2 */

	head = virtq_alloc_chain(&g_queue, 3u);
	if (head == 0xFFFFu) {
		platform_uart_puts("virtio-blk: virtq alloc_chain failed\n");
		return -1;
	}
	hdr_idx = head;
	data_idx = g_queue.desc[hdr_idx].next;
	status_idx = g_queue.desc[data_idx].next;

	g_queue.desc[hdr_idx].addr = (uint64_t)(uintptr_t)&g_req_hdr;
	g_queue.desc[hdr_idx].len = sizeof(g_req_hdr);
	g_queue.desc[hdr_idx].flags = VIRTQ_DESC_F_NEXT;
	g_queue.desc[hdr_idx].next = data_idx;

	g_queue.desc[data_idx].addr = (uint64_t)(uintptr_t)g_data_buf;
	g_queue.desc[data_idx].len = sizeof(g_data_buf);
	g_queue.desc[data_idx].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
	g_queue.desc[data_idx].next = status_idx;

	g_queue.desc[status_idx].addr = (uint64_t)(uintptr_t)&g_status;
	g_queue.desc[status_idx].len = 1u;
	g_queue.desc[status_idx].flags = VIRTQ_DESC_F_WRITE;
	g_queue.desc[status_idx].next = 0u;

	virtq_submit(&g_queue, head);
	g_completion_pending = 0u;
	mmio_write32(base + VIRTIO_MMIO_QUEUE_NOTIFY, 0u);

	/* Wait for the device to fire the SPI we registered above. The
	 * IRQ handler sets g_completion_pending and acks the device. We
	 * loop on WFI rather than busy-spin so the CPU sleeps between
	 * events. A bounded fallback poll backstops the unlikely case
	 * where the IRQ never arrives. */
	for (poll = 0; poll < POLL_TIMEOUT_ITERS; poll++) {
		__asm__ volatile("wfi");
		if (g_completion_pending)
			break;
	}

	if (!g_completion_pending) {
		platform_uart_puts("virtio-blk: read timed out (no IRQ)\n");
		return -1;
	}

	completed = virtq_drain_one(&g_queue, &completed_len);
	if (completed == 0xFFFFu) {
		platform_uart_puts(
		    "virtio-blk: IRQ fired but used ring empty\n");
		return -1;
	}

	if (completed != head) {
		platform_uart_puts(
		    "virtio-blk: completion id mismatch\n");
		return -1;
	}

	if (g_status != VIRTIO_BLK_S_OK) {
		k_snprintf(line, sizeof(line),
		           "virtio-blk: device returned status %u\n",
		           (unsigned int)g_status);
		platform_uart_puts(line);
		return -1;
	}

	platform_uart_puts("virtio-blk: read sector 0 OK; first 16 bytes:\n  ");
	hex_dump_16(g_data_buf);
	virtq_free_chain(&g_queue, head);
	return 0;
}
