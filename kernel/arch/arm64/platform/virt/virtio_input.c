/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * virtio_input.c — virtio-input driver for the arm64 virt machine.
 *
 * Phase 1 M2.5b. Mirrors virtio_blk.c's structure (single eventq,
 * IRQ-driven, DMA-allocated buffers) but tracks two device instances
 * — keyboard and mouse — so the desktop can launch on virt with
 * proper input.
 *
 * Concurrency: single-CPU, MMU on. Each device's ring state is
 * touched from kernel-thread context (init + chain refill) and IRQ
 * context (drain + push to inputdev). Device IRQs serialize against
 * the kernel thread because IRQ-handler returns are
 * context-synchronisation events on arm64; readers of the produced
 * events sleep on the inputdev wait queues so the producer side does
 * not require explicit locking. Promote when SMP arrives.
 */

#include "../platform.h"
#include "../../dma.h"
#include "dma.h"
#include "irq.h"
#include "inputdev.h"
#include "virtio_input.h"
#include "virtio_mmio.h"
#include "virtio_queue.h"
#include "kprintf.h"
#include "kstring.h"
#include <stdint.h>

/* virtio-input config-space layout (Virtio 1.x §5.8.4). */
#define VIRTIO_INPUT_CFG_UNSET       0x00u
#define VIRTIO_INPUT_CFG_ID_NAME     0x01u
#define VIRTIO_INPUT_CFG_ID_SERIAL   0x02u
#define VIRTIO_INPUT_CFG_ID_DEVIDS   0x03u
#define VIRTIO_INPUT_CFG_PROP_BITS   0x10u
#define VIRTIO_INPUT_CFG_EV_BITS     0x11u
#define VIRTIO_INPUT_CFG_ABS_INFO    0x12u

/* Config-space register offsets relative to VIRTIO_MMIO_CONFIG. */
#define VIRTIO_INPUT_CFG_SELECT      0x000u
#define VIRTIO_INPUT_CFG_SUBSEL      0x001u
#define VIRTIO_INPUT_CFG_SIZE        0x002u
#define VIRTIO_INPUT_CFG_STRING      0x008u  /* up to 128 bytes */

/* QEMU virt machine maps virtio-mmio slot N to SPI 16 + N. */
#define VIRTIO_MMIO_SPI_BASE         16u
#define VIRTIO_MMIO_BASE_ADDR        0x0A000000UL
#define VIRTIO_MMIO_STRIDE_BYTES     0x200u

/* Each event buffer holds one virtio_input_event = 8 bytes. The driver
 * keeps VIRTQ_SIZE=16 buffers in flight; total pool = 128 bytes ≪ one
 * page, but virt_dma_alloc is page-granular, so we allocate one page
 * per device for the buffer ring. */
#define VIRTIO_INPUT_EVENT_BYTES     8u
#define VIRTIO_INPUT_RING_BUFFERS    VIRTQ_SIZE
#define VIRTIO_INPUT_BUF_PAGES       1u
#define VIRTIO_INPUT_QUEUE_PAGES     2u

#define VIRTIO_INPUT_MAX_DEVICES     2u

/* Match by name prefix; QEMU advertises "QEMU Virtio Keyboard",
 * "QEMU Virtio Mouse", "QEMU Virtio Tablet". */
typedef enum {
	VIO_INPUT_KIND_UNKNOWN = 0,
	VIO_INPUT_KIND_KEYBOARD = 1,
	VIO_INPUT_KIND_MOUSE = 2,
} vio_input_kind_t;

struct virtio_input_event {
	uint16_t type;
	uint16_t code;
	uint32_t value;
} __attribute__((packed));

typedef struct {
	uintptr_t base;
	uint32_t slot;
	vio_input_kind_t kind;
	virtq_t queue;
	void *queue_backing;
	struct virtio_input_event *events; /* ring of VIRTQ_SIZE events */
	uint64_t events_phys;
} vio_input_dev_t;

