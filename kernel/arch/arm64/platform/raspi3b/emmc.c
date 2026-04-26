/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * emmc.c - BCM2835 EMMC/SDHCI block driver for Raspberry Pi 3.
 */

#include "../platform.h"
#include "blkdev.h"
#include <stdint.h>

#ifndef DRUNIX_DISK_SECTORS
#define DRUNIX_DISK_SECTORS 262144u
#endif

#define EMMC_BASE (PLATFORM_PERIPHERAL_BASE + 0x300000u)

#define EMMC_ARG2 0x00u
#define EMMC_BLKSIZECNT 0x04u
#define EMMC_ARG1 0x08u
#define EMMC_CMDTM 0x0Cu
#define EMMC_RESP0 0x10u
#define EMMC_DATA 0x20u
#define EMMC_STATUS 0x24u
#define EMMC_CONTROL0 0x28u
#define EMMC_CONTROL1 0x2Cu
#define EMMC_INTERRUPT 0x30u
#define EMMC_IRPT_MASK 0x34u
#define EMMC_IRPT_EN 0x38u

#define STATUS_CMD_INHIBIT (1u << 0)
#define STATUS_DAT_INHIBIT (1u << 1)

#define CONTROL1_CLK_INTLEN (1u << 0)
#define CONTROL1_CLK_STABLE (1u << 1)
#define CONTROL1_CLK_EN (1u << 2)
#define CONTROL1_DATA_TOUNIT_SHIFT 16u
#define CONTROL1_SRST_HC (1u << 24)

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

#define EMMC_TIMEOUT 1000000u
#define EMMC_INIT_RETRIES 1000u

static uint32_t g_emmc_rca;
static uint32_t g_emmc_block_addressing;
static int g_emmc_ready;

static volatile uint32_t *emmc_regs(void)
{
	return (volatile uint32_t *)(uintptr_t)EMMC_BASE;
}

static uint32_t emmc_read(uint32_t reg)
{
	return emmc_regs()[reg / sizeof(uint32_t)];
}

static void emmc_write(uint32_t reg, uint32_t value)
{
	emmc_regs()[reg / sizeof(uint32_t)] = value;
}

static void emmc_delay(void)
{
	for (volatile uint32_t i = 0; i < 1000u; i++)
		__asm__ volatile("nop");
}

static int emmc_wait_clear(uint32_t reg, uint32_t mask)
{
	for (uint32_t i = 0; i < EMMC_TIMEOUT; i++) {
		if ((emmc_read(reg) & mask) == 0u)
			return 0;
	}
	return -1;
}

static int emmc_wait_set(uint32_t reg, uint32_t mask)
{
	for (uint32_t i = 0; i < EMMC_TIMEOUT; i++) {
		if ((emmc_read(reg) & mask) == mask)
			return 0;
	}
	return -1;
}

static int emmc_wait_interrupt(uint32_t mask)
{
	for (uint32_t i = 0; i < EMMC_TIMEOUT; i++) {
		uint32_t status = emmc_read(EMMC_INTERRUPT);

		if (status & (INT_ERR | INT_ERROR_MASK)) {
			emmc_write(EMMC_INTERRUPT, status);
			return -1;
		}
		if (status & mask)
			return 0;
	}
	return -1;
}

static uint32_t emmc_cmdtm(uint32_t cmd, uint32_t flags)
{
	return (cmd << 24) | flags;
}

static int
emmc_send_command(uint32_t cmd, uint32_t arg, uint32_t flags, uint32_t *resp0)
{
	uint32_t inhibit = STATUS_CMD_INHIBIT;

	if (flags & CMD_ISDATA)
		inhibit |= STATUS_DAT_INHIBIT;
	if (emmc_wait_clear(EMMC_STATUS, inhibit) != 0)
		return -1;

	emmc_write(EMMC_INTERRUPT, INT_ALL);
	emmc_write(EMMC_ARG1, arg);
	emmc_write(EMMC_CMDTM, emmc_cmdtm(cmd, flags));

	if (emmc_wait_interrupt(INT_CMD_DONE) != 0)
		return -1;
	if (resp0)
		*resp0 = emmc_read(EMMC_RESP0);
	emmc_write(EMMC_INTERRUPT, INT_CMD_DONE);
	return 0;
}

static int emmc_app_command(uint32_t acmd, uint32_t arg, uint32_t *resp0)
{
	uint32_t rca_arg = g_emmc_rca << 16;

	if (emmc_send_command(SD_CMD_APP_CMD,
	                      rca_arg,
	                      CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                      0) != 0)
		return -1;
	return emmc_send_command(acmd, arg, CMD_RSPNS_TYPE_48, resp0);
}

static void emmc_set_clock_divider(uint32_t divider)
{
	uint32_t control1;

	control1 = emmc_read(EMMC_CONTROL1);
	control1 &= ~CONTROL1_CLK_EN;
	emmc_write(EMMC_CONTROL1, control1);

	control1 &= ~((0xFFu << 8) | (0x3u << 6));
	control1 |= ((divider & 0xFFu) << 8) | (((divider >> 8) & 0x3u) << 6);
	control1 |= CONTROL1_CLK_INTLEN | (0xEu << CONTROL1_DATA_TOUNIT_SHIFT);
	emmc_write(EMMC_CONTROL1, control1);
	(void)emmc_wait_set(EMMC_CONTROL1, CONTROL1_CLK_STABLE);
	emmc_write(EMMC_CONTROL1, control1 | CONTROL1_CLK_EN);
}

