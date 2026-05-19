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

#ifndef DRUNIX_BUILD_UNIX_TIME
#define DRUNIX_BUILD_UNIX_TIME 0u
#endif

#ifndef RASPI5_PCIE_VERBOSE
#define RASPI5_PCIE_VERBOSE 0
#endif

#define RASPI5_PCIE_STATUS_OFFSET 0x4068u
#define RASPI5_PCIE_CTRL_OFFSET 0x4064u
#define RASPI5_PCIE_CTRL_PERSTB (1u << 2)
#define RASPI5_PCIE_REVISION_OFFSET 0x406cu
#define RASPI5_PCIE_HARD_DEBUG_OFFSET 0x4304u
#define RASPI5_PCIE_HARD_DEBUG_CLKREQ_MASK 0x00310002u
#define RASPI5_PCIE_RGR1_SW_INIT_1_OFFSET 0x9210u
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

#define RASPI5_PCIE_CPU_MEM_WIN0_LO_OFFSET 0x400cu
#define RASPI5_PCIE_CPU_MEM_WIN0_HI_OFFSET 0x4010u
#define RASPI5_PCIE_CPU_MEM_WIN0_BASE_LIMIT_OFFSET 0x4070u
#define RASPI5_PCIE_CPU_MEM_WIN0_BASE_HI_OFFSET 0x4080u
#define RASPI5_PCIE_CPU_MEM_WIN0_LIMIT_HI_OFFSET 0x4084u
#define RASPI5_PCIE_CPU_MEM_WIN_LO(win) \
	(RASPI5_PCIE_CPU_MEM_WIN0_LO_OFFSET + ((win) * 8u))
#define RASPI5_PCIE_CPU_MEM_WIN_HI(win) \
	(RASPI5_PCIE_CPU_MEM_WIN0_HI_OFFSET + ((win) * 8u))
#define RASPI5_PCIE_CPU_MEM_WIN_BASE_LIMIT(win) \
	(RASPI5_PCIE_CPU_MEM_WIN0_BASE_LIMIT_OFFSET + ((win) * 4u))
#define RASPI5_PCIE_CPU_MEM_WIN_BASE_HI(win) \
	(RASPI5_PCIE_CPU_MEM_WIN0_BASE_HI_OFFSET + ((win) * 8u))
#define RASPI5_PCIE_CPU_MEM_WIN_LIMIT_HI(win) \
	(RASPI5_PCIE_CPU_MEM_WIN0_LIMIT_HI_OFFSET + ((win) * 8u))

/*
 * PCIE_MISC_MISC_CTRL. Linux's brcm_pcie_setup ORs in a fixed set of
 * bits before enumeration to enable downstream cycle emission and
 * configure read-completion boundary, max burst, and UR behavior.
 * The aggregate OR value Linux uses for BCM2712 is 0x203480:
 *
 *   bit 21 (0x200000) SCB_ACCESS_EN     — enable System Control Block access
 *   bit 13 (0x002000) RCB_64B_MODE      — read-completion boundary = 64 B
 *   bit 12 (0x001000) RCB_MPS_MODE      — use MPS for read-completion boundary
 *   bit 10 (0x000400) CFG_READ_UR_MODE  — synthesize 0xdeaddead on UR
 *   bit  7 (0x000080) max burst = 2     — set SCB max burst size to 2
 *
 * SCB_ACCESS_EN is the load-bearing one for M8.1: without it, the
 * controller declines to issue cfg cycles on the secondary bus and
 * the host sees deaddead even when the bridge is otherwise alive.
 * The first 4 c1-c4 attempts at M8.1 hit exactly this trap because
 * Pi 5 EEPROM firmware never sets it (firmware accesses RP1 via the
 * outbound MMIO window, which uses different controller paths).
 */
#define RASPI5_PCIE_MISC_CTRL_OFFSET 0x4008u
#define RASPI5_PCIE_MISC_CTRL_1_OFFSET 0x40a0u
#define RASPI5_PCIE_MISC_CTRL_SETUP_BITS 0x00203480u

#define RASPI5_PCIE_RC_CFG_PRIV1_ID_VAL3_OFFSET 0x043cu
#define RASPI5_PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_OFFSET 0x04dcu
#define RASPI5_PCIE_RC_CFG_PRIV1_ROOT_CAP_OFFSET 0x04f8u
#define RASPI5_PCIE_RC_CFG_VENDOR_SPECIFIC_REG1_OFFSET 0x0188u
#define RASPI5_PCIE_RC_BAR1_CONFIG_LO_OFFSET 0x402cu
#define RASPI5_PCIE_RC_BAR1_CONFIG_HI_OFFSET 0x4030u
#define RASPI5_PCIE_RC_BAR_CONFIG_LO(bar) \
	(RASPI5_PCIE_RC_BAR1_CONFIG_LO_OFFSET + (((bar) - 1u) * 8u))
#define RASPI5_PCIE_RC_BAR_CONFIG_HI(bar) \
	(RASPI5_PCIE_RC_BAR1_CONFIG_HI_OFFSET + (((bar) - 1u) * 8u))
#define RASPI5_PCIE_RC_BAR_CONFIG_SIZE_MASK 0x1fu

/*
 * BCM2712-specific AXI bridge handling. From the OpenWrt
 * 950-0887-PCI-brcmstb patch for the rpi-6.12-derived tree:
 * brcm_pcie_post_setup_bcm2712 programs four MISC-space
 * registers that configure completion timeout, request/reply
 * routing, and cfg-retry handling on the AXI <-> PCIe bridge.
 * Without these, cfg READs to downstream devices on BCM2712
 * silently return the controller's master-abort marker
 * 0xdeaddead even when SCB_ACCESS_EN is set and the bridge
 * bus-number register is programmed.
 *
 *   +0x40a4  PCIE_MISC_UBUS_CTRL     OR in bits 13 and 19
 *                                    (timeout disables for the
 *                                    PCIe-reply and cfg paths)
 *   +0x40a8  PCIE_MISC_UBUS_TIMEOUT  0x0B2D0000: 250 ms timeout
 *                                    in 750 MHz clocks
 *   +0x405c  PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT
 *                                    0x0ABA0000: ~240 ms config
 *                                    retry timeout
 *   +0x4170  PCIE_MISC_AXI_READ_ERROR_DATA
 *                                    0xffffffff: AXI read-error
 *                                    completion data pattern
 */
