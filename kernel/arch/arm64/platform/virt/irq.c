/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * irq.c - GICv3 driver for the QEMU "virt" machine.
 *
 * GICv3 layout on QEMU virt:
 *   GICD (distributor)         0x08000000-0x08010000
 *   GICR (redistributors)      0x080A0000-0x080F0000  (32 KiB stride; first
 *                              CPU at 0x080A0000, but two 64 KiB regions
 *                              per CPU when GICv3 redistributors are used:
 *                              RD_base then SGI_base = RD_base + 0x10000)
 *
 * v1.2 GPU/media MVP M1 only needs CPU 0 with the architectural CNTP_EL1
 * timer (PPI 30). SMP, ITS-driven MSIs, and SPI routing for virtio
 * arrive in M2+.
 */

#include "../platform.h"
#include "../../timer.h"
#include <stdint.h>

#define GICD_BASE                0x08000000UL
#define GICR_BASE_CPU0           0x080A0000UL
#define GICR_SGI_OFFSET          0x10000UL

#define GICD_CTLR_OFFSET         0x0000u
#define GICD_TYPER_OFFSET        0x0004u
#define GICD_IGROUPR0_OFFSET     0x0080u
#define GICD_ISENABLER0_OFFSET   0x0100u
#define GICD_ICENABLER0_OFFSET   0x0180u
#define GICD_ICPENDR0_OFFSET     0x0280u
#define GICD_IPRIORITYR0_OFFSET  0x0400u

#define GICD_CTLR_ARE_NS         (1u << 4)
#define GICD_CTLR_ENABLE_GRP1_NS (1u << 1)
#define GICD_CTLR_RWP            (1u << 31)

#define GICR_CTLR_OFFSET         0x0000u
#define GICR_TYPER_OFFSET        0x0008u
#define GICR_WAKER_OFFSET        0x0014u

#define GICR_WAKER_PROCESSOR_SLEEP (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1u << 2)

#define GICR_SGI_IGROUPR0_OFFSET     0x0080u
#define GICR_SGI_ISENABLER0_OFFSET   0x0100u
#define GICR_SGI_ICENABLER0_OFFSET   0x0180u
#define GICR_SGI_ICPENDR0_OFFSET     0x0280u
#define GICR_SGI_IPRIORITYR0_OFFSET  0x0400u

#define ARCH_INTID_CNTP_EL1      30u
#define ARCH_INTID_SPECIAL_FIRST 1020u
#define ARCH_INTID_SPURIOUS      1023u

#define GICD_RWP_TIMEOUT_ITERS   0x100000u

/* Drunix's per-platform IRQ table maps PLATFORM_IRQ_TIMER (= 0) to a
 * single registered handler. There is no need for a wider table on virt
 * M1; SMP and SPI-driven device IRQs add capacity in later milestones. */
static platform_irq_handler_fn g_timer_handler;

static void mmio_write32(uintptr_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static void gicd_wait_rwp(void)
{
	uint32_t i = 0;

	while ((mmio_read32(GICD_BASE + GICD_CTLR_OFFSET) & GICD_CTLR_RWP) != 0u) {
		if (++i > GICD_RWP_TIMEOUT_ITERS)
			break;
	}
}

/* ICC_SRE_EL1.SRE is sticky once set. The set-bit-0 + ISB sequence is
 * harmless if SRE was already enabled, and required if it wasn't.
 * Assumption: EL2 firmware did not lock SRE_EL2.Enable=0 before handing
 * EL1 control to Drunix; that is true for QEMU's default `-M virt` boot.
 */
static void icc_sre_el1_enable(void)
{
	uint64_t sre;

	__asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre));
	sre |= 1u;
	__asm__ volatile("msr S3_0_C12_C12_5, %0" : : "r"(sre));
	__asm__ volatile("isb");
}

static void icc_pmr_el1_write(uint64_t value)
{
	__asm__ volatile("msr S3_0_C4_C6_0, %0" : : "r"(value));
}

static void icc_bpr1_el1_write(uint64_t value)
{
	__asm__ volatile("msr S3_0_C12_C12_3, %0" : : "r"(value));
}

static void icc_ctlr_el1_write(uint64_t value)
{
	__asm__ volatile("msr S3_0_C12_C12_4, %0" : : "r"(value));
}

static void icc_igrpen1_el1_write(uint64_t value)
{
	__asm__ volatile("msr S3_0_C12_C12_7, %0" : : "r"(value));
	__asm__ volatile("isb");
}

static uint64_t icc_iar1_el1_read(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(value));
	return value;
}

/* Per ARM IHI 0069 §12.2: a DSB before EOIR ensures the handler's loads
 * and stores have retired before the priority drops, and an ISB after
 * keeps later instructions from speculating across the priority change.
 */