static int emmc_reset(void)
{
	emmc_write(EMMC_CONTROL1, CONTROL1_SRST_HC);
	if (emmc_wait_clear(EMMC_CONTROL1, CONTROL1_SRST_HC) != 0)
		return -1;

	emmc_write(EMMC_CONTROL0, 0u);
	emmc_write(EMMC_IRPT_EN, 0u);
	emmc_write(EMMC_IRPT_MASK, INT_ALL);
	emmc_write(EMMC_INTERRUPT, INT_ALL);
	emmc_set_clock_divider(0x80u);
	return 0;
}

static int emmc_init_card(void)
{
	uint32_t resp = 0;

	if (emmc_reset() != 0)
		return -1;
	if (emmc_send_command(SD_CMD_GO_IDLE, 0u, 0u, 0) != 0)
		return -1;
	emmc_delay();

	if (emmc_send_command(SD_CMD_SEND_IF_COND,
	                      SD_IF_COND_ARG,
	                      CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                      &resp) != 0)
		return -1;
	if ((resp & 0xFFFu) != SD_IF_COND_ARG)
		return -1;

	for (uint32_t i = 0; i < EMMC_INIT_RETRIES; i++) {
		if (emmc_app_command(SD_ACMD_SD_SEND_OP_COND, SD_ACMD41_ARG, &resp) !=
		    0)
			return -1;
		if (resp & SD_OCR_POWER_UP)
			break;
		emmc_delay();
	}
	if ((resp & SD_OCR_POWER_UP) == 0u)
		return -1;
	g_emmc_block_addressing = (resp & SD_OCR_HIGH_CAPACITY) != 0u;

	if (emmc_send_command(
	        SD_CMD_ALL_SEND_CID, 0u, CMD_RSPNS_TYPE_136 | CMD_CRCCHK_EN, 0) !=
	    0)
		return -1;
	if (emmc_send_command(SD_CMD_SEND_REL_ADDR,
	                      0u,
	                      CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                      &resp) != 0)
		return -1;
	g_emmc_rca = resp >> 16;
	if (g_emmc_rca == 0u)
		return -1;

	if (emmc_send_command(SD_CMD_SELECT_CARD,
	                      g_emmc_rca << 16,
	                      CMD_RSPNS_TYPE_48B | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                      0) != 0)
		return -1;
	if (emmc_send_command(SD_CMD_SET_BLOCKLEN,
	                      BLKDEV_SECTOR_SIZE,
	                      CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN,
	                      0) != 0)
		return -1;

	g_emmc_ready = 1;
	return 0;
}

static uint32_t emmc_card_lba_arg(uint32_t lba)
{
	return g_emmc_block_addressing ? lba : lba * BLKDEV_SECTOR_SIZE;
}

static void emmc_store_le32(uint8_t *buf, uint32_t word)
{
	buf[0] = (uint8_t)word;
	buf[1] = (uint8_t)(word >> 8);
	buf[2] = (uint8_t)(word >> 16);
	buf[3] = (uint8_t)(word >> 24);
}

static uint32_t emmc_load_le32(const uint8_t *buf)
{
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static int emmc_read_sector(uint32_t lba, uint8_t *buf)
{
	if (!g_emmc_ready || !buf || lba >= DRUNIX_DISK_SECTORS)
		return -1;

	emmc_write(EMMC_BLKSIZECNT, (1u << 16) | BLKDEV_SECTOR_SIZE);
	if (emmc_send_command(SD_CMD_READ_SINGLE_BLOCK,
	                      emmc_card_lba_arg(lba),
	                      CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN |
	                          CMD_ISDATA | TM_BLKCNT_EN |
	                          TM_DAT_DIR_CARD_TO_HOST,
	                      0) != 0)
		return -1;
	if (emmc_wait_interrupt(INT_READ_RDY) != 0)
		return -1;

	for (uint32_t i = 0; i < BLKDEV_SECTOR_SIZE; i += sizeof(uint32_t))
		emmc_store_le32(buf + i, emmc_read(EMMC_DATA));

	if (emmc_wait_interrupt(INT_DATA_DONE) != 0)
		return -1;
	emmc_write(EMMC_INTERRUPT, INT_READ_RDY | INT_DATA_DONE);
	return 0;
}

static int emmc_write_sector(uint32_t lba, const uint8_t *buf)
{
	if (!g_emmc_ready || !buf || lba >= DRUNIX_DISK_SECTORS)
		return -1;

	emmc_write(EMMC_BLKSIZECNT, (1u << 16) | BLKDEV_SECTOR_SIZE);
	if (emmc_send_command(SD_CMD_WRITE_SINGLE_BLOCK,
	                      emmc_card_lba_arg(lba),
	                      CMD_RSPNS_TYPE_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN |
	                          CMD_ISDATA | TM_BLKCNT_EN,
	                      0) != 0)
		return -1;
	if (emmc_wait_interrupt(INT_WRITE_RDY) != 0)
		return -1;

	for (uint32_t i = 0; i < BLKDEV_SECTOR_SIZE; i += sizeof(uint32_t))
		emmc_write(EMMC_DATA, emmc_load_le32(buf + i));

	if (emmc_wait_interrupt(INT_DATA_DONE) != 0)
		return -1;
	emmc_write(EMMC_INTERRUPT, INT_WRITE_RDY | INT_DATA_DONE);
	return 0;
}

static const blkdev_ops_t emmc_ops = {
    .read_sector = emmc_read_sector,
    .write_sector = emmc_write_sector,
};

int platform_block_register(void)
{
	if (!g_emmc_ready && emmc_init_card() != 0) {
		platform_uart_puts("ARM64 EMMC init failed\n");
		return -1;
	}

	if (blkdev_register_disk("sda", 8u, 0u, DRUNIX_DISK_SECTORS, &emmc_ops) !=
	    0)
		return -1;

	platform_uart_puts("ARM64 EMMC disk registered\n");
	return 0;
}
