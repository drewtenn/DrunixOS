/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * usb_xhci.c - Minimal RP1 xHCI + USB HID boot-keyboard path for Pi 5.
 */

#include "platform.h"
#include "../platform.h"
#include "tty.h"
#include "kstring.h"
#include "uart.h"
#include "arch/arm64/dma.h"
#include <stdint.h>

#define BIT(n) (1u << (n))
#define BIT64(n) (1ull << (n))

#define XHCI_CAPLENGTH 0x00u
#define XHCI_HCSPARAMS1 0x04u
#define XHCI_HCSPARAMS2 0x08u
#define XHCI_HCCPARAMS1 0x10u
#define XHCI_DBOFF 0x14u
#define XHCI_RTSOFF 0x18u

#define XHCI_USBCMD 0x00u
#define XHCI_USBSTS 0x04u
#define XHCI_PAGESIZE 0x08u
#define XHCI_CRCR 0x18u
#define XHCI_DCBAAP 0x30u
#define XHCI_CONFIG 0x38u
#define XHCI_PORTSC_BASE 0x400u
#define XHCI_PORT_STRIDE 0x10u

#define XHCI_IMAN 0x00u
#define XHCI_IMOD 0x04u
#define XHCI_ERSTSZ 0x08u
#define XHCI_ERSTBA 0x10u
#define XHCI_ERDP 0x18u
#define XHCI_INTR0 0x20u

#define XHCI_CMD_RUN BIT(0)
#define XHCI_CMD_RESET BIT(1)
#define XHCI_STS_HALT BIT(0)
#define XHCI_STS_EINT BIT(3)
#define XHCI_STS_PORT BIT(4)
#define XHCI_STS_CNR BIT(11)
#define XHCI_CONFIG_MAX_SLOTS_MASK 0xffu

#define XHCI_PORT_CONNECT BIT(0)
#define XHCI_PORT_PE BIT(1)
#define XHCI_PORT_RESET BIT(4)
#define XHCI_PORT_PLS_MASK (0xfu << 5)
#define XHCI_PORT_U0 0u
#define XHCI_PORT_POWER BIT(9)
#define XHCI_PORT_SPEED(sc) (((sc) >> 10) & 0x0fu)
#define XHCI_PORT_CSC BIT(17)
#define XHCI_PORT_PEC BIT(18)
#define XHCI_PORT_WRC BIT(19)
#define XHCI_PORT_OCC BIT(20)
#define XHCI_PORT_RC BIT(21)
#define XHCI_PORT_PLC BIT(22)
#define XHCI_PORT_CEC BIT(23)
#define XHCI_PORT_CHANGE_MASK                                                 \
	(XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC | XHCI_PORT_OCC |      \
	 XHCI_PORT_RC | XHCI_PORT_PLC | XHCI_PORT_CEC)
#define XHCI_PORT_RO ((1u << 0) | (1u << 3) | (0xfu << 10) | (1u << 30))
#define XHCI_PORT_RWS ((0xfu << 5) | (1u << 9) | (0x3u << 14) | (0x7u << 25))

#define XHCI_HCS_MAX_SLOTS(p) (((p) >> 0) & 0xffu)
#define XHCI_HCS_MAX_PORTS(p) (((p) >> 24) & 0x7fu)
#define XHCI_HCS_MAX_SCRATCHPAD(p) ((((p) >> 16) & 0x3e0u) | (((p) >> 27) & 0x1fu))
#define XHCI_HCC_64BYTE_CONTEXT(p) ((p) & BIT(2))

#define XHCI_CTX_SLOT_FLAG BIT(0)
#define XHCI_CTX_EP0_FLAG BIT(1)
#define XHCI_CTX_ADD_EP(dci) BIT(dci)
#define XHCI_SLOT_SPEED(speed) (((speed) & 0xfu) << 20)
#define XHCI_SLOT_LAST_CTX(dci) (((dci) & 0x1fu) << 27)
#define XHCI_SLOT_ROOT_PORT(port) (((port) & 0xffu) << 16)
#define XHCI_EP_INTERVAL(i) (((i) & 0xffu) << 16)
#define XHCI_EP_TYPE(t) (((t) & 0x7u) << 3)
#define XHCI_EP_ERROR_COUNT(n) (((n) & 0x3u) << 1)
#define XHCI_EP_MAX_PACKET(n) (((n) & 0xffffu) << 16)
#define XHCI_EP_AVG_TRB(n) ((n) & 0xffffu)
#define XHCI_EP_CTX_CTRL 4u
#define XHCI_EP_CTX_INT_IN 7u

#define XHCI_TRB_RING_SIZE 64u
#define XHCI_EVENT_RING_SIZE 64u
#define XHCI_CTRL_BUF_SIZE 256u
#define XHCI_CONFIG_BUF_SIZE 256u
#define XHCI_SCRATCHPAD_MAX 32u
#define XHCI_TRB_CYCLE BIT(0)
#define XHCI_TRB_CHAIN BIT(4)
#define XHCI_TRB_IOC BIT(5)
#define XHCI_TRB_IDT BIT(6)
#define XHCI_TRB_ISP BIT(2)
#define XHCI_TRB_DIR_IN BIT(16)
#define XHCI_TRB_TX_TYPE(t) (((t) & 0x3u) << 16)
#define XHCI_TRB_TYPE(t) (((t) & 0x3fu) << 10)
#define XHCI_TRB_SLOT_ID(slot) (((slot) & 0xffu) << 24)
#define XHCI_TRB_TO_SLOT_ID(ctrl) (((ctrl) >> 24) & 0xffu)
#define XHCI_TRB_TO_EP_ID(ctrl) (((ctrl) >> 16) & 0x1fu)
#define XHCI_TRB_LEN(len) ((len) & 0x1ffffu)
#define XHCI_TRB_COMP_CODE(status) (((status) >> 24) & 0xffu)
#define XHCI_TRB_RESIDUAL(status) ((status) & 0xffffffu)

