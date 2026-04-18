/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* Primary ATA controller I/O ports */
#define ATA_PRIMARY_BASE  0x1F0
#define ATA_PRIMARY_CTRL  0x3F6

/* Register offsets from base */
#define ATA_REG_DATA      0x00  /* 16-bit data port */
#define ATA_REG_ERROR     0x01
#define ATA_REG_FEATURES  0x01
#define ATA_REG_SECCOUNT  0x02
#define ATA_REG_LBA_LO    0x03
#define ATA_REG_LBA_MID   0x04
#define ATA_REG_LBA_HI    0x05
#define ATA_REG_DRV_HEAD  0x06
#define ATA_REG_STATUS    0x07
#define ATA_REG_COMMAND   0x07

/* Status register bits */
#define ATA_SR_BSY   0x80   /* controller busy */
#define ATA_SR_DRDY  0x40   /* drive ready */
#define ATA_SR_DRQ   0x08   /* data request — transfer ready */
#define ATA_SR_ERR   0x01   /* error */

/* ATA commands */
#define ATA_CMD_READ_SECTORS   0x20
#define ATA_CMD_WRITE_SECTORS  0x30
#define ATA_CMD_FLUSH_CACHE    0xE7
#define ATA_CMD_IDENTIFY       0xEC

/* Return codes */
#define ATA_OK       0
#define ATA_ERR     -1
#define ATA_TIMEOUT -2

void ata_init(void);
int  ata_read_sector(uint32_t lba, uint8_t *buf);
int  ata_write_sector(uint32_t lba, const uint8_t *buf);
void ata_register(void);   /* register "sda" and "sdb" with the blkdev registry */

#endif
