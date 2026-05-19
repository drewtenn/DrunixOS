/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * raspi5/hvs.c — M9.1 passive HVS observability.
 *
 * Read-only probe of the BCM2712 HVS6 channel control registers and a
 * window of dlist SRAM. Runs after the existing mailbox framebuffer
 * path has come up (so we already know /dev/fb0 works and the serial
 * console is alive) and traces what the firmware left behind. No
 * writes occur. The trace is what M9.2's dlist locator and
 * fingerprint validator will be built on top of.
 *
 * The constants live in hvs.h with citations to Linux source files
 * per docs/contributing/linux-reference.md. Nothing in this driver is
 * copied from Linux; the register offsets are independently re-stated
 * because no public Broadcom datasheet exists for HVS6.
 *
 * The HVS MMIO window (0x10_7c58_0000, span 0x1a000) sits inside the
 * L1[65] high-peripheral identity-map block established in M6 (see
 * docs/ch32-raspi5-bringup.md). No new MMU work is needed for M9.1.
 */

#include "hvs.h"
#include "../platform.h"

/*
 * Number of dlist words to dump per enabled channel. 24 words covers a
 * typical HVS6 single-plane head (CTL0 + position + size + pointer
 * words) with room for one optional CTL2/POS1 plus terminator. M9.2
 * will replace this raw window dump with a bounded structural parser.
 */
#define RASPI5_HVS_PROBE_DLIST_TRACE_WORDS 24u

/*
 * Wide-scan window. M9.1 v1 found ch0_ctrl0=0 and ch0_lptrs=0xffffffff,
 * which doesn't fit the HVS5 layout assumed by the named-register reads.
 * v2 adds an unconditional non-zero-only dump of the first 0x200 bytes
 * of HVS MMIO (covers global state + all three channels + a margin
 * beyond) and the first 64 dlist words, regardless of what LPTRS/DL
 * report. The hypothesis is that HVS6's "channel running" indicator
 * moved from CTRL0 bit 31 (Pi 4 / HVS5) to CTRL1 bit 31 or a global
 * register on HVS6. Print every non-zero word so we can see the actual
 * layout instead of trusting the HVS5 convention.
 */
#define RASPI5_HVS_PROBE_WIDE_SCAN_BYTES 0x200u
#define RASPI5_HVS_PROBE_DLIST_UNCONDITIONAL_WORDS 64u

static inline volatile uint32_t *raspi5_hvs_reg(uint32_t offset)
{
	return (volatile uint32_t *)(uintptr_t)(RASPI5_HVS_BASE + offset);
}

static inline uint32_t raspi5_hvs_read(uint32_t offset)
{
	return *raspi5_hvs_reg(offset);
}

/*
 * Local trace helpers. raspi5/video.c has equivalent helpers but
 * keeps them file-static; rather than expose those, hvs.c carries its
 * own small versions. They emit "<label>=0x<hex>" on serial. Kept
 * intentionally simple so M9.1 has zero dependency on kheap/printf
 * (the trace runs before the heap is heavily exercised; staying
 * allocation-free preserves the "even if everything else breaks,
 * we still get a trace" property).
 */
static void raspi5_hvs_trace_u32(const char *label, uint32_t v)
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

static void raspi5_hvs_trace_label_index(const char *prefix,
                                         uint8_t index,
                                         const char *suffix,
                                         uint32_t value)
{
	/* "raspi5 hvs: chN_<suffix>=0x<hex>" without sprintf. */
	char buf[64];
	const char *p = prefix;
	uint32_t n = 0u;
	while (*p && n < sizeof(buf) - 8u)
		buf[n++] = *p++;
	buf[n++] = (char)('0' + (index % 10u));
	buf[n++] = '_';
	const char *s = suffix;
	while (*s && n < sizeof(buf) - 1u)
		buf[n++] = *s++;
	buf[n] = '\0';
	raspi5_hvs_trace_u32(buf, value);
}

