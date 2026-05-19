/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_RASPI5_PCIE_H
#define KERNEL_ARCH_ARM64_PLATFORM_RASPI5_PCIE_H

/*
 * BCM2712 PCIe2 root complex sanity probe and RP1 endpoint identity
 * read. M8.1: the bring-up here is intentionally minimal, but it
 * cannot assume firmware touched RP1. When the kernel console uses
 * the Pi 5 debug JST-SH header, serial traffic goes through uart10
 * inside the SoC, not RP1 UART0. The probe therefore programs the
 * small CPU->PCIe and root-port forwarding windows needed for the
 * fixed RP1 aperture before checking xHCI MMIO reachability.
 *
 * This probe does NOT configure interrupts, do MSI/MSI-X setup,
 * walk the PCI device tree, or attempt link retraining if the
 * firmware-initialized link is reported down. Those are deferred
 * until something needs them (likely never for the M8 keyboard-only
 * MVP, which polls the xHCI event ring).
 */

#include <stdint.h>

typedef struct {
	uint32_t pcie_lo;
	uint32_t pcie_hi;
	uint32_t base_limit;
	uint32_t base_hi;
	uint32_t limit_hi;
} raspi5_pcie_outbound_window_t;

typedef struct {
	uint32_t ubus_timeout_offset;
	uint32_t ubus_timeout_value;
	uint32_t rc_config_retry_timeout_offset;
	uint32_t rc_config_retry_timeout_value;
	uint32_t axi_read_error_data_offset;
	uint32_t axi_read_error_data_value;
} raspi5_pcie_bcm2712_post_setup_t;

/*
 * Encode one Broadcom STB CPU->PCIe outbound window. The low register
 * pair holds the target PCIe address; base/limit describe the CPU
 * physical aperture in 1 MiB units split across low and high fields.
 */
static inline raspi5_pcie_outbound_window_t
raspi5_pcie_outbound_window_encode(uint64_t cpu_addr, uint64_t pcie_addr,
                                   uint64_t size)
{
	uint64_t cpu_addr_mb = cpu_addr / 0x100000ull;
	uint64_t limit_addr_mb = (cpu_addr + size - 1ull) / 0x100000ull;
	raspi5_pcie_outbound_window_t win;

	win.pcie_lo = (uint32_t)(pcie_addr & 0xffffffffull);
	win.pcie_hi = (uint32_t)(pcie_addr >> 32);
	win.base_limit = (uint32_t)(((cpu_addr_mb & 0xfffull) << 4) |
	                            ((limit_addr_mb & 0xfffull) << 20));
	win.base_hi = (uint32_t)((cpu_addr_mb >> 12) & 0xffull);
	win.limit_hi = (uint32_t)((limit_addr_mb >> 12) & 0xffull);
	return win;
}

static inline raspi5_pcie_bcm2712_post_setup_t
raspi5_pcie_bcm2712_post_setup_values(void)
{
	raspi5_pcie_bcm2712_post_setup_t setup;

	setup.ubus_timeout_offset = 0x40a8u;
	setup.ubus_timeout_value = 0x0b2d0000u;
	setup.rc_config_retry_timeout_offset = 0x405cu;
	setup.rc_config_retry_timeout_value = 0x0aba0000u;
	setup.axi_read_error_data_offset = 0x4170u;
	setup.axi_read_error_data_value = 0xffffffffu;
	return setup;
}

/*
 * Probe the BCM2712 PCIe2 root complex and the RP1 endpoint behind
 * it. Logs the result over the platform UART. Returns 0 if RP1 is
 * present and reachable (vendor 0x1de4, device 0x0001 at bus 1
 * dev 0 fn 0), -1 otherwise. Failure is non-fatal at the caller;
 * the kernel boot continues to a shell either way and M8.2 will
 * notice the absence when it tries to talk to the xHCI MMIO.
 */
int raspi5_pcie_probe_rp1(void);

#endif
