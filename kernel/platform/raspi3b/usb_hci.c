/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * usb_hci.c - Minimal BCM2835 DWC2 + USB HID boot-keyboard path.
 */

#include "usb_hci.h"
#include "../platform.h"
#include "../../drivers/tty.h"
#include "kstring.h"
#include "uart.h"
#include <stdint.h>

#define BIT(n) (1u << (n))

#define DWC2_BASE (PLATFORM_PERIPHERAL_BASE + 0x980000u)
#define DWC2_REG(offset)                                                       \
	(*(volatile uint32_t *)(uintptr_t)(DWC2_BASE + (offset)))

#define GAHBCFG 0x008u
#define GUSBCFG 0x00Cu
#define GINTSTS 0x014u
#define GINTMSK 0x018u
#define HCFG 0x400u
#define HPRT0 0x440u
#define HCCHAR(ch) (0x500u + 0x20u * (ch))
#define HCSPLT(ch) (0x504u + 0x20u * (ch))
#define HCINT(ch) (0x508u + 0x20u * (ch))
#define HCINTMSK(ch) (0x50Cu + 0x20u * (ch))
#define HCTSIZ(ch) (0x510u + 0x20u * (ch))
#define HCDMA(ch) (0x514u + 0x20u * (ch))

#define GAHBCFG_DMA_EN BIT(5)
#define GUSBCFG_FORCEHOSTMODE BIT(29)
#define HCFG_FSLSPCLKSEL_48_MHZ 1u
#define HPRT0_PWR BIT(12)
#define HPRT0_RST BIT(8)
#define HPRT0_ENA BIT(2)
#define HPRT0_ENACHG BIT(3)
#define HPRT0_CONNDET BIT(1)
#define HPRT0_CONNSTS BIT(0)

#define HCCHAR_CHENA BIT(31)
#define HCCHAR_EPDIR BIT(15)
#define HCCHAR_LSPDDEV BIT(17)
#define HCCHAR_DEVADDR_SHIFT 22
#define HCCHAR_EPTYPE_SHIFT 18
#define HCCHAR_EPNUM_SHIFT 11

#define HCINT_XFERCOMPL BIT(0)
#define HCINT_CHHLTD BIT(1)
#define HCINT_STALL BIT(3)
#define HCINT_NAK BIT(4)
#define HCINT_XACTERR BIT(7)
#define HCINT_ALL 0x00003FFFu

#define HCTSIZ_PID_SHIFT 29
#define HCTSIZ_PKTCNT_SHIFT 19
#define HCTSIZ_PID_DATA0 0u
#define HCTSIZ_PID_DATA1 2u
#define HCTSIZ_PID_SETUP 3u

#define USB_DIR_IN 0x80u
#define USB_REQ_GET_STATUS 0u
#define USB_REQ_CLEAR_FEATURE 1u
#define USB_REQ_SET_FEATURE 3u
#define USB_REQ_SET_ADDRESS 5u
#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_CONFIGURATION 9u
#define USB_DESC_DEVICE 1u
#define USB_DESC_CONFIG 2u
#define USB_DESC_INTERFACE 4u
#define USB_DESC_ENDPOINT 5u
#define USB_DESC_HUB 0x29u
#define USB_CLASS_HUB 9u
#define USB_CLASS_HID 3u
#define USB_SUBCLASS_BOOT 1u
#define USB_PROTOCOL_KEYBOARD 1u
#define USB_HUB_FEATURE_PORT_RESET 4u
#define USB_HUB_FEATURE_PORT_POWER 8u
#define USB_HUB_FEATURE_C_PORT_CONNECTION 16u
#define USB_HUB_FEATURE_C_PORT_RESET 20u
#define USB_HID_SET_IDLE 0x0Au
#define USB_HID_SET_PROTOCOL 0x0Bu

#define USB_XFER_CONTROL 0u
#define USB_XFER_INTERRUPT 3u
#define USB_ADDR_HUB 1u
#define USB_ADDR_KEYBOARD 2u
#define USB_ROOT_PORT_MPS 64u
#define USB_DMA_SIZE 256u
#define USB_TIMEOUT 200000u
#define USB_RESET_DELAY 20000u

