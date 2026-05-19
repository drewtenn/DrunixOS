/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * irq.c - GIC-400 (GICv2) driver for Raspberry Pi 5.
 *
 * The BCM2712 ships a GIC-400 that implements the GICv2 architecture
 * (ARM IHI 0048B). All control is MMIO-based: distributor at
 * PLATFORM_RASPI5_GICD_BASE, CPU interface at PLATFORM_RASPI5_GICC_BASE.
 *
 * Unlike the QEMU virt GICv3 backend (which goes through the EL1
 * system-register interface), this driver only acknowledges and
 * end-of-interrupts through GICC MMIO.
 *
 * Original M5 design included an ICC_SRE_EL1 probe to detect firmware
 * that left the GICv3 system-register interface enabled. First boot on
 * real Pi 5 hardware showed that the probe itself faults: Pi 5 TF-A
 * disables the GICv3 SR interface (the correct configuration for our
 * GICv2 driver), and reading ICC_SRE_EL1 from EL1 when the SR
 * interface is disabled is UNDEFINED per the ARMv8 GIC spec, generating
 * a sync exception with ESR_EL1.EC=0. The probe is removed; if a
 * future boot ever lands here with SRE accidentally enabled, the
 * GICC MMIO writes silently no-op and the failure mode shows up
 * downstream as no timer ticks — same outcome as before, just less
 * pretty.
 *
 * MVP scope (M5): one IRQ source — the architectural generic timer
 * non-secure physical PPI, INTID 30 (PPI 14 + 16).
 *
 * M9.4a (this file) adds SPI (Shared Peripheral Interrupt)
 * registration on top of the M5 baseline. The first consumer is M9.4b
 * (PV0 vblank for the HVS scanout flip queue); future consumers
 * include RP1 cascade and PCIe MSI. The SPI table is sized for the
 * full GIC-400 range (RASPI5_GIC_SPI_MAX SPIs) so the dispatch path
 * has a fixed-cost O(1) lookup and no allocation; the BSS cost is
 * ~4 KiB which is negligible.
 */

#include "../platform.h"
#include "../../timer.h"
#include "irq.h"
#include <stdint.h>

#define GICD_CTLR_OFFSET 0x000u
#define GICD_ISENABLER_OFFSET 0x100u
#define GICD_ICENABLER_OFFSET 0x180u
#define GICD_ICPENDR_OFFSET 0x280u
#define GICD_IPRIORITYR_OFFSET 0x400u
#define GICD_ITARGETSR_OFFSET 0x800u
#define GICD_ICFGR_OFFSET 0xC00u

#define GICC_CTLR_OFFSET 0x000u
#define GICC_PMR_OFFSET 0x004u
#define GICC_BPR_OFFSET 0x008u
#define GICC_IAR_OFFSET 0x00Cu
#define GICC_EOIR_OFFSET 0x010u

#define GIC_INTID_SPURIOUS 1023u
#define GIC_INTID_SPECIAL_FIRST 1020u

#define GICD_REG(off) (*(volatile uint32_t *)(PLATFORM_RASPI5_GICD_BASE + (off)))
#define GICC_REG(off) (*(volatile uint32_t *)(PLATFORM_RASPI5_GICC_BASE + (off)))

#define GICD_BYTE(off, irq)                                                    \
	(*(volatile uint8_t *)(PLATFORM_RASPI5_GICD_BASE + (off) + (irq)))

static platform_irq_handler_fn g_timer_handler;

/*
 * SPI handler table indexed by spi_id (= INTID - RASPI5_GIC_SPI_INTID_BASE).
 * NULL entry = no handler registered; the dispatch path then just
 * acks + EOIs the IRQ. Static BSS keeps this allocation-free; ~4 KiB
 * on a 64-bit kernel.
 */
static raspi5_spi_handler_fn g_spi_handlers[RASPI5_GIC_SPI_MAX];

