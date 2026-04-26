/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_blkdev.c - block-device registry tests.
 */

#include "ktest.h"
#include "blkdev.h"
#include "kstring.h"

static int null_read(uint32_t lba, uint8_t *buf)
{
	(void)lba;
	if (buf)
		k_memset(buf, 0, BLKDEV_SECTOR_SIZE);
	return 0;
}

static int null_write(uint32_t lba, const uint8_t *buf)
{
	(void)lba;
	(void)buf;
	return 0;
}

static const blkdev_ops_t null_ops = {
    .read_sector = null_read,
    .write_sector = null_write,
};

static uint32_t last_read_lba;
static uint32_t last_write_lba;
static uint8_t fake_disk[16][BLKDEV_SECTOR_SIZE];

static int fake_read(uint32_t lba, uint8_t *buf)
{
	last_read_lba = lba;
	if (!buf || lba >= 16)
		return -1;
	k_memcpy(buf, fake_disk[lba], BLKDEV_SECTOR_SIZE);
	return 0;
}

static int fake_write(uint32_t lba, const uint8_t *buf)
{
	last_write_lba = lba;
	if (!buf || lba >= 16)
		return -1;
	k_memcpy(fake_disk[lba], buf, BLKDEV_SECTOR_SIZE);
	return 0;
}

static const blkdev_ops_t fake_ops = {
    .read_sector = fake_read,
    .write_sector = fake_write,
};

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void write_mbr_entry(
    uint8_t *mbr, uint32_t slot, uint8_t type, uint32_t start, uint32_t sectors)
{
	uint8_t *ent = mbr + 446u + slot * 16u;
	ent[4] = type;
	put_le32(ent + 8, start);
	put_le32(ent + 12, sectors);
}

static void test_blkdev_enumerates_disks(ktest_case_t *tc)
{
	blkdev_info_t info;

	blkdev_reset();
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 100u, &null_ops), 0u);
	KTEST_ASSERT_EQ(
	    tc,
	    (uint32_t)blkdev_register_disk("sdb", 8u, 16u, 200u, &null_ops),
	    0u);

	KTEST_EXPECT_EQ(tc, blkdev_count(), 2u);
	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(0u, &info), 0u);
	KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "sda") == 0);
	KTEST_EXPECT_EQ(tc, info.kind, BLKDEV_KIND_DISK);
	KTEST_EXPECT_EQ(tc, info.major, 8u);
	KTEST_EXPECT_EQ(tc, info.minor, 0u);
	KTEST_EXPECT_EQ(tc, info.sectors, 100u);

	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(1u, &info), 0u);
	KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "sdb") == 0);
	KTEST_EXPECT_EQ(tc, info.kind, BLKDEV_KIND_DISK);
	KTEST_EXPECT_EQ(tc, info.major, 8u);
	KTEST_EXPECT_EQ(tc, info.minor, 16u);
	KTEST_EXPECT_EQ(tc, info.sectors, 200u);
}

static void test_blkdev_enforces_unique_disk_name_and_count(ktest_case_t *tc)
{
	blkdev_reset();
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 100u, &null_ops), 0u);
	KTEST_EXPECT_EQ(tc, blkdev_count(), 1u);
	KTEST_EXPECT_EQ(
	    tc,
	    (uint32_t)blkdev_register_disk("sda", 8u, 1u, 100u, &null_ops),
	    0xFFFFFFFFu);
	KTEST_EXPECT_EQ(tc, blkdev_count(), 1u);
}

static void test_blkdev_rejects_bad_disk_name(ktest_case_t *tc)
{
	blkdev_reset();
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)blkdev_register_disk("", 8u, 0u, 100u, &null_ops),
	                0xFFFFFFFFu);
	KTEST_EXPECT_EQ(
	    tc,
	    blkdev_register_disk((const char *)0, 8u, 0u, 100u, &null_ops),
	    0xFFFFFFFFu);
	KTEST_EXPECT_EQ(
	    tc,
	    (uint32_t)blkdev_register_disk("123456789012", 8u, 0u, 100u, &null_ops),
	    0xFFFFFFFFu);
	KTEST_EXPECT_EQ(tc, blkdev_count(), 0u);
}

static void test_blkdev_lookup_by_name(ktest_case_t *tc)
{
	blkdev_reset();
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 100u, &null_ops), 0u);

	KTEST_EXPECT_EQ(tc, blkdev_find_index("sda"), 0u);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)(uintptr_t)blkdev_get("sda"),
	                (uint32_t)(uintptr_t)&null_ops);
}

