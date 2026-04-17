/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ata.c — ATA PIO disk driver for the primary virtual hard disk.
 */

#include "ata.h"
#include "blkdev.h"
#include "klog.h"

extern void port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);

/* Read/write 16-bit words to/from the ATA data port using rep insw/outsw */
static inline void ata_insw(uint16_t port, uint16_t *buf, uint32_t count) {
    __asm__ volatile ("rep insw"
                      : "+D"(buf), "+c"(count)
                      : "d"(port)
                      : "memory");
}

static inline void ata_outsw(uint16_t port, const uint16_t *buf, uint32_t count) {
    __asm__ volatile ("rep outsw"
                      : "+S"(buf), "+c"(count)
                      : "d"(port)
                      : "memory");
}

/* Poll until BSY clears. Returns ATA_OK or ATA_TIMEOUT. */
static int ata_wait_bsy(void) {
    int i;
    for (i = 0; i < 100000; i++) {
        if (!(port_byte_in(ATA_PRIMARY_BASE + ATA_REG_STATUS) & ATA_SR_BSY))
            return ATA_OK;
    }
    return ATA_TIMEOUT;
}

/* Poll until DRQ sets (and BSY clears). Returns ATA_OK, ATA_ERR, or ATA_TIMEOUT. */
static int ata_wait_drq(void) {
    int i;
    uint8_t status;
    for (i = 0; i < 100000; i++) {
        status = port_byte_in(ATA_PRIMARY_BASE + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)  return ATA_ERR;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return ATA_OK;
    }
    return ATA_TIMEOUT;
}

/* 400ns delay: read alternate status register 4 times (each read ~100ns on real hw) */
static void ata_delay400(void) {
    port_byte_in(ATA_PRIMARY_CTRL);
    port_byte_in(ATA_PRIMARY_CTRL);
    port_byte_in(ATA_PRIMARY_CTRL);
    port_byte_in(ATA_PRIMARY_CTRL);
}

/* Common LBA28 transfer setup for read and write. */
static void ata_setup_lba(uint32_t lba, uint8_t slave) {
    /* bit 6 = LBA mode, bit 5 = 1 (obsolete), bit 4 selects slave. */
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_DRV_HEAD,
                  0xE0 | (slave ? 0x10 : 0x00) |
                  ((uint8_t)(lba >> 24) & 0x0F));
    ata_delay400();

    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT, 1);
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_LBA_LO,  (uint8_t)(lba));
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_LBA_HI,  (uint8_t)(lba >> 16));
}

void ata_init(void) {
    /* Soft-reset: assert SRST bit then clear it */
    port_byte_out(ATA_PRIMARY_CTRL, 0x04);
    port_byte_out(ATA_PRIMARY_CTRL, 0x00);
    ata_delay400();

    if (ata_wait_bsy() != ATA_OK) {
        klog("ATA", "reset timeout");
        return;
    }

    /* Check if a drive is present: non-0xFF status means something responded */
    uint8_t status = port_byte_in(ATA_PRIMARY_BASE + ATA_REG_STATUS);
    if (status == 0xFF) {
        klog("ATA", "no drive");
        return;
    }

    /* Wait for DRDY before declaring the drive ready */
    int i;
    for (i = 0; i < 100000; i++) {
        status = port_byte_in(ATA_PRIMARY_BASE + ATA_REG_STATUS);
        if (status & ATA_SR_DRDY) {
            klog("ATA", "drive ready");
            return;
        }
    }
    klog("ATA", "drive not ready (DRDY timeout)");
}

static int ata_read_sector_drive(uint32_t lba, uint8_t *buf, uint8_t slave) {
    if (ata_wait_bsy() != ATA_OK) return ATA_TIMEOUT;
    ata_setup_lba(lba, slave);
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);
    ata_delay400();

    int ret = ata_wait_drq();
    if (ret != ATA_OK) return ret;

    ata_insw(ATA_PRIMARY_BASE + ATA_REG_DATA, (uint16_t *)buf, 256);
    return ATA_OK;
}

static int ata_write_sector_drive(uint32_t lba, const uint8_t *buf, uint8_t slave) {
    if (ata_wait_bsy() != ATA_OK) return ATA_TIMEOUT;
    ata_setup_lba(lba, slave);
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);
    ata_delay400();

    int ret = ata_wait_drq();
    if (ret != ATA_OK) return ret;

    ata_outsw(ATA_PRIMARY_BASE + ATA_REG_DATA, (const uint16_t *)buf, 256);

    /* Flush write cache */
    port_byte_out(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_FLUSH_CACHE);
    if (ata_wait_bsy() != ATA_OK) return ATA_TIMEOUT;
    return ATA_OK;
}

int ata_read_sector(uint32_t lba, uint8_t *buf) {
    return ata_read_sector_drive(lba, buf, 0);
}

int ata_write_sector(uint32_t lba, const uint8_t *buf) {
    return ata_write_sector_drive(lba, buf, 0);
}

static int ata_read_sector_slave(uint32_t lba, uint8_t *buf) {
    return ata_read_sector_drive(lba, buf, 1);
}

static int ata_write_sector_slave(uint32_t lba, const uint8_t *buf) {
    return ata_write_sector_drive(lba, buf, 1);
}

static const blkdev_ops_t ata_master_ops = {
    .read_sector  = ata_read_sector,
    .write_sector = ata_write_sector,
};

static const blkdev_ops_t ata_slave_ops = {
    .read_sector  = ata_read_sector_slave,
    .write_sector = ata_write_sector_slave,
};

void ata_register(void) {
    blkdev_register("hd0", &ata_master_ops);
    blkdev_register("hd1", &ata_slave_ops);
}
