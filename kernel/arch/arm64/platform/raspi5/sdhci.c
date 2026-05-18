/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * sdhci.c - BCM2712 SDHCI block driver for Raspberry Pi 5.
 *
 * The BCM2712 SDIO1 controller (compatible "brcm,bcm2712-sdhci",
 * "brcm,sdhci-brcmstb" in the live device tree) implements SD Host
 * Controller Simplified Specification v3.00. Register layout is
 * standard SDHCI; the Broadcom-specific cfg block at
 * PLATFORM_RASPI5_SDHCI_CFG_BASE drives card-detect override,
 * SD/eMMC pin selection, and PHY clock-source setup.
 *
 * Pi 5 firmware may leave the controller in a partially initialized
 * SDHCI state after loading kernel8.img. The driver keeps the firmware
 * voltage mode, applies the BCM2712 cfg-block setup Linux uses for
 * this host, resets CMD/DAT state, restores SDHCI bus power, masks
 * signal-enable to suppress stray interrupts, then issues CMD0. Slow
 * init clock (~400 kHz from a hardcoded divider) is used for the
 * entire boot, like raspi3b — sufficient to mount ext3 and load
 * /bin/shell within seconds.
 *
 * Single-block PIO (CMD17) only. The blkdev_ops_t layer is
 * sector-granular; ext3 issues 8 sequential read_sector calls per
 * 4 KiB filesystem block. Multi-block (CMD18) and DMA are deferred.
 */

#include "../platform.h"
#include "blkdev.h"
#include "sdhci.h"
#include <stdint.h>

#ifndef DRUNIX_DISK_SECTORS
/* 16 GiB worth of 512-byte sectors. MBR scan limits to actual partition
 * sectors, so generous is safe. M6 caps reads to disk_sectors at the
 * blkdev layer; the actual usable range comes from the SD card's CSD. */
#define DRUNIX_DISK_SECTORS 33554432u
#endif

/* SDHCI v3.00 register offsets (Simplified Spec table 2-1). */
#define SDHCI_SDMA 0x00u
#define SDHCI_BLOCK_SIZE_COUNT 0x04u
#define SDHCI_ARGUMENT 0x08u
#define SDHCI_CMD_TRANSFER 0x0Cu
#define SDHCI_RESPONSE_0 0x10u
#define SDHCI_RESPONSE_2 0x18u
#define SDHCI_BUFFER 0x20u
#define SDHCI_PRESENT_STATE 0x24u
#define SDHCI_HOST_CONTROL_1 0x28u
#define SDHCI_POWER_CONTROL 0x29u
#define SDHCI_CLOCK_CONTROL 0x2Cu
#define SDHCI_INT_STATUS 0x30u
#define SDHCI_INT_STATUS_ENABLE 0x34u
#define SDHCI_INT_SIGNAL_ENABLE 0x38u
#define SDHCI_HOST_CONTROL_2 0x3Eu
#define SDHCI_CAPABILITIES 0x40u

/* Broadcom CFG block at PLATFORM_RASPI5_SDHCI_CFG_BASE. Offsets and bit
 * positions copied from Linux drivers/mmc/host/sdhci-brcmstb.c. BCM2712
 * routes card-detect override, SD/eMMC pinmux, and PHY clock-source
 * selection through this block. */
#define BCM2712_CFG_CTRL 0x000u
#define BCM2712_CFG_CTRL_SDCD_N_TEST_EN (1u << 31)
#define BCM2712_CFG_CTRL_SDCD_N_TEST_LEV (1u << 30)
#define BCM2712_CFG_SD_PIN_SEL 0x044u
#define BCM2712_CFG_SD_PIN_SEL_MASK 0x3u
#define BCM2712_CFG_SD_PIN_SEL_SD (1u << 1)
#define BCM2712_CFG_SD_PIN_SEL_MMC (1u << 0)
#define BCM2712_CFG_MAX_50MHZ_MODE 0x1ACu
#define BCM2712_CFG_MAX_50MHZ_MODE_STRAP_OVERRIDE (1u << 31)
#define BCM2712_CFG_MAX_50MHZ_MODE_ENABLE (1u << 0)

#define PSTATE_CMD_INHIBIT (1u << 0)
#define PSTATE_DAT_INHIBIT (1u << 1)