typedef struct {
	uint8_t request_type;
	uint8_t request;
	uint16_t value;
	uint16_t index;
	uint16_t length;
} __attribute__((packed)) usb_setup_t;

typedef struct {
	uint8_t addr;
	uint8_t ep;
	uint8_t mps;
	uint8_t low_speed;
	uint8_t data_pid;
	uint8_t report[8];
	int ready;
} arm64_usb_keyboard_t;

static volatile uint8_t g_usb_dma[USB_DMA_SIZE] __attribute__((aligned(64)));
static arm64_usb_keyboard_t g_keyboard;
static int g_usb_ready;
static volatile int g_usb_polling;

static uint32_t dwc2_read(uint32_t offset)
{
	return DWC2_REG(offset);
}

static void dwc2_write(uint32_t offset, uint32_t value)
{
	DWC2_REG(offset) = value;
}

static void usb_delay(uint32_t count)
{
	while (count-- != 0u)
		__asm__ volatile("nop");
}

static void usb_barrier(void)
{
	__asm__ volatile("dmb sy" ::: "memory");
}

static uint64_t usb_irq_save(void)
{
	uint64_t daif;

	__asm__ volatile("mrs %0, daif" : "=r"(daif));
	__asm__ volatile("msr daifset, #2" ::: "memory");
	return daif;
}

static void usb_irq_restore(uint64_t daif)
{
	__asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
}

static uint32_t usb_dma_addr(void)
{
	return (uint32_t)(uintptr_t)g_usb_dma;
}

static void usb_copy_to_dma(const void *src, uint32_t len)
{
	if (len > USB_DMA_SIZE)
		len = USB_DMA_SIZE;
	k_memcpy((void *)(uintptr_t)g_usb_dma, src, len);
	usb_barrier();
}

static void usb_copy_from_dma(void *dst, uint32_t len)
{
	if (len > USB_DMA_SIZE)
		len = USB_DMA_SIZE;
	usb_barrier();
	k_memcpy(dst, (const void *)(uintptr_t)g_usb_dma, len);
}

static int dwc2_reset_root_port(void)
{
	uint32_t port;
	uint32_t timeout;

	port = dwc2_read(HPRT0);
	port |= HPRT0_PWR;
	port &= ~(HPRT0_ENACHG | HPRT0_CONNDET);
	dwc2_write(HPRT0, port);
	usb_delay(USB_RESET_DELAY);

	port = dwc2_read(HPRT0);
	if ((port & HPRT0_CONNSTS) == 0u)
		return -1;

	dwc2_write(HPRT0,
	           (port | HPRT0_RST | HPRT0_PWR) &
	               ~(HPRT0_ENACHG | HPRT0_CONNDET));
	usb_delay(USB_RESET_DELAY);
	port = dwc2_read(HPRT0);
	dwc2_write(HPRT0, (port & ~HPRT0_RST) | HPRT0_PWR);

	timeout = USB_TIMEOUT;
	while ((dwc2_read(HPRT0) & HPRT0_ENA) == 0u) {
		if (timeout-- == 0u)
			return -1;
	}
	return 0;
}

static int dwc2_init_host(void)
{
	uint32_t usb_cfg;

	usb_cfg = dwc2_read(GUSBCFG);
	dwc2_write(GUSBCFG, usb_cfg | GUSBCFG_FORCEHOSTMODE);
	usb_delay(USB_RESET_DELAY);

	dwc2_write(GAHBCFG, GAHBCFG_DMA_EN);
	dwc2_write(GINTMSK, 0u);
	dwc2_write(GINTSTS, 0xFFFFFFFFu);
	dwc2_write(HCFG, HCFG_FSLSPCLKSEL_48_MHZ);
	return dwc2_reset_root_port();
}