#define XHCI_TRB_NORMAL 1u
#define XHCI_TRB_SETUP 2u
#define XHCI_TRB_DATA 3u
#define XHCI_TRB_STATUS 4u
#define XHCI_TRB_LINK 6u
#define XHCI_TRB_ENABLE_SLOT 9u
#define XHCI_TRB_ADDR_DEV 11u
#define XHCI_TRB_CONFIG_EP 12u
#define XHCI_TRB_TRANSFER_EVENT 32u
#define XHCI_TRB_CMD_COMPLETION_EVENT 33u
#define XHCI_TRB_PORT_STATUS_EVENT 34u

#define XHCI_TRB_DATA_OUT 2u
#define XHCI_TRB_DATA_IN 3u
#define XHCI_COMP_SUCCESS 1u
#define XHCI_COMP_SHORT_PACKET 13u

#define USB_DIR_IN 0x80u
#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_CONFIGURATION 9u
#define USB_DESC_DEVICE 1u
#define USB_DESC_CONFIG 2u
#define USB_DESC_INTERFACE 4u
#define USB_DESC_ENDPOINT 5u
#define USB_CLASS_HID 3u
#define USB_SUBCLASS_BOOT 1u
#define USB_PROTOCOL_KEYBOARD 1u
#define USB_HID_SET_IDLE 0x0au
#define USB_HID_SET_PROTOCOL 0x0bu
#define USB_XFER_INTERRUPT 3u

#define RASPI5_XHCI_DMA_BIAS 0x1000000000ull
#define RASPI5_XHCI_WAIT_LONG 1000000u
#define RASPI5_XHCI_WAIT_SHORT 100000u

typedef struct {
	uint64_t parameter;
	uint32_t status;
	uint32_t control;
} xhci_trb_t;

typedef struct {
	uint64_t base;
	uint16_t size;
	uint16_t rsvd0;
	uint32_t rsvd1;
} xhci_erst_entry_t;

typedef struct {
	uint8_t request_type;
	uint8_t request;
	uint16_t value;
	uint16_t index;
	uint16_t length;
} __attribute__((packed)) usb_setup_t;

typedef struct {
	uint64_t base;
	uint32_t caplen;
	uint32_t op_base;
	uint32_t db_base;
	uint32_t rt_base;
	uint32_t hcs1;
	uint32_t hcs2;
	uint32_t hcc1;
	uint32_t ctx_size;
	uint32_t max_ports;
	uint32_t port;
	uint32_t port_speed;
	uint32_t slot_id;
	uint32_t ep0_mps;
	uint32_t kb_dci;
	uint32_t kb_mps;
	uint8_t kb_interface;
	uint8_t kb_report[8];
	int ready;
	int intr_pending;
	int polling;
} raspi5_xhci_t;

static raspi5_xhci_t g_xhci;
static xhci_trb_t g_cmd_ring[XHCI_TRB_RING_SIZE] __attribute__((aligned(64)));
static xhci_trb_t g_event_ring[XHCI_EVENT_RING_SIZE] __attribute__((aligned(64)));
static xhci_trb_t g_ep0_ring[XHCI_TRB_RING_SIZE] __attribute__((aligned(64)));
static xhci_trb_t g_kb_ring[XHCI_TRB_RING_SIZE] __attribute__((aligned(64)));
static xhci_erst_entry_t g_erst[1] __attribute__((aligned(64)));
static uint64_t g_dcbaa[256] __attribute__((aligned(64)));
static uint64_t g_scratchpad_ptrs[XHCI_SCRATCHPAD_MAX] __attribute__((aligned(64)));
static uint8_t g_scratchpads[XHCI_SCRATCHPAD_MAX][4096]
    __attribute__((aligned(4096)));
static uint8_t g_input_ctx[4096] __attribute__((aligned(64)));
static uint8_t g_device_ctx[4096] __attribute__((aligned(64)));
static uint8_t g_ctrl_buf[XHCI_CTRL_BUF_SIZE] __attribute__((aligned(64)));
static uint8_t g_config_buf[XHCI_CONFIG_BUF_SIZE] __attribute__((aligned(64)));
static uint8_t g_report_buf[8] __attribute__((aligned(64)));

static uint32_t g_cmd_enq;
static uint32_t g_cmd_cycle;
static uint32_t g_event_deq;
static uint32_t g_event_cycle;
static uint32_t g_ep0_enq;
static uint32_t g_ep0_cycle;
static uint32_t g_kb_enq;
static uint32_t g_kb_cycle;

static uint32_t xhci_read32(uint64_t base, uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(base + off);
}

static void xhci_write32(uint64_t base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)(base + off) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void xhci_write64(uint64_t base, uint32_t off, uint64_t value)
{
	xhci_write32(base, off, (uint32_t)value);
	xhci_write32(base, off + 4u, (uint32_t)(value >> 32));
}

static void xhci_delay(uint32_t count)
{
	while (count-- != 0u)
		__asm__ volatile("nop");
}

static uint64_t xhci_irq_save(void)
{
	uint64_t daif;

	__asm__ volatile("mrs %0, daif" : "=r"(daif));
	__asm__ volatile("msr daifset, #2" ::: "memory");
	return daif;
}

static void xhci_irq_restore(uint64_t daif)
{
	__asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
}

static uint64_t xhci_dma_addr(const void *ptr)
{
	return RASPI5_XHCI_DMA_BIAS + (uint64_t)(uintptr_t)ptr;
}

static void xhci_trace_hex32(const char *label, uint32_t value)
{
	static const char hex[] = "0123456789abcdef";
	char buf[11];

	buf[0] = '0';
	buf[1] = 'x';
	for (uint32_t i = 0; i < 8u; i++)
		buf[2u + i] = hex[(value >> ((7u - i) * 4u)) & 0xfu];
	buf[10] = '\0';
	platform_uart_puts(label);
	platform_uart_puts(buf);
	platform_uart_puts("\n");
}