static vio_input_dev_t g_devs[VIRTIO_INPUT_MAX_DEVICES];
static uint32_t g_dev_count;

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

static void mmio_write8(uintptr_t addr, uint8_t value)
{
	*(volatile uint8_t *)addr = value;
}

static void status_or(uintptr_t base, uint32_t bit)
{
	uint32_t v = mmio_read32(base + VIRTIO_MMIO_STATUS);
	mmio_write32(base + VIRTIO_MMIO_STATUS, v | bit);
}

/*
 * Read the device name string via config space. Returns the byte
 * length written into `out` (NUL-terminated, capped at out_cap-1).
 */
static uint32_t vio_input_read_name(uintptr_t base, char *out, uint32_t out_cap)
{
	uint32_t size;
	uint32_t to_copy;

	if (!out || out_cap == 0)
		return 0;

	mmio_write8(base + VIRTIO_MMIO_CONFIG + VIRTIO_INPUT_CFG_SELECT,
	            VIRTIO_INPUT_CFG_ID_NAME);
	mmio_write8(base + VIRTIO_MMIO_CONFIG + VIRTIO_INPUT_CFG_SUBSEL, 0u);

	size = mmio_read8(base + VIRTIO_MMIO_CONFIG + VIRTIO_INPUT_CFG_SIZE);
	to_copy = (size < out_cap - 1u) ? size : (out_cap - 1u);
	for (uint32_t i = 0; i < to_copy; i++)
		out[i] = (char)mmio_read8(base + VIRTIO_MMIO_CONFIG +
		                          VIRTIO_INPUT_CFG_STRING + i);
	out[to_copy] = '\0';

	/* Reset config-select to UNSET so subsequent register reads do not
	 * keep streaming string bytes. */
	mmio_write8(base + VIRTIO_MMIO_CONFIG + VIRTIO_INPUT_CFG_SELECT,
	            VIRTIO_INPUT_CFG_UNSET);
	return to_copy;
}

static int name_contains(const char *haystack, const char *needle)
{
	uint32_t hlen = k_strlen(haystack);
	uint32_t nlen = k_strlen(needle);

	if (nlen > hlen)
		return 0;
	for (uint32_t i = 0; i + nlen <= hlen; i++) {
		if (k_memcmp(haystack + i, needle, nlen) == 0)
			return 1;
	}
	return 0;
}

static vio_input_kind_t classify_by_name(const char *name)
{
	/* Case-sensitive match against QEMU's advertised strings. */
	if (name_contains(name, "Keyboard"))
		return VIO_INPUT_KIND_KEYBOARD;
	if (name_contains(name, "Mouse") || name_contains(name, "Tablet"))
		return VIO_INPUT_KIND_MOUSE;
	return VIO_INPUT_KIND_UNKNOWN;
}

static void vio_input_dispatch_event(vio_input_dev_t *dev,
                                     const struct virtio_input_event *evt)
{
	if (dev->kind == VIO_INPUT_KIND_KEYBOARD) {
		kbdev_push_event(evt->type, evt->code,
		                 (int32_t)evt->value);
	} else if (dev->kind == VIO_INPUT_KIND_MOUSE) {
		mousedev_push_event(evt->type, evt->code,
		                    (int32_t)evt->value);
	}
}

/* Refill `head` (one descriptor) into the avail ring as a device-write
 * buffer that will receive the next input_event. */
static void vio_input_refill_one(vio_input_dev_t *dev, uint16_t head)
{
	uint16_t buf_idx = head; /* 1:1 mapping: descriptor i ↔ event[i]. */

	dev->queue.desc[head].addr = dev->events_phys +
	                              buf_idx * VIRTIO_INPUT_EVENT_BYTES;
	dev->queue.desc[head].len = VIRTIO_INPUT_EVENT_BYTES;
	dev->queue.desc[head].flags = VIRTQ_DESC_F_WRITE;
	dev->queue.desc[head].next = 0u;
	virtq_submit(&dev->queue, head);
}

