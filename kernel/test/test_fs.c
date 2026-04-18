/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_fs.c — in-kernel DUFS filesystem unit tests.
 */

#include "ktest.h"
#include "fs.h"
#include "kstring.h"

/*
 * DUFS v2 filesystem tests.
 *
 * ATA and the "sdb1" blkdev are initialized before ktest_run_all() runs, but
 * dufs_register() and vfs_mount() have not yet been called.  Each test calls
 * fs_init() on its own to load the superblock and bitmaps directly, then
 * exercises the public fs_* API.  Every test that creates a file unlinks it
 * before returning so the filesystem is clean when vfs_mount() runs later.
 *
 * Interrupts are not yet enabled at this point (interrupts_enable()/sti come
 * after ktest_run_all()), but ATA PIO polling does not require them.
 */

#define TEST_FILE    "_ktest_"
#define TEST_DIR     "_ktdir_"
#define TEST_SUBFILE "_ktdir_/_ktsub_"

#ifndef DRUNIX_ROOT_FS
#define DRUNIX_ROOT_FS "ext3"
#endif

static const char *fs_test_device(void)
{
    return (k_strcmp(DRUNIX_ROOT_FS, "dufs") == 0) ? "sda1" : "sdb1";
}

static int fs_test_init(void)
{
    if (dufs_use_device(fs_test_device()) != 0)
        return -1;
    return fs_init();
}

static int mem_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* ── Test cases ─────────────────────────────────────────────────────────── */

static void test_fs_use_device_sda1_and_sdb1(ktest_case_t *tc)
{
    KTEST_EXPECT_EQ(tc, (uint32_t)dufs_use_device("sda1"), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)dufs_use_device("sdb1"), 0u);
}

static void test_fs_init_ok(ktest_case_t *tc)
{
    int rc = fs_test_init();
    KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);
}

static void test_fs_create_and_unlink(ktest_case_t *tc)
{
    KTEST_ASSERT_EQ(tc, (uint32_t)fs_test_init(), 0u);

    int ino = fs_create(TEST_FILE);
    KTEST_EXPECT_TRUE(tc, ino > 0);

    /* Always unlink so the filesystem stays clean for subsequent tests. */
    int rc = fs_unlink(TEST_FILE);
    KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);

    /* After unlink the file must not be findable. */
    uint32_t found_ino, found_size;
    rc = fs_open(TEST_FILE, &found_ino, &found_size);
    KTEST_EXPECT_TRUE(tc, rc < 0);
}

static void test_fs_write_and_readback(ktest_case_t *tc)
{
    static const uint8_t data[] = {
        'k','t','e','s','t','-','w','r','i','t','e','-','o','k'
    };
    const uint32_t dlen = (uint32_t)sizeof(data);

    KTEST_ASSERT_EQ(tc, (uint32_t)fs_test_init(), 0u);

    int ino = fs_create(TEST_FILE);
    KTEST_ASSERT_TRUE(tc, ino > 0);

    int written = fs_write((uint32_t)ino, 0, data, dlen);
    KTEST_EXPECT_EQ(tc, (uint32_t)written, dlen);

    /* Read back via fs_read and verify content. */
    uint8_t buf[64];
    int n = fs_read((uint32_t)ino, 0, buf, dlen);
    KTEST_EXPECT_EQ(tc, (uint32_t)n, dlen);
    KTEST_EXPECT_TRUE(tc, mem_eq(buf, data, dlen));

    fs_unlink(TEST_FILE);
}

static void test_fs_open_after_write(ktest_case_t *tc)
{
    static const uint8_t data[] = {'h','e','l','l','o'};
    const uint32_t dlen = (uint32_t)sizeof(data);

    KTEST_ASSERT_EQ(tc, (uint32_t)fs_test_init(), 0u);

    int ino = fs_create(TEST_FILE);
    KTEST_ASSERT_TRUE(tc, ino > 0);

    fs_write((uint32_t)ino, 0, data, dlen);

    /* fs_open must return the same inode and the correct size. */
    uint32_t found_ino, found_size;
    int rc = fs_open(TEST_FILE, &found_ino, &found_size);
    KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);
    KTEST_EXPECT_EQ(tc, found_size, dlen);
    KTEST_EXPECT_EQ(tc, found_ino, (uint32_t)ino);

    fs_unlink(TEST_FILE);
}

static void test_fs_truncate_shrink_then_extend_zero_fills_gap(ktest_case_t *tc)
{
    static const uint8_t data[] = {'a','b','c','d','e','f'};
    static const uint8_t want[] = {'a','b',0,0,0,0};
    uint8_t buf[sizeof(data)];
    int ino;
    int n;

    KTEST_ASSERT_EQ(tc, (uint32_t)fs_test_init(), 0u);

    ino = fs_create(TEST_FILE);
    KTEST_ASSERT_TRUE(tc, ino > 0);
    KTEST_ASSERT_EQ(tc,
                    (uint32_t)fs_write((uint32_t)ino, 0, data,
                                       (uint32_t)sizeof(data)),
                    (uint32_t)sizeof(data));

    KTEST_ASSERT_EQ(tc, (uint32_t)fs_truncate((uint32_t)ino, 2u), 0u);
    KTEST_ASSERT_EQ(tc,
                    (uint32_t)fs_truncate((uint32_t)ino,
                                          (uint32_t)sizeof(data)),
                    0u);

    n = fs_read((uint32_t)ino, 0, buf, (uint32_t)sizeof(buf));
    KTEST_EXPECT_EQ(tc, (uint32_t)n, (uint32_t)sizeof(buf));
    KTEST_EXPECT_TRUE(tc, mem_eq(buf, want, (uint32_t)sizeof(want)));

    fs_unlink(TEST_FILE);
}