#define CLOCK_CTRL_INT_CLK_EN (1u << 0)
#define CLOCK_CTRL_INT_CLK_STABLE (1u << 1)
#define CLOCK_CTRL_SD_CLK_EN (1u << 2)
#define CLOCK_CTRL_DATA_TOUNIT_SHIFT 16u
#define CLOCK_CTRL_SRST_HC (1u << 24)
#define CLOCK_CTRL_SRST_CMD (1u << 25)
#define CLOCK_CTRL_SRST_DAT (1u << 26)
#define CLOCK_CTRL_SRST_CMDDAT (CLOCK_CTRL_SRST_CMD | CLOCK_CTRL_SRST_DAT)

#define HOST_CONTROL_2_S18EN (1u << 3) /* 1.8V Signaling Enable */

#define POWER_CTRL_BUS_POWER (1u << 0)
#define POWER_CTRL_VDD1_MASK (0x07u << 1)
#define POWER_CTRL_VDD1_3_3V (0x07u << 1) /* SDR 3.3V */

#define INT_CMD_DONE (1u << 0)
#define INT_DATA_DONE (1u << 1)
#define INT_WRITE_RDY (1u << 4)
#define INT_READ_RDY (1u << 5)
#define INT_ERR (1u << 15)
#define INT_ERROR_MASK 0x017F8000u
#define INT_ALL 0xFFFFFFFFu

#define CMD_RSPNS_TYPE_136 (1u << 16)
#define CMD_RSPNS_TYPE_48 (2u << 16)
#define CMD_RSPNS_TYPE_48B (3u << 16)
#define CMD_CRCCHK_EN (1u << 19)
#define CMD_IXCHK_EN (1u << 20)
#define CMD_ISDATA (1u << 21)
#define TM_BLKCNT_EN (1u << 1)
#define TM_DAT_DIR_CARD_TO_HOST (1u << 4)

#define SD_CMD_GO_IDLE 0u
#define SD_CMD_ALL_SEND_CID 2u
#define SD_CMD_SEND_REL_ADDR 3u
#define SD_CMD_SELECT_CARD 7u
#define SD_CMD_SEND_IF_COND 8u
#define SD_CMD_SET_BLOCKLEN 16u
#define SD_CMD_READ_SINGLE_BLOCK 17u
#define SD_CMD_WRITE_SINGLE_BLOCK 24u
#define SD_CMD_APP_CMD 55u
#define SD_ACMD_SD_SEND_OP_COND 41u

#define SD_OCR_POWER_UP (1u << 31)
#define SD_OCR_HIGH_CAPACITY (1u << 30)
#define SD_ACMD41_ARG 0x40FF8000u
#define SD_IF_COND_ARG 0x000001AAu

#define SDHCI_TIMEOUT 1000000u
#define SDHCI_INIT_RETRIES 1000u

static uint32_t g_sdhci_rca;
static uint32_t g_sdhci_block_addressing;
static uint32_t g_sdhci_card_sectors;
static int g_sdhci_ready;

static void sdhci_diag_hex32(const char *label, uint32_t v);

static volatile uint32_t *sdhci_regs(void)
{
	return (volatile uint32_t *)(uintptr_t)PLATFORM_RASPI5_SDHCI_HOST_BASE;
}

static uint32_t sdhci_cfg_read(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(PLATFORM_RASPI5_SDHCI_CFG_BASE +
	                                         off);
}

static void sdhci_cfg_write(uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)(PLATFORM_RASPI5_SDHCI_CFG_BASE + off) =
	    value;
}

static uint32_t sdhci_read(uint32_t reg)
{
	return sdhci_regs()[reg / sizeof(uint32_t)];
}

static void sdhci_write(uint32_t reg, uint32_t value)
{
	sdhci_regs()[reg / sizeof(uint32_t)] = value;
}

static uint16_t sdhci_read_u16(uint32_t reg)
{
	return *(volatile uint16_t *)(uintptr_t)(PLATFORM_RASPI5_SDHCI_HOST_BASE +
	                                         reg);
}

static uint8_t sdhci_read_u8(uint32_t reg)
{
	return *(volatile uint8_t *)(uintptr_t)(PLATFORM_RASPI5_SDHCI_HOST_BASE +
	                                        reg);
}