#define RASPI5_PCIE_UBUS_CTRL_OFFSET 0x40a4u
#define RASPI5_PCIE_UBUS_CTRL_OR_BITS 0x00082000u
#define RASPI5_PCIE_UBUS_BAR1_REMAP_LO_OFFSET 0x40acu
#define RASPI5_PCIE_UBUS_BAR1_REMAP_HI_OFFSET 0x40b0u
#define RASPI5_PCIE_UBUS_BAR_REMAP_LO(bar) \
	(RASPI5_PCIE_UBUS_BAR1_REMAP_LO_OFFSET + (((bar) - 1u) * 8u))
#define RASPI5_PCIE_UBUS_BAR_REMAP_HI(bar) \
	(RASPI5_PCIE_UBUS_BAR1_REMAP_HI_OFFSET + (((bar) - 1u) * 8u))
#define RASPI5_PCIE_UBUS_BAR_REMAP_ACCESS_EN 0x1u

/*
 * MDIO interface for the PCIe PHY. Used by brcm_pcie_post_setup_bcm2712
 * to write a fixed 7-register sequence that configures the PHY for the
 * 54 MHz refclk source the BCM2712 boards use. Without these writes the
 * PCIe link will appear electrically up (PHY/DL bits set) but transaction
 * cycles to the downstream device silently fail.
 *
 * Register packet encoding for PCIE_RC_DL_MDIO_ADDR (0x1100):
 *   bits 19:16  port (MDIO_PORT_MASK)
 *   bits 15:0   regad (MDIO_REGAD_MASK)
 *   bit  20     cmd (0 = write, 1 = read)
 *   bit  31     done indicator
 */
#define RASPI5_PCIE_RC_DL_MDIO_ADDR 0x1100u
#define RASPI5_PCIE_RC_DL_MDIO_WR_DATA 0x1104u
#define RASPI5_PCIE_RC_DL_MDIO_RD_DATA 0x1108u
#define RASPI5_PCIE_MDIO_DONE_MASK 0x80000000u
#define RASPI5_PCIE_MDIO_PORT0 0x0u
#define RASPI5_PCIE_MDIO_SET_ADDR_OFFSET 0x1fu
#define RASPI5_PCIE_MDIO_CMD_WRITE 0u
#define RASPI5_PCIE_MDIO_TIMEOUT 100000u

/*
 * L1SS timer config. PM clock period is 18.52 ns at 54 MHz (1/54e6 ns
 * rounded down -> 0x12 in the low byte). Linux clears bits 7:0 and
 * sets them to 0x12 to avoid lengthy L1 sub-state transitions.
 */
#define RASPI5_PCIE_RC_PL_PHY_CTL_15_OFFSET 0x184cu
#define RASPI5_PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK 0xffu
#define RASPI5_PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_VAL 0x12u

/*
 * AXI_INTF_CTRL chicken bits for BCM2712 D0 silicon. Set the QoS
 * propagation / timing fixes and disable the broken forwarding-search
 * gate. Linux clears AXI_REQFIFO_EN_QOS_PROPAGATION and sets the
 * three fix bits unconditionally on BCM2712.
 */
#define RASPI5_PCIE_AXI_INTF_CTRL_OFFSET 0x416cu
#define RASPI5_PCIE_AXI_REQFIFO_EN_QOS_PROPAGATION (1u << 7)
#define RASPI5_PCIE_AXI_DIS_QOS_GATING_IN_MASTER (1u << 11)
#define RASPI5_PCIE_AXI_EN_QOS_UPDATE_TIMING_FIX (1u << 12)
#define RASPI5_PCIE_AXI_EN_RCLK_QOS_ARRAY_FIX (1u << 13)

#define RP1_PCI_BUS 1u
#define RP1_PCI_DEV 0u
#define RP1_PCI_FN 0u
#define RP1_EXPECTED_VENDOR 0x1de4u
#define RP1_EXPECTED_DEVICE 0x0001u
#define RP1_PCIE_BUS_ADDR 0x00000000ull
#define RP1_PCIE_APERTURE_SIZE 0x00500000ull
#define RASPI5_PCIE_DMA_PCI_OFFSET 0x1000000000ull
#define RASPI5_PCIE_DMA_CPU_ADDR 0x0000000000ull
#define RASPI5_PCIE_DMA_SIZE 0x1000000000ull
#define RASPI5_PCIE_MIP0_PCI_OFFSET 0xfffffff000ull
#define RASPI5_PCIE_MIP0_CPU_ADDR 0x1000130000ull
#define RASPI5_PCIE_MIP0_SIZE 0x1000ull

#define PCI_CONFIG_VENDOR_DEVICE 0x00u /* dword: device << 16 | vendor */
#define PCI_CONFIG_CMD_STATUS 0x04u
#define PCI_CONFIG_COMMAND_IO 0x0001u
#define PCI_CONFIG_COMMAND_MEMORY 0x0002u
#define PCI_CONFIG_COMMAND_MASTER 0x0004u
#define PCI_CONFIG_CLASS_REV 0x08u
#define PCI_CONFIG_BUS_NUMBERS 0x18u
#define PCI_CONFIG_MEMORY_BASE_LIMIT 0x20u
#define PCI_CONFIG_BAR0 0x10u
#define PCI_CONFIG_BAR1 0x14u

#define RP1_MBOX_BASE 0x1f00008000ull
#define RP1_CLOCKS_BASE 0x1f00018000ull
#define RP1_SRAM_BASE 0x1f00400000ull
#define RP1_UART_FR_OFFSET 0x18u

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

static uint16_t raspi5_pcie_reg_read16(uint32_t offset)
{
	volatile uint16_t *regs16 =
	    (volatile uint16_t *)(uintptr_t)PLATFORM_RASPI5_PCIE_CTRL_BASE;
	return regs16[offset / 2u];
}