static void icc_eoir1_el1_write(uint64_t value)
{
	__asm__ volatile("dsb sy");
	__asm__ volatile("msr S3_0_C12_C12_1, %0" : : "r"(value));
	__asm__ volatile("isb");
}

static void gicd_init(void)
{
	uint32_t i;
	uint32_t typer = mmio_read32(GICD_BASE + GICD_TYPER_OFFSET);
	uint32_t lines = ((typer & 0x1Fu) + 1u) * 32u;

	mmio_write32(GICD_BASE + GICD_CTLR_OFFSET, 0u);
	gicd_wait_rwp();

	mmio_write32(GICD_BASE + GICD_CTLR_OFFSET,
	             GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_GRP1_NS);
	gicd_wait_rwp();

	/* Mask and clear-pending all SPIs (32 .. lines-1). PPIs/SGIs are
	 * controlled by the redistributor. M1 has no SPI consumers, so the
	 * SPIs start out disabled; M2 explicitly enables the ones it wires
	 * to virtio-mmio devices. */
	for (i = 32u; i < lines; i += 32u) {
		mmio_write32(GICD_BASE + GICD_ICENABLER0_OFFSET + (i / 8u),
		             0xFFFFFFFFu);
		mmio_write32(GICD_BASE + GICD_ICPENDR0_OFFSET + (i / 8u),
		             0xFFFFFFFFu);
	}
}

static void gicr_init_cpu0(void)
{
	uintptr_t rd = GICR_BASE_CPU0;
	uintptr_t sgi = rd + GICR_SGI_OFFSET;
	uint32_t waker;
	uint32_t i = 0;

	/* Wake the redistributor: clear ProcessorSleep, then wait until
	 * ChildrenAsleep deasserts. */
	waker = mmio_read32(rd + GICR_WAKER_OFFSET);
	waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
	mmio_write32(rd + GICR_WAKER_OFFSET, waker);

	while ((mmio_read32(rd + GICR_WAKER_OFFSET) & GICR_WAKER_CHILDREN_ASLEEP)
	       != 0u) {
		if (++i > 0x100000u)
			break; /* Defensive: loop forever risk. */
	}

	/* SGIs (0..15) and PPIs (16..31) live in the SGI_base region. */
	mmio_write32(sgi + GICR_SGI_IGROUPR0_OFFSET, 0xFFFFFFFFu);
	mmio_write32(sgi + GICR_SGI_ICENABLER0_OFFSET, 0xFFFFFFFFu);
	mmio_write32(sgi + GICR_SGI_ICPENDR0_OFFSET, 0xFFFFFFFFu);

	/* Priority for INTID n is byte n in IPRIORITYR. 0xA0 is mid-range. */
	*(volatile uint8_t *)(sgi + GICR_SGI_IPRIORITYR0_OFFSET +
	                      ARCH_INTID_CNTP_EL1) = 0xA0u;

	mmio_write32(sgi + GICR_SGI_ISENABLER0_OFFSET,
	             1u << ARCH_INTID_CNTP_EL1);
}

static void cpu_iface_init(void)
{
	icc_sre_el1_enable();
	icc_pmr_el1_write(0xFFu);
	icc_bpr1_el1_write(0u);
	icc_ctlr_el1_write(0u);
	icc_igrpen1_el1_write(1u);
}

void platform_irq_init(void)
{
	gicd_init();
	gicr_init_cpu0();
	cpu_iface_init();
}

void platform_irq_register(uint32_t irq, platform_irq_handler_fn fn)
{
	if (irq == PLATFORM_IRQ_TIMER)
		g_timer_handler = fn;
}

int platform_irq_dispatch(void)
{
	uint64_t iar = icc_iar1_el1_read();
	uint32_t intid = (uint32_t)(iar & 0xFFFFFFu);

	/* GICv3 reserves INTIDs 1020..1023 for special signals (Group 0/1
	 * targeted at a non-current EL, plus the Spurious INTID 1023).
	 * None of them require an EOIR write; treat the whole range as
	 * "nothing handled". Defensive against config drift; on the M1
	 * single-Group-1-NS setup only 1023 actually appears. */
	if (intid >= ARCH_INTID_SPECIAL_FIRST)
		return 0;

	/* Runs in IRQ context with interrupts disabled. Handlers must not
	 * sleep, must not block on locks held outside IRQ context, and must
	 * keep their work bounded in time. */
	if (intid == ARCH_INTID_CNTP_EL1) {
		arm64_timer_irq();
		if (g_timer_handler)
			g_timer_handler();
	}

	icc_eoir1_el1_write(iar);
	return 1;
}

void platform_irq_enable(void)
{
	__asm__ volatile("msr daifclr, #2");
}