static void xhci_trace_hex64(const char *label, uint64_t value)
{
	static const char hex[] = "0123456789abcdef";
	char buf[19];

	buf[0] = '0';
	buf[1] = 'x';
	for (uint32_t i = 0; i < 16u; i++)
		buf[2u + i] = hex[(value >> ((15u - i) * 4u)) & 0xfu];
	buf[18] = '\0';
	platform_uart_puts(label);
	platform_uart_puts(buf);
	platform_uart_puts("\n");
}

static void xhci_ring_init(xhci_trb_t *ring,
                           uint32_t count,
                           uint32_t *enq,
                           uint32_t *cycle)
{
	k_memset(ring, 0, count * sizeof(ring[0]));
	ring[count - 1u].parameter = xhci_dma_addr(ring);
	/*
	 * Producer rings start with PCS=1, but link TRBs stay at C=0 until
	 * xhci_ring_put() advances past them and gives them to the controller.
	 */
	ring[count - 1u].control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | BIT(1);
	arm64_dma_cache_clean(ring, count * sizeof(ring[0]));
	*enq = 0u;
	*cycle = 1u;
}

static xhci_trb_t *xhci_ring_put(xhci_trb_t *ring,
                                 uint32_t count,
                                 uint32_t *enq,
                                 uint32_t *cycle,
                                 uint64_t parameter,
                                 uint32_t status,
                                 uint32_t control)
{
	xhci_trb_t *trb = &ring[*enq];

	trb->parameter = parameter;
	trb->status = status;
	trb->control = control | (*cycle ? XHCI_TRB_CYCLE : 0u);
	arm64_dma_cache_clean(trb, sizeof(*trb));
	*enq += 1u;
	if (*enq == count - 1u) {
		ring[*enq].control ^= XHCI_TRB_CYCLE;
		arm64_dma_cache_clean(&ring[*enq], sizeof(ring[*enq]));
		*cycle ^= 1u;
		*enq = 0u;
	}
	return trb;
}

static void xhci_doorbell(uint32_t slot, uint32_t target)
{
	volatile uint32_t *db =
	    (volatile uint32_t *)(uintptr_t)(g_xhci.base + g_xhci.db_base +
	                                     slot * 4u);

	(void)*db;
	*db = target;
	(void)*db;
	__asm__ volatile("dsb sy" ::: "memory");
}

static int xhci_next_event(xhci_trb_t *out)
{
	xhci_trb_t *ev = &g_event_ring[g_event_deq];
	uint32_t ctrl;

	arm64_dma_cache_invalidate(ev, sizeof(*ev));
	ctrl = ev->control;
	if ((ctrl & XHCI_TRB_CYCLE) != (g_event_cycle ? XHCI_TRB_CYCLE : 0u))
		return 0;

	*out = *ev;
	g_event_deq++;
	if (g_event_deq == XHCI_EVENT_RING_SIZE) {
		g_event_deq = 0u;
		g_event_cycle ^= 1u;
	}
	xhci_write64(g_xhci.base,
	             g_xhci.rt_base + XHCI_INTR0 + XHCI_ERDP,
	             xhci_dma_addr(&g_event_ring[g_event_deq]) | BIT64(3));
	return 1;
}

static int xhci_wait_command(xhci_trb_t *cmd, xhci_trb_t *completion)
{
	uint64_t cmd_dma = xhci_dma_addr(cmd);

	for (uint32_t i = 0; i < RASPI5_XHCI_WAIT_LONG; i++) {
		xhci_trb_t ev;
		uint32_t code;

		if (!xhci_next_event(&ev)) {
			xhci_delay(10u);
			continue;
		}
		if (((ev.control >> 10) & 0x3fu) != XHCI_TRB_CMD_COMPLETION_EVENT)
			continue;
		if (ev.parameter != cmd_dma)
			continue;
		*completion = ev;
		code = XHCI_TRB_COMP_CODE(ev.status);
		if (code == XHCI_COMP_SUCCESS)
			return 0;
		xhci_trace_hex32("raspi5 xhci: cmd_comp=", code);
		xhci_trace_hex32("raspi5 xhci: cmd_status=", ev.status);
		return -1;
	}
	arm64_dma_cache_invalidate(&g_event_ring[0], sizeof(g_event_ring[0]));
	xhci_trace_hex32("raspi5 xhci: cmd_timeout_ctrl=", cmd->control);
	xhci_trace_hex32("raspi5 xhci: usbsts=",
	                 xhci_read32(g_xhci.base, g_xhci.op_base + XHCI_USBSTS));
	xhci_trace_hex32("raspi5 xhci: crcr_lo=",
	                 xhci_read32(g_xhci.base, g_xhci.op_base + XHCI_CRCR));
	xhci_trace_hex32("raspi5 xhci: erdp_lo=",
	                 xhci_read32(g_xhci.base,
	                             g_xhci.rt_base + XHCI_INTR0 + XHCI_ERDP));
	xhci_trace_hex32("raspi5 xhci: ev0_status=", g_event_ring[0].status);
	xhci_trace_hex32("raspi5 xhci: ev0_ctrl=", g_event_ring[0].control);
	return -1;
}

static int xhci_command(uint64_t parameter,
                        uint32_t status,
                        uint32_t control,
                        xhci_trb_t *completion)
{
	xhci_trb_t *cmd;

	cmd = xhci_ring_put(g_cmd_ring,
	                    XHCI_TRB_RING_SIZE,
	                    &g_cmd_enq,
	                    &g_cmd_cycle,
	                    parameter,
	                    status,
	                    control);
	arm64_dma_cache_clean(g_cmd_ring, sizeof(g_cmd_ring));
	xhci_doorbell(0u, 0u);
	return xhci_wait_command(cmd, completion);
}

static uint32_t *xhci_input_ctrl_ctx(void)
{
	return (uint32_t *)(uintptr_t)g_input_ctx;
}

static uint32_t *xhci_input_slot_ctx(void)
{
	return (uint32_t *)(uintptr_t)(g_input_ctx + g_xhci.ctx_size);
}

