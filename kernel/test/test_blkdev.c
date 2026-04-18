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

static void test_blkdev_enumerates_disks(ktest_case_t *tc)
{
    blkdev_info_t info;

    blkdev_reset();
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 100u, &null_ops), 0u);
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_register_disk("sdb", 8u, 16u, 200u, &null_ops), 0u);

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
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 100u, &null_ops), 0u);
    KTEST_EXPECT_EQ(tc, blkdev_count(), 1u);
    KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_register_disk("sda", 8u, 1u, 100u, &null_ops), 0xFFFFFFFFu);
    KTEST_EXPECT_EQ(tc, blkdev_count(), 1u);
}

static void test_blkdev_rejects_bad_disk_name(ktest_case_t *tc)
{
    blkdev_reset();
    KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_register_disk("", 8u, 0u, 100u, &null_ops), 0xFFFFFFFFu);
    KTEST_EXPECT_EQ(tc, blkdev_register_disk((const char *)0, 8u, 0u, 100u, &null_ops), 0xFFFFFFFFu);
    KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_register_disk("123456789012", 8u, 0u, 100u, &null_ops), 0xFFFFFFFFu);
    KTEST_EXPECT_EQ(tc, blkdev_count(), 0u);
}

static void test_blkdev_lookup_by_name(ktest_case_t *tc)
{
    blkdev_reset();
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_register_disk("sda", 8u, 0u, 100u, &null_ops), 0u);

    KTEST_EXPECT_EQ(tc, blkdev_find_index("sda"), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_get("sda"), (uint32_t)&null_ops);
}

static void test_blkdev_register_and_partition_stub_inheritance(ktest_case_t *tc)
{
    blkdev_info_t info;

    blkdev_reset();
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_register("legacy", &null_ops), 0u);
    KTEST_EXPECT_EQ(tc, blkdev_find_index("legacy"), 0u);
    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_register_part("legacy1", 0u, 1u, 4u, 8u), 0u);
    KTEST_EXPECT_EQ(tc, blkdev_count(), 2u);

    KTEST_ASSERT_EQ(tc, (uint32_t)blkdev_info_at(1u, &info), 0u);
    KTEST_EXPECT_EQ(tc, info.kind, BLKDEV_KIND_PART);
    KTEST_EXPECT_EQ(tc, info.parent_index, 0u);
    KTEST_EXPECT_EQ(tc, info.start_sector, 4u);
    KTEST_EXPECT_EQ(tc, info.sectors, 8u);
    KTEST_EXPECT_EQ(tc, info.partition_number, 1u);
    KTEST_EXPECT_EQ(tc, (uint32_t)blkdev_get("legacy1"), (uint32_t)&null_ops);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_blkdev_enumerates_disks),
    KTEST_CASE(test_blkdev_enforces_unique_disk_name_and_count),
    KTEST_CASE(test_blkdev_rejects_bad_disk_name),
    KTEST_CASE(test_blkdev_lookup_by_name),
    KTEST_CASE(test_blkdev_register_and_partition_stub_inheritance),
};

static ktest_suite_t suite = KTEST_SUITE("blkdev", cases);

ktest_suite_t *ktest_suite_blkdev(void) { return &suite; }