static void raspi5_hvs_dump_offset_trace(uint32_t base_offset,
                                         uint32_t bytes,
                                         const char *prefix)
{
	/*
	 * Walk [base_offset, base_offset+bytes) one 32-bit word at a time
	 * and trace any non-zero value with "prefix[NN]=0x<hex>" where NN
	 * is the byte offset. Skipping zeros keeps the boot trace
	 * readable; the layout of HVS6 is unknown enough that we want to
	 * see every non-zero register without prejudice. Bounds-checked
	 * against the HVS MMIO window.
	 */
	uint32_t i;
	if (base_offset >= RASPI5_HVS_SIZE)
		return;
	if (base_offset + bytes > RASPI5_HVS_SIZE)
		bytes = (uint32_t)(RASPI5_HVS_SIZE - base_offset);
	for (i = 0u; i < bytes; i += 4u) {
		uint32_t value = raspi5_hvs_read(base_offset + i);
		if (value == 0u)
			continue;
		{
			char tag[40];
			uint32_t n = 0u;
			uint32_t off = base_offset + i;
			const char *p = prefix;
			int j;
			while (*p && n < sizeof(tag) - 12u)
				tag[n++] = *p++;
			tag[n++] = '[';
			tag[n++] = '0';
			tag[n++] = 'x';
			for (j = 3; j >= 0; j--) {
				uint32_t nibble = (off >> (j * 4)) & 0xfu;
				tag[n++] =
				    (char)(nibble < 10u ? '0' + nibble
				                       : 'a' + (nibble - 10u));
			}
			tag[n++] = ']';
			tag[n] = '\0';
			raspi5_hvs_trace_u32(tag, value);
		}
	}
}

static void raspi5_hvs_dump_dlist_window(uint32_t head_word_offset)
{
	/*
	 * Trace a small window of dlist SRAM starting from head_word_offset
	 * (a word offset, not a byte offset, per HVS6 LPTRS encoding).
	 * Every read is bounded to the dlist SRAM extent so a corrupted or
	 * surprising LPTRS value can't drag us off the end of the HVS
	 * window. If the head offset itself is out of range, just trace
	 * the reason and return.
	 */
	uint32_t word_offset = head_word_offset;

	if (word_offset >= RASPI5_HVS_DLIST_WORD_COUNT) {
		platform_uart_puts(
		    "raspi5 hvs: dlist head out of range; skipping dlist dump\n");
		return;
	}

	platform_uart_puts("raspi5 hvs: dlist dump (24 words from active head)\n");
	for (uint32_t i = 0u; i < RASPI5_HVS_PROBE_DLIST_TRACE_WORDS; i++) {
		uint32_t off;
		uint32_t value;
		char tag[20];
		uint32_t n;

		if (word_offset + i >= RASPI5_HVS_DLIST_WORD_COUNT) {
			platform_uart_puts(
			    "raspi5 hvs: dlist dump truncated (window edge)\n");
			break;
		}
		off = RASPI5_HVS_DLIST_OFFSET + (word_offset + i) * 4u;
		value = raspi5_hvs_read(off);

		/* tag = "  dl[NN]" with NN as two-digit decimal */
		n = 0u;
		tag[n++] = ' ';
		tag[n++] = ' ';
		tag[n++] = 'd';
		tag[n++] = 'l';
		tag[n++] = '[';
		if (i >= 10u)
			tag[n++] = (char)('0' + (i / 10u));
		tag[n++] = (char)('0' + (i % 10u));
		tag[n++] = ']';
		tag[n] = '\0';
		raspi5_hvs_trace_u32(tag, value);
	}
}

static void raspi5_hvs_select_layout(uint32_t version,
                                     raspi5_hvs_layout_t *layout)
{
	uint8_t rev = (uint8_t)(version & RASPI5_HVS_VERSION_REV_MASK);
	layout->revision = rev;
	layout->known = 0u;

	if (rev == RASPI5_HVS_REV_BCM2712_D0) {
		layout->channel_base[0] = RASPI5_HVS_D0_CHANNEL_BASE0;
		layout->channel_base[1] =
		    RASPI5_HVS_D0_CHANNEL_BASE0 + 1u * RASPI5_HVS_D0_CHANNEL_STRIDE;
		layout->channel_base[2] =
		    RASPI5_HVS_D0_CHANNEL_BASE0 + 2u * RASPI5_HVS_D0_CHANNEL_STRIDE;
		layout->disp_ctrl0_offset = RASPI5_HVS_D0_DISP_CTRL0_OFFSET;
		layout->disp_ctrl1_offset = RASPI5_HVS_D0_DISP_CTRL1_OFFSET;
		layout->disp_lptrs_offset = RASPI5_HVS_D0_DISP_LPTRS_OFFSET;
		layout->disp_dl_offset = RASPI5_HVS_D0_DISP_DL_OFFSET;
		layout->known = 1u;
		return;
	}
	if (rev == RASPI5_HVS_REV_BCM2712_C0) {
		layout->channel_base[0] = RASPI5_HVS_C0_CHANNEL_BASE0;
		layout->channel_base[1] =
		    RASPI5_HVS_C0_CHANNEL_BASE0 + 1u * RASPI5_HVS_C0_CHANNEL_STRIDE;
		layout->channel_base[2] =
		    RASPI5_HVS_C0_CHANNEL_BASE0 + 2u * RASPI5_HVS_C0_CHANNEL_STRIDE;
		layout->disp_ctrl0_offset = RASPI5_HVS_C0_DISP_CTRL0_OFFSET;
		layout->disp_ctrl1_offset = RASPI5_HVS_C0_DISP_CTRL1_OFFSET;
		layout->disp_lptrs_offset = RASPI5_HVS_C0_DISP_LPTRS_OFFSET;
		layout->disp_dl_offset = RASPI5_HVS_C0_DISP_DL_OFFSET;
		layout->known = 1u;
		return;
	}
	/* Unknown silicon revision. Leave channel_base[] zeroed so the
	 * caller skips per-channel reads; the wide-scan + dlist-scan
	 * dumps still run unconditionally and are sufficient to learn
	 * the new layout without risking misleading reads through
	 * D0-assumed offsets. layout->known stays 0. */
	layout->channel_base[0] = 0u;
	layout->channel_base[1] = 0u;
	layout->channel_base[2] = 0u;
	layout->disp_ctrl0_offset = 0u;
	layout->disp_ctrl1_offset = 0u;
	layout->disp_lptrs_offset = 0u;
	layout->disp_dl_offset = 0u;
}