static uint32_t *xhci_input_ep_ctx(uint32_t dci)
{
	return (uint32_t *)(uintptr_t)(g_input_ctx + (1u + dci) * g_xhci.ctx_size);
}

static uint32_t xhci_portsc_offset(uint32_t port)
{
	return g_xhci.op_base + XHCI_PORTSC_BASE + (port - 1u) * XHCI_PORT_STRIDE;
}

static uint32_t xhci_port_neutral(uint32_t state)
{
	return (state & XHCI_PORT_RO) | (state & XHCI_PORT_RWS);
}

static int xhci_halt_reset(void)
{
	uint32_t cmd;

	cmd = xhci_read32(g_xhci.base, g_xhci.op_base + XHCI_USBCMD);
	cmd &= ~XHCI_CMD_RUN;
	xhci_write32(g_xhci.base, g_xhci.op_base + XHCI_USBCMD, cmd);
	for (uint32_t i = 0; i < RASPI5_XHCI_WAIT_LONG; i++) {
		if ((xhci_read32(g_xhci.base, g_xhci.op_base + XHCI_USBSTS) &
		     XHCI_STS_HALT) != 0u)
			break;
		if (i == RASPI5_XHCI_WAIT_LONG - 1u)
			return -1;
	}

	xhci_write32(g_xhci.base,
	             g_xhci.op_base + XHCI_USBCMD,
	             xhci_read32(g_xhci.base, g_xhci.op_base + XHCI_USBCMD) |
	                 XHCI_CMD_RESET);
	for (uint32_t i = 0; i < RASPI5_XHCI_WAIT_LONG; i++) {
		uint32_t usbcmd = xhci_read32(g_xhci.base, g_xhci.op_base + XHCI_USBCMD);
		uint32_t usbsts = xhci_read32(g_xhci.base, g_xhci.op_base + XHCI_USBSTS);

		if ((usbcmd & XHCI_CMD_RESET) == 0u && (usbsts & XHCI_STS_CNR) == 0u)
			return 0;
	}
	return -1;
}

static void xhci_setup_scratchpads(void)
{
	uint32_t scratchpads = XHCI_HCS_MAX_SCRATCHPAD(g_xhci.hcs2);

	if (scratchpads > XHCI_SCRATCHPAD_MAX)
		scratchpads = XHCI_SCRATCHPAD_MAX;
	g_dcbaa[0] = 0u;
	if (scratchpads != 0u) {
		for (uint32_t i = 0; i < scratchpads; i++)
			g_scratchpad_ptrs[i] = xhci_dma_addr(g_scratchpads[i]);
		arm64_dma_cache_clean(g_scratchpad_ptrs, sizeof(g_scratchpad_ptrs));
		arm64_dma_cache_clean(g_scratchpads, sizeof(g_scratchpads));
		g_dcbaa[0] = xhci_dma_addr(g_scratchpad_ptrs);
	}
}

static int xhci_init_controller(uint64_t base)
{
	uint32_t capbase;
	uint32_t erstsz;
	uint32_t max_slots;

	k_memset(&g_xhci, 0, sizeof(g_xhci));
	g_xhci.base = base;
	capbase = xhci_read32(base, XHCI_CAPLENGTH);
	if (capbase == 0xffffffffu || capbase == 0u)
		return -1;

	g_xhci.caplen = capbase & 0xffu;
	g_xhci.op_base = g_xhci.caplen;
	g_xhci.hcs1 = xhci_read32(base, XHCI_HCSPARAMS1);
	g_xhci.hcs2 = xhci_read32(base, XHCI_HCSPARAMS2);
	g_xhci.hcc1 = xhci_read32(base, XHCI_HCCPARAMS1);
	g_xhci.db_base = xhci_read32(base, XHCI_DBOFF) & ~0x3u;
	g_xhci.rt_base = xhci_read32(base, XHCI_RTSOFF) & ~0x1fu;
	g_xhci.ctx_size = XHCI_HCC_64BYTE_CONTEXT(g_xhci.hcc1) ? 64u : 32u;
	g_xhci.max_ports = XHCI_HCS_MAX_PORTS(g_xhci.hcs1);
	max_slots = XHCI_HCS_MAX_SLOTS(g_xhci.hcs1);
	if (max_slots == 0u)
		return -1;
	if (max_slots > 8u)
		max_slots = 8u;

	if (xhci_halt_reset() != 0)
		return -1;

	k_memset(g_dcbaa, 0, sizeof(g_dcbaa));
	k_memset(g_input_ctx, 0, sizeof(g_input_ctx));
	k_memset(g_device_ctx, 0, sizeof(g_device_ctx));
	k_memset(g_ctrl_buf, 0, sizeof(g_ctrl_buf));
	k_memset(g_config_buf, 0, sizeof(g_config_buf));
	k_memset(g_report_buf, 0, sizeof(g_report_buf));
	xhci_setup_scratchpads();
	xhci_ring_init(g_cmd_ring, XHCI_TRB_RING_SIZE, &g_cmd_enq, &g_cmd_cycle);
	xhci_ring_init(g_ep0_ring, XHCI_TRB_RING_SIZE, &g_ep0_enq, &g_ep0_cycle);
	xhci_ring_init(g_kb_ring, XHCI_TRB_RING_SIZE, &g_kb_enq, &g_kb_cycle);
	k_memset(g_event_ring, 0, sizeof(g_event_ring));
	g_event_deq = 0u;
	g_event_cycle = 1u;

	g_erst[0].base = xhci_dma_addr(g_event_ring);
	g_erst[0].size = XHCI_EVENT_RING_SIZE;
	g_erst[0].rsvd0 = 0u;
	g_erst[0].rsvd1 = 0u;
	arm64_dma_cache_clean(g_event_ring, sizeof(g_event_ring));
	arm64_dma_cache_clean(g_erst, sizeof(g_erst));
	arm64_dma_cache_clean(g_dcbaa, sizeof(g_dcbaa));

	xhci_write64(base, g_xhci.op_base + XHCI_DCBAAP, xhci_dma_addr(g_dcbaa));
	xhci_write64(base,
	             g_xhci.op_base + XHCI_CRCR,
	             xhci_dma_addr(g_cmd_ring) | XHCI_TRB_CYCLE);
	xhci_write32(base, g_xhci.op_base + XHCI_CONFIG, max_slots);

	erstsz = xhci_read32(base, g_xhci.rt_base + XHCI_INTR0 + XHCI_ERSTSZ);
	xhci_write32(base,
	             g_xhci.rt_base + XHCI_INTR0 + XHCI_ERSTSZ,
	             (erstsz & 0xffff0000u) | 1u);
	xhci_write64(base, g_xhci.rt_base + XHCI_INTR0 + XHCI_ERSTBA, xhci_dma_addr(g_erst));
	xhci_write64(base,
	             g_xhci.rt_base + XHCI_INTR0 + XHCI_ERDP,
	             xhci_dma_addr(g_event_ring) | BIT64(3));
	xhci_write32(base, g_xhci.rt_base + XHCI_INTR0 + XHCI_IMAN, 0u);
	xhci_write32(base, g_xhci.rt_base + XHCI_INTR0 + XHCI_IMOD, 0u);
	xhci_write32(base, g_xhci.op_base + XHCI_USBSTS, 0xffffffffu);
	xhci_write32(base,
	             g_xhci.op_base + XHCI_USBCMD,
	             xhci_read32(base, g_xhci.op_base + XHCI_USBCMD) | XHCI_CMD_RUN);
	for (uint32_t i = 0; i < RASPI5_XHCI_WAIT_LONG; i++) {
		if ((xhci_read32(base, g_xhci.op_base + XHCI_USBSTS) &
		     XHCI_STS_HALT) == 0u)
			return 0;
	}
	return -1;
}