static int dwc2_transfer(uint8_t devaddr,
                         uint8_t ep,
                         uint8_t dir_in,
                         uint8_t type,
                         uint8_t low_speed,
                         uint16_t mps,
                         uint8_t pid,
                         uint32_t len,
                         uint32_t *actual_out)
{
	uint32_t hcchar;
	uint32_t hcint;
	uint32_t packet_count;
	uint32_t timeout;

	if (mps == 0u || len > USB_DMA_SIZE)
		return -1;

	dwc2_write(HCINT(0), HCINT_ALL);
	dwc2_write(HCINTMSK(0), HCINT_ALL);
	dwc2_write(HCSPLT(0), 0u);
	dwc2_write(HCDMA(0), usb_dma_addr());
	packet_count = len == 0u ? 1u : (len + mps - 1u) / mps;
	dwc2_write(HCTSIZ(0),
	           ((uint32_t)pid << HCTSIZ_PID_SHIFT) |
	               (packet_count << HCTSIZ_PKTCNT_SHIFT) | len);

	hcchar = ((uint32_t)devaddr << HCCHAR_DEVADDR_SHIFT) |
	         ((uint32_t)type << HCCHAR_EPTYPE_SHIFT) |
	         ((uint32_t)(ep & 0x0Fu) << HCCHAR_EPNUM_SHIFT) | mps |
	         HCCHAR_CHENA;
	if (dir_in)
		hcchar |= HCCHAR_EPDIR;
	if (low_speed)
		hcchar |= HCCHAR_LSPDDEV;

	usb_barrier();
	dwc2_write(HCCHAR(0), hcchar);

	timeout = USB_TIMEOUT;
	for (;;) {
		hcint = dwc2_read(HCINT(0));
		if ((hcint & HCINT_CHHLTD) != 0u)
			break;
		if (timeout-- == 0u)
			return -1;
	}

	if (actual_out) {
		uint32_t remaining = dwc2_read(HCTSIZ(0)) & 0x7FFFFu;
		*actual_out = len >= remaining ? len - remaining : 0u;
	}
	dwc2_write(HCINT(0), hcint & HCINT_ALL);

	if ((hcint & HCINT_NAK) != 0u)
		return 1;
	if ((hcint & (HCINT_STALL | HCINT_XACTERR)) != 0u)
		return -1;
	return (hcint & HCINT_XFERCOMPL) != 0u ? 0 : -1;
}

static int usb_control(uint8_t addr,
                       uint8_t low_speed,
                       uint16_t mps,
                       uint8_t request_type,
                       uint8_t request,
                       uint16_t value,
                       uint16_t index,
                       void *data,
                       uint16_t length)
{
	usb_setup_t setup;
	uint32_t actual;
	int rc;

	setup.request_type = request_type;
	setup.request = request;
	setup.value = value;
	setup.index = index;
	setup.length = length;
	usb_copy_to_dma(&setup, sizeof(setup));
	rc = dwc2_transfer(addr,
	                   0u,
	                   0u,
	                   USB_XFER_CONTROL,
	                   low_speed,
	                   mps,
	                   HCTSIZ_PID_SETUP,
	                   sizeof(setup),
	                   &actual);
	if (rc != 0 || actual != sizeof(setup))
		return -1;

	if (length != 0u) {
		if ((request_type & USB_DIR_IN) == 0u)
			usb_copy_to_dma(data, length);
		rc = dwc2_transfer(addr,
		                   0u,
		                   (request_type & USB_DIR_IN) != 0u,
		                   USB_XFER_CONTROL,
		                   low_speed,
		                   mps,
		                   HCTSIZ_PID_DATA1,
		                   length,
		                   &actual);
		if (rc != 0)
			return -1;
		if ((request_type & USB_DIR_IN) != 0u)
			usb_copy_from_dma(data, length);
	}

	return dwc2_transfer(addr,
	                     0u,
	                     (request_type & USB_DIR_IN) == 0u,
	                     USB_XFER_CONTROL,
	                     low_speed,
	                     mps,
	                     HCTSIZ_PID_DATA1,
	                     0u,
	                     &actual) == 0
	           ? 0
	           : -1;
}

