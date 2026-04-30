/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_blk.c - virtio-blk front-end for the QEMU virt platform.
 *
 * Phase 1 of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
 * M2.1 brought up the device and read sector 0 once, polling.
 * M2.2 switched completion to an IRQ via a GICv3 SPI.
 * M2.3 splits the driver into init + multi-shot read/write, allocates
 * its DMA buffers from the virt DMA pool (dma.h), and registers a
 * Drunix blkdev so M2.4's mount path can find /dev/sda.
 *
 * Concurrency: single-CPU, MMU off. Read and write block on the
 * device IRQ, so reentrant access from a single thread of control is
 * fine. SMP and concurrent submissions land later; the static state
 * below is documented so the eventual upgrade is mechanical.
 */

#include "../platform.h"
#include "../../dma.h"
#include "blkdev.h"
#include "dma.h"
#include "irq.h"
#include "kprintf.h"
#include "virtio_blk.h"
#include "virtio_mmio.h"
#include "virtio_queue.h"
#include <stdint.h>

#define VIRTIO_BLK_T_IN 0u
#define VIRTIO_BLK_T_OUT 1u

#define VIRTIO_BLK_S_OK 0u
#define VIRTIO_BLK_S_IOERR 1u
#define VIRTIO_BLK_S_UNSUPP 2u

/* virtio-blk config space (Virtio 1.0 §5.2.4). */
#define VIRTIO_BLK_CFG_CAPACITY_LO 0x000u
#define VIRTIO_BLK_CFG_CAPACITY_HI 0x004u

#define VIRTIO_BLK_SECTOR_BYTES 512u

/* Bounded fallback so a wedged device shows up in the boot log instead
 * of hanging forever. WFI returns on any unmasked IRQ, so on a healthy
 * device the loop exits on the first iteration. 0x100000 (~1M) is plenty
 * of slack for a single I/O; M2.4 should switch to a CNTP_EL0 wall-
 * clock deadline once the timer subsystem is the source of truth. */
#define POLL_TIMEOUT_ITERS 0x100000u

/* QEMU virt machine maps virtio-mmio slot N to SPI (16 + N), per
 * hw/arm/virt.c's VIRT_MMIO_IRQ + slot index. */
#define VIRTIO_MMIO_SPI_BASE 16u
#define VIRTIO_MMIO_BASE_ADDR 0x0A000000UL
#define VIRTIO_MMIO_STRIDE_BYTES 0x200u

/* Virtqueue backing region: 8 KiB rounded by virtio_queue.c. Two pages.
 * Request header + data scratch each occupy one page so virt_dma_alloc
 * pointer arithmetic stays page-granular. */
#define VIRTIO_BLK_QUEUE_PAGES 2u
#define VIRTIO_BLK_REQ_HDR_PAGES 1u
#define VIRTIO_BLK_DATA_PAGES 1u

/* Naturally aligned: 4 + 4 + 8 = 16 bytes with no padding. The
 * `packed` attribute is omitted deliberately — it would be a no-op
 * for the current layout and would suppress alignment diagnostics
 * if a member type were ever changed in a way that introduces
 * padding. */
struct virtio_blk_req_hdr {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};

static int g_initialized;
static uintptr_t g_dev_base;
static uint32_t g_capacity_sectors;
static virtq_t g_queue;
static struct virtio_blk_req_hdr *g_req_hdr;
static uint8_t *g_data_buf;
/* g_status points into the data-buffer page (last byte). The device
 * writes it via DMA; readers pair the read with arm64_dma_rmb() rather
 * than relying on `volatile`. */
static uint8_t *g_status;

/* Single-CPU + IRQ context only on M2.x: the IRQ handler's write to
 * g_completion_pending is published to the main path because the IRQ
 * return is a context-synchronization event. Promote to atomic_uint
 * with explicit acquire/release ordering when SMP lands. */
static volatile uint32_t g_completion_pending;