static int xhci_reset_port(void)
{
	for (uint32_t port = 1u; port <= g_xhci.max_ports; port++) {
		uint32_t off = xhci_portsc_offset(port);
		uint32_t sc = xhci_read32(g_xhci.base, off);

		if ((sc & XHCI_PORT_CONNECT) == 0u)
			continue;
		if ((sc & XHCI_PORT_POWER) == 0u) {
			xhci_write32(g_xhci.base, off, xhci_port_neutral(sc) | XHCI_PORT_POWER);
			xhci_delay(RASPI5_XHCI_WAIT_SHORT);
			sc = xhci_read32(g_xhci.base, off);
		}
		xhci_write32(g_xhci.base, off, xhci_port_neutral(sc) | XHCI_PORT_RESET);
		for (uint32_t i = 0; i < RASPI5_XHCI_WAIT_LONG; i++) {
			sc = xhci_read32(g_xhci.base, off);
			if ((sc & XHCI_PORT_RESET) == 0u && (sc & XHCI_PORT_RC) != 0u)
				break;
		}
		sc = xhci_read32(g_xhci.base, off);
		xhci_write32(g_xhci.base, off, xhci_port_neutral(sc) | XHCI_PORT_CHANGE_MASK);
		sc = xhci_read32(g_xhci.base, off);
		if ((sc & XHCI_PORT_PE) == 0u)
			continue;
		g_xhci.port = port;
		g_xhci.port_speed = XHCI_PORT_SPEED(sc);
		xhci_trace_hex32("raspi5 xhci: port=", port);
		xhci_trace_hex32("raspi5 xhci: speed=", g_xhci.port_speed);
		return 0;
	}
	return -1;
}

static int xhci_enable_slot(void)
{
	xhci_trb_t ev;

	if (xhci_command(0u, 0u, XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), &ev) != 0)
		return -1;
	g_xhci.slot_id = XHCI_TRB_TO_SLOT_ID(ev.control);
	return g_xhci.slot_id != 0u ? 0 : -1;
}

static int xhci_address_device(void)
{
	uint32_t *ctrl;
	uint32_t *slot;
	uint32_t *ep0;
	xhci_trb_t ev;

	k_memset(g_input_ctx, 0, sizeof(g_input_ctx));
	ctrl = xhci_input_ctrl_ctx();
	slot = xhci_input_slot_ctx();
	ep0 = xhci_input_ep_ctx(1u);

	ctrl[1] = XHCI_CTX_SLOT_FLAG | XHCI_CTX_EP0_FLAG;
	slot[0] = XHCI_SLOT_SPEED(g_xhci.port_speed) | XHCI_SLOT_LAST_CTX(1u);
	slot[1] = XHCI_SLOT_ROOT_PORT(g_xhci.port);
	if (g_xhci.port_speed == 4u)
		g_xhci.ep0_mps = 512u;
	else if (g_xhci.port_speed == 3u)
		g_xhci.ep0_mps = 64u;
	else
		g_xhci.ep0_mps = 8u;
	ep0[1] = XHCI_EP_ERROR_COUNT(3u) | XHCI_EP_TYPE(XHCI_EP_CTX_CTRL) |
	         XHCI_EP_MAX_PACKET(g_xhci.ep0_mps);
	ep0[2] = (uint32_t)(xhci_dma_addr(g_ep0_ring) | XHCI_TRB_CYCLE);
	ep0[3] = (uint32_t)(xhci_dma_addr(g_ep0_ring) >> 32);
	ep0[4] = XHCI_EP_AVG_TRB(8u);

	g_dcbaa[g_xhci.slot_id] = xhci_dma_addr(g_device_ctx);
	arm64_dma_cache_clean(g_dcbaa, sizeof(g_dcbaa));
	arm64_dma_cache_clean(g_input_ctx, sizeof(g_input_ctx));
	arm64_dma_cache_clean(g_device_ctx, sizeof(g_device_ctx));

	if (xhci_command(xhci_dma_addr(g_input_ctx),
	                 0u,
	                 XHCI_TRB_TYPE(XHCI_TRB_ADDR_DEV) |
	                     XHCI_TRB_SLOT_ID(g_xhci.slot_id),
	                 &ev) != 0)
		return -1;
	return 0;
}