static void vio_input_irq_drain(vio_input_dev_t *dev)
{
	uint32_t isr =
	    mmio_read32(dev->base + VIRTIO_MMIO_INTERRUPT_STATUS);

	mmio_write32(dev->base + VIRTIO_MMIO_INTERRUPT_ACK, isr);

	for (;;) {
		uint32_t completed_len = 0;
		uint16_t head = virtq_drain_one(&dev->queue, &completed_len);

		if (head == 0xFFFFu)
			break;

		/* The device wrote sizeof(virtio_input_event) bytes via DMA
		 * — invalidate the cache line before reading. */
		arm64_dma_cache_invalidate(&dev->events[head],
		                           VIRTIO_INPUT_EVENT_BYTES);
		vio_input_dispatch_event(dev, &dev->events[head]);

		/* Refill the descriptor and resubmit. */
		vio_input_refill_one(dev, head);
	}

	mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_NOTIFY, 0u);
}

/* Per-device IRQ handlers. Two static thunks because the platform IRQ
 * dispatch table holds parameter-less function pointers. */
static void vio_input_irq_dev0(void)
{
	if (g_dev_count > 0)
		vio_input_irq_drain(&g_devs[0]);
}

static void vio_input_irq_dev1(void)
{
	if (g_dev_count > 1)
		vio_input_irq_drain(&g_devs[1]);
}

static int vio_input_init_one(vio_input_dev_t *dev)
{
	uint32_t qmax;
	uint32_t spi;
	platform_irq_handler_fn handler;

	dev->queue_backing = virt_dma_alloc(VIRTIO_INPUT_QUEUE_PAGES);
	if (!dev->queue_backing) {
		platform_uart_puts("virtio-input: queue backing alloc failed\n");
		return -1;
	}

	dev->events = virt_dma_alloc(VIRTIO_INPUT_BUF_PAGES);
	if (!dev->events) {
		virt_dma_free(dev->queue_backing, VIRTIO_INPUT_QUEUE_PAGES);
		dev->queue_backing = 0;
		platform_uart_puts("virtio-input: event ring alloc failed\n");
		return -1;
	}
	dev->events_phys = virt_virt_to_phys(dev->events);

	if (virtq_init(&dev->queue, dev->queue_backing,
	               VIRTIO_INPUT_QUEUE_PAGES * VIRT_DMA_PAGE_SIZE) != 0) {
		platform_uart_puts("virtio-input: virtq_init failed\n");
		virt_dma_free(dev->events, VIRTIO_INPUT_BUF_PAGES);
		virt_dma_free(dev->queue_backing, VIRTIO_INPUT_QUEUE_PAGES);
		return -1;
	}

	/* Reset → ack → driver → features-ok → queue setup → driver-ok. */
	mmio_write32(dev->base + VIRTIO_MMIO_STATUS, 0u);
	status_or(dev->base, VIRTIO_STATUS_ACKNOWLEDGE);
	status_or(dev->base, VIRTIO_STATUS_DRIVER);
	mmio_write32(dev->base + VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0u);
	mmio_write32(dev->base + VIRTIO_MMIO_DRIVER_FEATURES, 0u);
	status_or(dev->base, VIRTIO_STATUS_FEATURES_OK);
	if ((mmio_read32(dev->base + VIRTIO_MMIO_STATUS) &
	     VIRTIO_STATUS_FEATURES_OK) == 0u) {
		platform_uart_puts(
		    "virtio-input: device rejected feature negotiation\n");
		status_or(dev->base, VIRTIO_STATUS_FAILED);
		return -1;
	}

	mmio_write32(dev->base + VIRTIO_MMIO_GUEST_PAGE_SIZE, VIRT_DMA_PAGE_SIZE);
	mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_SEL, 0u); /* eventq */
	qmax = mmio_read32(dev->base + VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (qmax < VIRTQ_SIZE) {
		platform_uart_puts(
		    "virtio-input: device QueueNumMax below VIRTQ_SIZE\n");
		return -1;
	}
	mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_NUM, VIRTQ_SIZE);
	mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_ALIGN, VIRT_DMA_PAGE_SIZE);
	mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_PFN,
	             (uint32_t)(dev->queue.base_phys >> 12));

	/* Pre-populate every descriptor with a buffer for the device to
	 * write into. virtq_alloc_chain always returns descriptor 0 first
	 * for a fresh queue, then 1, etc., so we map slot i ↔ event[i]
	 * 1:1 by allocating singletons and submitting them in order. */
	for (uint32_t i = 0; i < VIRTIO_INPUT_RING_BUFFERS; i++) {
		uint16_t head = virtq_alloc_chain(&dev->queue, 1u);

		if (head == 0xFFFFu) {
			platform_uart_puts(
			    "virtio-input: failed to seed eventq descriptors\n");
			return -1;
		}
		vio_input_refill_one(dev, head);
	}

	/* Wire IRQ. */
	spi = VIRTIO_MMIO_SPI_BASE + dev->slot;
	handler = (dev == &g_devs[0]) ? vio_input_irq_dev0
	                              : vio_input_irq_dev1;
	if (virt_irq_register_spi(spi, handler) != 0) {
		platform_uart_puts(
		    "virtio-input: SPI handler registration failed\n");
		return -1;
	}
	virt_irq_enable_spi(spi, 0xA0u);

	status_or(dev->base, VIRTIO_STATUS_DRIVER_OK);

	/* Kick — tell the device the queue is loaded. */
	mmio_write32(dev->base + VIRTIO_MMIO_QUEUE_NOTIFY, 0u);
	return 0;
}

