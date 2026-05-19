/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_RASPI5_IRQ_H
#define KERNEL_PLATFORM_RASPI5_IRQ_H

#include <stdint.h>

/*
 * SPI registration surface for raspi5 / GIC-400.
 *
 * The Pi 5 IRQ driver's M5 baseline only wired the architectural
 * generic-timer PPI (INTID 30). Every other IRQ source — PixelValve
 * vblank, HVS EOF, RP1 cascade, future PCIe MSI — needs an SPI
 * registration. M9.4a adds the registration table + enable/disable
 * helpers; M9.4b lands the first consumer (PV0 vblank for the M9.3
 * scanout flip queue).
 *
 * spi_id matches the Device Tree convention used by bcm2712.dtsi
 * ("interrupts = <GIC_SPI 101 ...>" means spi_id = 101 here).
 * The GIC INTID is spi_id + 32 (PPIs occupy INTIDs 16..31).
 *
 * GIC-400 supports up to 480 SPIs (INTIDs 32..511). The handler
 * table is sized for that maximum; the static array is ~4 KiB of BSS,
 * negligible alongside the M9.3 scanout carve-out.
 */

#define RASPI5_GIC_SPI_MAX 480u
#define RASPI5_GIC_SPI_INTID_BASE 32u

#define RASPI5_GIC_TRIGGER_LEVEL 0u
#define RASPI5_GIC_TRIGGER_EDGE 1u

/*
 * Default IRQ priority for SPI sources, matching the timer PPI. GIC
 * priorities are unsigned bytes (0 = highest, 0xFF = lowest). The
 * timer uses 0xA0; using the same default for SPIs keeps every IRQ at
 * the same priority level so the GIC's priority-mask gating works
 * uniformly. Callers that want a different priority pass it
 * explicitly to raspi5_irq_enable_spi().
 */
#define RASPI5_GIC_SPI_PRIORITY_DEFAULT 0xA0u

typedef void (*raspi5_spi_handler_fn)(void);

/*
 * Register an SPI handler. Pass the spi_id (DT convention), not the
 * INTID. The handler runs from platform_irq_dispatch with IRQs
 * masked at the CPU (DAIF.I = 1) and the GIC interface still holding
 * the active state for this IRQ — handler must complete quickly and
 * must not re-enable IRQs.
 *
 * Returns 0 on success, -1 if spi_id is out of range. Re-registering
 * the same spi_id replaces the previous handler (no warning).
 */
int raspi5_irq_register_spi(uint32_t spi_id, raspi5_spi_handler_fn handler);

/*
 * Configure and enable an SPI. priority is the GIC priority byte
 * (lower = higher priority; 0xA0 is the default elsewhere in the
 * driver). trigger is RASPI5_GIC_TRIGGER_LEVEL for level-sensitive
 * interrupts (active-high), or RASPI5_GIC_TRIGGER_EDGE for rising-
 * edge-triggered. Pi 5's PixelValve vblank is level-triggered per
 * bcm2712.dtsi; most peripheral IRQs follow the same pattern.
 *
 * Targets CPU 0 unconditionally — Drunix runs uniprocessor on Pi 5.
 *
 * Idempotent. Calling on a spi_id that's already enabled re-applies
 * the priority and trigger fields and leaves the enable set.
 */
void raspi5_irq_enable_spi(uint32_t spi_id, uint8_t priority,
                           uint32_t trigger);

/*
 * Mask an SPI at the distributor. Does NOT clear the registered
 * handler — caller can re-enable later without re-registering.
 */
void raspi5_irq_disable_spi(uint32_t spi_id);

#endif
