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

static ktest_case_t cases[] = {
    KTEST_CASE(test_blkdev_enumerates_disks),
};

static ktest_suite_t suite = KTEST_SUITE("blkdev", cases);

ktest_suite_t *ktest_suite_blkdev(void) { return &suite; }