static void sdhci_write_u8(uint32_t reg, uint8_t value)
{
	*(volatile uint8_t *)(uintptr_t)(PLATFORM_RASPI5_SDHCI_HOST_BASE + reg) =
	    value;
}

static void sdhci_delay(void)
{
	for (volatile uint32_t i = 0; i < 1000u; i++)
		__asm__ volatile("nop");
}

static void sdhci_delay_ms_approx(uint32_t ms)
{
	for (volatile uint32_t i = 0; i < ms * 50000u; i++)
		__asm__ volatile("nop");
}

static int sdhci_wait_clear(uint32_t reg, uint32_t mask)
{
	for (uint32_t i = 0; i < SDHCI_TIMEOUT; i++) {
		if ((sdhci_read(reg) & mask) == 0u)
			return 0;
	}
	return -1;
}

static int sdhci_wait_set(uint32_t reg, uint32_t mask)
{
	for (uint32_t i = 0; i < SDHCI_TIMEOUT; i++) {
		if ((sdhci_read(reg) & mask) == mask)
			return 0;
	}
	return -1;
}

static int sdhci_wait_interrupt(uint32_t mask)
{
	for (uint32_t i = 0; i < SDHCI_TIMEOUT; i++) {
		uint32_t status = sdhci_read(SDHCI_INT_STATUS);

		if (status & (INT_ERR | INT_ERROR_MASK)) {
			sdhci_write(SDHCI_INT_STATUS, status);
			return -1;
		}
		if (status & mask)
			return 0;
	}
	return -1;
}

static uint32_t sdhci_cmd_xfer(uint32_t cmd, uint32_t flags)
{
	return (cmd << 24) | flags;
}

static int sdhci_send_command(uint32_t cmd,
                              uint32_t arg,
                              uint32_t flags,
                              uint32_t *resp0)
{
	uint32_t inhibit = PSTATE_CMD_INHIBIT;

	if (flags & CMD_ISDATA)
		inhibit |= PSTATE_DAT_INHIBIT;
	if (sdhci_wait_clear(SDHCI_PRESENT_STATE, inhibit) != 0) {
		sdhci_diag_hex32("raspi5: CMD inhibit stuck, PRESENT_STATE",
		                 sdhci_read(SDHCI_PRESENT_STATE));
		return -1;
	}

	sdhci_write(SDHCI_INT_STATUS, INT_ALL);
	sdhci_write(SDHCI_ARGUMENT, arg);
	sdhci_write(SDHCI_CMD_TRANSFER, sdhci_cmd_xfer(cmd, flags));

	if (sdhci_wait_interrupt(INT_CMD_DONE) != 0) {
		sdhci_diag_hex32("raspi5: CMD_DONE timeout, INT_STATUS",
		                 sdhci_read(SDHCI_INT_STATUS));
		sdhci_diag_hex32("raspi5: CMD_DONE timeout, PRESENT_STATE",
		                 sdhci_read(SDHCI_PRESENT_STATE));
		return -1;
	}
	if (resp0)
		*resp0 = sdhci_read(SDHCI_RESPONSE_0);
	sdhci_write(SDHCI_INT_STATUS, INT_CMD_DONE);
	return 0;
}

static int sdhci_app_command(uint32_t acmd, uint32_t arg, uint32_t *resp0)
{
	uint32_t rca_arg = g_sdhci_rca << 16;

	if (sdhci_send_command(SD_CMD_APP_CMD,
	                       rca_arg,
	                       CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                       0) != 0)
		return -1;
	return sdhci_send_command(acmd, arg, CMD_RSPNS_TYPE_48, resp0);
}

static void sdhci_set_clock_divider(uint32_t divider)
{
	uint32_t control;

	control = sdhci_read(SDHCI_CLOCK_CONTROL);
	control &= ~CLOCK_CTRL_SD_CLK_EN;
	sdhci_write(SDHCI_CLOCK_CONTROL, control);

	/* v3.00 split divider: bits 15:8 (low 8), bits 7:6 (high 2). */
	control &= ~((0xFFu << 8) | (0x3u << 6));
	control |= ((divider & 0xFFu) << 8) | (((divider >> 8) & 0x3u) << 6);
	control |= CLOCK_CTRL_INT_CLK_EN | (0xEu << CLOCK_CTRL_DATA_TOUNIT_SHIFT);
	sdhci_write(SDHCI_CLOCK_CONTROL, control);
	(void)sdhci_wait_set(SDHCI_CLOCK_CONTROL, CLOCK_CTRL_INT_CLK_STABLE);
	sdhci_write(SDHCI_CLOCK_CONTROL, control | CLOCK_CTRL_SD_CLK_EN);
}