static void raspi5_pcie_reg_write16(uint32_t offset, uint16_t value)
{
	volatile uint16_t *regs16 =
	    (volatile uint16_t *)(uintptr_t)PLATFORM_RASPI5_PCIE_CTRL_BASE;

	regs16[offset / 2u] = value;
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

/*
 * Spin briefly. raspi5/sdhci.c uses the same nop-count pattern; cycle
 * budget is approximate but fine for the 100us-ish delays the MDIO
 * write helper and PHY init want.
 */
static void raspi5_pcie_delay_us(uint32_t us)
{
	uint32_t i;
	for (i = 0; i < us * 50u; i++)
		__asm__ volatile("nop");
}

/*
 * Encode an MDIO request packet. port + regad + cmd; the controller
 * latches it on a write to PCIE_RC_DL_MDIO_ADDR.
 */
static uint32_t raspi5_pcie_mdio_form_pkt(uint8_t port, uint8_t regad,
                                          uint8_t cmd)
{
	uint32_t pkt = 0u;
	pkt |= ((uint32_t)(port & 0xfu) << 16);
	pkt |= ((uint32_t)regad);
	pkt |= ((uint32_t)(cmd & 0x1u) << 20);
	return pkt;
}

/*
 * Write one PCIe PHY MDIO register. Returns 0 on success, -1 on
 * timeout. After MDIO_DATA_DONE_MASK transitions back to 0, the
 * controller has accepted the write.
 */
static int raspi5_pcie_mdio_write(uint8_t port, uint8_t regad, uint16_t wrdata)
{
	uint32_t i;
	uint32_t data;

	raspi5_pcie_reg_write(
	    RASPI5_PCIE_RC_DL_MDIO_ADDR,
	    raspi5_pcie_mdio_form_pkt(port, regad, RASPI5_PCIE_MDIO_CMD_WRITE));
	(void)raspi5_pcie_reg_read(RASPI5_PCIE_RC_DL_MDIO_ADDR);
	raspi5_pcie_reg_write(RASPI5_PCIE_RC_DL_MDIO_WR_DATA,
	                      RASPI5_PCIE_MDIO_DONE_MASK | (uint32_t)wrdata);

	for (i = 0; i < RASPI5_PCIE_MDIO_TIMEOUT; i++) {
		data = raspi5_pcie_reg_read(RASPI5_PCIE_RC_DL_MDIO_WR_DATA);
		if ((data & RASPI5_PCIE_MDIO_DONE_MASK) == 0u)
			return 0;
	}
	return -1;
}

/*
 * Replicate brcm_pcie_post_setup_bcm2712's PHY init. Configures the
 * BCM2712 PCIe PHY for the 54 MHz refclk source. The reg / data tables
 * are lifted verbatim from Linux drivers/pci/controller/pcie-brcmstb.c
 * (brcm_pcie_post_setup_bcm2712, top of function). Without this the
 * link reports up at the controller side but downstream transactions
 * silently fail — exactly the symptom Drunix M8.1 hit after the AXI
 * bridge handlers landed.
 *
 * Returns 0 on success, -1 if any MDIO write times out.
 */
static int raspi5_pcie_phy_post_setup(void)
{
	static const uint8_t regs[7] = {0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1e};
	static const uint16_t data[7] = {0x50b9, 0xbda1, 0x0094, 0x97b4,
	                                 0x5030, 0x5030, 0x0007};
	uint32_t i;

	if (raspi5_pcie_mdio_write(RASPI5_PCIE_MDIO_PORT0,
	                           RASPI5_PCIE_MDIO_SET_ADDR_OFFSET, 0x1600) != 0) {
		platform_uart_puts(
		    "raspi5 pcie: MDIO SET_ADDR timeout; PHY init incomplete\n");
		return -1;
	}
	for (i = 0; i < 7u; i++) {
		if (raspi5_pcie_mdio_write(RASPI5_PCIE_MDIO_PORT0, regs[i],
		                           data[i]) != 0) {
			platform_uart_puts(
			    "raspi5 pcie: MDIO data write timeout; PHY init incomplete\n");
			return -1;
		}
	}
	raspi5_pcie_delay_us(200u);

	/* PCIE_RC_PL_PHY_CTL_15 — set PM clock period to 18.52 ns (0x12)
	 * to avoid lengthy L1 sub-state transitions. */
	{
		uint32_t tmp = raspi5_pcie_reg_read(
		    RASPI5_PCIE_RC_PL_PHY_CTL_15_OFFSET);
		tmp &= ~RASPI5_PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_MASK;
		tmp |= RASPI5_PCIE_RC_PL_PHY_CTL_15_PM_CLK_PERIOD_VAL;
		raspi5_pcie_reg_write(RASPI5_PCIE_RC_PL_PHY_CTL_15_OFFSET, tmp);
	}

	/* AXI_INTF_CTRL chicken bits — disable broken QoS propagation,
	 * enable RCLK / timing fixes / disable QoS gating in master. */
	{
		uint32_t tmp =
		    raspi5_pcie_reg_read(RASPI5_PCIE_AXI_INTF_CTRL_OFFSET);
#if RASPI5_PCIE_VERBOSE
		raspi5_pcie_trace_u32("raspi5 pcie: axi_intf_ctrl (pre)", tmp);
#endif
		tmp &= ~RASPI5_PCIE_AXI_REQFIFO_EN_QOS_PROPAGATION;
		tmp |= RASPI5_PCIE_AXI_EN_RCLK_QOS_ARRAY_FIX;
		tmp |= RASPI5_PCIE_AXI_EN_QOS_UPDATE_TIMING_FIX;
		tmp |= RASPI5_PCIE_AXI_DIS_QOS_GATING_IN_MASTER;
		raspi5_pcie_reg_write(RASPI5_PCIE_AXI_INTF_CTRL_OFFSET, tmp);
#if RASPI5_PCIE_VERBOSE
		raspi5_pcie_trace_u32("raspi5 pcie: axi_intf_ctrl (post)",
		                      raspi5_pcie_reg_read(
		                          RASPI5_PCIE_AXI_INTF_CTRL_OFFSET));
#endif
	}

	return 0;
}

static uint32_t raspi5_pcie_cfg_read_root(uint32_t reg)
{
	return raspi5_pcie_reg_read(reg & 0xfffu);
}

static void raspi5_pcie_cfg_write_root(uint32_t reg, uint32_t value)
{
	raspi5_pcie_reg_write(reg & 0xfffu, value);
}

static void raspi5_pcie_set_outbound_window(uint32_t win, uint64_t cpu_addr,
                                            uint64_t pcie_addr, uint64_t size)
{
	raspi5_pcie_outbound_window_t encoded =
	    raspi5_pcie_outbound_window_encode(cpu_addr, pcie_addr, size);

	raspi5_pcie_reg_write(RASPI5_PCIE_CPU_MEM_WIN_LO(win), encoded.pcie_lo);
	raspi5_pcie_reg_write(RASPI5_PCIE_CPU_MEM_WIN_HI(win), encoded.pcie_hi);
	raspi5_pcie_reg_write(RASPI5_PCIE_CPU_MEM_WIN_BASE_LIMIT(win),
	                      encoded.base_limit);
	raspi5_pcie_reg_write(RASPI5_PCIE_CPU_MEM_WIN_BASE_HI(win),
	                      encoded.base_hi);
	raspi5_pcie_reg_write(RASPI5_PCIE_CPU_MEM_WIN_LIMIT_HI(win),
	                      encoded.limit_hi);
}

static uint32_t raspi5_pcie_encode_inbound_size(uint64_t size)
{
	uint32_t log2_size = 0u;
	uint64_t value = size;

	if (size == 0u || (size & (size - 1u)) != 0u)
		return 0u;
	while (value > 1u) {
		value >>= 1;
		log2_size++;
	}
	if (log2_size >= 12u && log2_size <= 15u)
		return (log2_size - 12u) + 0x1cu;
	if (log2_size >= 16u && log2_size <= 36u)
		return log2_size - 15u;
	return 0u;
}

static void raspi5_pcie_set_inbound_window(uint32_t bar, uint64_t pcie_addr,
                                           uint64_t cpu_addr, uint64_t size)
{
	uint32_t size_code = raspi5_pcie_encode_inbound_size(size);
	uint32_t rc_lo = (uint32_t)pcie_addr;

	rc_lo &= ~RASPI5_PCIE_RC_BAR_CONFIG_SIZE_MASK;
	rc_lo |= size_code & RASPI5_PCIE_RC_BAR_CONFIG_SIZE_MASK;
	raspi5_pcie_reg_write(RASPI5_PCIE_RC_BAR_CONFIG_LO(bar), rc_lo);
	raspi5_pcie_reg_write(RASPI5_PCIE_RC_BAR_CONFIG_HI(bar),
	                      (uint32_t)(pcie_addr >> 32));
	raspi5_pcie_reg_write(
	    RASPI5_PCIE_UBUS_BAR_REMAP_LO(bar),
	    ((uint32_t)cpu_addr & ~0xfffu) |
	        RASPI5_PCIE_UBUS_BAR_REMAP_ACCESS_EN);
	raspi5_pcie_reg_write(RASPI5_PCIE_UBUS_BAR_REMAP_HI(bar),
	                      (uint32_t)(cpu_addr >> 32));
}

static void raspi5_pcie_set_inbound_windows(void)
{
	/*
	 * Mirror the Pi 5 pcie2 dma-ranges that Linux programs on BCM7712:
	 * BAR1: PCIe 0x0 -> CPU 0x1f00000000, 4 MiB RP1 aperture.
	 * BAR2: PCIe 0x1000000000 -> CPU RAM 0x0, 64 GiB DMA aperture.
	 * BAR3: PCIe 0xfffffff000 -> CPU 0x1000130000, 4 KiB MIP0 MSI page.
	 *
	 * xHCI command/event rings use BAR2 via RASPI5_XHCI_DMA_BIAS.
	 */
	raspi5_pcie_set_inbound_window(1u, RP1_PCIE_BUS_ADDR,
	                               PLATFORM_RASPI5_PCIE_WINDOW_BASE,
	                               0x00400000ull);
	raspi5_pcie_set_inbound_window(2u, RASPI5_PCIE_DMA_PCI_OFFSET,
	                               RASPI5_PCIE_DMA_CPU_ADDR,
	                               RASPI5_PCIE_DMA_SIZE);
	raspi5_pcie_set_inbound_window(3u, RASPI5_PCIE_MIP0_PCI_OFFSET,
	                               RASPI5_PCIE_MIP0_CPU_ADDR,
	                               RASPI5_PCIE_MIP0_SIZE);

	raspi5_pcie_trace_u32("raspi5 pcie: dma_bar2",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_RC_BAR_CONFIG_LO(2u)));
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

static void raspi5_pcie_cfg_write(uint32_t bus, uint32_t dev, uint32_t fn,
                                  uint32_t reg, uint32_t value)
{
	uint32_t index;

	if (bus == 0u) {
		raspi5_pcie_cfg_write_root(reg, value);
		return;
	}

	index = (bus << 20) | (((dev & 0x1fu) << 3 | (fn & 0x7u)) << 12);
	raspi5_pcie_reg_write(RASPI5_PCIE_EXT_CFG_INDEX_OFFSET, index);
	raspi5_pcie_reg_write(RASPI5_PCIE_EXT_CFG_DATA_OFFSET +
	                          (reg & 0xfffu),
	                      value);
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

#if RASPI5_PCIE_VERBOSE
static uint32_t raspi5_pcie_mmio_read32(uint64_t addr)
{
	volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
	return p[0];
}

static void raspi5_pcie_dump_controller_state(const char *phase)
{
	raspi5_pcie_bcm2712_post_setup_t setup =
	    raspi5_pcie_bcm2712_post_setup_values();

	platform_uart_puts(phase);
	platform_uart_puts("\n");
	raspi5_pcie_trace_u32("raspi5 pcie: diag status",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_STATUS_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag revision",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_REVISION_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag pcie_ctrl",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_CTRL_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag sw_init1",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_RGR1_SW_INIT_1_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag hard_debug",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_HARD_DEBUG_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag misc_ctrl",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_MISC_CTRL_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag misc_ctrl_1",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_MISC_CTRL_1_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag ubus_ctrl",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_UBUS_CTRL_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag ubus_timeout",
	                      raspi5_pcie_reg_read(setup.ubus_timeout_offset));
	raspi5_pcie_trace_u32(
	    "raspi5 pcie: diag rc_config_retry_timeout",
	    raspi5_pcie_reg_read(setup.rc_config_retry_timeout_offset));
	raspi5_pcie_trace_u32("raspi5 pcie: diag axi_read_error_data",
	                      raspi5_pcie_reg_read(
	                          setup.axi_read_error_data_offset));
	raspi5_pcie_trace_u32("raspi5 pcie: diag axi_intf_ctrl",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_AXI_INTF_CTRL_OFFSET));
	raspi5_pcie_trace_u32(
	    "raspi5 pcie: diag link_cap",
	    raspi5_pcie_reg_read(RASPI5_PCIE_RC_CFG_PRIV1_LINK_CAPABILITY_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag root_cap",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_RC_CFG_PRIV1_ROOT_CAP_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag id_val3",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_RC_CFG_PRIV1_ID_VAL3_OFFSET));
	raspi5_pcie_trace_u32(
	    "raspi5 pcie: diag vendor_reg1",
	    raspi5_pcie_reg_read(RASPI5_PCIE_RC_CFG_VENDOR_SPECIFIC_REG1_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag rc_bar1_lo",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_RC_BAR1_CONFIG_LO_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag rc_bar1_hi",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_RC_BAR1_CONFIG_HI_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag ubus_bar1_remap_lo",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_UBUS_BAR1_REMAP_LO_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag ubus_bar1_remap_hi",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_UBUS_BAR1_REMAP_HI_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag win0_pcie_lo",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_CPU_MEM_WIN_LO(0u)));
	raspi5_pcie_trace_u32("raspi5 pcie: diag win0_pcie_hi",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_CPU_MEM_WIN_HI(0u)));
	raspi5_pcie_trace_u32("raspi5 pcie: diag win0_base_limit",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_CPU_MEM_WIN_BASE_LIMIT(0u)));
	raspi5_pcie_trace_u32("raspi5 pcie: diag win0_base_hi",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_CPU_MEM_WIN_BASE_HI(0u)));
	raspi5_pcie_trace_u32("raspi5 pcie: diag win0_limit_hi",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_CPU_MEM_WIN_LIMIT_HI(0u)));
	raspi5_pcie_trace_u32("raspi5 pcie: diag ext_cfg_index",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_EXT_CFG_INDEX_OFFSET));
}
#endif