static int xhci_wait_transfer(uint32_t dci, uint32_t requested, uint32_t *actual)
{
	for (uint32_t i = 0; i < RASPI5_XHCI_WAIT_LONG; i++) {
		xhci_trb_t ev;
		uint32_t type;
		uint32_t code;
		uint32_t residual;

		if (!xhci_next_event(&ev)) {
			xhci_delay(10u);
			continue;
		}
		type = (ev.control >> 10) & 0x3fu;
		if (type == XHCI_TRB_PORT_STATUS_EVENT)
			continue;
		if (type != XHCI_TRB_TRANSFER_EVENT)
			continue;
		if (XHCI_TRB_TO_SLOT_ID(ev.control) != g_xhci.slot_id ||
		    XHCI_TRB_TO_EP_ID(ev.control) != dci)
			continue;
		code = XHCI_TRB_COMP_CODE(ev.status);
		residual = XHCI_TRB_RESIDUAL(ev.status);
		if (actual)
			*actual = requested >= residual ? requested - residual : 0u;
		if (code == XHCI_COMP_SUCCESS || code == XHCI_COMP_SHORT_PACKET)
			return 0;
		xhci_trace_hex32("raspi5 xhci: transfer comp=", code);
		return -1;
	}
	return -1;
}

static int xhci_control(uint8_t request_type,
                        uint8_t request,
                        uint16_t value,
                        uint16_t index,
                        void *data,
                        uint16_t length)
{
	usb_setup_t setup;
	uint64_t setup_param;
	uint32_t setup_ctrl;
	uint32_t actual = 0u;

	if (length > XHCI_CTRL_BUF_SIZE)
		return -1;
	setup.request_type = request_type;
	setup.request = request;
	setup.value = value;
	setup.index = index;
	setup.length = length;
	setup_param = (uint64_t)setup.request_type |
	              ((uint64_t)setup.request << 8) |
	              ((uint64_t)setup.value << 16) |
	              ((uint64_t)setup.index << 32) |
	              ((uint64_t)setup.length << 48);
	setup_ctrl = XHCI_TRB_TYPE(XHCI_TRB_SETUP) | XHCI_TRB_IDT;
	if (length != 0u)
		setup_ctrl |= XHCI_TRB_TX_TYPE((request_type & USB_DIR_IN) != 0u
		                                   ? XHCI_TRB_DATA_IN
		                                   : XHCI_TRB_DATA_OUT);
	xhci_ring_put(g_ep0_ring,
	              XHCI_TRB_RING_SIZE,
	              &g_ep0_enq,
	              &g_ep0_cycle,
	              setup_param,
	              XHCI_TRB_LEN(8u),
	              setup_ctrl);

	if (length != 0u) {
		if ((request_type & USB_DIR_IN) == 0u && data)
			k_memcpy(g_ctrl_buf, data, length);
		arm64_dma_cache_clean(g_ctrl_buf, length);
		xhci_ring_put(g_ep0_ring,
		              XHCI_TRB_RING_SIZE,
		              &g_ep0_enq,
		              &g_ep0_cycle,
		              xhci_dma_addr(g_ctrl_buf),
		              XHCI_TRB_LEN(length),
		              XHCI_TRB_TYPE(XHCI_TRB_DATA) |
		                  ((request_type & USB_DIR_IN) != 0u ? XHCI_TRB_DIR_IN |
		                                                           XHCI_TRB_ISP
		                                                       : 0u));
	}

	xhci_ring_put(g_ep0_ring,
	              XHCI_TRB_RING_SIZE,
	              &g_ep0_enq,
	              &g_ep0_cycle,
	              0u,
	              0u,
	              XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC |
	                  ((length == 0u || (request_type & USB_DIR_IN) == 0u)
	                       ? XHCI_TRB_DIR_IN
	                       : 0u));
	arm64_dma_cache_clean(g_ep0_ring, sizeof(g_ep0_ring));
	xhci_doorbell(g_xhci.slot_id, 1u);
	if (xhci_wait_transfer(1u, length, &actual) != 0)
		return -1;
	if (length != 0u && (request_type & USB_DIR_IN) != 0u && data) {
		arm64_dma_cache_invalidate(g_ctrl_buf, length);
		k_memcpy(data, g_ctrl_buf, length);
	}
	return 0;
}

static int xhci_get_descriptor(uint8_t type, uint8_t index, void *buf, uint16_t len)
{
	return xhci_control(0x80u,
	                    USB_REQ_GET_DESCRIPTOR,
	                    ((uint16_t)type << 8) | index,
	                    0u,
	                    buf,
	                    len);
}

static int xhci_set_configuration(uint8_t config)
{
	return xhci_control(
	    0x00u, USB_REQ_SET_CONFIGURATION, config, 0u, 0, 0u);
}

static int xhci_hid_class_request(uint8_t request, uint16_t value)
{
	return xhci_control(
	    0x21u, request, value, g_xhci.kb_interface, 0, 0u);
}

static int xhci_config_total_length(const uint8_t *config, uint16_t *out)
{
	if (!config || !out || config[0] < 9u || config[1] != USB_DESC_CONFIG)
		return -1;
	*out = (uint16_t)config[2] | ((uint16_t)config[3] << 8);
	return 0;
}