static int usb_get_descriptor(uint8_t addr,
                              uint8_t low_speed,
                              uint16_t mps,
                              uint8_t type,
                              uint8_t index,
                              void *buf,
                              uint16_t len)
{
	return usb_control(addr,
	                   low_speed,
	                   mps,
	                   0x80u,
	                   USB_REQ_GET_DESCRIPTOR,
	                   ((uint16_t)type << 8) | index,
	                   0u,
	                   buf,
	                   len);
}

static int
usb_set_address(uint8_t addr, uint8_t low_speed, uint16_t mps, uint8_t new_addr)
{
	int rc;

	rc = usb_control(
	    addr, low_speed, mps, 0x00u, USB_REQ_SET_ADDRESS, new_addr, 0u, 0, 0u);
	usb_delay(USB_RESET_DELAY);
	return rc;
}

static int usb_set_configuration(uint8_t addr,
                                 uint8_t low_speed,
                                 uint16_t mps,
                                 uint8_t config)
{
	return usb_control(addr,
	                   low_speed,
	                   mps,
	                   0x00u,
	                   USB_REQ_SET_CONFIGURATION,
	                   config,
	                   0u,
	                   0,
	                   0u);
}

static int usb_hub_port_set_feature(uint8_t feature, uint8_t port)
{
	return usb_control(USB_ADDR_HUB,
	                   0u,
	                   USB_ROOT_PORT_MPS,
	                   0x23u,
	                   USB_REQ_SET_FEATURE,
	                   feature,
	                   port,
	                   0,
	                   0u);
}

static int usb_hub_port_clear_feature(uint8_t feature, uint8_t port)
{
	return usb_control(USB_ADDR_HUB,
	                   0u,
	                   USB_ROOT_PORT_MPS,
	                   0x23u,
	                   USB_REQ_CLEAR_FEATURE,
	                   feature,
	                   port,
	                   0,
	                   0u);
}

static int usb_hub_port_status(uint8_t port, uint8_t status[4])
{
	return usb_control(USB_ADDR_HUB,
	                   0u,
	                   USB_ROOT_PORT_MPS,
	                   0xA3u,
	                   USB_REQ_GET_STATUS,
	                   0u,
	                   port,
	                   status,
	                   4u);
}

static int usb_config_total_length(const uint8_t *config, uint16_t *out)
{
	if (!config || !out || config[0] < 9u || config[1] != USB_DESC_CONFIG)
		return -1;
	*out = (uint16_t)config[2] | ((uint16_t)config[3] << 8);
	return 0;
}

static int usb_enumerate_hub(void)
{
	uint8_t desc[18];
	uint8_t config[128];
	uint16_t total;
	uint8_t mps;

	k_memset(desc, 0, sizeof(desc));
	if (usb_get_descriptor(
	        0u, 0u, USB_ROOT_PORT_MPS, USB_DESC_DEVICE, 0u, desc, 8u) != 0)
		return -1;
	mps = desc[7] ? desc[7] : USB_ROOT_PORT_MPS;
	if (usb_set_address(0u, 0u, mps, USB_ADDR_HUB) != 0)
		return -1;
	if (usb_get_descriptor(USB_ADDR_HUB,
	                       0u,
	                       USB_ROOT_PORT_MPS,
	                       USB_DESC_DEVICE,
	                       0u,
	                       desc,
	                       sizeof(desc)) != 0)
		return -1;
	if (desc[4] != USB_CLASS_HUB)
		return -1;

	k_memset(config, 0, sizeof(config));
	if (usb_get_descriptor(USB_ADDR_HUB,
	                       0u,
	                       USB_ROOT_PORT_MPS,
	                       USB_DESC_CONFIG,
	                       0u,
	                       config,
	                       9u) != 0)
		return -1;
	if (usb_config_total_length(config, &total) != 0)
		return -1;
	if (total > sizeof(config))
		total = sizeof(config);
	if (usb_get_descriptor(USB_ADDR_HUB,
	                       0u,
	                       USB_ROOT_PORT_MPS,
	                       USB_DESC_CONFIG,
	                       0u,
	                       config,
	                       total) != 0)
		return -1;
	if (usb_set_configuration(USB_ADDR_HUB, 0u, USB_ROOT_PORT_MPS, config[5]) !=
	    0)
		return -1;
	return 0;
}