static void virtio_blk_irq_handler(void)
{
	uint32_t isr =
	    *(volatile uint32_t *)(g_dev_base + VIRTIO_MMIO_INTERRUPT_STATUS);

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

static int virtio_blk_alloc_buffers(void *queue_backing)
{
	g_req_hdr = virt_dma_alloc(VIRTIO_BLK_REQ_HDR_PAGES);
	if (!g_req_hdr) {
		platform_uart_puts("virtio-blk: req hdr DMA alloc failed\n");
		return -1;
	}

	g_data_buf = virt_dma_alloc(VIRTIO_BLK_DATA_PAGES);
	if (!g_data_buf) {
		platform_uart_puts("virtio-blk: data buf DMA alloc failed\n");
		virt_dma_free(g_req_hdr, VIRTIO_BLK_REQ_HDR_PAGES);
		g_req_hdr = 0;
		return -1;
	}

	/* Place the status byte at the end of the data buffer page; the
	 * device only writes 1 byte and the data buffer is already DMA-
	 * safe. Saves a page versus a dedicated alloc. */
	g_status = g_data_buf + VIRT_DMA_PAGE_SIZE - 1u;

	if (virtq_init(&g_queue,
	               queue_backing,
	               VIRTIO_BLK_QUEUE_PAGES * VIRT_DMA_PAGE_SIZE) != 0) {
		platform_uart_puts("virtio-blk: virtq_init failed\n");
		return -1;
	}

	return 0;
}

static void virtio_blk_free_buffers(void *queue_backing)
{
	if (queue_backing)
		virt_dma_free(queue_backing, VIRTIO_BLK_QUEUE_PAGES);
	if (g_data_buf) {
		virt_dma_free(g_data_buf, VIRTIO_BLK_DATA_PAGES);
		g_data_buf = 0;
		g_status = 0;
	}
	if (g_req_hdr) {
		virt_dma_free(g_req_hdr, VIRTIO_BLK_REQ_HDR_PAGES);
		g_req_hdr = 0;
	}
}

static int virtio_blk_perform_io(uint32_t lba,
                                 uint32_t type,
                                 uint8_t *read_dst,
                                 const uint8_t *write_src)
{
	uint16_t head;
	uint16_t hdr_idx;
	uint16_t data_idx;
	uint16_t status_idx;
	uint16_t completed;
	uint32_t completed_len = 0;
	uint32_t poll;
	int rc;
	char line[64];

	if (!g_initialized)
		return -1;

	g_req_hdr->type = type;
	g_req_hdr->reserved = 0u;
	g_req_hdr->sector = (uint64_t)lba;

	if (type == VIRTIO_BLK_T_OUT) {
		for (uint32_t i = 0; i < VIRTIO_BLK_SECTOR_BYTES; i++)
			g_data_buf[i] = write_src[i];
	}

	*g_status = 0xFFu; /* sentinel; device writes 0/1/2 */

	/* M2.4b: clean the request header and (write path) data buffer to
	 * PoC so the device sees the bytes we just wrote, not the older
	 * cached contents. The sentinel-write of *g_status above is also
	 * cleaned so the device's status update has a deterministic
	 * baseline to overwrite.
	 *
	 * For the read path, also invalidate the data buffer pre-submit
	 * so any dirty cache lines (from a prior CPU write to the same
	 * buffer) cannot evict and clobber the device's DMA write. Mirrors
	 * Linux's dma_map_single(DMA_FROM_DEVICE) discipline. */
	arm64_dma_cache_clean(g_req_hdr, sizeof(*g_req_hdr));
	if (type == VIRTIO_BLK_T_OUT)
		arm64_dma_cache_clean(g_data_buf, VIRTIO_BLK_SECTOR_BYTES);
	else
		arm64_dma_cache_invalidate(g_data_buf, VIRTIO_BLK_SECTOR_BYTES);
	arm64_dma_cache_clean(g_status, 1u);

	head = virtq_alloc_chain(&g_queue, 3u);
	if (head == 0xFFFFu) {
		platform_uart_puts("virtio-blk: virtq alloc_chain failed\n");
		return -1;
	}
	hdr_idx = head;
	data_idx = g_queue.desc[hdr_idx].next;
	status_idx = g_queue.desc[data_idx].next;

	g_queue.desc[hdr_idx].addr = virt_virt_to_phys(g_req_hdr);
	g_queue.desc[hdr_idx].len = sizeof(*g_req_hdr);
	g_queue.desc[hdr_idx].flags = VIRTQ_DESC_F_NEXT;
	g_queue.desc[hdr_idx].next = data_idx;

	g_queue.desc[data_idx].addr = virt_virt_to_phys(g_data_buf);
	g_queue.desc[data_idx].len = VIRTIO_BLK_SECTOR_BYTES;
	g_queue.desc[data_idx].flags =
	    VIRTQ_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0u);
	g_queue.desc[data_idx].next = status_idx;

	g_queue.desc[status_idx].addr = virt_virt_to_phys(g_status);
	g_queue.desc[status_idx].len = 1u;
	g_queue.desc[status_idx].flags = VIRTQ_DESC_F_WRITE;
	g_queue.desc[status_idx].next = 0u;

	g_completion_pending = 0u;
	virtq_submit(&g_queue, head);
	mmio_write32(g_dev_base + VIRTIO_MMIO_QUEUE_NOTIFY, 0u);

	/*
	 * Wait for the device IRQ. WFI returns when any IRQ is pending in
	 * the GIC regardless of DAIF mask state, but the IRQ handler that
	 * sets g_completion_pending only runs when DAIF.I is clear.
	 *
	 * Syscall and exception entry on arm64 enters EL1 with DAIF.I set
	 * (HW-imposed). M2.5b's desktop launch is the first user→user exec
	 * to call vfs_read from syscall context — older boot-time loads
	 * worked because the boot path runs with IRQs already unmasked
	 * (arch_interrupts_enable → daifclr in start_kernel). Without this
	 * temporary unmask, virtio_blk_perform_io spins on WFI returns
	 * that wake but never advance, and times out after 1M iters.
	 *
	 * Save and restore DAIF so we don't accidentally leave IRQs
	 * unmasked in a caller that depended on them being masked.
	 */
	uint64_t saved_daif;
	__asm__ volatile("mrs %0, daif" : "=r"(saved_daif));
	__asm__ volatile("msr daifclr, #2");

	for (poll = 0; poll < POLL_TIMEOUT_ITERS; poll++) {
		__asm__ volatile("wfi");
		if (g_completion_pending)
			break;
	}

	__asm__ volatile("msr daif, %0" : : "r"(saved_daif) : "memory");

	rc = -1;
	if (!g_completion_pending) {
		platform_uart_puts("virtio-blk: I/O timed out (no IRQ)\n");
		goto out;
	}

	completed = virtq_drain_one(&g_queue, &completed_len);
	if (completed == 0xFFFFu) {
		platform_uart_puts("virtio-blk: IRQ fired but used ring empty\n");
		goto out;
	}
	if (completed != head) {
		platform_uart_puts("virtio-blk: completion id mismatch\n");
		goto out;
	}

	/* M2.4b: invalidate the status byte and (read path) data buffer
	 * before reading them. The device wrote these via DMA, bypassing
	 * the CPU cache; without invalidate the CPU could return the
	 * sentinel/old data we wrote pre-submit. */
	arm64_dma_cache_invalidate(g_status, 1u);
	if (*g_status != VIRTIO_BLK_S_OK) {
		k_snprintf(line,
		           sizeof(line),
		           "virtio-blk: device returned status %u\n",
		           (unsigned int)*g_status);
		platform_uart_puts(line);
		goto out;
	}

	if (type == VIRTIO_BLK_T_IN && read_dst) {
		arm64_dma_rmb();
		arm64_dma_cache_invalidate(g_data_buf, VIRTIO_BLK_SECTOR_BYTES);
		for (uint32_t i = 0; i < VIRTIO_BLK_SECTOR_BYTES; i++)
			read_dst[i] = g_data_buf[i];
	}

	rc = 0;

out:
	virtq_free_chain(&g_queue, head);
	return rc;
}