static int xhci_find_keyboard_endpoint(const uint8_t *config,
                                       uint16_t len,
                                       uint8_t *interface,
                                       uint8_t *ep,
                                       uint8_t *mps,
                                       uint8_t *interval)
{
	uint16_t offset = 0u;
	uint8_t keyboard_interface = 0xffu;
	int in_keyboard_interface = 0;

	while (offset + 2u <= len) {
		uint8_t desc_len = config[offset];
		uint8_t desc_type = config[offset + 1u];

		if (desc_len < 2u || offset + desc_len > len)
			break;
		if (desc_type == USB_DESC_INTERFACE && desc_len >= 9u) {
			in_keyboard_interface =
			    config[offset + 5u] == USB_CLASS_HID &&
			    config[offset + 6u] == USB_SUBCLASS_BOOT &&
			    config[offset + 7u] == USB_PROTOCOL_KEYBOARD;
			if (in_keyboard_interface)
				keyboard_interface = config[offset + 2u];
		} else if (desc_type == USB_DESC_ENDPOINT && desc_len >= 7u &&
		           in_keyboard_interface &&
		           (config[offset + 2u] & USB_DIR_IN) != 0u &&
		           (config[offset + 3u] & 0x03u) == USB_XFER_INTERRUPT) {
			*interface = keyboard_interface;
			*ep = config[offset + 2u] & 0x0fu;
			*mps = config[offset + 4u] ? config[offset + 4u] : 8u;
			*interval = config[offset + 6u] ? config[offset + 6u] : 10u;
			return 0;
		}
		offset = (uint16_t)(offset + desc_len);
	}
	return -1;
}

static uint32_t xhci_interval_ctx(uint8_t interval)
{
	uint32_t ctx_interval = 3u;
	uint32_t value = interval ? interval : 1u;

	if (g_xhci.port_speed == 1u || g_xhci.port_speed == 2u) {
		value = value > 255u ? 255u : value;
		while ((1u << (ctx_interval - 3u)) < value && ctx_interval < 10u)
			ctx_interval++;
		return ctx_interval;
	}
	if (value > 0u)
		value--;
	if (value > 15u)
		value = 15u;
	return value;
}

static int xhci_configure_keyboard_endpoint(uint8_t ep, uint8_t mps, uint8_t interval)
{
	uint32_t *ctrl;
	uint32_t *slot;
	uint32_t *epctx;
	xhci_trb_t ev;

	g_xhci.kb_dci = (uint32_t)ep * 2u + 1u;
	g_xhci.kb_mps = mps;
	k_memset(g_input_ctx, 0, sizeof(g_input_ctx));
	ctrl = xhci_input_ctrl_ctx();
	slot = xhci_input_slot_ctx();
	epctx = xhci_input_ep_ctx(g_xhci.kb_dci);

	ctrl[1] = XHCI_CTX_SLOT_FLAG | XHCI_CTX_ADD_EP(g_xhci.kb_dci);
	slot[0] = XHCI_SLOT_SPEED(g_xhci.port_speed) |
	          XHCI_SLOT_LAST_CTX(g_xhci.kb_dci);
	slot[1] = XHCI_SLOT_ROOT_PORT(g_xhci.port);
	epctx[0] = XHCI_EP_INTERVAL(xhci_interval_ctx(interval));
	epctx[1] = XHCI_EP_ERROR_COUNT(3u) | XHCI_EP_TYPE(XHCI_EP_CTX_INT_IN) |
	           XHCI_EP_MAX_PACKET(mps);
	epctx[2] = (uint32_t)(xhci_dma_addr(g_kb_ring) | XHCI_TRB_CYCLE);
	epctx[3] = (uint32_t)(xhci_dma_addr(g_kb_ring) >> 32);
	epctx[4] = XHCI_EP_AVG_TRB(mps);
	arm64_dma_cache_clean(g_input_ctx, sizeof(g_input_ctx));

	if (xhci_command(xhci_dma_addr(g_input_ctx),
	                 0u,
	                 XHCI_TRB_TYPE(XHCI_TRB_CONFIG_EP) |
	                     XHCI_TRB_SLOT_ID(g_xhci.slot_id),
	                 &ev) != 0)
		return -1;
	return 0;
}

static int xhci_enumerate_keyboard(void)
{
	uint8_t desc[18];
	uint16_t total;
	uint8_t endpoint = 1u;
	uint8_t endpoint_mps = 8u;
	uint8_t interval = 10u;

	k_memset(desc, 0, sizeof(desc));
	if (xhci_get_descriptor(USB_DESC_DEVICE, 0u, desc, 8u) != 0)
		return -1;
	if (desc[7] != 0u && desc[7] < g_xhci.ep0_mps)
		g_xhci.ep0_mps = desc[7];
	if (xhci_get_descriptor(USB_DESC_DEVICE, 0u, desc, sizeof(desc)) != 0)
		return -1;

	k_memset(g_config_buf, 0, sizeof(g_config_buf));
	if (xhci_get_descriptor(USB_DESC_CONFIG, 0u, g_config_buf, 9u) != 0)
		return -1;
	if (xhci_config_total_length(g_config_buf, &total) != 0)
		return -1;
	if (total > sizeof(g_config_buf))
		total = sizeof(g_config_buf);
	if (xhci_get_descriptor(USB_DESC_CONFIG, 0u, g_config_buf, total) != 0)
		return -1;
	if (xhci_find_keyboard_endpoint(g_config_buf,
	                                total,
	                                &g_xhci.kb_interface,
	                                &endpoint,
	                                &endpoint_mps,
	                                &interval) != 0)
		return -1;
	if (xhci_set_configuration(g_config_buf[5]) != 0)
		return -1;
	(void)xhci_hid_class_request(USB_HID_SET_PROTOCOL, 0u);
	(void)xhci_hid_class_request(USB_HID_SET_IDLE, 0u);
	if (xhci_configure_keyboard_endpoint(endpoint, endpoint_mps, interval) != 0)
		return -1;

	xhci_trace_hex32("raspi5 xhci: keyboard_ep=", endpoint);
	xhci_trace_hex32("raspi5 xhci: keyboard_mps=", g_xhci.kb_mps);
	return 0;
}

static int key_in_report(const uint8_t report[8], uint8_t usage)
{
	for (uint32_t i = 2u; i < 8u; i++) {
		if (report[i] == usage)
			return 1;
	}
	return 0;
}