static void sdhci_diag_hex32(const char *label, uint32_t v)
{
	char line[48];
	uint32_t hi = (v >> 16) & 0xFFFFu;
	uint32_t lo = v & 0xFFFFu;

	(void)hi;
	(void)lo;
	platform_uart_puts(label);
	platform_uart_puts("=0x");
	{
		static const char hexd[] = "0123456789abcdef";
		char b[10];
		int i;
		for (i = 0; i < 8; i++)
			b[i] = hexd[(v >> ((7 - i) * 4)) & 0xFu];
		b[8] = '\n';
		b[9] = '\0';
		platform_uart_puts(b);
	}
	(void)label;
	(void)line;
}

static void sdhci_restore_power(uint8_t pre_power)
{
	uint8_t power = pre_power | POWER_CTRL_BUS_POWER;

	if ((power & POWER_CTRL_VDD1_MASK) == 0u)
		power |= POWER_CTRL_VDD1_3_3V;
	sdhci_write_u8(SDHCI_POWER_CONTROL, power);
	sdhci_delay_ms_approx(1u);
}

static int sdhci_reset(void)
{
	uint8_t pre_power;

	/* 0. Diagnostics — confirm MMIO is alive before we touch state.
	 * If the L1[64] mapping is wrong, these reads fault (sync
	 * exception) rather than returning bogus data. Also capture
	 * the firmware-left POWER_CONTROL; the failing serial trace
	 * showed bit 0 set before reset and clear before CMD0. */
	sdhci_diag_hex32("raspi5: SDHCI CAPS",
	                 sdhci_read(SDHCI_CAPABILITIES));
	pre_power = sdhci_read_u8(SDHCI_POWER_CONTROL);
	sdhci_diag_hex32("raspi5: pre-reset POWER_CONTROL", pre_power);
	sdhci_diag_hex32("raspi5: pre-reset CFG_CTRL",
	                 sdhci_cfg_read(BCM2712_CFG_CTRL));

	/* Broadcom CFG-block setup: BCM2712 routes card-detect through
	 * the CFG block instead of the standard SDHCI present-state bit.
	 * Without this, the controller treats the slot as empty and
	 * suppresses bus-power / clock to the card, which manifests as
	 * CMD0 timing out with INT_STATUS=0. Linux's sdhci-brcmstb
	 * cfginit for "brcm,bcm2712-sdhci" sets SDCD_N_TEST_EN (use the
	 * TEST_LEV bit instead of the physical pin) and clears
	 * SDCD_N_TEST_LEV (active-low = card present). */
	{
		uint32_t ctrl = sdhci_cfg_read(BCM2712_CFG_CTRL);
		ctrl &= ~BCM2712_CFG_CTRL_SDCD_N_TEST_LEV;
		ctrl |= BCM2712_CFG_CTRL_SDCD_N_TEST_EN;
		sdhci_cfg_write(BCM2712_CFG_CTRL, ctrl);
		sdhci_diag_hex32("raspi5: post-cfg CFG_CTRL",
		                 sdhci_cfg_read(BCM2712_CFG_CTRL));
	}

	/* The Pi 5 DT advertises UHS modes. Linux's BCM2712 cfginit
	 * selects the delay-line PHY clock source for UHS-capable hosts
	 * by forcing MAX_50MHZ_MODE out of strap-controlled mode. Keep
	 * that setup even though Drunix stays at identification speed for
	 * now; otherwise the cfg block can keep routing the host through
	 * the firmware-selected high-speed clock path. */
	{
		uint32_t mode = sdhci_cfg_read(BCM2712_CFG_MAX_50MHZ_MODE);
		mode &= ~BCM2712_CFG_MAX_50MHZ_MODE_ENABLE;
		mode |= BCM2712_CFG_MAX_50MHZ_MODE_STRAP_OVERRIDE;
		sdhci_cfg_write(BCM2712_CFG_MAX_50MHZ_MODE, mode);
		sdhci_diag_hex32("raspi5: post-cfg MAX_50MHZ_MODE",
		                 sdhci_cfg_read(BCM2712_CFG_MAX_50MHZ_MODE));
	}

	/* CFG_SD_PIN_SEL: select SD card pinmux rather than eMMC. Linux's
	 * sdhci_bcm2712_set_clock writes SDIO_CFG_SD_PIN_SEL based on
	 * mmc->ios.timing — SD_HS (and other SD modes) get
	 * SDIO_CFG_SD_PIN_SEL_SD (bit 1). Our card-present override above
	 * appears to reset pinmux to default on transition, so we
	 * re-establish SD pinmux explicitly here. Without this, the
	 * controller clocks commands out onto bus pins that aren't
	 * connected to the microSD slot — CMD_INHIBIT stays asserted
	 * forever because the bus never quiets. */
	{
		uint32_t sel = sdhci_cfg_read(BCM2712_CFG_SD_PIN_SEL);
		sel &= ~(uint32_t)BCM2712_CFG_SD_PIN_SEL_MASK;
		sel |= BCM2712_CFG_SD_PIN_SEL_SD;
		sdhci_cfg_write(BCM2712_CFG_SD_PIN_SEL, sel);
		sdhci_diag_hex32("raspi5: post-cfg SD_PIN_SEL",
		                 sdhci_cfg_read(BCM2712_CFG_SD_PIN_SEL));
	}

	/* 1. Software reset of CMD and DAT lines only — not the full
	 * SDHCI_CLOCK_CONTROL.SW_RST_ALL bit, because on BCM2712 that
	 * also clears more host state than this bring-up path needs. The
	 * CMD/DAT resets are enough to flush any leftover state from the
	 * firmware's 50 MHz use of the card. We then re-program the
	 * clock to slow init speed; CLOCK_CONTROL was set to ~50 MHz
	 * by firmware and stays at whatever divider firmware chose
	 * until sdhci_set_clock_divider overwrites it below. */
	sdhci_write(SDHCI_CLOCK_CONTROL,
	            sdhci_read(SDHCI_CLOCK_CONTROL) | CLOCK_CTRL_INT_CLK_EN |
	                CLOCK_CTRL_SD_CLK_EN | CLOCK_CTRL_SRST_CMDDAT);
	if (sdhci_wait_clear(SDHCI_CLOCK_CONTROL, CLOCK_CTRL_SRST_CMDDAT) != 0) {
		platform_uart_puts("raspi5: SDHCI SRST_CMD/DAT timeout\n");
		return -1;
	}

	/* 2. Do NOT clear HOST_CONTROL_2.S18EN. Pi 5 firmware leaves
	 * the controller in 1.8V UHS-I mode (POWER_CONTROL=0x06,
	 * VDD1=1.8V). Clearing S18EN cascades into VDD1=3.3V on this
	 * controller — but the card stays in 1.8V signaling mode since
	 * it was negotiated there by firmware, and there's no clean way
	 * for us to re-init the card at 3.3V without a real power-cycle
	 * (BUS_POWER is read-only on BCM2712). So we live in firmware's
	 * 1.8V world: the card and host are already matched, CMD0 over
	 * 1.8V signaling will reset the card to identification state at
	 * 1.8V (per SD spec, card stays at 1.8V across CMD0), and we
	 * proceed from there. */
	sdhci_diag_hex32("raspi5: pre-reset HOST_CONTROL_2",
	                 sdhci_read_u16(SDHCI_HOST_CONTROL_2));

	/* 3. Restore SDHCI power after CMD/DAT reset. The serial trace
	 * shows firmware hands off POWER_CONTROL=0x0f, but the reset path
	 * leaves it at 0x0e before CMD0. Preserve the firmware-selected
	 * voltage bits and reassert bus power; if firmware left no voltage
	 * selection, fall back to default-speed 3.3V. */
	sdhci_restore_power(pre_power);
	sdhci_diag_hex32("raspi5: post-power POWER_CONTROL",
	                 sdhci_read_u8(SDHCI_POWER_CONTROL));

	/* 4. Mask SDHCI interrupts at the controller. The GIC also masks
	 * SPI 273 (gicd_disable_all_spis from M5), but belt-and-braces
	 * matters when firmware may have left signal-enable on. */
	sdhci_write(SDHCI_HOST_CONTROL_1, 0u);
	sdhci_write(SDHCI_INT_SIGNAL_ENABLE, 0u);
	sdhci_write(SDHCI_INT_STATUS_ENABLE, INT_ALL);
	sdhci_write(SDHCI_INT_STATUS, INT_ALL);

	/* 5. Slow init clock — same divider raspi3b uses. From any
	 * reasonable BCM2712 clk_emmc2 base (~100-200 MHz) this lands
	 * well under the 400 kHz SD spec ceiling for card init. On this
	 * controller the clock path can clear POWER_CONTROL.BUS_POWER, so
	 * restore power again after the clock is stable. */
	sdhci_set_clock_divider(0x80u);
	sdhci_restore_power(pre_power);
	sdhci_diag_hex32("raspi5: post-clock-power POWER_CONTROL",
	                 sdhci_read_u8(SDHCI_POWER_CONTROL));
	sdhci_diag_hex32("raspi5: post-reset CLOCK_CONTROL",
	                 sdhci_read(SDHCI_CLOCK_CONTROL));
	sdhci_diag_hex32("raspi5: post-reset PRESENT_STATE",
	                 sdhci_read(SDHCI_PRESENT_STATE));
	sdhci_diag_hex32("raspi5: post-reset POWER_CONTROL",
	                 sdhci_read_u8(SDHCI_POWER_CONTROL));
	return 0;
}

