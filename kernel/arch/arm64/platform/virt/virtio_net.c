/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_net.c - virtio-net driver for the arm64 virt machine.
 *
 * M4 commit 1: probe-only enumeration (read-only scan).
 * M4 commit 2: feature negotiation + MAC discovery, legacy transport
 *              (version 1) only.
 * M4 commit 3: DMA-pool ring allocation and per-queue setup. Both
 *              receiveq (queue 0) and transmitq (queue 1) are
 *              configured at the MMIO level, with packet buffer pools
 *              allocated from the virt DMA pool. DRIVER_OK is NOT yet
 *              set — commit 4 primes the receiveq with writable
 *              buffers and asserts DRIVER_OK.
 *
 * Commit 3 deliberately does not:
 *   - Set DRIVER_OK. Without RX buffers posted, the device would
 *     silently drop incoming frames. Commit 4 owns the prime+kick.
 *   - Wire IRQs. Commit 4 owns SPI handler registration.
 *   - Touch transmit submission. Commit 5 owns TX.
 *
 * DMA discipline (per docs/contributing/aarch64-dma.md):
 *   - All descriptor backing AND all packet buffers come from
 *     virt_dma_alloc. virt_virt_to_phys() is DMA-pool-only and
 *     silently returns 0 on non-pool pointers; commit 3 asserts each
 *     buffer's translation is non-zero so a regression to stack /
 *     static / kheap memory shows up as a hard failure rather than
 *     as silent device writes to physical zero.
 *   - virtq_init() handles the cache_clean for the descriptor table
 *     and avail / used rings; this driver only needs additional
 *     cache_clean on packet buffers when commit 4 hands them to the
 *     device.
 */

#include "../platform.h"
#include "../../dma.h"
#include "dma.h"
#include "irq.h"
#include "virtio_mmio.h"
#include "virtio_net.h"
#include "virtio_queue.h"
#include "kprintf.h"
#include "kstring.h"
#include <stdint.h>

/* QEMU virt machine maps virtio-mmio slot N at base 0x0A000000 + N*0x200,
 * for N in 0..31. */
#define VIRTIO_NET_MMIO_BASE_ADDR       0x0A000000UL
#define VIRTIO_NET_MMIO_STRIDE_BYTES    0x200UL
#define VIRTIO_NET_MMIO_MAX_SLOTS       32u
#define VIRTIO_NET_MMIO_MAGIC           0x74726976u
#define VIRTIO_NET_MMIO_VERSION_LEGACY  1u

/* Virtio 1.x §5.1.3 — virtio-net feature bits. */
#define VIRTIO_NET_F_MAC                5u   /* page 0 */

/* virtio-net config space layout (Virtio 1.x §5.1.4). */
#define VIRTIO_NET_CFG_MAC_OFFSET       0u
#define VIRTIO_NET_MAC_BYTES            6u

/* Virtio 1.x §5.1.2 — virtqueue indices. */
#define VIRTIO_NET_QUEUE_RX             0u
#define VIRTIO_NET_QUEUE_TX             1u

/* Per-direction packet buffer count and per-buffer size. The buffer
 * size accommodates a 10-byte virtio_net_hdr plus a full Ethernet MTU
 * frame (1514 bytes); rounded up to 2048 to keep buffer addresses
 * aligned and to reserve headroom for future merge-rxbuf or virtio
 * 1.0 12-byte headers. 16 buffers × 2048 bytes = 32 KiB per direction
 * = 8 pages from the virt DMA pool. */
#define VIRTIO_NET_RING_BUFFERS         16u  /* matches VIRTQ_SIZE */
#define VIRTIO_NET_BUFFER_BYTES         2048u
#define VIRTIO_NET_POOL_PAGES                                                 \
	((VIRTIO_NET_RING_BUFFERS * VIRTIO_NET_BUFFER_BYTES +                     \
	  VIRT_DMA_PAGE_SIZE - 1u) / VIRT_DMA_PAGE_SIZE)

/* Queue backing must satisfy virtq_backing_size() (currently 8 KiB);
 * 2 pages cover that with 4 KiB headroom for any future growth. */