int raspi5_hvs_probe_passive(raspi5_hvs_probe_state_t *state)
{
	uint8_t ch;
	uint8_t enabled_count = 0u;
	uint8_t first_enabled = 0xffu;
	uint32_t version;

	if (!state)
		return -1;

	platform_uart_puts("raspi5 hvs: probe start (read-only)\n");
	raspi5_hvs_trace_u32("raspi5 hvs: base_lo",
	                     (uint32_t)(RASPI5_HVS_BASE & 0xffffffffu));
	raspi5_hvs_trace_u32("raspi5 hvs: base_hi",
	                     (uint32_t)(RASPI5_HVS_BASE >> 32));
	raspi5_hvs_trace_u32("raspi5 hvs: window_size", (uint32_t)RASPI5_HVS_SIZE);

	version = raspi5_hvs_read(RASPI5_HVS_VERSION_OFFSET);
	state->version = version;
	raspi5_hvs_trace_u32("raspi5 hvs: version_word", version);
	raspi5_hvs_select_layout(version, &state->layout);
	raspi5_hvs_trace_u32("raspi5 hvs: revision_byte",
	                     (uint32_t)state->layout.revision);
	raspi5_hvs_trace_u32("raspi5 hvs: layout_known",
	                     (uint32_t)state->layout.known);
	raspi5_hvs_trace_u32("raspi5 hvs: ch0_base",
	                     state->layout.channel_base[0]);
	raspi5_hvs_trace_u32("raspi5 hvs: ch1_base",
	                     state->layout.channel_base[1]);
	raspi5_hvs_trace_u32("raspi5 hvs: ch2_base",
	                     state->layout.channel_base[2]);
	raspi5_hvs_trace_u32("raspi5 hvs: ctrl0_off",
	                     state->layout.disp_ctrl0_offset);
	raspi5_hvs_trace_u32("raspi5 hvs: ctrl1_off",
	                     state->layout.disp_ctrl1_offset);
	raspi5_hvs_trace_u32("raspi5 hvs: lptrs_off",
	                     state->layout.disp_lptrs_offset);
	raspi5_hvs_trace_u32("raspi5 hvs: dl_off",
	                     state->layout.disp_dl_offset);

	if (!state->layout.known) {
		platform_uart_puts(
		    "raspi5 hvs: unknown silicon revision — skipping per-channel "
		    "named reads; relying on wide scan + dlist scan below.\n");
		state->channel_enabled_count = 0u;
		state->active_channel = 0xffu;
		goto wide_scan;
	}

	for (ch = 0u; ch < RASPI5_HVS_CHANNEL_COUNT; ch++) {
		uint32_t chan_base = state->layout.channel_base[ch];
		uint32_t ctrl0 =
		    raspi5_hvs_read(chan_base + state->layout.disp_ctrl0_offset);
		uint32_t ctrl1 =
		    raspi5_hvs_read(chan_base + state->layout.disp_ctrl1_offset);
		uint32_t lptrs =
		    raspi5_hvs_read(chan_base + state->layout.disp_lptrs_offset);
		uint32_t dl =
		    raspi5_hvs_read(chan_base + state->layout.disp_dl_offset);
		/* Check both CTRL0 bit 31 (HVS5 convention) and CTRL1 bit 31
		 * (apparent HVS6 D0 convention per the v2 trace where channel
		 * 0's CTRL1=0xc0001120 with bits 31+30 set on the active
		 * channel). Treat either as "enabled" until M9.2 nails down
		 * which bit really means "running". */
		uint8_t enabled = ((ctrl0 & RASPI5_HVS_DISP_CTRL0_ENABLE_BIT) ||
		                   (ctrl1 & RASPI5_HVS_DISP_CTRL0_ENABLE_BIT))
		                      ? 1u
		                      : 0u;

		state->channel_ctrl0[ch] = ctrl0;
		state->channel_ctrl1[ch] = ctrl1;
		state->channel_lptrs[ch] = lptrs;
		state->channel_dl[ch] = dl;
		state->channel_enabled[ch] = enabled;

		raspi5_hvs_trace_label_index("raspi5 hvs: ch", ch, "ctrl0", ctrl0);
		raspi5_hvs_trace_label_index("raspi5 hvs: ch", ch, "ctrl1", ctrl1);
		raspi5_hvs_trace_label_index("raspi5 hvs: ch", ch, "lptrs", lptrs);
		raspi5_hvs_trace_label_index("raspi5 hvs: ch", ch, "dl", dl);
		raspi5_hvs_trace_label_index("raspi5 hvs: ch", ch, "enabled",
		                             (uint32_t)enabled);

		if (enabled) {
			enabled_count++;
			if (first_enabled == 0xffu)
				first_enabled = ch;
		}
	}

	state->channel_enabled_count = enabled_count;
	state->active_channel = first_enabled;

	raspi5_hvs_trace_u32("raspi5 hvs: channels_enabled_any",
	                     (uint32_t)enabled_count);

wide_scan:
	/*
	 * v2: wide-scan dump of non-zero MMIO words in the first 0x200
	 * bytes of HVS MMIO. Catches registers our named-offset reads
	 * miss (RUN at +0x4c, global state at +0x00..+0x2c, channel-3
	 * spillover beyond +0x8c, undocumented HVS6-specific control).
	 * Cheap and definitive: if there's a non-zero register anywhere
	 * in this window, the trace shows it.
	 */
	platform_uart_puts("raspi5 hvs: wide scan (non-zero words in [0x000, 0x200))\n");
	raspi5_hvs_dump_offset_trace(0x000u,
	                             RASPI5_HVS_PROBE_WIDE_SCAN_BYTES,
	                             "raspi5 hvs: mmio");

	/*
	 * v2: unconditional dlist SRAM dump. The v1 probe gated the dlist
	 * dump on a non-trivial LPTRS, but the v1 trace showed LPTRS =
	 * 0xffffffff (possibly invalid offset, possibly different register).
	 * Dumping the first 64 dlist words unconditionally lets us see
	 * whether firmware wrote anything to dlist SRAM regardless of
	 * what LPTRS reports.
	 */
	platform_uart_puts("raspi5 hvs: dlist scan (non-zero words in first 64 dlist slots)\n");
	raspi5_hvs_dump_offset_trace(RASPI5_HVS_DLIST_OFFSET,
	                             RASPI5_HVS_PROBE_DLIST_UNCONDITIONAL_WORDS * 4u,
	                             "raspi5 hvs: dlist");

	if (first_enabled == 0xffu) {
		platform_uart_puts(
		    "raspi5 hvs: no channel reports CTRL0.ENABLE — but see wide "
		    "scan above; CTRL1 bit 31 may carry the enable on HVS6.\n");
		return 0;
	}

	raspi5_hvs_trace_u32("raspi5 hvs: active_channel",
	                     (uint32_t)first_enabled);

	{
		uint32_t lptrs = state->channel_lptrs[first_enabled];
		uint32_t latched_dl = state->channel_dl[first_enabled];
		uint32_t lptrs_head = lptrs & RASPI5_HVS_DISP_LPTRS_HEAD_MASK;
		uint32_t dl_head = latched_dl & RASPI5_HVS_DISP_DL_HEAD_MASK;

		raspi5_hvs_trace_u32("raspi5 hvs: active_lptrs_head", lptrs_head);
		raspi5_hvs_trace_u32("raspi5 hvs: active_dl_head", dl_head);

		if (lptrs_head < RASPI5_HVS_BOOTLOADER_DLIST_END) {
			platform_uart_puts(
			    "raspi5 hvs: note — LPTRS head inside bootloader-reserved "
			    "region (<32 words)\n");
		}

		/*
		 * Dump from the latched DL head (what the channel is actually
		 * scanning out from) when available — that's the authoritative
		 * pointer for M9.2's locator. Fall back to LPTRS when DL is
		 * zero (no latched dlist yet, firmware paused, etc.).
		 */
		raspi5_hvs_dump_dlist_window(dl_head != 0u ? dl_head : lptrs_head);
	}

	platform_uart_puts("raspi5 hvs: probe done\n");
	return 0;
}