static int sdhci_init_card(void)
{
	uint32_t resp = 0;

	if (sdhci_reset() != 0) {
		platform_uart_puts("raspi5: SDHCI reset failed\n");
		return -1;
	}
	if (sdhci_send_command(SD_CMD_GO_IDLE, 0u, 0u, 0) != 0) {
		platform_uart_puts("raspi5: CMD0 (GO_IDLE) failed\n");
		return -1;
	}
	sdhci_delay();

	if (sdhci_send_command(SD_CMD_SEND_IF_COND,
	                       SD_IF_COND_ARG,
	                       CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                       &resp) != 0) {
		platform_uart_puts("raspi5: CMD8 (SEND_IF_COND) failed\n");
		return -1;
	}
	if ((resp & 0xFFFu) != SD_IF_COND_ARG) {
		sdhci_diag_hex32("raspi5: CMD8 echo bad resp", resp);
		return -1;
	}

	for (uint32_t i = 0; i < SDHCI_INIT_RETRIES; i++) {
		if (sdhci_app_command(SD_ACMD_SD_SEND_OP_COND, SD_ACMD41_ARG, &resp) !=
		    0) {
			platform_uart_puts("raspi5: ACMD41 command failed\n");
			sdhci_diag_hex32("raspi5: ACMD41 last resp", resp);
			return -1;
		}
		if (resp & SD_OCR_POWER_UP)
			break;
		sdhci_delay();
	}
	if ((resp & SD_OCR_POWER_UP) == 0u) {
		platform_uart_puts("raspi5: ACMD41 timeout, no POWER_UP\n");
		sdhci_diag_hex32("raspi5: ACMD41 final resp", resp);
		return -1;
	}
	g_sdhci_block_addressing = (resp & SD_OCR_HIGH_CAPACITY) != 0u;

	if (sdhci_send_command(SD_CMD_ALL_SEND_CID,
	                       0u,
	                       CMD_RSPNS_TYPE_136 | CMD_CRCCHK_EN,
	                       0) != 0) {
		platform_uart_puts("raspi5: CMD2 (ALL_SEND_CID) failed\n");
		return -1;
	}
	if (sdhci_send_command(SD_CMD_SEND_REL_ADDR,
	                       0u,
	                       CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                       &resp) != 0) {
		platform_uart_puts("raspi5: CMD3 (SEND_REL_ADDR) failed\n");
		return -1;
	}
	g_sdhci_rca = resp >> 16;
	if (g_sdhci_rca == 0u) {
		platform_uart_puts("raspi5: CMD3 returned RCA=0\n");
		return -1;
	}

	/* Skip CSD parse (CMD9) for MVP. Hardcoded DRUNIX_DISK_SECTORS is
	 * a generous upper bound; the MBR scan limits per-partition reads
	 * to the actual partition size declared in the partition table.
	 * Cards smaller than 16 GiB just have unused trailing LBAs that
	 * MBR never asks for. Add a real CSD parse if/when this matters. */
	g_sdhci_card_sectors = DRUNIX_DISK_SECTORS;

	if (sdhci_send_command(SD_CMD_SELECT_CARD,
	                       g_sdhci_rca << 16,
	                       CMD_RSPNS_TYPE_48B | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                       0) != 0) {
		platform_uart_puts("raspi5: CMD7 (SELECT_CARD) failed\n");
		return -1;
	}
	if (sdhci_send_command(SD_CMD_SET_BLOCKLEN,
	                       BLKDEV_SECTOR_SIZE,
	                       CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                       0) != 0) {
		platform_uart_puts("raspi5: CMD16 (SET_BLOCKLEN) failed\n");
		return -1;
	}

	g_sdhci_ready = 1;
	platform_uart_puts("raspi5: SDHCI init OK\n");
	return 0;
}