#define VIRTIO_NET_QUEUE_BACKING_PAGES  2u

/* Maximum Ethernet frame size (excluding the 10-byte virtio_net_hdr
 * the device prepends). Standard Ethernet II 14-byte header + 1500-
 * byte payload = 1514. The driver-local RX ring stores up to this
 * many bytes per slot. */
#define VIRTIO_NET_HDR_BYTES            10u
#define VIRTIO_NET_MAX_FRAME            1514u

/* Driver-local RX ring: a small SPSC ring that holds Ethernet frames
 * pulled off the virtqueue's used ring by the IRQ handler, awaiting
 * pickup by the consumer (commit 6's /dev/net0 chardev). 8 slots
 * keeps memory bounded; a future commit can size it from the device
 * MTU * QoS budget. */
#define VIRTIO_NET_DRIVER_RX_SLOTS      8u

/* QEMU virt machine maps virtio-mmio slot N -> SPI 16+N (Virtio
 * 1.0 / qemu/hw/arm/virt.c). Reused locally so this file stays
 * self-contained until later commits move the constant into a
 * shared header. */
#define VIRTIO_MMIO_SPI_BASE            16u

/* Bits of VIRTIO_MMIO_INTERRUPT_STATUS. */
#define VIRTIO_MMIO_ISR_USED_RING       (1u << 0)
#define VIRTIO_MMIO_ISR_CONFIG_CHANGE   (1u << 1)

_Static_assert(VIRTIO_NET_RING_BUFFERS == VIRTQ_SIZE,
               "virtio-net buffer count must match VIRTQ_SIZE");

/*
 * Driver-local RX ring entry. Holds one Ethernet frame's payload (no
 * virtio_net_hdr) along with the byte count copied. The IRQ handler
 * publishes here; consumer (commit 6) reads from here.
 */
typedef struct {
	uint8_t bytes[VIRTIO_NET_MAX_FRAME];
	uint32_t len;
} virtio_net_driver_rx_slot_t;

static struct {
	int found;
	int features_ok;
	int rings_ready;
	int driver_ok;
	uintptr_t base;
	uint32_t slot;
	uint32_t version;
	uint8_t mac[VIRTIO_NET_MAC_BYTES];

	/* Receive queue + per-buffer pool. */
	virtq_t rx_queue;
	void *rx_queue_backing;
	void *rx_pool;
	uint8_t *rx_buffers[VIRTIO_NET_RING_BUFFERS];
	uint64_t rx_buffers_phys[VIRTIO_NET_RING_BUFFERS];

	/* Transmit queue + per-buffer pool. */
	virtq_t tx_queue;
	void *tx_queue_backing;
	void *tx_pool;
	uint8_t *tx_buffers[VIRTIO_NET_RING_BUFFERS];
	uint64_t tx_buffers_phys[VIRTIO_NET_RING_BUFFERS];

	/* Driver-local RX ring (SPSC: producer = IRQ, consumer = chardev). */
	virtio_net_driver_rx_slot_t driver_rx[VIRTIO_NET_DRIVER_RX_SLOTS];
	uint16_t driver_rx_head;       /* next write slot */
	uint16_t driver_rx_tail;       /* next read slot */
	uint16_t driver_rx_count;      /* slots currently full */

	/* RX counters. */
	uint32_t rx_packets;
	uint32_t rx_drops_short;
	uint32_t rx_drops_oversize;
	uint32_t rx_drops_ring_full;
} g_state;

/*
 * Opaque RX token. Carries a pointer to the post-header Ethernet
 * frame bytes plus the validated frame byte count. The token is
 * constructed ONLY by virtio_net_rx_invalidate_and_take(), which
 * performs arm64_dma_cache_invalidate() before any field of the
 * underlying buffer is read. The publish path takes this token by
 * value, which makes "invalidate before parse" a property of the
 * function signature rather than a comment in the code.
 *
 * Defined here at file scope (not in the header) so external
 * callers cannot construct a token without going through the
 * invalidate path.
 */
