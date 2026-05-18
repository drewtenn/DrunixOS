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
/*
 * BCM2712 production silicon moved the link / port status bits up
 * from the classic bcm-stb positions (BIT 4/5/7). First-Pi-5-boot
 * trace showed a known-good link returning status 0x0001e080 — none
 * of the BIT 4/5 bits set. The current bit positions are BIT 13/14/16
 * (Raspberry Pi Linux drivers/pci/controller/pcie-brcmstb.c uses the
 * BCM7712 config table at lines 1953-1960 for BCM2712, with masks
 * scattered in the file). BIT(15) was also observed set on the same
 * boot but its meaning is not documented in any reference I found;
 * treating it as informational and not checking it gates bring-up
 * unnecessarily.
 */
#define RASPI5_PCIE_STATUS_PHY_LINKUP (1u << 13)
#define RASPI5_PCIE_STATUS_DL_ACTIVE (1u << 14)
#define RASPI5_PCIE_STATUS_RC_MODE (1u << 16)

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
 * Linux's brcm_pcie_map_bus splits config access into two paths:
 *
 * - Root bus (bus 0): direct, at controller_base + reg. Only dev 0
 *   is valid; other devs on the root bus return -1 / 0xffffffff.
 *   The root port itself appears here as bus 0 dev 0 fn 0.
 *
 * - Downstream buses (1+): indirect via INDEX/DATA. The INDEX
 *   register is written with PCIE_ECAM_OFFSET(bus, devfn, 0) =
 *   (bus << 20) | (devfn << 12). DATA + (reg & 0xfff) reads the
 *   target dword. The first attempt at this on Drunix returned the
 *   controller's master-abort marker 0xdeaddead, which means the
 *   config cycle reached the bridge but no device answered — so
 *   we now scan a small window of buses + devices and log what we
 *   find rather than trusting "RP1 must be at bus 1".
 */
static void raspi5_pcie_trace_u32(const char *label, uint32_t v);
static void raspi5_pcie_trace_u64(const char *label, uint64_t v);

static uint32_t raspi5_pcie_cfg_read_root(uint32_t reg)
{
	return raspi5_pcie_reg_read(reg & 0xfffu);
}

static void raspi5_pcie_cfg_write_root(uint32_t reg, uint32_t value)
{
	raspi5_pcie_reg_write(reg & 0xfffu, value);
}

static uint32_t raspi5_pcie_cfg_read(uint32_t bus, uint32_t dev, uint32_t fn,
                                     uint32_t reg)
{
	uint32_t index;
	uint32_t value;

	if (bus == 0u)
		return raspi5_pcie_cfg_read_root(reg);

	index = (bus << 20) | (((dev & 0x1fu) << 3 | (fn & 0x7u)) << 12);
	raspi5_pcie_reg_write(RASPI5_PCIE_EXT_CFG_INDEX_OFFSET, index);
	value = raspi5_pcie_reg_read(RASPI5_PCIE_EXT_CFG_DATA_OFFSET +
	                             (reg & 0xfffu));
	return value;
}

/*
 * Print a one-line description of a (bus, dev) location's first
 * config dword. Skips slots that report 0xffffffff or the BCM
 * 0xdeaddead master-abort marker.
 */
static void raspi5_pcie_log_slot(uint32_t bus, uint32_t dev)
{
	uint32_t vd = raspi5_pcie_cfg_read(bus, dev, 0u,
	                                   PCI_CONFIG_VENDOR_DEVICE);
	char label[40];
	uint32_t i;
	const char *prefix = "raspi5 pcie: scan b";
	static const char hexd[] = "0123456789abcdef";

	if (vd == 0xffffffffu || vd == 0xdeaddeadu)
		return;

	for (i = 0; prefix[i] != '\0'; i++)
		label[i] = prefix[i];
	label[i++] = hexd[bus & 0xfu];
	label[i++] = 'd';
	label[i++] = hexd[(dev >> 4) & 0xfu];
	label[i++] = hexd[dev & 0xfu];
	label[i] = '\0';
	raspi5_pcie_trace_u32(label, vd);
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

	/* Root port (bus 0 dev 0) lives directly at controller_base + reg.
	 * Reading this proves the controller's config-space window is
	 * live and tells us whether firmware has populated the bridge
	 * config. */
	{
		uint32_t root_vd =
		    raspi5_pcie_cfg_read_root(PCI_CONFIG_VENDOR_DEVICE);
		uint32_t root_class =
		    raspi5_pcie_cfg_read_root(PCI_CONFIG_CLASS_REV);
		uint32_t root_pri_sec =
		    raspi5_pcie_cfg_read_root(0x18u); /* primary / secondary / subordinate */
		raspi5_pcie_trace_u32("raspi5 pcie: root_vd", root_vd);
		raspi5_pcie_trace_u32("raspi5 pcie: root_class", root_class);
		raspi5_pcie_trace_u32("raspi5 pcie: root_bus_nums", root_pri_sec);

		/* Pi 5 EEPROM firmware brings the PCIe link up and assigns
		 * BARs but does NOT do OS-style bridge enumeration: the
		 * root port's primary / secondary / subordinate bus number
		 * register at config offset 0x18 stays zero, which means
		 * the bridge won't forward config cycles to downstream
		 * buses. Without this write, every cfg_read on bus 1
		 * returns the controller's master-abort marker 0xdeaddead.
		 *
		 * Standard PCI bus-number register layout (dword at 0x18):
		 *   bits 7:0    primary bus number (the bus this bridge sits on)
		 *   bits 15:8   secondary bus number (the bus directly behind)
		 *   bits 23:16  subordinate bus number (highest bus reachable)
		 *   bits 31:24  secondary latency timer (ignored)
		 *
		 * For a single-endpoint topology (RP1 is the only thing
		 * downstream), primary=0, secondary=1, subordinate=1 is
		 * the right value: 0x00010100. */
		if ((root_pri_sec & 0xffffffu) == 0u) {
			platform_uart_puts(
			    "raspi5 pcie: firmware left bus numbers zero; programming 0x00010100\n");
			raspi5_pcie_cfg_write_root(0x18u, 0x00010100u);
			root_pri_sec = raspi5_pcie_cfg_read_root(0x18u);
			raspi5_pcie_trace_u32("raspi5 pcie: root_bus_nums (post-write)",
			                      root_pri_sec);
		}
	}

	/* Scan downstream buses 0..3, slot 0 only (PCIe is point-to-
	 * point so multi-slot is meaningless behind a root port). The
	 * BCM controller returns 0xdeaddead for unanswered cycles and
	 * 0xffffffff for plain "nothing there"; raspi5_pcie_log_slot
	 * skips both, so any line that does print is a real device. */
	{
		uint32_t b;
		for (b = 0u; b < 4u; b++)
			raspi5_pcie_log_slot(b, 0u);
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