int arm64_virt_input_register_all(void)
{
	uint32_t found = 0;
	char line[128];

	g_dev_count = 0;

	for (uint32_t slot = 0; slot < 32u && g_dev_count < VIRTIO_INPUT_MAX_DEVICES;
	     slot++) {
		uintptr_t base = VIRTIO_MMIO_BASE_ADDR +
		                 slot * VIRTIO_MMIO_STRIDE_BYTES;
		uint32_t magic = mmio_read32(base + VIRTIO_MMIO_MAGIC_VALUE);
		uint32_t device_id;
		char name[64];
		vio_input_kind_t kind;

		if (magic != 0x74726976u)
			continue;

		device_id = mmio_read32(base + VIRTIO_MMIO_DEVICE_ID);
		if (device_id != VIRTIO_DEV_ID_INPUT)
			continue;

		(void)vio_input_read_name(base, name, sizeof(name));
		kind = classify_by_name(name);
		if (kind == VIO_INPUT_KIND_UNKNOWN) {
			k_snprintf(line,
			           sizeof(line),
			           "virtio-input: slot %u name '%s' unrecognised; skipping\n",
			           (unsigned int)slot, name);
			platform_uart_puts(line);
			continue;
		}

		g_devs[g_dev_count].base = base;
		g_devs[g_dev_count].slot = slot;
		g_devs[g_dev_count].kind = kind;

		if (vio_input_init_one(&g_devs[g_dev_count]) != 0)
			continue;

		k_snprintf(line,
		           sizeof(line),
		           "virtio-input: %s @ slot %u (%s)\n",
		           kind == VIO_INPUT_KIND_KEYBOARD ? "keyboard" : "mouse",
		           (unsigned int)slot,
		           name);
		platform_uart_puts(line);
		g_dev_count++;
		found++;
	}

	if (found == 0)
		platform_uart_puts(
		    "virtio-input: no devices found; /dev/kbd and /dev/mouse "
		    "will only fire from other transports\n");
	return (int)found;
}