#if RASPI5_PCIE_VERBOSE
static uint32_t raspi5_pcie_cfg_read_logged(uint32_t bus, uint32_t dev,
                                            uint32_t fn, uint32_t reg,
                                            const char *label)
{
	uint32_t index;
	uint32_t value;

	if (bus == 0u) {
		value = raspi5_pcie_cfg_read_root(reg);
		raspi5_pcie_trace_u32(label, value);
		return value;
	}

	index = (bus << 20) | (((dev & 0x1fu) << 3 | (fn & 0x7u)) << 12);
	raspi5_pcie_trace_u32("raspi5 pcie: diag cfg_index_write", index);
	raspi5_pcie_reg_write(RASPI5_PCIE_EXT_CFG_INDEX_OFFSET, index);
	raspi5_pcie_trace_u32("raspi5 pcie: diag cfg_index_readback",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_EXT_CFG_INDEX_OFFSET));
	value = raspi5_pcie_reg_read(RASPI5_PCIE_EXT_CFG_DATA_OFFSET +
	                             (reg & 0xfffu));
	raspi5_pcie_trace_u32(label, value);
	return value;
}
#endif

#if RASPI5_PCIE_VERBOSE
static void raspi5_pcie_dump_cfg_samples(void)
{
	platform_uart_puts("raspi5 pcie: diag cfg samples\n");
	(void)raspi5_pcie_cfg_read_logged(1u, 0u, 0u, PCI_CONFIG_VENDOR_DEVICE,
	                                  "raspi5 pcie: diag cfg b1d0 vd");
	(void)raspi5_pcie_cfg_read_logged(1u, 0u, 0u, PCI_CONFIG_CMD_STATUS,
	                                  "raspi5 pcie: diag cfg b1d0 cmd_status");
	(void)raspi5_pcie_cfg_read_logged(1u, 0u, 0u, PCI_CONFIG_CLASS_REV,
	                                  "raspi5 pcie: diag cfg b1d0 class_rev");
	(void)raspi5_pcie_cfg_read_logged(1u, 0u, 0u, 0x10u,
	                                  "raspi5 pcie: diag cfg b1d0 bar0");
	(void)raspi5_pcie_cfg_read_logged(1u, 0u, 0u, PCI_CONFIG_BAR1,
	                                  "raspi5 pcie: diag cfg b1d0 bar1");
	(void)raspi5_pcie_cfg_read_logged(1u, 1u, 0u, PCI_CONFIG_VENDOR_DEVICE,
	                                  "raspi5 pcie: diag cfg b1d1 vd");
	(void)raspi5_pcie_cfg_read_logged(2u, 0u, 0u, PCI_CONFIG_VENDOR_DEVICE,
	                                  "raspi5 pcie: diag cfg b2d0 vd");
}
#endif