typedef struct {
	const uint8_t *frame;
	uint32_t len;
} virtio_net_rx_token_t;

static uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static void mmio_write32(uintptr_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static uint8_t mmio_read8(uintptr_t addr)
{
	return *(volatile uint8_t *)addr;
}

static void status_or(uintptr_t base, uint32_t bit)
{
	uint32_t v = mmio_read32(base + VIRTIO_MMIO_STATUS);
	mmio_write32(base + VIRTIO_MMIO_STATUS, v | bit);
}

/*
 * Slice a single contiguous DMA pool into VIRTIO_NET_RING_BUFFERS
 * fixed-size buffers. Records each buffer's virtual address and its
 * physical translation. Caller has already verified `pool != NULL`.
 *
 * Returns 0 on success, -1 if any buffer's phys translation is zero
 * — a sentinel that virt_virt_to_phys was called on a non-pool
 * pointer (would silently DMA to physical 0 and overwrite the vector
 * table).
 */
static int virtio_net_slice_pool(void *pool,
                                 uint8_t **out_bufs,
                                 uint64_t *out_phys)
{
	uint8_t *base = (uint8_t *)pool;

	for (uint32_t i = 0; i < VIRTIO_NET_RING_BUFFERS; i++) {
		uint8_t *buf = base + i * VIRTIO_NET_BUFFER_BYTES;
		uint64_t phys = virt_virt_to_phys(buf);

		if (phys == 0u) {
			platform_uart_puts(
			    "virtio-net: packet buffer phys translation is "
			    "zero (pool not from virt_dma_alloc?)\n");
			return -1;
		}
		out_bufs[i] = buf;
		out_phys[i] = phys;
	}
	return 0;
}

/*
 * Configure one virtqueue at the MMIO level (legacy transport).
 * Caller has already allocated `backing` via virt_dma_alloc and
 * initialized the virtq_t with virtq_init(). This function:
 *   1. selects the queue (QUEUE_SEL)
 *   2. checks QUEUE_NUM_MAX >= VIRTQ_SIZE
 *   3. writes QUEUE_NUM, QUEUE_ALIGN, QUEUE_PFN
 *
 * Returns 0 on success, -1 if QUEUE_NUM_MAX is too small. On failure,
 * caller is responsible for marking driver state un-ready and freeing
 * the queue backing.
 */
static int virtio_net_setup_queue(uintptr_t base,
                                  uint32_t queue_index,
                                  const virtq_t *queue)
{
	uint32_t qmax;
	char line[96];

	mmio_write32(base + VIRTIO_MMIO_QUEUE_SEL, queue_index);

	qmax = mmio_read32(base + VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (qmax < VIRTQ_SIZE) {
		k_snprintf(line, sizeof(line),
		           "virtio-net: queue %u QueueNumMax=%u below "
		           "VIRTQ_SIZE=%u\n",
		           (unsigned int)queue_index,
		           (unsigned int)qmax,
		           (unsigned int)VIRTQ_SIZE);
		platform_uart_puts(line);
		return -1;
	}

	mmio_write32(base + VIRTIO_MMIO_QUEUE_NUM, VIRTQ_SIZE);
	mmio_write32(base + VIRTIO_MMIO_QUEUE_ALIGN, VIRT_DMA_PAGE_SIZE);
	mmio_write32(base + VIRTIO_MMIO_QUEUE_PFN,
	             (uint32_t)(queue->base_phys >> 12));
	return 0;
}

/*
 * Refill descriptor `head` with a writable RX buffer (1:1 mapping
 * descriptor i <-> rx_buffers[i]). Marks the descriptor VIRTQ_DESC_F_
 * WRITE so the device fills it on the next packet, and submits it via
 * virtq_submit. The submit path performs the desc cache_clean and
 * dma_wmb required by aarch64-dma.md.
 */
static void virtio_net_rx_refill_one(uint16_t head)
{
	struct virtq_desc *desc;

	if (head >= VIRTIO_NET_RING_BUFFERS)
		return;

	desc = &g_state.rx_queue.desc[head];
	desc->addr = g_state.rx_buffers_phys[head];
	desc->len = VIRTIO_NET_BUFFER_BYTES;
	desc->flags = VIRTQ_DESC_F_WRITE;
	desc->next = 0u;
	virtq_submit(&g_state.rx_queue, head);
}

/*
 * Construct an RX token from a used-ring completion. Performs the
 * cache invalidate FIRST (the structural barrier — see token type
 * comment), then validates length bounds. Returns 1 if the token
 * is valid (caller should publish), 0 if the completion should be
 * dropped (counter has been incremented and caller should refill).
 *
 * This is the only path that constructs a virtio_net_rx_token_t.
 */
static int virtio_net_rx_invalidate_and_take(uint16_t buf_index,
                                             uint32_t used_len,
                                             virtio_net_rx_token_t *out)
{
	uint8_t *raw;

	if (buf_index >= VIRTIO_NET_RING_BUFFERS)
		return 0;

	raw = g_state.rx_buffers[buf_index];

	/* Cache invalidate before reading any byte of the buffer. The
	 * device wrote up to `used_len` bytes via DMA; on M2.4+ MMU+
	 * caches a stale clean line could otherwise mask the device
	 * write. Invalidate covers the whole buffer span the device
	 * may have written, capped at the configured buffer size. */
	if (used_len > VIRTIO_NET_BUFFER_BYTES)
		used_len = VIRTIO_NET_BUFFER_BYTES;
	arm64_dma_cache_invalidate(raw, used_len);

	if (used_len < VIRTIO_NET_HDR_BYTES) {
		g_state.rx_drops_short++;
		return 0;
	}
	if (used_len > VIRTIO_NET_HDR_BYTES + VIRTIO_NET_MAX_FRAME) {
		g_state.rx_drops_oversize++;
		return 0;
	}

	out->frame = raw + VIRTIO_NET_HDR_BYTES;
	out->len = used_len - VIRTIO_NET_HDR_BYTES;
	return 1;
}

/*
 * Publish a validated frame to the driver-local RX ring. Takes the
 * token by value, which is the structural enforcement that the
 * caller went through invalidate_and_take. Increments rx_packets on
 * success or rx_drops_ring_full on consumer back-pressure.
 */
static void virtio_net_publish_frame(const virtio_net_rx_token_t *token)
{
	virtio_net_driver_rx_slot_t *slot;

	if (g_state.driver_rx_count >= VIRTIO_NET_DRIVER_RX_SLOTS) {
		g_state.rx_drops_ring_full++;
		return;
	}

	slot = &g_state.driver_rx[g_state.driver_rx_head];
	k_memcpy(slot->bytes, token->frame, token->len);
	slot->len = token->len;

	g_state.driver_rx_head =
	    (uint16_t)((g_state.driver_rx_head + 1u) %
	               VIRTIO_NET_DRIVER_RX_SLOTS);
	g_state.driver_rx_count =
	    (uint16_t)(g_state.driver_rx_count + 1u);
	g_state.rx_packets++;
}

/*
 * IRQ handler. Acks both used-ring (bit 0) and config-change (bit 1)
 * by writing the snapshot back to INTERRUPT_ACK; config-change is
 * acked-and-ignored for now (link-status tracking lands in a later
 * commit). Drains the receiveq used ring, validates and publishes
 * each frame, refills the descriptor regardless of validation
 * outcome, and notifies queue 0 to keep the device fed.
 *
 * Race avoidance: the ISR is acked once at entry, then the drain loop
 * runs until the used ring is empty. After the inner loop exits the
 * handler re-checks used->idx (via virtq_drain_one) and re-loops if
 * the device published more completions between drain-empty and now.
 * Without this re-check a publish that lands after ACK but before the
 * outer return would not raise a fresh interrupt edge and the
 * completion would be stuck until the next traffic-driven IRQ.
 */
static void virtio_net_irq_handler(void)
{
	uintptr_t base = g_state.base;
	uint32_t isr;

	if (!g_state.driver_ok)
		return;

	isr = mmio_read32(base + VIRTIO_MMIO_INTERRUPT_STATUS);
	mmio_write32(base + VIRTIO_MMIO_INTERRUPT_ACK, isr);

	if ((isr & VIRTIO_MMIO_ISR_USED_RING) == 0u)
		return;

	/* Outer loop closes the ack-then-drain race: after the inner
	 * drain returns "empty" we re-poll once. If the device published
	 * a completion between the inner exit and here, the next
	 * virtq_drain_one returns it and we loop back. Bounded by
	 * VIRTQ_SIZE iterations as a paranoia stop. */
	for (uint32_t guard = 0; guard <= VIRTQ_SIZE; guard++) {
		int drained_any = 0;

		for (;;) {
			uint32_t used_len = 0;
			uint16_t head = virtq_drain_one(&g_state.rx_queue,
			                                &used_len);
			virtio_net_rx_token_t token;

			if (head == 0xFFFFu)
				break;
			drained_any = 1;

			if (virtio_net_rx_invalidate_and_take(head, used_len,
			                                      &token))
				virtio_net_publish_frame(&token);
			/* else: drop counters already incremented */

			virtio_net_rx_refill_one(head);
		}

		if (!drained_any)
			break;
	}

	mmio_write32(base + VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_NET_QUEUE_RX);
}

/*
 * Pre-fill the receiveq with all VIRTIO_NET_RING_BUFFERS writable
 * buffers BEFORE DRIVER_OK is asserted. Each buffer is invalidated
 * before being handed to the device, ensuring no stale dirty cache
 * line could be flushed over a device write later. Returns 0 on
 * success.
 */
static int virtio_net_rx_prime(void)
{
	for (uint32_t i = 0; i < VIRTIO_NET_RING_BUFFERS; i++) {
		uint16_t head = virtq_alloc_chain(&g_state.rx_queue, 1u);

		if (head == 0xFFFFu) {
			platform_uart_puts(
			    "virtio-net: failed to seed receiveq descriptors\n");
			return -1;
		}

		/* Pre-invalidate the buffer before handing it to the
		 * device, so a stale dirty cache line cannot evict over
		 * the device's write later. */
		arm64_dma_cache_invalidate(g_state.rx_buffers[head],
		                           VIRTIO_NET_BUFFER_BYTES);
		virtio_net_rx_refill_one(head);
	}
	return 0;
}

/*
 * Free any DMA pool allocations recorded in g_state and zero the
 * slots so a re-init runs from a clean state. Safe to call on any
 * partially-initialized state — only frees the slots that have
 * been populated.
 */
static void virtio_net_free_dma_resources(void)
{
	if (g_state.tx_pool) {
		virt_dma_free(g_state.tx_pool, VIRTIO_NET_POOL_PAGES);
		g_state.tx_pool = 0;
	}
	if (g_state.rx_pool) {
		virt_dma_free(g_state.rx_pool, VIRTIO_NET_POOL_PAGES);
		g_state.rx_pool = 0;
	}
	if (g_state.tx_queue_backing) {
		virt_dma_free(g_state.tx_queue_backing,
		              VIRTIO_NET_QUEUE_BACKING_PAGES);
		g_state.tx_queue_backing = 0;
	}
	if (g_state.rx_queue_backing) {
		virt_dma_free(g_state.rx_queue_backing,
		              VIRTIO_NET_QUEUE_BACKING_PAGES);
		g_state.rx_queue_backing = 0;
	}
	k_memset(g_state.rx_buffers, 0, sizeof(g_state.rx_buffers));
	k_memset(g_state.rx_buffers_phys, 0, sizeof(g_state.rx_buffers_phys));
	k_memset(g_state.tx_buffers, 0, sizeof(g_state.tx_buffers));
	k_memset(g_state.tx_buffers_phys, 0, sizeof(g_state.tx_buffers_phys));
}

/*
 * Drive the legacy virtio handshake on `base` and offer F_MAC only.
 * Returns 0 on success, -1 on failure.
 */
static int virtio_net_negotiate(uintptr_t base)
{
	uint32_t status;
	uint32_t device_features;

	mmio_write32(base + VIRTIO_MMIO_STATUS, 0u);
	status_or(base, VIRTIO_STATUS_ACKNOWLEDGE);
	status_or(base, VIRTIO_STATUS_DRIVER);

	mmio_write32(base + VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0u);
	device_features = mmio_read32(base + VIRTIO_MMIO_DEVICE_FEATURES);

	if ((device_features & (1u << VIRTIO_NET_F_MAC)) == 0u) {
		platform_uart_puts(
		    "virtio-net: device does not offer F_MAC; aborting\n");
		status_or(base, VIRTIO_STATUS_FAILED);
		return -1;
	}

	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0u);
	mmio_write32(base + VIRTIO_MMIO_DRIVER_FEATURES,
	             1u << VIRTIO_NET_F_MAC);

	status_or(base, VIRTIO_STATUS_FEATURES_OK);
	status = mmio_read32(base + VIRTIO_MMIO_STATUS);
	if ((status & VIRTIO_STATUS_FEATURES_OK) == 0u) {
		platform_uart_puts(
		    "virtio-net: device rejected feature subset; aborting\n");
		status_or(base, VIRTIO_STATUS_FAILED);
		return -1;
	}

	return 0;
}

/*
 * Allocate queue backings + packet pools, initialize virtqs, and
 * configure both queues at the MMIO level. Returns 0 on success, -1
 * on any failure (with all earlier allocations freed and g_state
 * reset to un-allocated).
 */
static int virtio_net_alloc_and_setup_queues(uintptr_t base)
{
	g_state.rx_queue_backing = virt_dma_alloc(VIRTIO_NET_QUEUE_BACKING_PAGES);
	if (!g_state.rx_queue_backing) {
		platform_uart_puts("virtio-net: rx queue backing alloc failed\n");
		goto fail;
	}

	g_state.tx_queue_backing = virt_dma_alloc(VIRTIO_NET_QUEUE_BACKING_PAGES);
	if (!g_state.tx_queue_backing) {
		platform_uart_puts("virtio-net: tx queue backing alloc failed\n");
		goto fail;
	}

	g_state.rx_pool = virt_dma_alloc(VIRTIO_NET_POOL_PAGES);
	if (!g_state.rx_pool) {
		platform_uart_puts("virtio-net: rx pool alloc failed\n");
		goto fail;
	}

	g_state.tx_pool = virt_dma_alloc(VIRTIO_NET_POOL_PAGES);
	if (!g_state.tx_pool) {
		platform_uart_puts("virtio-net: tx pool alloc failed\n");
		goto fail;
	}

	if (virtq_init(&g_state.rx_queue, g_state.rx_queue_backing,
	               VIRTIO_NET_QUEUE_BACKING_PAGES * VIRT_DMA_PAGE_SIZE) != 0) {
		platform_uart_puts("virtio-net: rx virtq_init failed\n");
		goto fail;
	}

	if (virtq_init(&g_state.tx_queue, g_state.tx_queue_backing,
	               VIRTIO_NET_QUEUE_BACKING_PAGES * VIRT_DMA_PAGE_SIZE) != 0) {
		platform_uart_puts("virtio-net: tx virtq_init failed\n");
		goto fail;
	}

	if (virtio_net_slice_pool(g_state.rx_pool,
	                          g_state.rx_buffers,
	                          g_state.rx_buffers_phys) != 0)
		goto fail;
	if (virtio_net_slice_pool(g_state.tx_pool,
	                          g_state.tx_buffers,
	                          g_state.tx_buffers_phys) != 0)
		goto fail;

	mmio_write32(base + VIRTIO_MMIO_GUEST_PAGE_SIZE, VIRT_DMA_PAGE_SIZE);

	if (virtio_net_setup_queue(base, VIRTIO_NET_QUEUE_RX,
	                           &g_state.rx_queue) != 0)
		goto fail;
	if (virtio_net_setup_queue(base, VIRTIO_NET_QUEUE_TX,
	                           &g_state.tx_queue) != 0)
		goto fail;

	return 0;

fail:
	virtio_net_free_dma_resources();
	return -1;
}

static void virtio_net_read_mac(uintptr_t base, uint8_t *out)
{
	for (uint32_t i = 0; i < VIRTIO_NET_MAC_BYTES; i++)
		out[i] = mmio_read8(base + VIRTIO_MMIO_CONFIG +
		                    VIRTIO_NET_CFG_MAC_OFFSET + i);
}

int arm64_virt_virtio_net_init(void)
{
	char line[96];

	/* Free any prior state so re-init does not double-allocate. */
	virtio_net_free_dma_resources();

	g_state.found = 0;
	g_state.features_ok = 0;
	g_state.rings_ready = 0;
	g_state.driver_ok = 0;
	g_state.base = 0;
	g_state.slot = 0;
	g_state.version = 0;
	g_state.driver_rx_head = 0;
	g_state.driver_rx_tail = 0;
	g_state.driver_rx_count = 0;
	g_state.rx_packets = 0;
	g_state.rx_drops_short = 0;
	g_state.rx_drops_oversize = 0;
	g_state.rx_drops_ring_full = 0;
	k_memset(g_state.mac, 0, sizeof(g_state.mac));

	for (uint32_t slot = 0; slot < VIRTIO_NET_MMIO_MAX_SLOTS; slot++) {
		uintptr_t base = VIRTIO_NET_MMIO_BASE_ADDR +
		                 slot * VIRTIO_NET_MMIO_STRIDE_BYTES;
		uint32_t magic;
		uint32_t device_id;
		uint32_t version;

		magic = mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE);
		if (magic != VIRTIO_NET_MMIO_MAGIC)
			continue;

		device_id = mmio_read32(base + VIRTIO_MMIO_DEVICE_ID);
		if (device_id != VIRTIO_DEV_ID_NET)
			continue;

		version = mmio_read32(base + VIRTIO_MMIO_VERSION);

		if (version != VIRTIO_NET_MMIO_VERSION_LEGACY) {
			k_snprintf(line, sizeof(line),
			           "virtio-net: slot %u rejects non-legacy "
			           "transport (version %u)\n",
			           (unsigned int)slot,
			           (unsigned int)version);
			platform_uart_puts(line);
			continue;
		}

		g_state.found = 1;
		g_state.base = base;
		g_state.slot = slot;
		g_state.version = version;

		k_snprintf(line, sizeof(line),
		           "virtio-net: device at slot %u "
		           "(base 0x%X, version %u)\n",
		           (unsigned int)slot,
		           (unsigned int)base,
		           (unsigned int)version);
		platform_uart_puts(line);

		if (virtio_net_negotiate(base) != 0)
			return -1;

		virtio_net_read_mac(base, g_state.mac);
		g_state.features_ok = 1;

		k_snprintf(line, sizeof(line),
		           "virtio-net: features_ok mac="
		           "%02x:%02x:%02x:%02x:%02x:%02x\n",
		           g_state.mac[0], g_state.mac[1], g_state.mac[2],
		           g_state.mac[3], g_state.mac[4], g_state.mac[5]);
		platform_uart_puts(line);

		if (virtio_net_alloc_and_setup_queues(base) != 0) {
			status_or(base, VIRTIO_STATUS_FAILED);
			return -1;
		}
		g_state.rings_ready = 1;

		k_snprintf(line, sizeof(line),
		           "virtio-net: rings ready "
		           "(rx/tx %u buffers x %u bytes each)\n",
		           (unsigned int)VIRTIO_NET_RING_BUFFERS,
		           (unsigned int)VIRTIO_NET_BUFFER_BYTES);
		platform_uart_puts(line);

		/* Prime the receiveq with writable buffers BEFORE
		 * asserting DRIVER_OK. The device begins receiving as
		 * soon as DRIVER_OK is set; without buffers posted it
		 * would silently drop frames. */
		if (virtio_net_rx_prime() != 0) {
			status_or(base, VIRTIO_STATUS_FAILED);
			return -1;
		}

		/* Wire the GICv3 SPI: virtio-mmio slot N -> SPI 16+N. */
		{
			uint32_t spi = VIRTIO_MMIO_SPI_BASE + slot;
			if (virt_irq_register_spi(spi,
			                          virtio_net_irq_handler) != 0) {
				platform_uart_puts(
				    "virtio-net: SPI handler registration failed\n");
				status_or(base, VIRTIO_STATUS_FAILED);
				return -1;
			}
			virt_irq_enable_spi(spi, 0xA0u);
		}

		status_or(base, VIRTIO_STATUS_DRIVER_OK);
		g_state.driver_ok = 1;

		/* Notify queue 0 so the device picks up the buffers we
		 * just primed. */
		mmio_write32(base + VIRTIO_MMIO_QUEUE_NOTIFY,
		             VIRTIO_NET_QUEUE_RX);

		platform_uart_puts(
		    "virtio-net: driver_ok; receive queue primed\n");
		return 1;
	}

	platform_uart_puts("virtio-net: no device present\n");
	return 0;
}

