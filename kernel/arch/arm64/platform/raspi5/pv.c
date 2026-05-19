/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * raspi5/pv.c — M9.4b1 passive PixelValve PV0 observability.
 *
 * Read-only probe of BCM2712 PV0 to identify its register layout on
 * real D0 silicon. M9.4b2 will register a vblank IRQ handler on
 * GIC SPI 101; M9.4b3 will use the handler to pace HVS plane flips.
 *
 * Constants live in pv.h with citations to Linux drivers/gpu/drm/vc4/
 * vc4_regs.h. Per docs/contributing/linux-reference.md no Linux
 * source is copied; the offsets are independently restated and the
 * actual register layout on D0 is confirmed empirically by this
 * probe's output before any M9.4b2 write code is written.
 *
 * MMIO 0x107c400000 sits inside the L1[65] high-peripheral identity
 * map block (Device-nGnRnE) established in M6 for the SoC high
 * peripheral window. No new MMU work needed.
 */

#include "pv.h"
#include "../platform.h"

/*
 * Speculative offsets (Pi 4 / HVS5 layout per Linux vc4_regs.h).
 * The probe reads them into state->* and also does a wide scan so
 * the trace shows whether these offsets carry sensible values on
 * D0 or whether HVS6 shifted them.
 */
#define RASPI5_PV_CONTROL_OFFSET 0x00u
#define RASPI5_PV_V_CONTROL_OFFSET 0x04u
#define RASPI5_PV_HORZA_OFFSET 0x0cu
#define RASPI5_PV_HORZB_OFFSET 0x10u
#define RASPI5_PV_VERTA_OFFSET 0x14u
#define RASPI5_PV_VERTB_OFFSET 0x18u
#define RASPI5_PV_INTEN_OFFSET 0x24u
#define RASPI5_PV_INTSTAT_OFFSET 0x28u
#define RASPI5_PV_STAT_OFFSET 0x2cu

static inline volatile uint32_t *raspi5_pv0_reg(uint32_t offset)
{
	return (volatile uint32_t *)(uintptr_t)(RASPI5_PV0_BASE + offset);
}

static inline uint32_t raspi5_pv0_read(uint32_t offset)
{
	return *raspi5_pv0_reg(offset);
}

/*
 * Local trace helpers — same allocation-free pattern hvs.c uses so
 * the probe runs without depending on kprintf or kheap, and so the
 * trace survives even if higher layers fail.
 */
static void raspi5_pv0_trace_u32(const char *label, uint32_t v)
{
	char buf[11];
	platform_uart_puts(label);
	platform_uart_puts("=0x");
	for (int i = 7; i >= 0; i--) {
		uint32_t nibble = (v >> (i * 4)) & 0xfu;
		buf[7 - i] = (char)(nibble < 10u ? '0' + nibble : 'a' + (nibble - 10u));
	}
	buf[8] = '\n';
	buf[9] = '\0';
	platform_uart_puts(buf);
}

static void raspi5_pv0_trace_u64(const char *label, uint64_t v)
{
	char buf[19];
	platform_uart_puts(label);
	platform_uart_puts("=0x");
	for (int i = 15; i >= 0; i--) {
		uint32_t nibble = (uint32_t)((v >> (i * 4)) & 0xfu);
		buf[15 - i] = (char)(nibble < 10u ? '0' + nibble : 'a' + (nibble - 10u));
	}
	buf[16] = '\n';
	buf[17] = '\0';
	platform_uart_puts(buf);
}

static void raspi5_pv0_dump_window(void)
{
	uint32_t off;

	platform_uart_puts("raspi5 pv0: wide scan (non-zero words in [0x000, 0x100))\n");
	for (off = 0u; off < RASPI5_PV_WINDOW_SIZE; off += 4u) {
		uint32_t value = raspi5_pv0_read(off);
		if (value == 0u)
			continue;
		{
			char tag[28];
			uint32_t n = 0u;
			int j;
			const char *prefix = "raspi5 pv0: mmio";
			const char *p = prefix;
			while (*p)
				tag[n++] = *p++;
			tag[n++] = '[';
			tag[n++] = '0';
			tag[n++] = 'x';
			for (j = 2; j >= 0; j--) {
				uint32_t nibble = (off >> (j * 4)) & 0xfu;
				tag[n++] = (char)(nibble < 10u ? '0' + nibble
				                              : 'a' + (nibble - 10u));
			}
			tag[n++] = ']';
			tag[n] = '\0';
			raspi5_pv0_trace_u32(tag, value);
		}
	}
}

int raspi5_pv0_probe_passive(raspi5_pv0_probe_state_t *state)
{
	platform_uart_puts("raspi5 pv0: probe start (read-only)\n");
	raspi5_pv0_trace_u64("raspi5 pv0: base", RASPI5_PV0_BASE);
	raspi5_pv0_trace_u32("raspi5 pv0: window_size",
	                     (uint32_t)RASPI5_PV_WINDOW_SIZE);

	if (state) {
		state->control = raspi5_pv0_read(RASPI5_PV_CONTROL_OFFSET);
		state->v_control = raspi5_pv0_read(RASPI5_PV_V_CONTROL_OFFSET);
		state->horza = raspi5_pv0_read(RASPI5_PV_HORZA_OFFSET);
		state->horzb = raspi5_pv0_read(RASPI5_PV_HORZB_OFFSET);
		state->verta = raspi5_pv0_read(RASPI5_PV_VERTA_OFFSET);
		state->vertb = raspi5_pv0_read(RASPI5_PV_VERTB_OFFSET);
		state->int_en = raspi5_pv0_read(RASPI5_PV_INTEN_OFFSET);
		state->int_stat = raspi5_pv0_read(RASPI5_PV_INTSTAT_OFFSET);
		state->stat = raspi5_pv0_read(RASPI5_PV_STAT_OFFSET);

		raspi5_pv0_trace_u32("raspi5 pv0: control (HVS5 layout)",
		                     state->control);
		raspi5_pv0_trace_u32("raspi5 pv0: v_control", state->v_control);
		raspi5_pv0_trace_u32("raspi5 pv0: horza", state->horza);
		raspi5_pv0_trace_u32("raspi5 pv0: horzb", state->horzb);
		raspi5_pv0_trace_u32("raspi5 pv0: verta", state->verta);
		raspi5_pv0_trace_u32("raspi5 pv0: vertb", state->vertb);
		raspi5_pv0_trace_u32("raspi5 pv0: int_en", state->int_en);
		raspi5_pv0_trace_u32("raspi5 pv0: int_stat", state->int_stat);
		raspi5_pv0_trace_u32("raspi5 pv0: stat", state->stat);
	}

	/*
	 * Wide scan complements the named reads. If the HVS6 PV layout
	 * shifted registers, the named reads above land on whatever
	 * happens to live at the HVS5 offsets; the wide scan shows the
	 * real picture. Same approach M9.1 took with HVS6 and the same
	 * one that found the actual D0 channel base offsets at 0x100 /
	 * 0x140 / 0x180 instead of 0x30 / 0x50 / 0x70.
	 */
	raspi5_pv0_dump_window();
	platform_uart_puts("raspi5 pv0: probe done\n");
	return 0;
}
