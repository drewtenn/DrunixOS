/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "rootfs.h"

#include "blkdev.h"
#include "kstring.h"
#include <stdint.h>

extern const uint8_t arm64_rootfs_start[];
extern const uint8_t arm64_rootfs_end[];

static int arm64_rootfs_read_sector(uint32_t lba, uint8_t *buf)
{
	uint32_t size = (uint32_t)(arm64_rootfs_end - arm64_rootfs_start);
	uint32_t sectors = size / BLKDEV_SECTOR_SIZE;
	uint32_t off;

	if (!buf || lba >= sectors)
		return -1;
	off = lba * BLKDEV_SECTOR_SIZE;
	k_memcpy(buf, arm64_rootfs_start + off, BLKDEV_SECTOR_SIZE);
	return 0;
}

static int arm64_rootfs_write_sector(uint32_t lba, const uint8_t *buf)
{
	uint32_t size = (uint32_t)(arm64_rootfs_end - arm64_rootfs_start);
	uint32_t sectors = size / BLKDEV_SECTOR_SIZE;
	uint32_t off;

	if (!buf || lba >= sectors)
		return -1;
	off = lba * BLKDEV_SECTOR_SIZE;
	k_memcpy((uint8_t *)arm64_rootfs_start + off, buf, BLKDEV_SECTOR_SIZE);
	return 0;
}

int arm64_rootfs_register(void)
{
	static const blkdev_ops_t ops = {
	    .read_sector = arm64_rootfs_read_sector,
	    .write_sector = arm64_rootfs_write_sector,
	};
	uint32_t size = (uint32_t)(arm64_rootfs_end - arm64_rootfs_start);
	uint32_t sectors = size / BLKDEV_SECTOR_SIZE;

	return blkdev_register_disk("sda1", 0, 1, sectors, &ops);
}