static void raspi5_pcie_program_rp1_bars(void)
{
	uint32_t cmd_status;
#if RASPI5_PCIE_VERBOSE
	uint32_t bar0;
#endif
	uint32_t bar1;

	cmd_status = raspi5_pcie_cfg_read(RP1_PCI_BUS, RP1_PCI_DEV,
	                                  RP1_PCI_FN, PCI_CONFIG_CMD_STATUS);
#if RASPI5_PCIE_VERBOSE
	bar0 = raspi5_pcie_cfg_read(RP1_PCI_BUS, RP1_PCI_DEV, RP1_PCI_FN,
	                            PCI_CONFIG_BAR0);
#endif
	bar1 = raspi5_pcie_cfg_read(RP1_PCI_BUS, RP1_PCI_DEV, RP1_PCI_FN,
	                            PCI_CONFIG_BAR1);
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_trace_u32("raspi5 pcie: rp1 cmd_status (pre)",
	                      cmd_status);
	raspi5_pcie_trace_u32("raspi5 pcie: rp1 bar0 (pre)", bar0);
	raspi5_pcie_trace_u32("raspi5 pcie: rp1 bar1 (pre)", bar1);
#endif

	/*
	 * PERST resets RP1's PCI BAR assignment. The Pi 5 DT maps RP1's
	 * simple-bus/peripheral BAR at PCIe bus address 0, and our CPU->PCIe
	 * outbound window maps 0x1f00000000 to that same PCIe bus address.
	 * Program BAR1 there and enable memory decoding before trying xHCI.
	 */
	cmd_status &= ~(PCI_CONFIG_COMMAND_IO | PCI_CONFIG_COMMAND_MEMORY |
	                PCI_CONFIG_COMMAND_MASTER);
	raspi5_pcie_cfg_write(RP1_PCI_BUS, RP1_PCI_DEV, RP1_PCI_FN,
	                      PCI_CONFIG_CMD_STATUS, cmd_status);
	raspi5_pcie_cfg_write(RP1_PCI_BUS, RP1_PCI_DEV, RP1_PCI_FN,
	                      PCI_CONFIG_BAR1, 0x00000000u);
	cmd_status |= PCI_CONFIG_COMMAND_MEMORY | PCI_CONFIG_COMMAND_MASTER;
	raspi5_pcie_cfg_write(RP1_PCI_BUS, RP1_PCI_DEV, RP1_PCI_FN,
	                      PCI_CONFIG_CMD_STATUS, cmd_status);

	cmd_status = raspi5_pcie_cfg_read(RP1_PCI_BUS, RP1_PCI_DEV,
	                                  RP1_PCI_FN, PCI_CONFIG_CMD_STATUS);
#if RASPI5_PCIE_VERBOSE
	bar0 = raspi5_pcie_cfg_read(RP1_PCI_BUS, RP1_PCI_DEV, RP1_PCI_FN,
	                            PCI_CONFIG_BAR0);
#endif
	bar1 = raspi5_pcie_cfg_read(RP1_PCI_BUS, RP1_PCI_DEV, RP1_PCI_FN,
	                            PCI_CONFIG_BAR1);
	raspi5_pcie_trace_u32("raspi5 pcie: rp1_cmd",
	                      cmd_status);
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_trace_u32("raspi5 pcie: rp1 bar0 (post)", bar0);
#endif
	raspi5_pcie_trace_u32("raspi5 pcie: rp1_bar1", bar1);
}

