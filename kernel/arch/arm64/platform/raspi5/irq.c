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
 * end-of-interrupts through GICC MMIO. Before doing so, it probes
 * ICC_SRE_EL1: if firmware left the GICv3 system-register interface
 * enabled, MMIO writes to GICC are ignored and IRQs would never
 * dispatch — surface that loudly instead of hanging silently.
 *
 * MVP scope (M5): one IRQ source — the architectural generic timer
 * non-secure physical PPI, INTID 30 (PPI 14 + 16). No SPI plumbing;
 * the UART is polled.
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

static uint64_t icc_sre_el1_read(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(value));
	return value;
}

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

	for (i = 32u; i < 256u; i += 32u) {
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
	GICD_REG(GICD_CTLR_OFFSET) = 1u; /* EnableGrp0 */
	dsb_sy();
}

static void gicc_init(void)
{
	GICC_REG(GICC_PMR_OFFSET) = 0xFFu;
	GICC_REG(GICC_BPR_OFFSET) = 0u;
	GICC_REG(GICC_CTLR_OFFSET) = 1u;
	isb();
}

void platform_irq_init(void)
{
	uint64_t sre = icc_sre_el1_read();

	if (sre & 1ull) {
		platform_uart_puts(
		    "raspi5: ICC_SRE_EL1.SRE=1 - firmware left GICv3 system-register "
		    "interface enabled; GICv2 MMIO driver cannot run. Halting.\n");
		for (;;)
			__asm__ volatile("wfe");
	}

	gicd_init();
	gicc_init();
}

void platform_irq_register(uint32_t irq, platform_irq_handler_fn fn)
{
	if (irq == PLATFORM_IRQ_TIMER)
		g_timer_handler = fn;
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