static void test_fs_unlink_removes_file(ktest_case_t *tc)
{
    KTEST_ASSERT_EQ(tc, (uint32_t)fs_test_init(), 0u);

    int ino = fs_create(TEST_FILE);
    KTEST_ASSERT_TRUE(tc, ino > 0);

    int rc = fs_unlink(TEST_FILE);
    KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);

    uint32_t found_ino, found_size;
    rc = fs_open(TEST_FILE, &found_ino, &found_size);
    KTEST_EXPECT_TRUE(tc, rc < 0);
}

/* ── Directory test cases ───────────────────────────────────────────────── */

static void test_fs_mkdir_creates_entry(ktest_case_t *tc)
{
    KTEST_ASSERT_EQ(tc, (uint32_t)fs_test_init(), 0u);

    int rc = fs_mkdir(TEST_DIR);
    KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);

    /* Directory must appear in root listing with trailing '/'. */
    char buf[512];
    int n = fs_list(0, buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);

    /* Search for "TEST_DIR/" in the listing. */
    int found = 0;
    int i = 0;
    while (i < n) {
        char *entry = buf + i;
        const char *want = TEST_DIR "/";
        int j = 0;
        while (want[j] && entry[j] && want[j] == entry[j]) j++;
        if (want[j] == '\0' && entry[j] == '\0') { found = 1; break; }
        while (i < n && buf[i] != '\0') i++;
        i++;
    }
    KTEST_EXPECT_TRUE(tc, found);

    /* Clean up: remove the directory and reload from disk. */
    fs_rmdir(TEST_DIR);
    fs_test_init();
}

static void test_fs_create_in_subdir(ktest_case_t *tc)
{
    KTEST_ASSERT_EQ(tc, (uint32_t)fs_test_init(), 0u);

    /* Create the directory first. */
    int rc = fs_mkdir(TEST_DIR);
    KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);

    /* Create a file inside it. */
    int ino = fs_create(TEST_SUBFILE);
    KTEST_EXPECT_TRUE(tc, ino > 0);

    /* The file should be findable by path. */
    uint32_t found_ino, found_size;
    rc = fs_open(TEST_SUBFILE, &found_ino, &found_size);
    KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);
    KTEST_EXPECT_EQ(tc, found_ino, (uint32_t)ino);

    /* It must NOT appear in root listing (it's in a subdirectory). */
    char buf[512];
    int n = fs_list(0, buf, sizeof(buf));
    int i = 0, found_in_root = 0;
    while (i < n) {
        char *entry = buf + i;
        const char *want = "_ktsub_";
        int j = 0;
        while (want[j] && entry[j] && want[j] == entry[j]) j++;
        if (want[j] == '\0' && entry[j] == '\0') { found_in_root = 1; break; }
        while (i < n && buf[i] != '\0') i++;
        i++;
    }
    KTEST_EXPECT_TRUE(tc, !found_in_root);

    /* It must appear in the subdir listing. */
    n = fs_list(TEST_DIR, buf, sizeof(buf));
    i = 0;
    int found_in_dir = 0;
    while (i < n) {
        char *entry = buf + i;
        const char *want = "_ktsub_";
        int j = 0;
        while (want[j] && entry[j] && want[j] == entry[j]) j++;
        if (want[j] == '\0' && entry[j] == '\0') { found_in_dir = 1; break; }
        while (i < n && buf[i] != '\0') i++;
        i++;
    }
    KTEST_EXPECT_TRUE(tc, found_in_dir);

    /* Clean up. */
    fs_unlink(TEST_SUBFILE);
    fs_rmdir(TEST_DIR);
    fs_test_init();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

static ktest_case_t cases[] = {
    KTEST_CASE(test_fs_use_device_sda1_and_sdb1),
    KTEST_CASE(test_fs_init_ok),
    KTEST_CASE(test_fs_create_and_unlink),
    KTEST_CASE(test_fs_write_and_readback),
    KTEST_CASE(test_fs_open_after_write),
    KTEST_CASE(test_fs_truncate_shrink_then_extend_zero_fills_gap),
    KTEST_CASE(test_fs_unlink_removes_file),
    KTEST_CASE(test_fs_mkdir_creates_entry),
    KTEST_CASE(test_fs_create_in_subdir),
};

static ktest_suite_t suite = KTEST_SUITE("fs", cases);

ktest_suite_t *ktest_suite_fs(void) { return &suite; }