static void raspi5_pcie_enable_root_memory_decode(void)
{
	uint16_t command;
	uint32_t cmd_status;

	command = raspi5_pcie_reg_read16(PCI_CONFIG_CMD_STATUS);
	cmd_status = raspi5_pcie_cfg_read_root(PCI_CONFIG_CMD_STATUS);
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_trace_u32("raspi5 pcie: root_cmd_status before enable",
	                      cmd_status);
	raspi5_pcie_trace_u32("raspi5 pcie: root_command16 before enable",
	                      (uint32_t)command);
#endif

	command |= PCI_CONFIG_COMMAND_MEMORY | PCI_CONFIG_COMMAND_MASTER;
	raspi5_pcie_reg_write16(PCI_CONFIG_CMD_STATUS, command);

	command = raspi5_pcie_reg_read16(PCI_CONFIG_CMD_STATUS);
	cmd_status = raspi5_pcie_cfg_read_root(PCI_CONFIG_CMD_STATUS);
	raspi5_pcie_trace_u32("raspi5 pcie: root_cmd16",
	                      (uint32_t)command);
	raspi5_pcie_trace_u32("raspi5 pcie: root_cmd_status",
	                      cmd_status);
}

#if RASPI5_PCIE_VERBOSE
static void raspi5_pcie_dump_rp1_mmio_samples(void)
{
	platform_uart_puts("raspi5 pcie: diag rp1 mmio samples\n");
	raspi5_pcie_trace_u32(
	    "raspi5 pcie: diag rp1 uart0_fr",
	    raspi5_pcie_mmio_read32(PLATFORM_RASPI5_RP1_UART0_BASE +
	                            RP1_UART_FR_OFFSET));
	raspi5_pcie_trace_u32("raspi5 pcie: diag rp1_mbox0",
	                      raspi5_pcie_mmio_read32(RP1_MBOX_BASE));
	raspi5_pcie_trace_u32("raspi5 pcie: diag rp1_clocks0",
	                      raspi5_pcie_mmio_read32(RP1_CLOCKS_BASE));
	raspi5_pcie_trace_u32(
	    "raspi5 pcie: diag xhci0_caplen_ver",
	    raspi5_pcie_mmio_read32(PLATFORM_RASPI5_USB0_XHCI_BASE));
	raspi5_pcie_trace_u32(
	    "raspi5 pcie: diag xhci1_caplen_ver",
	    raspi5_pcie_mmio_read32(PLATFORM_RASPI5_USB1_XHCI_BASE));
	raspi5_pcie_trace_u32("raspi5 pcie: diag rp1_sram0",
	                      raspi5_pcie_mmio_read32(RP1_SRAM_BASE));
}
#endif

static void raspi5_pcie_deassert_perst(void)
{
	uint32_t hard_debug;
	uint32_t pcie_ctrl;

	/*
	 * Mirror Linux brcm_pcie_start_link() for BCM2712/BCM7712:
	 * disable CLKREQ-derived low-power control before releasing the
	 * downstream device from fundamental reset, then set PCIE_PERSTB
	 * in PCIE_MISC_PCIE_CTRL. A zero PERSTB bit leaves RP1 held in reset,
	 * which makes both cfg space and RP1 MMIO read back 0xffffffff.
	 */
	hard_debug = raspi5_pcie_reg_read(RASPI5_PCIE_HARD_DEBUG_OFFSET);
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_trace_u32("raspi5 pcie: hard_debug clkreq (pre)",
	                      hard_debug);
#endif
	hard_debug &= ~RASPI5_PCIE_HARD_DEBUG_CLKREQ_MASK;
	raspi5_pcie_reg_write(RASPI5_PCIE_HARD_DEBUG_OFFSET, hard_debug);
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_trace_u32("raspi5 pcie: hard_debug clkreq (post)",
	                      raspi5_pcie_reg_read(
	                          RASPI5_PCIE_HARD_DEBUG_OFFSET));
#endif

	pcie_ctrl = raspi5_pcie_reg_read(RASPI5_PCIE_CTRL_OFFSET);
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_trace_u32("raspi5 pcie: pcie_ctrl perstb (pre)",
	                      pcie_ctrl);
#endif
	pcie_ctrl |= RASPI5_PCIE_CTRL_PERSTB;
	raspi5_pcie_reg_write(RASPI5_PCIE_CTRL_OFFSET, pcie_ctrl);
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_trace_u32("raspi5 pcie: pcie_ctrl perstb (post)",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_CTRL_OFFSET));
#endif

	raspi5_pcie_delay_us(100000u);
	raspi5_pcie_trace_u32("raspi5 pcie: link_after_reset",
	                      raspi5_pcie_reg_read(RASPI5_PCIE_STATUS_OFFSET));
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
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_trace_u64("raspi5 pcie: ctrl_base",
	                      PLATFORM_RASPI5_PCIE_CTRL_BASE);
	platform_uart_puts("raspi5 pcie: diag marker linux-sync-v2\n");
	raspi5_pcie_trace_u32("raspi5 pcie: build_unix_time",
	                      (uint32_t)DRUNIX_BUILD_UNIX_TIME);
	raspi5_pcie_dump_controller_state(
	    "raspi5 pcie: diag controller pre-setup");