static char hid_usage_ascii(uint8_t usage, uint8_t modifiers)
{
	static const char shifted_digits[] = ")!@#$%^&*(";
	int shift = (modifiers & 0x22u) != 0u;

	if (usage >= 0x04u && usage <= 0x1du) {
		char c = (char)('a' + usage - 0x04u);
		if (shift)
			c = (char)(c - 'a' + 'A');
		if ((modifiers & 0x11u) != 0u)
			c = (char)((c >= 'A' && c <= 'Z' ? c - 'A' : c - 'a') + 1);
		return c;
	}
	if (usage >= 0x1eu && usage <= 0x27u) {
		uint8_t digit = usage == 0x27u ? 0u : usage - 0x1du;
		char c = (char)('0' + digit);

		return shift ? shifted_digits[digit] : c;
	}

	switch (usage) {
	case 0x28u:
		return '\r';
	case 0x29u:
		return 27;
	case 0x2au:
		return '\b';
	case 0x2bu:
		return '\t';
	case 0x2cu:
		return ' ';
	case 0x2du:
		return shift ? '_' : '-';
	case 0x2eu:
		return shift ? '+' : '=';
	case 0x2fu:
		return shift ? '{' : '[';
	case 0x30u:
		return shift ? '}' : ']';
	case 0x31u:
		return shift ? '|' : '\\';
	case 0x33u:
		return shift ? ':' : ';';
	case 0x34u:
		return shift ? '"' : '\'';
	case 0x35u:
		return shift ? '~' : '`';
	case 0x36u:
		return shift ? '<' : ',';
	case 0x37u:
		return shift ? '>' : '.';
	case 0x38u:
		return shift ? '?' : '/';
	default:
		return 0;
	}
}

static void hid_deliver_report(const uint8_t report[8])
{
	for (uint32_t i = 2u; i < 8u; i++) {
		uint8_t usage = report[i];
		char c;

		if (usage == 0u || usage == 1u)
			continue;
		if (key_in_report(g_xhci.kb_report, usage))
			continue;
		c = hid_usage_ascii(usage, report[0]);
		if (c)
			tty_input_char(0, c);
	}
	k_memcpy(g_xhci.kb_report, report, sizeof(g_xhci.kb_report));
}

static void xhci_submit_keyboard_intr(void)
{
	k_memset(g_report_buf, 0, sizeof(g_report_buf));
	arm64_dma_cache_clean(g_report_buf, sizeof(g_report_buf));
	xhci_ring_put(g_kb_ring,
	              XHCI_TRB_RING_SIZE,
	              &g_kb_enq,
	              &g_kb_cycle,
	              xhci_dma_addr(g_report_buf),
	              XHCI_TRB_LEN(sizeof(g_report_buf)),
	              XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC |
	                  XHCI_TRB_ISP);
	arm64_dma_cache_clean(g_kb_ring, sizeof(g_kb_ring));
	g_xhci.intr_pending = 1;
	xhci_doorbell(g_xhci.slot_id, g_xhci.kb_dci);
}

int platform_usb_hci_register(void)
{
	uint64_t bases[2] = {PLATFORM_RASPI5_USB0_XHCI_BASE,
	                     PLATFORM_RASPI5_USB1_XHCI_BASE};

	for (uint32_t i = 0; i < 2u; i++) {
		xhci_trace_hex64("raspi5 xhci: probe base=", bases[i]);
		if (xhci_init_controller(bases[i]) != 0) {
			platform_uart_puts("raspi5 xhci: init failed\n");
			continue;
		}
		if (xhci_reset_port() != 0) {
			platform_uart_puts("raspi5 xhci: no enabled port\n");
			continue;
		}
		if (xhci_enable_slot() != 0) {
			platform_uart_puts("raspi5 xhci: enable slot failed\n");
			continue;
		}
		if (xhci_address_device() != 0) {
			platform_uart_puts("raspi5 xhci: address device failed\n");
			continue;
		}
		if (xhci_enumerate_keyboard() != 0) {
			platform_uart_puts("raspi5 xhci: enumerate keyboard failed\n");
			continue;
		}
		k_memset(g_xhci.kb_report, 0, sizeof(g_xhci.kb_report));
		g_xhci.ready = 1;
		g_xhci.intr_pending = 0;
		xhci_submit_keyboard_intr();
		platform_uart_puts("raspi5 xhci: USB keyboard enabled\n");
		return 0;
	}
	return -1;
}

void platform_usb_hci_poll(void)
{
	uint64_t irq_state;

	if (!g_xhci.ready)
		return;
	irq_state = xhci_irq_save();
	if (g_xhci.polling) {
		xhci_irq_restore(irq_state);
		return;
	}
	g_xhci.polling = 1;

	for (;;) {
		xhci_trb_t ev;
		uint32_t type;
		uint32_t code;
		uint32_t actual;

		if (!xhci_next_event(&ev))
			break;
		type = (ev.control >> 10) & 0x3fu;
		if (type == XHCI_TRB_PORT_STATUS_EVENT) {
			xhci_write32(g_xhci.base,
			             g_xhci.op_base + XHCI_USBSTS,
			             XHCI_STS_PORT | XHCI_STS_EINT);
			continue;
		}
		if (type != XHCI_TRB_TRANSFER_EVENT ||
		    XHCI_TRB_TO_SLOT_ID(ev.control) != g_xhci.slot_id ||
		    XHCI_TRB_TO_EP_ID(ev.control) != g_xhci.kb_dci)
			continue;

		code = XHCI_TRB_COMP_CODE(ev.status);
		actual = sizeof(g_report_buf) >= XHCI_TRB_RESIDUAL(ev.status)
		             ? sizeof(g_report_buf) - XHCI_TRB_RESIDUAL(ev.status)
		             : 0u;
		g_xhci.intr_pending = 0;
		if ((code == XHCI_COMP_SUCCESS || code == XHCI_COMP_SHORT_PACKET) &&
		    actual != 0u) {
			arm64_dma_cache_invalidate(g_report_buf, sizeof(g_report_buf));
			hid_deliver_report(g_report_buf);
		}
	}
	if (!g_xhci.intr_pending)
		xhci_submit_keyboard_intr();

	g_xhci.polling = 0;
	xhci_irq_restore(irq_state);
}
