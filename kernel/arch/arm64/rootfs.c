/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "rootfs.h"

#include "blkdev.h"
#include "kstring.h"
#include <stdint.h>

extern const uint8_t arm64_rootfs_start[];
extern const uint8_t arm64_rootfs_end[];

#ifndef DRUNIX_DISK_SECTORS
#define DRUNIX_DISK_SECTORS 262144u
#endif

#define ARM64_ROOTFS_PARTITION_START 2048u

static int arm64_rootfs_read_sector(uint32_t lba, uint8_t *buf)
{
	uint32_t size =
	    (uint32_t)((uintptr_t)arm64_rootfs_end - (uintptr_t)arm64_rootfs_start);
	uint32_t sectors = size / BLKDEV_SECTOR_SIZE;
	uint32_t off;

	if (!buf || lba < ARM64_ROOTFS_PARTITION_START)
		return -1;
	lba -= ARM64_ROOTFS_PARTITION_START;
	if (lba >= sectors)
		return -1;
	off = lba * BLKDEV_SECTOR_SIZE;
	k_memcpy(buf, arm64_rootfs_start + off, BLKDEV_SECTOR_SIZE);
	return 0;
}

static int arm64_rootfs_write_sector(uint32_t lba, const uint8_t *buf)
{
	uint32_t size =
	    (uint32_t)((uintptr_t)arm64_rootfs_end - (uintptr_t)arm64_rootfs_start);
	uint32_t sectors = size / BLKDEV_SECTOR_SIZE;
	uint32_t off;

	if (!buf || lba < ARM64_ROOTFS_PARTITION_START)
		return -1;
	lba -= ARM64_ROOTFS_PARTITION_START;
	if (lba >= sectors)
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
	int sda;
	int sdb;

	if (blkdev_register_disk("sda", 8, 0, DRUNIX_DISK_SECTORS, &ops) != 0)
		return -1;
	sda = blkdev_find_index("sda");
	if (sda < 0 || blkdev_register_part("sda1",
	                                    (uint32_t)sda,
	                                    1,
	                                    ARM64_ROOTFS_PARTITION_START,
	                                    DRUNIX_DISK_SECTORS -
	                                        ARM64_ROOTFS_PARTITION_START) != 0)
		return -1;

	if (blkdev_register_disk("sdb", 8, 16, DRUNIX_DISK_SECTORS, &ops) != 0)
		return -1;
	sdb = blkdev_find_index("sdb");
	if (sdb < 0 || blkdev_register_part("sdb1",
	                                    (uint32_t)sdb,
	                                    1,
	                                    ARM64_ROOTFS_PARTITION_START,
	                                    DRUNIX_DISK_SECTORS -
	                                        ARM64_ROOTFS_PARTITION_START) != 0)
		return -1;

	return 0;
}