static void dsb_sy(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static void isb(void)
{
	__asm__ volatile("isb" ::: "memory");
}

static void gicd_disable_all_spis(void)
{
	uint32_t i;

	/* GIC-400 supports up to 480 SPIs. Pi 5 firmware may leave some
	 * non-secure SPIs enabled on handoff; mask the full range so the
	 * kernel takes a clean slate even before we start wiring SPI
	 * handlers in a follow-on milestone. */
	for (i = 32u; i < 480u; i += 32u) {
		GICD_REG(GICD_ICENABLER_OFFSET + (i / 8u)) = 0xFFFFFFFFu;
		GICD_REG(GICD_ICPENDR_OFFSET + (i / 8u)) = 0xFFFFFFFFu;
	}
}

static void gicd_init(void)
{
	GICD_REG(GICD_CTLR_OFFSET) = 0u;
	dsb_sy();

	gicd_disable_all_spis();

	/* Generic timer PPI: priority and enable. PPIs are banked per-CPU;
	 * GICD_ITARGETSR for IDs 0..31 is read-only. */
	GICD_BYTE(GICD_IPRIORITYR_OFFSET, PLATFORM_RASPI5_TIMER_INTID) = 0xA0u;
	GICD_REG(GICD_ISENABLER_OFFSET) = 1u << PLATFORM_RASPI5_TIMER_INTID;

	dsb_sy();
	/* GIC-400 with Security Extensions exposes different GICD_CTLR
	 * layouts to secure and non-secure accesses. In the non-secure
	 * view (which is what Pi 5 firmware hands us after TF-A drops to
	 * EL1 NS), bit 0 is EnableGrp1NS; bit 1 is RES0. In the secure
	 * view bit 0 is EnableGrp0 and bit 1 is EnableGrp1NS. Writing
	 * 0x3 sets both bits — harmlessly RES0 in NS view, correctly
	 * enabling both groups in secure view, so the driver works no
	 * matter which state firmware leaves us in. The non-secure
	 * physical timer fires as Group 1 NS, so EnableGrp1NS is the bit
	 * that actually matters here. (Reviewer caught the original
	 * 0x1-as-EnableGrp0-only encoding before flashing real hardware.) */
	GICD_REG(GICD_CTLR_OFFSET) = 0x3u;
	dsb_sy();
}

static void gicc_init(void)
{
	GICC_REG(GICC_PMR_OFFSET) = 0xFFu;
	GICC_REG(GICC_BPR_OFFSET) = 0u;
	/* GICC_CTLR.EnableGrp1 (bit 0 in NS view, bit 1 in secure view)
	 * must be set for Group 1 NS interrupts to reach the CPU. Write
	 * 0x3 for the same dual-view safety as GICD_CTLR above. */
	GICC_REG(GICC_CTLR_OFFSET) = 0x3u;
	isb();
}

void platform_irq_init(void)
{
	gicd_init();
	gicc_init();
}

void platform_irq_register(uint32_t irq, platform_irq_handler_fn fn)
{
	if (irq == PLATFORM_IRQ_TIMER)
		g_timer_handler = fn;
}

int raspi5_irq_register_spi(uint32_t spi_id, raspi5_spi_handler_fn handler)
{
	if (spi_id >= RASPI5_GIC_SPI_MAX)
		return -1;
	g_spi_handlers[spi_id] = handler;
	return 0;
}

void raspi5_irq_enable_spi(uint32_t spi_id, uint8_t priority, uint32_t trigger)
{
	uint32_t intid;
	uint32_t reg_index;
	uint32_t bit_index;

	if (spi_id >= RASPI5_GIC_SPI_MAX)
		return;
	intid = spi_id + RASPI5_GIC_SPI_INTID_BASE;

	GICD_BYTE(GICD_IPRIORITYR_OFFSET, intid) = priority;
	/*
	 * Target CPU 0 unconditionally. GIC-400 GICD_ITARGETSR is a
	 * byte-per-INTID register; bit 0 of the byte selects CPU 0.
	 * Drunix is uniprocessor on Pi 5 so a fixed target is correct.
	 * For PPIs (INTIDs 0..31) the register is read-only and the
	 * write is a no-op; SPIs (INTIDs 32+) accept writes here.
	 */
	GICD_BYTE(GICD_ITARGETSR_OFFSET, intid) = 0x01u;

	/*
	 * GICD_ICFGR has 2 bits per INTID: the upper bit selects the
	 * trigger type (0 = level, 1 = edge). 16 INTIDs per 32-bit word.
	 */
	{
		uint32_t cfg_offset =
		    GICD_ICFGR_OFFSET + (intid / 16u) * 4u;
		uint32_t cfg_shift = (intid % 16u) * 2u + 1u;
		volatile uint32_t *cfg_reg =
		    (volatile uint32_t *)(PLATFORM_RASPI5_GICD_BASE + cfg_offset);
		uint32_t value = *cfg_reg;
		if (trigger == RASPI5_GIC_TRIGGER_EDGE)
			value |= (1u << cfg_shift);
		else
			value &= ~(1u << cfg_shift);
		*cfg_reg = value;
	}

	dsb_sy();

	reg_index = intid / 32u;
	bit_index = intid % 32u;
	GICD_REG(GICD_ISENABLER_OFFSET + reg_index * 4u) = 1u << bit_index;
	dsb_sy();
}

void raspi5_irq_disable_spi(uint32_t spi_id)
{
	uint32_t intid;
	uint32_t reg_index;
	uint32_t bit_index;

	if (spi_id >= RASPI5_GIC_SPI_MAX)
		return;
	intid = spi_id + RASPI5_GIC_SPI_INTID_BASE;
	reg_index = intid / 32u;
	bit_index = intid % 32u;
	GICD_REG(GICD_ICENABLER_OFFSET + reg_index * 4u) = 1u << bit_index;
	dsb_sy();
}

int platform_irq_dispatch(void)
{
	uint32_t iar = GICC_REG(GICC_IAR_OFFSET);
	uint32_t intid = iar & 0xFFFFFFu;

	if (intid >= GIC_INTID_SPECIAL_FIRST)
		return 0;

	if (intid == PLATFORM_RASPI5_TIMER_INTID) {
		arm64_timer_irq();
		if (g_timer_handler)
			g_timer_handler();
	} else if (intid >= RASPI5_GIC_SPI_INTID_BASE &&
	           intid < RASPI5_GIC_SPI_INTID_BASE + RASPI5_GIC_SPI_MAX) {
		raspi5_spi_handler_fn handler =
		    g_spi_handlers[intid - RASPI5_GIC_SPI_INTID_BASE];
		if (handler)
			handler();
	}

	dsb_sy();
	GICC_REG(GICC_EOIR_OFFSET) = iar;
	isb();
	return 1;
}

void platform_irq_enable(void)
{
	__asm__ volatile("msr daifclr, #2");
}