int arm64_virt_virtio_net_device_found(void)
{
	return g_state.found;
}

uintptr_t arm64_virt_virtio_net_mmio_base(void)
{
	return g_state.base;
}

uint32_t arm64_virt_virtio_net_slot(void)
{
	return g_state.slot;
}

uint32_t arm64_virt_virtio_net_version(void)
{
	return g_state.version;
}

int arm64_virt_virtio_net_features_ok(void)
{
	return g_state.features_ok;
}

const uint8_t *arm64_virt_virtio_net_mac(void)
{
	return g_state.mac;
}

int arm64_virt_virtio_net_rings_ready(void)
{
	return g_state.rings_ready;
}

uint32_t arm64_virt_virtio_net_buffer_count(void)
{
	return VIRTIO_NET_RING_BUFFERS;
}

uint64_t arm64_virt_virtio_net_rx_buffer_phys(uint32_t i)
{
	if (i >= VIRTIO_NET_RING_BUFFERS)
		return 0;
	return g_state.rx_buffers_phys[i];
}

uint64_t arm64_virt_virtio_net_tx_buffer_phys(uint32_t i)
{
	if (i >= VIRTIO_NET_RING_BUFFERS)
		return 0;
	return g_state.tx_buffers_phys[i];
}

uint64_t arm64_virt_virtio_net_rx_queue_phys(void)
{
	return g_state.rings_ready ? g_state.rx_queue.base_phys : 0;
}

