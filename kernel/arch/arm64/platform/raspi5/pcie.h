/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_RASPI5_PCIE_H
#define KERNEL_ARCH_ARM64_PLATFORM_RASPI5_PCIE_H

/*
 * BCM2712 PCIe2 root complex sanity probe and RP1 endpoint identity
 * read. M8.1: the bring-up here is intentionally minimal — Pi 5 EEPROM
 * firmware initializes the PCIe link and assigns RP1's BARs before
 * kernel handoff (RP1 UART0 has been the kernel debug console since
 * M5; that traffic flows over this exact path), so the kernel only
 * has to verify the link is alive and log RP1's vendor / device IDs
 * and BAR1 base. The xHCI host controllers for M8.2 / M8.3 live
 * inside RP1's BAR1 at fixed offsets defined in raspi5/platform.h.
 *
 * This probe does NOT configure interrupts, do MSI/MSI-X setup,
 * walk the PCI device tree, or attempt link retraining if the
 * firmware-initialized link is reported down. Those are deferred
 * until something needs them (likely never for the M8 keyboard-only
 * MVP, which polls the xHCI event ring).
 */

#include <stdint.h>

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
