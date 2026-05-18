/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pcie.c — BCM2712 PCIe2 root complex sanity probe.
 *
 * Pi 5 EEPROM firmware initializes the PCIe2 link to RP1 before
 * kernel handoff (the kernel debug UART since M5 is RP1 UART0,
 * which is only reachable if firmware did so). The probe here just
 * confirms the link is up, reads RP1's PCIe config space to verify
 * vendor 0x1de4 / device 0x0001 at bus 1 dev 0 fn 0, prints BAR1,
 * and derives the xHCI MMIO bases for M8.2 to consume.
 *
 * The BCM2712 PCIe2 controller uses Broadcom's indirect CAM rather
 * than ECAM for downstream device config access:
 *
 *   PCIE_MISC_PCIE_STATUS  at controller_base + 0x4068
 *   PCIE_EXT_CFG_INDEX     at controller_base + 0x9000
 *   PCIE_EXT_CFG_DATA      at controller_base + 0x8000
 *
 * The INDEX register encodes the bus/dev/fn/reg of the config access;
 * the next read or write at DATA targets that location. Linux's
 * drivers/pci/controller/pcie-brcmstb.c is the canonical reference.
 *
 * No new MMU mapping: controller_base 0x10_0012_0000 sits inside the
 * L1[64] SOC_LOW Device block established in M6.
 */

#include "pcie.h"
#include "platform.h"
#include "../platform.h"
#include <stdint.h>

#define RASPI5_PCIE_STATUS_OFFSET 0x4068u
#define RASPI5_PCIE_STATUS_PHY_LINKUP (1u << 4)
#define RASPI5_PCIE_STATUS_DL_ACTIVE (1u << 5)
#define RASPI5_PCIE_STATUS_RC_MODE (1u << 7)

#define RASPI5_PCIE_EXT_CFG_INDEX_OFFSET 0x9000u
#define RASPI5_PCIE_EXT_CFG_DATA_OFFSET 0x8000u

#define RP1_PCI_BUS 1u
#define RP1_PCI_DEV 0u
#define RP1_PCI_FN 0u
#define RP1_EXPECTED_VENDOR 0x1de4u
#define RP1_EXPECTED_DEVICE 0x0001u

#define PCI_CONFIG_VENDOR_DEVICE 0x00u /* dword: device << 16 | vendor */
#define PCI_CONFIG_CMD_STATUS 0x04u
#define PCI_CONFIG_CLASS_REV 0x08u
#define PCI_CONFIG_BAR1 0x14u

static volatile uint32_t *raspi5_pcie_regs(void)
{
	return (volatile uint32_t *)(uintptr_t)PLATFORM_RASPI5_PCIE_CTRL_BASE;
}

static uint32_t raspi5_pcie_reg_read(uint32_t offset)
{
	return raspi5_pcie_regs()[offset / 4u];
}

static void raspi5_pcie_reg_write(uint32_t offset, uint32_t value)
{
	raspi5_pcie_regs()[offset / 4u] = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Linux's brcm_pcie_map_bus encodes the indirect-CAM address as
 * (bus << 20) | (dev << 15) | (fn << 12) | (reg & 0xfff). For root-
 * bus access this driver writes to INDEX and reads from DATA; the
 * actual config dword lives at DATA + (reg & 0xfff).
 */
static uint32_t raspi5_pcie_cfg_read(uint32_t bus, uint32_t dev, uint32_t fn,
                                     uint32_t reg)
{
	uint32_t index = (bus << 20) | (dev << 15) | (fn << 12);
	uint32_t value;

	raspi5_pcie_reg_write(RASPI5_PCIE_EXT_CFG_INDEX_OFFSET, index);
	value = raspi5_pcie_reg_read(RASPI5_PCIE_EXT_CFG_DATA_OFFSET +
	                             (reg & 0xfffu));
	return value;
}

static void raspi5_pcie_trace_u32(const char *label, uint32_t v)
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

static void raspi5_pcie_trace_u64(const char *label, uint64_t v)
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

int raspi5_pcie_probe_rp1(void)
{
	uint32_t status;
	uint32_t vendor_device;
	uint32_t class_rev;
	uint32_t bar1;
	uint16_t vendor;
	uint16_t device;

	platform_uart_puts("raspi5 pcie: probe start\n");
	raspi5_pcie_trace_u64("raspi5 pcie: ctrl_base",
	                      PLATFORM_RASPI5_PCIE_CTRL_BASE);

	status = raspi5_pcie_reg_read(RASPI5_PCIE_STATUS_OFFSET);
	raspi5_pcie_trace_u32("raspi5 pcie: status", status);
	if ((status & RASPI5_PCIE_STATUS_PHY_LINKUP) == 0u) {
		platform_uart_puts("raspi5 pcie: PHY link down; aborting probe\n");
		return -1;
	}
	if ((status & RASPI5_PCIE_STATUS_DL_ACTIVE) == 0u) {
		platform_uart_puts(
		    "raspi5 pcie: data-link inactive; aborting probe\n");
		return -1;
	}

	vendor_device = raspi5_pcie_cfg_read(RP1_PCI_BUS, RP1_PCI_DEV,
	                                     RP1_PCI_FN,
	                                     PCI_CONFIG_VENDOR_DEVICE);
	raspi5_pcie_trace_u32("raspi5 pcie: vendor_device", vendor_device);
	vendor = (uint16_t)(vendor_device & 0xffffu);
	device = (uint16_t)((vendor_device >> 16) & 0xffffu);

	if (vendor != RP1_EXPECTED_VENDOR || device != RP1_EXPECTED_DEVICE) {
		platform_uart_puts(
		    "raspi5 pcie: vendor / device mismatch; not RP1\n");
		return -1;
	}

	class_rev = raspi5_pcie_cfg_read(RP1_PCI_BUS, RP1_PCI_DEV, RP1_PCI_FN,
	                                 PCI_CONFIG_CLASS_REV);
	raspi5_pcie_trace_u32("raspi5 pcie: class_rev", class_rev);

	bar1 = raspi5_pcie_cfg_read(RP1_PCI_BUS, RP1_PCI_DEV, RP1_PCI_FN,
	                            PCI_CONFIG_BAR1);
	raspi5_pcie_trace_u32("raspi5 pcie: bar1", bar1);

	/* The xHCI host MMIO addresses are derived from the DT
	 * pcie@1000120000/rp1/usb@2/300000 nodes translated through the
	 * standard RP1->PCIe->CPU outbound ranges. They are constant for
	 * Pi 5; the probe prints them for cross-check against BAR1 on
	 * any future board where firmware places BAR1 differently. */
	raspi5_pcie_trace_u64("raspi5 pcie: xhci0_base",
	                      PLATFORM_RASPI5_USB0_XHCI_BASE);
	raspi5_pcie_trace_u64("raspi5 pcie: xhci1_base",
	                      PLATFORM_RASPI5_USB1_XHCI_BASE);

	platform_uart_puts("raspi5 pcie: RP1 reachable\n");
	return 0;
}