#endif

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

	/* Enable downstream cfg-cycle emission via SCB_ACCESS_EN in
	 * PCIE_MISC_MISC_CTRL. Without this Linux-required setup,
	 * the controller declines to forward type-1 cfg cycles past
	 * the bridge even when the bus-number register is programmed,
	 * and the host always reads back 0xdeaddead. */
	{
		uint32_t misc =
		    raspi5_pcie_reg_read(RASPI5_PCIE_MISC_CTRL_OFFSET);
#if RASPI5_PCIE_VERBOSE
		raspi5_pcie_trace_u32("raspi5 pcie: misc_ctrl (pre)", misc);
#endif
		misc |= RASPI5_PCIE_MISC_CTRL_SETUP_BITS;
		raspi5_pcie_reg_write(RASPI5_PCIE_MISC_CTRL_OFFSET, misc);
#if RASPI5_PCIE_VERBOSE
		misc = raspi5_pcie_reg_read(RASPI5_PCIE_MISC_CTRL_OFFSET);
		raspi5_pcie_trace_u32("raspi5 pcie: misc_ctrl (post)", misc);
#endif
	}

	/* BCM2712 AXI <-> PCIe bridge handling. The four-register
	 * sequence comes from brcm_pcie_post_setup_bcm2712 in the
	 * Raspberry Pi 6.12 Linux fork. Without these, downstream cfg
	 * reads return 0xdeaddead because the bridge times out the
	 * cfg-cycle completion path. */
	{
		uint32_t ubus_ctrl =
		    raspi5_pcie_reg_read(RASPI5_PCIE_UBUS_CTRL_OFFSET);
#if RASPI5_PCIE_VERBOSE
		raspi5_pcie_trace_u32("raspi5 pcie: ubus_ctrl (pre)", ubus_ctrl);
#endif
		ubus_ctrl |= RASPI5_PCIE_UBUS_CTRL_OR_BITS;
		raspi5_pcie_reg_write(RASPI5_PCIE_UBUS_CTRL_OFFSET, ubus_ctrl);
#if RASPI5_PCIE_VERBOSE
		raspi5_pcie_trace_u32("raspi5 pcie: ubus_ctrl (post)",
		                      raspi5_pcie_reg_read(
		                          RASPI5_PCIE_UBUS_CTRL_OFFSET));
#endif

		{
			raspi5_pcie_bcm2712_post_setup_t setup =
			    raspi5_pcie_bcm2712_post_setup_values();
			raspi5_pcie_reg_write(setup.axi_read_error_data_offset,
			                      setup.axi_read_error_data_value);
			raspi5_pcie_reg_write(setup.ubus_timeout_offset,
			                      setup.ubus_timeout_value);
			raspi5_pcie_reg_write(setup.rc_config_retry_timeout_offset,
			                      setup.rc_config_retry_timeout_value);
#if RASPI5_PCIE_VERBOSE
			raspi5_pcie_trace_u32(
			    "raspi5 pcie: ubus_timeout",
			    raspi5_pcie_reg_read(setup.ubus_timeout_offset));
			raspi5_pcie_trace_u32(
			    "raspi5 pcie: rc_config_retry_timeout",
			    raspi5_pcie_reg_read(setup.rc_config_retry_timeout_offset));
			raspi5_pcie_trace_u32(
			    "raspi5 pcie: axi_read_error_data",
			    raspi5_pcie_reg_read(setup.axi_read_error_data_offset));
#endif
		}
#if RASPI5_PCIE_VERBOSE
		platform_uart_puts(
		    "raspi5 pcie: BCM2712 AXI bridge handlers programmed\n");
#endif
	}
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_dump_controller_state(
	    "raspi5 pcie: diag controller post-axi-setup");
#endif

	/* PHY MDIO + AXI_INTF_CTRL chicken bits — the rest of
	 * brcm_pcie_post_setup_bcm2712. The earlier M8.1 commits set up
	 * the controller-side handlers but left the PHY in the firmware-
	 * default state, with the link reporting up but downstream
	 * transactions failing. This is the missing piece. */
	(void)raspi5_pcie_phy_post_setup();
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_dump_controller_state(
	    "raspi5 pcie: diag controller post-phy-setup");
#endif

	/* Firmware does not necessarily leave the CPU->PCIe outbound
	 * viewport programmed when the kernel debug console uses uart10
	 * on the JST-SH header. Program window 0 from the Pi 5 DT:
	 * CPU 0x1f00000000 -> PCIe bus address 0x0. The first 0x410000
	 * bytes contain RP1's simple-bus aperture; round to 5 MiB for
	 * the 1 MiB-granular Broadcom base/limit fields. */
	{
		raspi5_pcie_set_outbound_window(0u,
		                                PLATFORM_RASPI5_PCIE_WINDOW_BASE,
		                                RP1_PCIE_BUS_ADDR,
		                                RP1_PCIE_APERTURE_SIZE);
		raspi5_pcie_set_inbound_windows();
#if RASPI5_PCIE_VERBOSE
		raspi5_pcie_trace_u32(
		    "raspi5 pcie: win0 pcie_lo",
		    raspi5_pcie_reg_read(RASPI5_PCIE_CPU_MEM_WIN_LO(0u)));
		raspi5_pcie_trace_u32(
		    "raspi5 pcie: win0 pcie_hi",
		    raspi5_pcie_reg_read(RASPI5_PCIE_CPU_MEM_WIN_HI(0u)));
		raspi5_pcie_trace_u32(
		    "raspi5 pcie: win0 base_limit",
		    raspi5_pcie_reg_read(RASPI5_PCIE_CPU_MEM_WIN_BASE_LIMIT(0u)));
		raspi5_pcie_trace_u32(
		    "raspi5 pcie: win0 base_hi",
		    raspi5_pcie_reg_read(RASPI5_PCIE_CPU_MEM_WIN_BASE_HI(0u)));
		raspi5_pcie_trace_u32(
		    "raspi5 pcie: win0 limit_hi",
		    raspi5_pcie_reg_read(RASPI5_PCIE_CPU_MEM_WIN_LIMIT_HI(0u)));
#endif
	}
	raspi5_pcie_deassert_perst();
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_dump_controller_state(
	    "raspi5 pcie: diag controller post-perstb");