uint64_t arm64_virt_virtio_net_tx_queue_phys(void)
{
	return g_state.rings_ready ? g_state.tx_queue.base_phys : 0;
}

int arm64_virt_virtio_net_driver_ok(void)
{
	return g_state.driver_ok;
}

int32_t arm64_virt_virtio_net_rx_dequeue(uint8_t *out, uint32_t max_len)
{
	virtio_net_driver_rx_slot_t *slot;
	uint32_t copy;

	if (!out || g_state.driver_rx_count == 0u)
		return 0;

	slot = &g_state.driver_rx[g_state.driver_rx_tail];
	if (slot->len > max_len)
		return -1;

	copy = slot->len;
	k_memcpy(out, slot->bytes, copy);

	g_state.driver_rx_tail =
	    (uint16_t)((g_state.driver_rx_tail + 1u) %
	               VIRTIO_NET_DRIVER_RX_SLOTS);
	g_state.driver_rx_count =
	    (uint16_t)(g_state.driver_rx_count - 1u);
	return (int32_t)copy;
}

uint32_t arm64_virt_virtio_net_rx_packets(void)
{
	return g_state.rx_packets;
}

uint32_t arm64_virt_virtio_net_rx_drops_short(void)
{
	return g_state.rx_drops_short;
}

uint32_t arm64_virt_virtio_net_rx_drops_oversize(void)
{
	return g_state.rx_drops_oversize;
}

uint32_t arm64_virt_virtio_net_rx_drops_ring_full(void)
{
	return g_state.rx_drops_ring_full;
}
