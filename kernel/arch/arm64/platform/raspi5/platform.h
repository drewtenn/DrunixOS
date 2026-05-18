/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_RASPI5_PLATFORM_H
#define KERNEL_PLATFORM_RASPI5_PLATFORM_H

/*
 * Raspberry Pi 5 (BCM2712) memory map. Addresses taken from the live
 * device tree at arch/arm64/boot/dts/broadcom/bcm2712.dtsi and the
 * companion rp1.dtsi in the Raspberry Pi Linux fork.
 *
 *   SoC peripheral range:  0x10_0000_0000 .. 0x10_8000_0000 (per the
 *   soc@107c000000 node `ranges` mapping child 0x0 -> CPU 0x10_0000_0000
 *   with size 0x8000_0000). Split into two 1 GiB identity-mapped Device
 *   blocks because L1 blocks are 1 GiB each:
 *     LOW  (L1[64]): 0x10_0000_0000 .. 0x10_4000_0000
 *       - sdio1 SDHCI host registers:         0x10_00ff_f000 (M6)
 *       - sdio1 SDHCI cfg registers:          0x10_00ff_f400 (M6)
 *     HIGH (L1[65]): 0x10_4000_0000 .. 0x10_8000_0000
 *       - uart10 (PL011 debug, JST-SH header):0x10_7d00_1000
 *       - GIC-400 distributor (GICD):         0x10_7fff_9000
 *       - GIC-400 CPU interface (GICC):       0x10_7fff_a000
 *   PCIe2 outbound window (L1[124]): 0x1f_0000_0000 .. 0x1f_4000_0000
 *     - RP1 UART0 (40-pin header, GPIO14/15): 0x1f_0003_0000
 */

#define PLATFORM_RASPI5_SOC_LOW_BASE 0x1000000000ULL
#define PLATFORM_RASPI5_SOC_LOW_SIZE 0x40000000ULL

#define PLATFORM_RASPI5_SOC_WINDOW_BASE 0x1040000000ULL
#define PLATFORM_RASPI5_SOC_WINDOW_SIZE 0x40000000ULL

#define PLATFORM_RASPI5_PCIE_WINDOW_BASE 0x1f00000000ULL
#define PLATFORM_RASPI5_PCIE_WINDOW_SIZE 0x40000000ULL

/*
 * BCM2712 SDIO1 SDHCI host controller. From the bcm2712-rpi-5-b.dtb:
 *   mmc@fff000 { compatible = "brcm,bcm2712-sdhci", "brcm,sdhci-brcmstb";
 *                reg = <0xfff000 0x260 0xfff400 0x200>; ... };
 * Translated through the soc@107c000000 ranges, that's CPU physical
 * 0x10_00ff_f000 (host) + 0x10_00ff_f400 (Broadcom cfg).
 */
#define PLATFORM_RASPI5_SDHCI_HOST_BASE 0x1000fff000ULL
#define PLATFORM_RASPI5_SDHCI_CFG_BASE 0x1000fff400ULL

/*
 * Primary debug UART. RP1 UART0 is the default because Pi 5 firmware
 * with `enable_uart=1 uart_2ndstage=1` in config.txt routes the
 * firmware-stage serial output here, and a USB-UART adapter on the
 * 40-pin GPIO header pins 8 (TXD) and 10 (RXD) hits it directly.
 *
 * Users with the Raspberry Pi Debug Probe (JST-SH connector) can flip
 * this to PLATFORM_RASPI5_UART10_BASE at build time by passing
 * `RASPI5_UART=jstsh` to make.
 */
#define PLATFORM_RASPI5_RP1_UART0_BASE 0x1f00030000ULL
#define PLATFORM_RASPI5_UART10_BASE 0x107d001000ULL

#ifndef PLATFORM_RASPI5_UART_BASE
#define PLATFORM_RASPI5_UART_BASE PLATFORM_RASPI5_RP1_UART0_BASE
#endif

#define PLATFORM_RASPI5_GICD_BASE 0x107fff9000ULL
#define PLATFORM_RASPI5_GICC_BASE 0x107fffa000ULL

/* Non-secure physical timer PPI on Pi 5 is PPI 14 -> INTID 30, the
 * architectural generic-timer assignment used everywhere. Matches the
 * INTID the virt GICv3 driver wires for CNTP_EL1. */
#define PLATFORM_RASPI5_TIMER_INTID 30u

/*
 * M5 commit 2: FDT-driven RAM layout init. Called from platform_init
 * after UART is up; populates the per-platform layout consumed by
 * platform_ram_layout()/platform_mm_classify(). Idempotent on repeat
 * calls; falls back to a compile-time default (256 MiB at 0) if the
 * FDT pointer is missing or the blob fails validation.
 */
void raspi5_ram_layout_init(void);

#endif