static void
test_blkdev_register_and_partition_stub_inheritance(ktest_case_t *tc)
{
	blkdev_info_t info;

	blkdev_reset();
	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_register("legacy", &null_ops), 0u);
	KTEST_EXPECT_EQ(tc, blkdev_find_index("legacy"), 0u);
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_part("legacy1", 0u, 1u, 4u, 8u), 0u);
	KTEST_EXPECT_EQ(tc, blkdev_count(), 2u);

	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(1u, &info), 0u);
	KTEST_EXPECT_EQ(tc, info.kind, BLKDEV_KIND_PART);
	KTEST_EXPECT_EQ(tc, info.parent_index, 0u);
	KTEST_EXPECT_EQ(tc, info.start_sector, 4u);
	KTEST_EXPECT_EQ(tc, info.sectors, 8u);
	KTEST_EXPECT_EQ(tc, info.partition_number, 1u);
	KTEST_EXPECT_TRUE(tc, blkdev_get("legacy1") != &null_ops);
}

static void test_blkdev_partition_translates_lba(ktest_case_t *tc)
{
	uint8_t buf[BLKDEV_SECTOR_SIZE];
	uint8_t write_buf[BLKDEV_SECTOR_SIZE];
	int disk;

	blkdev_reset();
	k_memset(fake_disk, 0, sizeof(fake_disk));
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 16u, &fake_ops), 0u);
	disk = blkdev_find_index("sda");
	KTEST_ASSERT_TRUE(tc, disk >= 0);
	KTEST_ASSERT_EQ(
	    tc,
	    (uint32_t)blkdev_register_part("sda1", (uint32_t)disk, 1u, 4u, 8u),
	    0u);

	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_get("sda1")->read_sector(2u, buf), 0u);
	KTEST_EXPECT_EQ(tc, last_read_lba, 6u);
	KTEST_EXPECT_TRUE(tc, blkdev_get("sda1")->read_sector(8u, buf) < 0);

	for (uint32_t i = 0; i < BLKDEV_SECTOR_SIZE; i++)
		write_buf[i] = (uint8_t)(i & 0xFFu);
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_get("sda1")->write_sector(3u, write_buf), 0u);
	KTEST_EXPECT_EQ(tc, last_write_lba, 7u);
	KTEST_EXPECT_EQ(
	    tc,
	    (uint32_t)k_memcmp(fake_disk[7], write_buf, BLKDEV_SECTOR_SIZE),
	    0u);
	KTEST_EXPECT_TRUE(tc, blkdev_get("sda1")->write_sector(8u, write_buf) < 0);
}

static void test_blkdev_scan_mbr_registers_primary_partition(ktest_case_t *tc)
{
	blkdev_info_t info;
	int disk;

	blkdev_reset();
	k_memset(fake_disk, 0, sizeof(fake_disk));
	fake_disk[0][510] = 0x55;
	fake_disk[0][511] = 0xAA;
	write_mbr_entry(fake_disk[0], 0u, 0x83u, 1u, 7u);
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 16u, &fake_ops), 0u);
	disk = blkdev_find_index("sda");
	KTEST_ASSERT_TRUE(tc, disk >= 0);

	KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_scan_mbr((uint32_t)disk), 1u);
	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(1u, &info), 0u);
	KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "sda1") == 0);
	KTEST_EXPECT_EQ(tc, info.kind, BLKDEV_KIND_PART);
	KTEST_EXPECT_EQ(tc, info.start_sector, 1u);
	KTEST_EXPECT_EQ(tc, info.sectors, 7u);
}

static void test_blkdev_scan_mbr_rejects_bad_signature(ktest_case_t *tc)
{
	int disk;

	blkdev_reset();
	k_memset(fake_disk, 0, sizeof(fake_disk));
	fake_disk[0][510] = 0x00;
	fake_disk[0][511] = 0xAA;
	write_mbr_entry(fake_disk[0], 0u, 0x83u, 1u, 7u);
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 16u, &fake_ops), 0u);
	disk = blkdev_find_index("sda");
	KTEST_ASSERT_TRUE(tc, disk >= 0);

	KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_scan_mbr((uint32_t)disk), 0u);
	KTEST_EXPECT_EQ(tc, blkdev_count(), 1u);
	KTEST_EXPECT_EQ(tc, blkdev_find_index("sda1"), -1);
}

static void test_blkdev_scan_mbr_skips_empty_slots_and_registers_valid_entries(
    ktest_case_t *tc)
{
	blkdev_info_t info;
	int disk;

	blkdev_reset();
	k_memset(fake_disk, 0, sizeof(fake_disk));
	fake_disk[0][510] = 0x55;
	fake_disk[0][511] = 0xAA;
	write_mbr_entry(fake_disk[0], 1u, 0x83u, 1u, 7u);
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 16u, &fake_ops), 0u);
	disk = blkdev_find_index("sda");
	KTEST_ASSERT_TRUE(tc, disk >= 0);

	KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_scan_mbr((uint32_t)disk), 1u);
	KTEST_EXPECT_EQ(tc, blkdev_count(), 2u);
	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(1u, &info), 0u);
	KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "sda2") == 0);
	KTEST_EXPECT_EQ(tc, info.kind, BLKDEV_KIND_PART);
	KTEST_EXPECT_EQ(tc, info.start_sector, 1u);
	KTEST_EXPECT_EQ(tc, info.sectors, 7u);
}