static uint32_t sdhci_card_lba_arg(uint32_t lba)
{
	return g_sdhci_block_addressing ? lba : lba * BLKDEV_SECTOR_SIZE;
}

static void sdhci_store_le32(uint8_t *buf, uint32_t word)
{
	buf[0] = (uint8_t)word;
	buf[1] = (uint8_t)(word >> 8);
	buf[2] = (uint8_t)(word >> 16);
	buf[3] = (uint8_t)(word >> 24);
}

static uint32_t sdhci_load_le32(const uint8_t *buf)
{
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static int sdhci_read_sector(uint32_t lba, uint8_t *buf)
{
	if (!g_sdhci_ready || !buf || lba >= g_sdhci_card_sectors)
		return -1;

	sdhci_write(SDHCI_BLOCK_SIZE_COUNT, (1u << 16) | BLKDEV_SECTOR_SIZE);
	if (sdhci_send_command(SD_CMD_READ_SINGLE_BLOCK,
	                       sdhci_card_lba_arg(lba),
	                       CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN |
	                           CMD_ISDATA | TM_BLKCNT_EN |
	                           TM_DAT_DIR_CARD_TO_HOST,
	                       0) != 0)
		return -1;
	if (sdhci_wait_interrupt(INT_READ_RDY) != 0)
		return -1;

	for (uint32_t i = 0; i < BLKDEV_SECTOR_SIZE; i += sizeof(uint32_t))
		sdhci_store_le32(buf + i, sdhci_read(SDHCI_BUFFER));

	if (sdhci_wait_interrupt(INT_DATA_DONE) != 0)
		return -1;
	sdhci_write(SDHCI_INT_STATUS, INT_READ_RDY | INT_DATA_DONE);
	return 0;
}

static int sdhci_write_sector(uint32_t lba, const uint8_t *buf)
{
	if (!g_sdhci_ready || !buf || lba >= g_sdhci_card_sectors)
		return -1;

	sdhci_write(SDHCI_BLOCK_SIZE_COUNT, (1u << 16) | BLKDEV_SECTOR_SIZE);
	if (sdhci_send_command(SD_CMD_WRITE_SINGLE_BLOCK,
	                       sdhci_card_lba_arg(lba),
	                       CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN |
	                           CMD_ISDATA | TM_BLKCNT_EN,
	                       0) != 0)
		return -1;
	if (sdhci_wait_interrupt(INT_WRITE_RDY) != 0)
		return -1;

	for (uint32_t i = 0; i < BLKDEV_SECTOR_SIZE; i += sizeof(uint32_t))
		sdhci_write(SDHCI_BUFFER, sdhci_load_le32(buf + i));

	if (sdhci_wait_interrupt(INT_DATA_DONE) != 0)
		return -1;
	sdhci_write(SDHCI_INT_STATUS, INT_WRITE_RDY | INT_DATA_DONE);
	return 0;
}

static const blkdev_ops_t sdhci_ops = {
    .read_sector = sdhci_read_sector,
    .write_sector = sdhci_write_sector,
};

int platform_block_register(void)
{
	if (!g_sdhci_ready && sdhci_init_card() != 0) {
		platform_uart_puts("raspi5: SDHCI init failed\n");
		return -1;
	}

	if (blkdev_register_disk(
	        "sda", 8u, 0u, g_sdhci_card_sectors, &sdhci_ops) != 0)
		return -1;

	platform_uart_puts("raspi5: SDHCI disk registered as sda\n");
	return 0;
}