static int usb_prepare_hub_port(void)
{
	uint8_t status[4];
	uint32_t timeout;

	(void)usb_control(USB_ADDR_HUB,
	                  0u,
	                  USB_ROOT_PORT_MPS,
	                  0xA0u,
	                  USB_REQ_GET_DESCRIPTOR,
	                  ((uint16_t)USB_DESC_HUB << 8),
	                  0u,
	                  status,
	                  sizeof(status));
	(void)usb_hub_port_set_feature(USB_HUB_FEATURE_PORT_POWER, 1u);
	usb_delay(USB_RESET_DELAY);
	if (usb_hub_port_set_feature(USB_HUB_FEATURE_PORT_RESET, 1u) != 0)
		return -1;
	usb_delay(USB_RESET_DELAY * 4u);

	timeout = 100u;
	while (timeout-- != 0u) {
		k_memset(status, 0, sizeof(status));
		if (usb_hub_port_status(1u, status) == 0 &&
		    (status[0] & 0x03u) == 0x03u) {
			(void)usb_hub_port_clear_feature(USB_HUB_FEATURE_C_PORT_CONNECTION,
			                                 1u);
			(void)usb_hub_port_clear_feature(USB_HUB_FEATURE_C_PORT_RESET, 1u);
			return 0;
		}
		usb_delay(USB_RESET_DELAY);
	}
	return -1;
}

static int usb_find_keyboard_endpoint(const uint8_t *config,
                                      uint16_t len,
                                      uint8_t *interface,
                                      uint8_t *ep,
                                      uint8_t *mps)
{
	uint16_t offset = 0u;
	uint8_t keyboard_interface = 0xFFu;
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
			*ep = config[offset + 2u] & 0x0Fu;
			*mps = config[offset + 4u] ? config[offset + 4u] : 8u;
			return 0;
		}
		offset = (uint16_t)(offset + desc_len);
	}
	return -1;
}