int virtio_blk_init(void)
{
	uintptr_t base;
	uint32_t version;
	uint32_t cap_lo;
	uint32_t cap_hi;
	uint32_t qmax;
	void *queue_backing;
	uint32_t slot;
	uint32_t spi;
	char line[96];

	if (g_initialized)
		return 0;

	if (!virtio_mmio_find(VIRTIO_DEV_ID_BLOCK, &base, &version)) {
		platform_uart_puts("virtio-blk: no device found on bus\n");
		return -1;
	}

	if (version != 1u) {
		platform_uart_puts(
		    "virtio-blk: M2.1 only supports legacy (v1) transport\n");
		return -1;
	}

	g_dev_base = base;

	cap_lo =
	    mmio_read32(base + VIRTIO_MMIO_CONFIG + VIRTIO_BLK_CFG_CAPACITY_LO);
	cap_hi =
	    mmio_read32(base + VIRTIO_MMIO_CONFIG + VIRTIO_BLK_CFG_CAPACITY_HI);
	if (cap_lo == 0u && cap_hi == 0u) {
		platform_uart_puts("virtio-blk: device reports zero capacity\n");
		return -1;
	}
	g_capacity_sectors = cap_lo;
	k_snprintf(line,
	           sizeof(line),
	           "virtio-blk: device @ 0x%X, capacity=%u sectors (hi=0x%X)\n",
	           (unsigned int)base,
	           (unsigned int)cap_lo,
	           (unsigned int)cap_hi);
	platform_uart_puts(line);

	queue_backing = virt_dma_alloc(VIRTIO_BLK_QUEUE_PAGES);
	if (!queue_backing) {
		platform_uart_puts("virtio-blk: queue backing DMA alloc failed\n");
		return -1;
	}

	if (virtio_blk_alloc_buffers(queue_backing) != 0) {
		virt_dma_free(queue_backing, VIRTIO_BLK_QUEUE_PAGES);
		return -1;
	}

	/* Per Virtio 1.0 §3.1.1: reset, ack, driver, feature negotiation,
	 * features-ok, queue setup, driver-ok. M2.3 negotiates no
	 * features beyond the base virtio-blk read/write ops. */
	mmio_write32(base + VIRTIO_MMIO_STATUS, 0u);
	status_or(base, VIRTIO_STATUS_ACKNOWLEDGE);
	status_or(base, VIRTIO_STATUS_DRIVER);
	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0u);
	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES, 0u);
	status_or(base, VIRTIO_STATUS_FEATURES_OK);

	if ((mmio_read32(base + VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) ==
	    0u) {
		platform_uart_puts("virtio-blk: device rejected feature negotiation\n");
		status_or(base, VIRTIO_STATUS_FAILED);
		virtio_blk_free_buffers(queue_backing);
		return -1;
	}

	mmio_write32(base + VIRTIO_MMIO_GUEST_PAGE_SIZE, VIRT_DMA_PAGE_SIZE);
	mmio_write32(base + VIRTIO_MMIO_QUEUE_SEL, 0u);

	qmax = mmio_read32(base + VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (qmax < VIRTQ_SIZE) {
		platform_uart_puts("virtio-blk: device QueueNumMax below VIRTQ_SIZE\n");
		virtio_blk_free_buffers(queue_backing);
		return -1;
	}
	mmio_write32(base + VIRTIO_MMIO_QUEUE_NUM, VIRTQ_SIZE);
	mmio_write32(base + VIRTIO_MMIO_QUEUE_ALIGN, VIRT_DMA_PAGE_SIZE);

	/* QueuePFN is in 4 KiB units. */
	mmio_write32(base + VIRTIO_MMIO_QUEUE_PFN,
	             (uint32_t)(g_queue.base_phys >> 12));

	slot =
	    (uint32_t)((base - VIRTIO_MMIO_BASE_ADDR) / VIRTIO_MMIO_STRIDE_BYTES);
	spi = VIRTIO_MMIO_SPI_BASE + slot;

	if (virt_irq_register_spi(spi, virtio_blk_irq_handler) != 0) {
		platform_uart_puts("virtio-blk: SPI handler registration failed\n");
		virtio_blk_free_buffers(queue_backing);
		return -1;
	}
	virt_irq_enable_spi(spi, 0xA0u);

	status_or(base, VIRTIO_STATUS_DRIVER_OK);

	g_initialized = 1;
	return 0;
}

uint32_t virtio_blk_capacity_sectors(void)
{
	return g_capacity_sectors;
}

int virtio_blk_read_sector(uint32_t lba, uint8_t *buf)
{
	if (!buf)
		return -1;
	return virtio_blk_perform_io(lba, VIRTIO_BLK_T_IN, buf, 0);
}

int virtio_blk_write_sector(uint32_t lba, const uint8_t *buf)
{
	if (!buf)
		return -1;
	return virtio_blk_perform_io(lba, VIRTIO_BLK_T_OUT, 0, buf);
}

int virtio_blk_smoke(void)
{
	uint8_t scratch[VIRTIO_BLK_SECTOR_BYTES];

	if (virtio_blk_init() != 0)
		return -1;

	if (virtio_blk_read_sector(0u, scratch) != 0)
		return -1;

	platform_uart_puts("virtio-blk: read sector 0 OK; first 16 bytes:\n  ");
	hex_dump_16(scratch);
	return 0;
}

static const blkdev_ops_t virtio_blk_drunix_ops = {
    .read_sector = virtio_blk_read_sector,
    .write_sector = virtio_blk_write_sector,
};

int platform_block_register(void)
{
	if (virtio_blk_init() != 0) {
		platform_uart_puts("ARM64 virtio-blk init failed\n");
		return -1;
	}

	if (g_capacity_sectors == 0u) {
		platform_uart_puts(
		    "ARM64 virtio-blk: zero capacity; not registering\n");
		return -1;
	}

	if (blkdev_register_disk(
	        "sda", 8u, 0u, g_capacity_sectors, &virtio_blk_drunix_ops) < 0)
		return -1;

	platform_uart_puts(
	    "ARM64 virtio-blk disk registered (mount path gated to M2.4)\n");
	return 0;
}
