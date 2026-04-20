/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * blkdev_part.c - MBR partition devices over parent block devices.
 */

#include "blkdev.h"
#include "kprintf.h"
#include "kstring.h"

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int part_translate(uint32_t part_index,
                          uint32_t lba,
                          blkdev_info_t *info_out,
                          const blkdev_ops_t **parent_ops_out,
                          uint32_t *parent_lba_out)
{
	blkdev_info_t info;

	if (blkdev_info_for_index(part_index, &info) != 0 ||
	    info.kind != BLKDEV_KIND_PART || lba >= info.sectors)
		return -1;
	*parent_ops_out = blkdev_ops_at(info.parent_index);
	if (!*parent_ops_out)
		return -1;
	*parent_lba_out = info.start_sector + lba;
	if (info_out)
		*info_out = info;
	return 0;
}

static int part_read_sector_for(uint32_t part_index, uint32_t lba, uint8_t *buf)
{
	const blkdev_ops_t *parent_ops;
	uint32_t parent_lba;

	if (part_translate(part_index, lba, 0, &parent_ops, &parent_lba) != 0)
		return -1;
	return parent_ops->read_sector(parent_lba, buf);
}

static int
part_write_sector_for(uint32_t part_index, uint32_t lba, const uint8_t *buf)
{
	const blkdev_ops_t *parent_ops;
	uint32_t parent_lba;

	if (part_translate(part_index, lba, 0, &parent_ops, &parent_lba) != 0)
		return -1;
	return parent_ops->write_sector(parent_lba, buf);
}

#define DEFINE_PART_OPS(n)                                                     \
	static int part##n##_read_sector(uint32_t lba, uint8_t *buf)               \
	{                                                                          \
		return part_read_sector_for((uint32_t)(n), lba, buf);                  \
	}                                                                          \
	static int part##n##_write_sector(uint32_t lba, const uint8_t *buf)        \
	{                                                                          \
		return part_write_sector_for((uint32_t)(n), lba, buf);                 \
	}                                                                          \
	static const blkdev_ops_t part##n##_ops = {                                \
	    .read_sector = part##n##_read_sector,                                  \
	    .write_sector = part##n##_write_sector,                                \
	};

DEFINE_PART_OPS(0)
DEFINE_PART_OPS(1)
DEFINE_PART_OPS(2)
DEFINE_PART_OPS(3)
DEFINE_PART_OPS(4)
DEFINE_PART_OPS(5)
DEFINE_PART_OPS(6)
DEFINE_PART_OPS(7)
DEFINE_PART_OPS(8)
DEFINE_PART_OPS(9)
DEFINE_PART_OPS(10)
DEFINE_PART_OPS(11)
DEFINE_PART_OPS(12)
DEFINE_PART_OPS(13)
DEFINE_PART_OPS(14)
DEFINE_PART_OPS(15)

const blkdev_ops_t *blkdev_part_ops_for_index(uint32_t index)
{
	static const blkdev_ops_t *ops_by_index[BLKDEV_MAX] = {
	    &part0_ops,
	    &part1_ops,
	    &part2_ops,
	    &part3_ops,
	    &part4_ops,
	    &part5_ops,
	    &part6_ops,
	    &part7_ops,
	    &part8_ops,
	    &part9_ops,
	    &part10_ops,
	    &part11_ops,
	    &part12_ops,
	    &part13_ops,
	    &part14_ops,
	    &part15_ops,
	};

	if (index >= BLKDEV_MAX)
		return 0;
	return ops_by_index[index];
}

int blkdev_scan_mbr(uint32_t disk_index)
{
	blkdev_info_t disk;
	const blkdev_ops_t *ops;
	uint8_t mbr[BLKDEV_SECTOR_SIZE];
	int registered = 0;

	if (blkdev_info_for_index(disk_index, &disk) != 0 ||
	    disk.kind != BLKDEV_KIND_DISK)
		return -1;
	ops = blkdev_ops_at(disk_index);
	if (!ops || ops->read_sector(0, mbr) != 0)
		return -1;
	if (mbr[510] != 0x55 || mbr[511] != 0xAA)
		return 0;

	for (uint32_t slot = 0; slot < 4; slot++) {
		uint8_t *ent = mbr + 446u + slot * 16u;
		uint8_t type = ent[4];
		uint32_t start = le32(ent + 8);
		uint32_t sectors = le32(ent + 12);
		char name[BLKDEV_NAME_MAX];
		int name_len;

		if (type == 0 || sectors == 0)
			continue;
		if (type == 0x05u || type == 0x0Fu)
			continue;
		if (start >= disk.sectors || sectors > disk.sectors - start)
			continue;
		name_len = k_snprintf(name, sizeof(name), "%s%u", disk.name, slot + 1u);
		if (name_len < 0 || (uint32_t)name_len >= sizeof(name))
			continue;
		if (blkdev_register_part(name, disk_index, slot + 1u, start, sectors) ==
		    0)
			registered++;
	}
	return registered;
}