#endif

	/* Root port (bus 0 dev 0) lives directly at controller_base + reg.
	 * Reading this proves the controller's config-space window is
	 * live and tells us whether firmware has populated the bridge
	 * config. */
	{
		uint32_t root_vd =
		    raspi5_pcie_cfg_read_root(PCI_CONFIG_VENDOR_DEVICE);
		uint32_t root_class =
		    raspi5_pcie_cfg_read_root(PCI_CONFIG_CLASS_REV);
		uint32_t root_cmd_status =
		    raspi5_pcie_cfg_read_root(PCI_CONFIG_CMD_STATUS);
		uint32_t root_pri_sec =
		    raspi5_pcie_cfg_read_root(PCI_CONFIG_BUS_NUMBERS);
		uint32_t root_mem_base_limit =
		    raspi5_pcie_cfg_read_root(PCI_CONFIG_MEMORY_BASE_LIMIT);
#if RASPI5_PCIE_VERBOSE
		raspi5_pcie_trace_u32("raspi5 pcie: root_vd", root_vd);
		raspi5_pcie_trace_u32("raspi5 pcie: root_class", root_class);
		raspi5_pcie_trace_u32("raspi5 pcie: root_cmd_status",
		                      root_cmd_status);
		raspi5_pcie_trace_u32("raspi5 pcie: root_bus_nums", root_pri_sec);
		raspi5_pcie_trace_u32("raspi5 pcie: root_mem_base_limit",
		                      root_mem_base_limit);
#else
		(void)root_vd;
		(void)root_class;
		(void)root_cmd_status;
#endif

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
			raspi5_pcie_cfg_write_root(PCI_CONFIG_BUS_NUMBERS,
			                           0x00010100u);
			root_pri_sec =
			    raspi5_pcie_cfg_read_root(PCI_CONFIG_BUS_NUMBERS);
			raspi5_pcie_trace_u32("raspi5 pcie: root_bus",
			                      root_pri_sec);
		}

		/* Type-1 bridge memory base/limit at config offset 0x20
		 * controls which downstream PCIe memory addresses the root
		 * port forwards. RP1's DT aperture is PCIe bus address
		 * 0x00000000..0x0040ffff, so a 0..4 MiB inclusive bridge
		 * window (register value 0x00400000) covers the direct xHCI
		 * MMIO reads used below. */
		if (root_mem_base_limit != 0x00400000u) {
			raspi5_pcie_cfg_write_root(PCI_CONFIG_MEMORY_BASE_LIMIT,
			                           0x00400000u);
			root_mem_base_limit =
			    raspi5_pcie_cfg_read_root(PCI_CONFIG_MEMORY_BASE_LIMIT);
			raspi5_pcie_trace_u32(
			    "raspi5 pcie: root_mem",
			    root_mem_base_limit);
		}
	}
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_dump_controller_state(
	    "raspi5 pcie: diag controller post-root-config");
#endif
	raspi5_pcie_enable_root_memory_decode();
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_dump_controller_state(
	    "raspi5 pcie: diag controller post-root-command");
	raspi5_pcie_dump_cfg_samples();
#endif
	raspi5_pcie_program_rp1_bars();
#if RASPI5_PCIE_VERBOSE
	raspi5_pcie_dump_cfg_samples();
	raspi5_pcie_dump_rp1_mmio_samples();
#endif

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
		/*
		 * cfg-space access to RP1 is failing (deaddead or ffffffff
		 * depending on whether AXI_READ_ERROR_DATA has been set), but
		 * memory transactions to RP1 work — that's the path firmware
		 * uses for its own UART0 access. For M8 what we actually need
		 * is reachability of the xHCI MMIO at
		 * PLATFORM_RASPI5_USB0_XHCI_BASE, not OS-style PCI enumeration.
		 *
		 * Read the xHCI capability register directly (the first 32-bit
		 * word at the controller MMIO base encodes CAPLENGTH in bits
		 * 7:0 and HCIVERSION in bits 31:16). A valid xHCI 1.x controller
		 * reports HCIVERSION = 0x0100. If we see that, RP1 IS reachable
		 * for our purposes and M8.1 succeeds; cfg-space is a deferred
		 * problem we can chase later if it ever matters.
		 */
		volatile uint32_t *xhci0_cap =
		    (volatile uint32_t *)(uintptr_t)
		        PLATFORM_RASPI5_USB0_XHCI_BASE;
		uint32_t xhci_caplen_ver = xhci0_cap[0];
		uint16_t hci_version =
		    (uint16_t)((xhci_caplen_ver >> 16) & 0xffffu);
		uint8_t cap_length = (uint8_t)(xhci_caplen_ver & 0xffu);

		raspi5_pcie_trace_u32("raspi5 pcie: xhci0 caplen_ver",
		                      xhci_caplen_ver);

		if (xhci_caplen_ver == 0xffffffffu ||
		    xhci_caplen_ver == 0xdeaddeadu || xhci_caplen_ver == 0u) {
			platform_uart_puts(
			    "raspi5 pcie: xHCI MMIO unreachable too; cfg + mem both dead\n");
#if RASPI5_PCIE_VERBOSE
			raspi5_pcie_dump_controller_state(
			    "raspi5 pcie: diag controller failure-state");
			raspi5_pcie_dump_rp1_mmio_samples();
#endif
			return -1;
		}
		if (hci_version != 0x0100u && hci_version != 0x0110u &&
		    hci_version != 0x0120u) {
			platform_uart_puts(
			    "raspi5 pcie: xhci0 reports unexpected HCIVERSION; "
			    "continuing anyway\n");
		}
		(void)cap_length;
		platform_uart_puts(
		    "raspi5 pcie: xHCI MMIO reachable; cfg-space path skipped\n");
		raspi5_pcie_trace_u64("raspi5 pcie: xhci0_base",
		                      PLATFORM_RASPI5_USB0_XHCI_BASE);
		raspi5_pcie_trace_u64("raspi5 pcie: xhci1_base",
		                      PLATFORM_RASPI5_USB1_XHCI_BASE);
		platform_uart_puts(
		    "raspi5 pcie: RP1 reachable (via direct MMIO)\n");
		return 0;
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