static int usb_enumerate_keyboard(void)
{
	uint8_t desc[18];
	uint8_t config[128];
	uint8_t interface = 0u;
	uint8_t endpoint = 1u;
	uint8_t endpoint_mps = 8u;
	uint16_t total;
	uint8_t mps;

	k_memset(desc, 0, sizeof(desc));
	if (usb_get_descriptor(
	        0u, 0u, USB_ROOT_PORT_MPS, USB_DESC_DEVICE, 0u, desc, 8u) != 0)
		return -1;
	mps = desc[7] ? desc[7] : 8u;
	if (usb_set_address(0u, 0u, mps, USB_ADDR_KEYBOARD) != 0)
		return -1;
	if (usb_get_descriptor(USB_ADDR_KEYBOARD,
	                       0u,
	                       mps,
	                       USB_DESC_DEVICE,
	                       0u,
	                       desc,
	                       sizeof(desc)) != 0)
		return -1;

	k_memset(config, 0, sizeof(config));
	if (usb_get_descriptor(
	        USB_ADDR_KEYBOARD, 0u, mps, USB_DESC_CONFIG, 0u, config, 9u) != 0)
		return -1;
	if (usb_config_total_length(config, &total) != 0)
		return -1;
	if (total > sizeof(config))
		total = sizeof(config);
	if (usb_get_descriptor(
	        USB_ADDR_KEYBOARD, 0u, mps, USB_DESC_CONFIG, 0u, config, total) !=
	    0)
		return -1;
	if (usb_find_keyboard_endpoint(
	        config, total, &interface, &endpoint, &endpoint_mps) != 0)
		return -1;
	if (usb_set_configuration(USB_ADDR_KEYBOARD, 0u, mps, config[5]) != 0)
		return -1;
	(void)usb_control(USB_ADDR_KEYBOARD,
	                  0u,
	                  mps,
	                  0x21u,
	                  USB_HID_SET_PROTOCOL,
	                  0u,
	                  interface,
	                  0,
	                  0u);
	(void)usb_control(USB_ADDR_KEYBOARD,
	                  0u,
	                  mps,
	                  0x21u,
	                  USB_HID_SET_IDLE,
	                  0u,
	                  interface,
	                  0,
	                  0u);

	g_keyboard.addr = USB_ADDR_KEYBOARD;
	g_keyboard.ep = endpoint;
	g_keyboard.mps = endpoint_mps;
	g_keyboard.low_speed = 0u;
	g_keyboard.data_pid = HCTSIZ_PID_DATA0;
	k_memset(g_keyboard.report, 0, sizeof(g_keyboard.report));
	g_keyboard.ready = 1;
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

	if (usage >= 0x04u && usage <= 0x1Du) {
		char c = (char)('a' + usage - 0x04u);
		if (shift)
			c = (char)(c - 'a' + 'A');
		if ((modifiers & 0x11u) != 0u)
			c = (char)((c >= 'A' && c <= 'Z' ? c - 'A' : c - 'a') + 1);
		return c;
	}
	if (usage >= 0x1Eu && usage <= 0x27u) {
		char c = usage == 0x27u ? '0' : (char)('1' + usage - 0x1Eu);
		return shift ? shifted_digits[c - '0'] : c;
	}

	switch (usage) {
	case 0x28u:
		return '\r';
	case 0x29u:
		return 27;
	case 0x2Au:
		return '\b';
	case 0x2Bu:
		return '\t';
	case 0x2Cu:
		return ' ';
	case 0x2Du:
		return shift ? '_' : '-';
	case 0x2Eu:
		return shift ? '+' : '=';
	case 0x2Fu:
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
		if (key_in_report(g_keyboard.report, usage))
			continue;
		c = hid_usage_ascii(usage, report[0]);
		if (c)
			tty_input_char(0, c);
	}
	k_memcpy(g_keyboard.report, report, sizeof(g_keyboard.report));
}

int arm64_usb_keyboard_init(void)
{
	k_memset(&g_keyboard, 0, sizeof(g_keyboard));
	g_usb_ready = 0;

	if (dwc2_init_host() != 0)
		return -1;
	if (usb_enumerate_hub() != 0)
		return -1;
	if (usb_prepare_hub_port() != 0)
		return -1;
	if (usb_enumerate_keyboard() != 0)
		return -1;

	g_usb_ready = 1;
	uart_puts("ARM64 USB keyboard enabled\n");
	return 0;
}

void arm64_usb_keyboard_poll(void)
{
	uint8_t report[8];
	uint32_t actual = 0u;
	uint64_t irq_state;
	int rc;

	if (!g_usb_ready || !g_keyboard.ready)
		return;

	irq_state = usb_irq_save();
	if (g_usb_polling) {
		usb_irq_restore(irq_state);
		return;
	}
	g_usb_polling = 1;

	k_memset((void *)(uintptr_t)g_usb_dma, 0, sizeof(report));
	rc = dwc2_transfer(g_keyboard.addr,
	                   g_keyboard.ep,
	                   1u,
	                   USB_XFER_INTERRUPT,
	                   g_keyboard.low_speed,
	                   g_keyboard.mps,
	                   g_keyboard.data_pid,
	                   sizeof(report),
	                   &actual);
	if (rc == 0) {
		g_keyboard.data_pid = g_keyboard.data_pid == HCTSIZ_PID_DATA0
		                          ? HCTSIZ_PID_DATA1
		                          : HCTSIZ_PID_DATA0;
		if (actual != 0u) {
			usb_copy_from_dma(report, sizeof(report));
			hid_deliver_report(report);
		}
	}

	g_usb_polling = 0;
	usb_irq_restore(irq_state);
}

int platform_usb_hci_register(void)
{
	return arm64_usb_keyboard_init();
}

void platform_usb_hci_poll(void)
{
	arm64_usb_keyboard_poll();
}

int platform_block_register(void)
{
	return 0;
}