static void test_blkdev_scan_mbr_skips_out_of_range_entries(ktest_case_t *tc)
{
	blkdev_info_t info;
	int disk;

	blkdev_reset();
	k_memset(fake_disk, 0, sizeof(fake_disk));
	fake_disk[0][510] = 0x55;
	fake_disk[0][511] = 0xAA;
	write_mbr_entry(fake_disk[0], 0u, 0x83u, 16u, 1u);
	write_mbr_entry(fake_disk[0], 1u, 0x83u, 10u, 8u);
	write_mbr_entry(fake_disk[0], 2u, 0x83u, 1u, 7u);
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 16u, &fake_ops), 0u);
	disk = blkdev_find_index("sda");
	KTEST_ASSERT_TRUE(tc, disk >= 0);

	KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_scan_mbr((uint32_t)disk), 1u);
	KTEST_EXPECT_EQ(tc, blkdev_count(), 2u);
	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(1u, &info), 0u);
	KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "sda3") == 0);
	KTEST_EXPECT_EQ(tc, info.start_sector, 1u);
	KTEST_EXPECT_EQ(tc, info.sectors, 7u);
}

static void
test_blkdev_scan_mbr_skips_extended_partition_types(ktest_case_t *tc)
{
	blkdev_info_t info;
	int disk;

	blkdev_reset();
	k_memset(fake_disk, 0, sizeof(fake_disk));
	fake_disk[0][510] = 0x55;
	fake_disk[0][511] = 0xAA;
	write_mbr_entry(fake_disk[0], 0u, 0x05u, 1u, 7u);
	write_mbr_entry(fake_disk[0], 1u, 0x0Fu, 1u, 7u);
	write_mbr_entry(fake_disk[0], 2u, 0x83u, 2u, 6u);
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 16u, &fake_ops), 0u);
	disk = blkdev_find_index("sda");
	KTEST_ASSERT_TRUE(tc, disk >= 0);

	KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_scan_mbr((uint32_t)disk), 1u);
	KTEST_EXPECT_EQ(tc, blkdev_count(), 2u);
	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(1u, &info), 0u);
	KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "sda3") == 0);
	KTEST_EXPECT_EQ(tc, info.start_sector, 2u);
	KTEST_EXPECT_EQ(tc, info.sectors, 6u);
}

static void
test_blkdev_scan_mbr_skips_truncated_partition_names(ktest_case_t *tc)
{
	blkdev_info_t info;
	int disk;

	blkdev_reset();
	k_memset(fake_disk, 0, sizeof(fake_disk));
	fake_disk[0][510] = 0x55;
	fake_disk[0][511] = 0xAA;
	write_mbr_entry(fake_disk[0], 0u, 0x83u, 1u, 7u);
	KTEST_ASSERT_EQ(
	    tc,
	    (uint32_t)blkdev_register_disk("abcdefghijk", 8u, 0u, 16u, &fake_ops),
	    0u);
	disk = blkdev_find_index("abcdefghijk");
	KTEST_ASSERT_TRUE(tc, disk >= 0);

	KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_scan_mbr((uint32_t)disk), 0u);
	KTEST_EXPECT_EQ(tc, blkdev_count(), 1u);
	KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(0u, &info), 0u);
	KTEST_EXPECT_TRUE(tc, k_strcmp(info.name, "abcdefghijk") == 0);
	KTEST_EXPECT_EQ(tc, blkdev_find_index("abcdefghijk1"), -1);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_blkdev_enumerates_disks),
    KTEST_CASE(test_blkdev_enforces_unique_disk_name_and_count),
    KTEST_CASE(test_blkdev_rejects_bad_disk_name),
    KTEST_CASE(test_blkdev_lookup_by_name),
    KTEST_CASE(test_blkdev_register_and_partition_stub_inheritance),
    KTEST_CASE(test_blkdev_partition_translates_lba),
    KTEST_CASE(test_blkdev_scan_mbr_registers_primary_partition),
    KTEST_CASE(test_blkdev_scan_mbr_rejects_bad_signature),
    KTEST_CASE(
        test_blkdev_scan_mbr_skips_empty_slots_and_registers_valid_entries),
    KTEST_CASE(test_blkdev_scan_mbr_skips_out_of_range_entries),
    KTEST_CASE(test_blkdev_scan_mbr_skips_extended_partition_types),
    KTEST_CASE(test_blkdev_scan_mbr_skips_truncated_partition_names),
};

static ktest_suite_t suite = KTEST_SUITE("blkdev", cases);

ktest_suite_t *ktest_suite_blkdev(void)
{
	return &suite;
}
