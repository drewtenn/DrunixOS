/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_vfs.c — in-kernel virtual filesystem layer unit tests.
 */

#include "ktest.h"
#include "vfs.h"
#include "blkdev.h"
#include "procfs.h"
#include "process.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "klog.h"
#include "kstring.h"
#include "mem_forensics.h"
#include "vma.h"

#define ROOT_FILE_INO   11u
#define ROOT_FILE_SIZE  5u
#define CHILD_FILE_INO  22u
#define CHILD_FILE_SIZE 7u

static int has_entry(const char *buf, int n, const char *want)
{
    int i = 0;

    while (i < n) {
        const char *entry = buf + i;
        if (k_strcmp(entry, want) == 0)
            return 1;
        i += (int)k_strlen(entry) + 1;
    }

    return 0;
}

static int count_entry(const char *buf, int n, const char *want)
{
    int i = 0;
    int found = 0;

    while (i < n) {
        const char *entry = buf + i;
        if (k_strcmp(entry, want) == 0)
            found++;
        i += (int)k_strlen(entry) + 1;
    }

    return found;
}

static int mock_root_open(void *ctx, const char *path,
                          uint32_t *inode_out, uint32_t *size_out)
{
    (void)ctx;
    if (k_strcmp(path, "hello") == 0) {
        *inode_out = ROOT_FILE_INO;
        *size_out = ROOT_FILE_SIZE;
        return 0;
    }
    if (k_strcmp(path, "mnt/childfile") == 0) {
        *inode_out = 99;
        *size_out = 99;
        return 0;
    }
    return -1;
}

static int mock_root_getdents(void *ctx, const char *path, char *buf,
                              uint32_t bufsz)
{
    uint32_t written = 0;

    (void)ctx;
    if (!buf)
        return -1;

    if (!path || path[0] == '\0') {
        static const char root_entries[] = "hello\0mnt/\0dev/\0";
        uint32_t n = (uint32_t)sizeof(root_entries);
        if (n > bufsz)
            n = bufsz;
        k_memcpy(buf, root_entries, n);
        return (int)n;
    }

    if (k_strcmp(path, "mnt") == 0) {
        static const char mnt_entries[] = "shadowed\0";
        written = (uint32_t)sizeof(mnt_entries);
        if (written > bufsz)
            written = bufsz;
        k_memcpy(buf, mnt_entries, written);
        return (int)written;
    }

    return -1;
}

static int mock_root_stat(void *ctx, const char *path, vfs_stat_t *st)
{
    (void)ctx;
    if (k_strcmp(path, "hello") == 0) {
        st->type = 1;
        st->size = ROOT_FILE_SIZE;
        st->link_count = 1;
        st->mtime = 0;
        return 0;
    }
    if (k_strcmp(path, "mnt") == 0 || k_strcmp(path, "dev") == 0) {
        st->type = 2;
        st->size = 0;
        st->link_count = 1;
        st->mtime = 0;
        return 0;
    }
    if (k_strcmp(path, "mnt/childfile") == 0) {
        st->type = 1;
        st->size = 99;
        st->link_count = 1;
        st->mtime = 0;
        return 0;
    }
    return -1;
}

static int mock_root_read(void *ctx, uint32_t inode_num, uint32_t offset,
                          uint8_t *buf, uint32_t count)
{
    static const char data[] = "root!";
    uint32_t size = (uint32_t)sizeof(data) - 1u;

    (void)ctx;
    if (inode_num != ROOT_FILE_INO || !buf)
        return -1;
    if (offset >= size)
        return 0;
    if (count > size - offset)
        count = size - offset;
    k_memcpy(buf, data + offset, count);
    return (int)count;
}

static int mock_sub_open(void *ctx, const char *path,
                         uint32_t *inode_out, uint32_t *size_out)
{
    (void)ctx;
    if (k_strcmp(path, "childfile") == 0) {
        *inode_out = CHILD_FILE_INO;
        *size_out = CHILD_FILE_SIZE;
        return 0;
    }
    return -1;
}

static int mock_sub_read(void *ctx, uint32_t inode_num, uint32_t offset,
                         uint8_t *buf, uint32_t count)
{
    static const char data[] = "child!!";
    uint32_t size = (uint32_t)sizeof(data) - 1u;

    (void)ctx;
    if (inode_num != CHILD_FILE_INO || !buf)
        return -1;
    if (offset >= size)
        return 0;
    if (count > size - offset)
        count = size - offset;
    k_memcpy(buf, data + offset, count);
    return (int)count;
}

static int mock_sub_getdents(void *ctx, const char *path, char *buf,
                             uint32_t bufsz)
{
    static const char entries[] = "childfile\0";
    uint32_t n = (uint32_t)sizeof(entries);

    (void)ctx;
    if (!buf)
        return -1;
    if (path && path[0] != '\0')
        return -1;

    if (n > bufsz)
        n = bufsz;
    k_memcpy(buf, entries, n);
    return (int)n;
}

static int mock_sub_stat(void *ctx, const char *path, vfs_stat_t *st)
{
    (void)ctx;
    if (k_strcmp(path, "childfile") != 0)
        return -1;
    st->type = 1;
    st->size = CHILD_FILE_SIZE;
    st->link_count = 1;
    st->mtime = 0;
    return 0;
}

static const fs_ops_t mock_root_ops = {
    .init     = 0,
    .open     = mock_root_open,
    .getdents = mock_root_getdents,
    .create   = 0,
    .unlink   = 0,
    .mkdir    = 0,
    .rmdir    = 0,
    .rename   = 0,
    .stat     = mock_root_stat,
    .read     = mock_root_read,
};

static const fs_ops_t mock_sub_ops = {
    .init     = 0,
    .open     = mock_sub_open,
    .getdents = mock_sub_getdents,
    .create   = 0,
    .unlink   = 0,
    .mkdir    = 0,
    .rmdir    = 0,
    .rename   = 0,
    .stat     = mock_sub_stat,
    .read     = mock_sub_read,
};

static int setup_mount_tree(void)
{
    vfs_reset();
    if (vfs_register("root", &mock_root_ops) != 0)
        return -1;
    if (vfs_register("sub", &mock_sub_ops) != 0)
        return -1;
    if (vfs_mount("/", "root") != 0)
        return -1;
    if (vfs_mount("/mnt", "sub") != 0)
        return -1;
    if (vfs_mount("/dev", "devfs") != 0)
        return -1;
    return 0;
}

static int setup_mount_tree_with_proc(void)
{
    if (setup_mount_tree() != 0)
        return -1;
    if (vfs_mount("/proc", "procfs") != 0)
        return -1;
    return 0;
}

static int setup_mount_tree_with_sys(void)
{
    if (setup_mount_tree() != 0)
        return -1;
    if (vfs_mount("/sys", "sysfs") != 0)
        return -1;
    return 0;
}

static int read_vfs_text_path(const char *path, char *buf, uint32_t cap)
{
    vfs_file_ref_t ref;
    uint32_t size = 0;
    int n;

    if (!buf || cap == 0)
        return -1;
    if (vfs_open_file(path, &ref, &size) != 0)
        return -1;
    n = vfs_read(ref, 0, (uint8_t *)buf, cap - 1u);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static int add_procfs_test_process(void)
{
    static process_t proc;

    sched_init();
    k_memset(&proc, 0, sizeof(proc));
    proc.saved_esp = 1; /* skip initial-frame synthesis in sched_add() */
    proc.state = PROC_UNUSED;
    proc.pid = 0;
    proc.parent_pid = 42;
    proc.pgid = 7;
    proc.sid = 7;
    proc.tty_id = 0;
    proc.heap_start = 0x00410000u;
    proc.brk = 0x00418000u;
    proc.image_start = 0x00400000u;
    proc.image_end = 0x00410000u;
    proc.stack_low_limit = USER_STACK_TOP - 0x4000u;
    k_strncpy(proc.name, "shell", sizeof(proc.name) - 1);
    k_strncpy(proc.psargs, "/bin/shell", sizeof(proc.psargs) - 1);
    proc.open_files[0].type = FD_TYPE_TTY;
    proc.open_files[0].u.tty.tty_idx = 0;
    proc.open_files[1].type = FD_TYPE_STDOUT;
    proc.open_files[1].writable = 1;
    return sched_add(&proc);
}

static void init_procfs_layout_process(process_t *proc, int include_image_vma)
{
    k_memset(proc, 0, sizeof(*proc));
    proc->saved_esp = 1; /* skip initial-frame synthesis in sched_add() */
    proc->state = PROC_UNUSED;
    proc->pid = 0;
    proc->parent_pid = 42;
    proc->pgid = 7;
    proc->sid = 7;
    proc->tty_id = 0;
    proc->image_start = 0x00400000u;
    proc->image_end = 0x00410000u;
    proc->heap_start = 0x00410000u;
    proc->brk = 0x00418000u;
    proc->stack_low_limit =
        USER_STACK_TOP - (uint32_t)USER_STACK_PAGES * 0x1000u;
    k_strncpy(proc->name, "shell", sizeof(proc->name) - 1);
    k_strncpy(proc->psargs, "/bin/shell", sizeof(proc->psargs) - 1);
    proc->open_files[0].type = FD_TYPE_TTY;
    proc->open_files[0].u.tty.tty_idx = 0;
    proc->open_files[1].type = FD_TYPE_STDOUT;
    proc->open_files[1].writable = 1;

    vma_init(proc);
    if (include_image_vma) {
        vma_add(proc, proc->image_start, proc->image_end,
                VMA_FLAG_READ | VMA_FLAG_EXEC | VMA_FLAG_PRIVATE,
                VMA_KIND_IMAGE);
    }
    vma_add(proc, proc->heap_start, proc->brk,
            VMA_FLAG_READ | VMA_FLAG_WRITE |
            VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
            VMA_KIND_HEAP);
    vma_add(proc,
            USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * 0x1000u,
            USER_STACK_TOP,
            VMA_FLAG_READ | VMA_FLAG_WRITE |
            VMA_FLAG_ANON | VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
            VMA_KIND_STACK);
}

static int map_procfs_test_page(process_t *proc, uint32_t virt, uint32_t flags)
{
    uint32_t phys;

    if (!proc || proc->pd_phys == 0)
        return -1;

    phys = pmm_alloc_page();
    if (phys == 0)
        return -1;

    if (paging_map_page(proc->pd_phys, virt, phys, flags) != 0) {
        pmm_free_page(phys);
        return -1;
    }

    return 0;
}

static int add_procfs_mapped_layout_process(int include_image_vma)
{
    static process_t proc;

    sched_init();
    init_procfs_layout_process(&proc, include_image_vma);
    proc.pd_phys = paging_create_user_space();
    if (proc.pd_phys == 0)
        return -1;
    if (map_procfs_test_page(&proc, proc.image_start, PG_PRESENT | PG_USER) != 0)
        return -1;
    if (map_procfs_test_page(&proc, proc.heap_start,
                             PG_PRESENT | PG_USER | PG_WRITABLE) != 0)
        return -1;
    if (map_procfs_test_page(&proc, USER_STACK_TOP - 0x1000u,
                             PG_PRESENT | PG_USER | PG_WRITABLE) != 0)
        return -1;

    return sched_add(&proc);
}

static void teardown_procfs_test_process(uint32_t pid)
{
    process_t *proc = sched_find_pid(pid);

    if (proc)
        process_release_user_space(proc);
    sched_init();
    vfs_reset();
}

static int read_procfs_text_file(uint32_t kind, uint32_t pid, char *buf, uint32_t cap)
{
    int n = procfs_read_file(kind, pid, 0u, 0u, buf, cap - 1u);

    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static void test_open_no_mount(ktest_case_t *tc)
{
    vfs_file_ref_t ref;
    uint32_t size = 0;
    vfs_reset();
    KTEST_EXPECT_TRUE(tc, vfs_open_file("anything", &ref, &size) < 0);
    vfs_reset();
}

static void test_getdents_no_mount(ktest_case_t *tc)
{
    char buf[64];
    vfs_reset();
    KTEST_EXPECT_TRUE(tc, vfs_getdents(0, buf, sizeof(buf)) < 0);
    vfs_reset();
}

static void test_create_no_mount(ktest_case_t *tc)
{
    vfs_reset();
    KTEST_EXPECT_TRUE(tc, vfs_create("newfile") < 0);
    vfs_reset();
}

static void test_unlink_no_mount(ktest_case_t *tc)
{
    vfs_reset();
    KTEST_EXPECT_TRUE(tc, vfs_unlink("newfile") < 0);
    vfs_reset();
}

static void test_cross_mount_open_prefers_child_mount(ktest_case_t *tc)
{
    vfs_file_ref_t ref;
    uint32_t size = 0;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree(), 0u);
    KTEST_EXPECT_EQ(tc,
                    (uint32_t)vfs_open_file("mnt/childfile", &ref, &size),
                    0u);
    KTEST_EXPECT_EQ(tc, ref.inode_num, CHILD_FILE_INO);
    KTEST_EXPECT_EQ(tc, size, CHILD_FILE_SIZE);
    vfs_reset();
}

static void test_file_ref_read_uses_owning_mount(ktest_case_t *tc)
{
    vfs_file_ref_t ref;
    uint32_t size = 0;
    char buf[8];
    int n;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree(), 0u);
    KTEST_EXPECT_EQ(tc,
                    (uint32_t)vfs_open_file("mnt/childfile", &ref, &size),
                    0u);
    KTEST_EXPECT_EQ(tc, size, CHILD_FILE_SIZE);

    n = vfs_read(ref, 0u, (uint8_t *)buf, 7u);
    KTEST_EXPECT_EQ(tc, (uint32_t)n, 7u);
    buf[7] = '\0';
    KTEST_EXPECT_TRUE(tc, k_strcmp(buf, "child!!") == 0);
    vfs_reset();
}

static void test_root_listing_includes_mount_points_once(ktest_case_t *tc)
{
    char buf[128];
    int n;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree(), 0u);
    n = vfs_getdents(0, buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "hello"));
    KTEST_EXPECT_EQ(tc, (uint32_t)count_entry(buf, n, "mnt/"), 1u);
    KTEST_EXPECT_EQ(tc, (uint32_t)count_entry(buf, n, "dev/"), 1u);
    vfs_reset();
}

static void test_mount_root_stat_reports_directory(ktest_case_t *tc)
{
    vfs_stat_t st;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_stat("mnt", &st), 0u);
    KTEST_EXPECT_EQ(tc, st.type, 2u);
    vfs_reset();
}

static void test_dev_namespace_lists_and_resolves_devices(ktest_case_t *tc)
{
    char buf[128];
    int n;
    vfs_node_t node;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree(), 0u);
    KTEST_EXPECT_TRUE(tc, blkdev_get("sda") != 0);
    KTEST_EXPECT_TRUE(tc, blkdev_get("sda1") != 0);
    KTEST_EXPECT_TRUE(tc, blkdev_get("sdb") != 0);
    KTEST_EXPECT_TRUE(tc, blkdev_get("sdb1") != 0);

    n = vfs_getdents("dev", buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "stdin"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "tty0"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "sda"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "sda1"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "sdb"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "sdb1"));

    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_resolve("/dev/tty0", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, (uint32_t)VFS_NODE_TTY);
    KTEST_EXPECT_EQ(tc, node.dev_id, 0u);

    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_resolve("/dev/sda", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, (uint32_t)VFS_NODE_BLOCKDEV);
    KTEST_EXPECT_TRUE(tc, k_strcmp(node.dev_name, "sda") == 0);

    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_resolve("/dev/sda1", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, (uint32_t)VFS_NODE_BLOCKDEV);
    KTEST_EXPECT_TRUE(tc, k_strcmp(node.dev_name, "sda1") == 0);

    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_resolve("/dev/sdb", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, (uint32_t)VFS_NODE_BLOCKDEV);
    KTEST_EXPECT_TRUE(tc, k_strcmp(node.dev_name, "sdb") == 0);

    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_resolve("/dev/sdb1", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, (uint32_t)VFS_NODE_BLOCKDEV);
    KTEST_EXPECT_TRUE(tc, k_strcmp(node.dev_name, "sdb1") == 0);
    vfs_reset();
}

static void test_sysfs_block_tree_lists_disks_and_partition_metadata(ktest_case_t *tc)
{
    char buf[256];
    char text[64];
    int n;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_sys(), 0u);

    n = vfs_getdents("/sys", buf, sizeof(buf));
    KTEST_ASSERT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "block/"));

    n = vfs_getdents("/sys/block", buf, sizeof(buf));
    KTEST_ASSERT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "sda/"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "sdb/"));
    KTEST_EXPECT_TRUE(tc, !has_entry(buf, n, "sda1/"));

    n = vfs_getdents("/sys/block/sda", buf, sizeof(buf));
    KTEST_ASSERT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "size"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "dev"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "type"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "sda1/"));

    n = vfs_getdents("/sys/block/sda/sda1", buf, sizeof(buf));
    KTEST_ASSERT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "size"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "dev"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "type"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "partition"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "start"));

    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sda/size",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "102400\n") == 0);
    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sda/dev",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "8:0\n") == 0);
    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sda/type",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "disk\n") == 0);

    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sda/sda1/size",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "100352\n") == 0);
    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sda/sda1/dev",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "8:1\n") == 0);
    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sda/sda1/type",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "part\n") == 0);
    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sda/sda1/partition",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "1\n") == 0);
    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sda/sda1/start",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "2048\n") == 0);

    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sdb/dev",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "8:16\n") == 0);
    KTEST_ASSERT_TRUE(tc,
                      read_vfs_text_path("/sys/block/sdb/sdb1/dev",
                                         text, sizeof(text)) > 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(text, "8:17\n") == 0);
    vfs_reset();
}

static void test_proc_namespace_lists_modules_and_pid_dirs(ktest_case_t *tc)
{
    char buf[128];
    int n;
    int pid;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    pid = add_procfs_test_process();
    KTEST_EXPECT_EQ(tc, (uint32_t)pid, 1u);

    n = vfs_getdents("proc", buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "kmsg"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "modules"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "1/"));
    vfs_reset();
}

static void test_proc_pid_directory_lists_status_maps_and_fd(ktest_case_t *tc)
{
    char buf[128];
    int n;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)add_procfs_test_process(), 1u);

    n = vfs_getdents("proc/1", buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "status"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "maps"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "fd/"));
    vfs_reset();
}

static void test_proc_pid_directory_lists_vmstat_and_fault(ktest_case_t *tc)
{
    char buf[128];
    int n;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)add_procfs_test_process(), 1u);

    n = vfs_getdents("proc/1", buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "vmstat"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "fault"));
    vfs_reset();
}

static void test_proc_vmstat_reports_image_totals_and_region_count(ktest_case_t *tc)
{
    char buf[256];
    int n;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)add_procfs_mapped_layout_process(1), 1u);

    n = read_procfs_text_file(PROCFS_FILE_VMSTAT, 1u, buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "Image:\t4096/65536") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "Heap:\t4096/32768") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "Stack:\t4096/262144") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "Regions:\t3") != 0);
    teardown_procfs_test_process(1u);
}

static void test_proc_maps_preserves_mapped_subranges(ktest_case_t *tc)
{
    char buf[512];
    int n;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)add_procfs_mapped_layout_process(1), 1u);

    n = read_procfs_text_file(PROCFS_FILE_MAPS, 1u, buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "00400000-00401000 r--p shell") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "00410000-00411000 rw-p [heap]") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "bffff000-c0000000 rw-p [stack]") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "00410000-00418000 rw-p [heap]") == 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "4096/32768") == 0);
    teardown_procfs_test_process(1u);
}

static void test_proc_maps_matches_mem_forensics_renderer(ktest_case_t *tc)
{
    static char procfs_buf[512];
    static char render_buf[512];
    uint32_t rendered = 0;
    int n;
    process_t *proc;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)add_procfs_mapped_layout_process(1), 1u);

    proc = sched_find_pid(1u);
    KTEST_ASSERT_NOT_NULL(tc, proc);

    n = read_procfs_text_file(PROCFS_FILE_MAPS, 1u, procfs_buf, sizeof(procfs_buf));
    KTEST_ASSERT_TRUE(tc, n > 0);

    KTEST_ASSERT_EQ(tc,
                    (uint32_t)mem_forensics_render_maps(proc, render_buf,
                                                       sizeof(render_buf),
                                                       &rendered),
                    0u);
    render_buf[rendered] = '\0';
    KTEST_EXPECT_TRUE(tc, k_strcmp(procfs_buf, render_buf) == 0);
    teardown_procfs_test_process(1u);
}

static void test_proc_fallback_image_surfaces_stay_visible(ktest_case_t *tc)
{
    char vmstat[256];
    char maps[512];
    int n;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)add_procfs_mapped_layout_process(0), 1u);

    n = read_procfs_text_file(PROCFS_FILE_VMSTAT, 1u, vmstat, sizeof(vmstat));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(vmstat, "Image:\t4096/65536") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(vmstat, "Regions:\t3") != 0);

    n = read_procfs_text_file(PROCFS_FILE_MAPS, 1u, maps, sizeof(maps));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(maps, "00400000-00401000 r--p shell") != 0);
    teardown_procfs_test_process(1u);
}

static void test_proc_fd_directory_lists_open_fds(ktest_case_t *tc)
{
    char buf[128];
    int n;
    vfs_node_t node;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)add_procfs_test_process(), 1u);

    n = vfs_getdents("proc/1/fd", buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "0"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "1"));

    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_resolve("proc/1/status", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, (uint32_t)VFS_NODE_PROCFILE);
    vfs_reset();
}

static void test_proc_kmsg_renders_retained_kernel_log(ktest_case_t *tc)
{
    char buf[512];
    uint32_t size = 0;
    uint32_t offset = 0;
    int n;
    vfs_node_t node;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    klog_silent("TESTLOG", "procfs-kmsg-visible");

    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_resolve("proc/kmsg", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, (uint32_t)VFS_NODE_PROCFILE);
    KTEST_EXPECT_EQ(tc, node.proc_kind, (uint32_t)PROCFS_FILE_KMSG);

    KTEST_EXPECT_EQ(tc,
                    (uint32_t)procfs_file_size(node.proc_kind,
                                               node.proc_pid,
                                               node.proc_index,
                                               &size),
                    0u);
    KTEST_EXPECT_TRUE(tc, size > 0u);

    offset = (size > (uint32_t)(sizeof(buf) - 1u))
             ? size - (uint32_t)(sizeof(buf) - 1u)
             : 0u;
    n = procfs_read_file(node.proc_kind, node.proc_pid, node.proc_index,
                         offset, buf, sizeof(buf) - 1u);
    KTEST_EXPECT_TRUE(tc, n > 0);
    buf[n] = '\0';

    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "INFO") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "[TESTLOG]") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "procfs-kmsg-visible") != 0);
    vfs_reset();
}

static void test_proc_fault_reports_empty_state_without_crash(ktest_case_t *tc)
{
    char buf[256];
    int n;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)add_procfs_test_process(), 1u);

    n = procfs_read_file(PROCFS_FILE_FAULT, 1u, 0u, 0u, buf, sizeof(buf) - 1u);
    KTEST_EXPECT_TRUE(tc, n > 0);
    buf[n] = '\0';
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "State:\tnone") != 0);
    vfs_reset();
}

static void test_proc_fault_reports_crash_context(ktest_case_t *tc)
{
    char buf[256];
    int n;
    process_t *proc;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree_with_proc(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)add_procfs_test_process(), 1u);

    proc = sched_find_pid(1u);
    KTEST_ASSERT_NOT_NULL(tc, proc);
    proc->crash.valid = 1;
    proc->crash.signum = SIGSEGV;
    proc->crash.cr2 = 0xDEADBEEFu;
    proc->crash.frame.eip = 0x00401234u;
    proc->crash.frame.vector = 14u;
    proc->crash.frame.error_code = 0x6u;

    n = procfs_read_file(PROCFS_FILE_FAULT, 1u, 0u, 0u, buf, sizeof(buf) - 1u);
    KTEST_EXPECT_TRUE(tc, n > 0);
    buf[n] = '\0';
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "Signal:\t11") != 0);
    KTEST_EXPECT_TRUE(tc, k_strstr(buf, "CR2:\t0xdeadbeef") != 0);
    vfs_reset();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

static ktest_case_t cases[] = {
    KTEST_CASE(test_open_no_mount),
    KTEST_CASE(test_getdents_no_mount),
    KTEST_CASE(test_create_no_mount),
    KTEST_CASE(test_unlink_no_mount),
    KTEST_CASE(test_cross_mount_open_prefers_child_mount),
    KTEST_CASE(test_file_ref_read_uses_owning_mount),
    KTEST_CASE(test_root_listing_includes_mount_points_once),
    KTEST_CASE(test_mount_root_stat_reports_directory),
    KTEST_CASE(test_dev_namespace_lists_and_resolves_devices),
    KTEST_CASE(test_sysfs_block_tree_lists_disks_and_partition_metadata),
    KTEST_CASE(test_proc_namespace_lists_modules_and_pid_dirs),
    KTEST_CASE(test_proc_pid_directory_lists_status_maps_and_fd),
    KTEST_CASE(test_proc_pid_directory_lists_vmstat_and_fault),
    KTEST_CASE(test_proc_vmstat_reports_image_totals_and_region_count),
    KTEST_CASE(test_proc_maps_preserves_mapped_subranges),
    KTEST_CASE(test_proc_maps_matches_mem_forensics_renderer),
    KTEST_CASE(test_proc_fallback_image_surfaces_stay_visible),
    KTEST_CASE(test_proc_fd_directory_lists_open_fds),
    KTEST_CASE(test_proc_kmsg_renders_retained_kernel_log),
    KTEST_CASE(test_proc_fault_reports_empty_state_without_crash),
    KTEST_CASE(test_proc_fault_reports_crash_context),
};

static ktest_suite_t suite = KTEST_SUITE("vfs", cases);

ktest_suite_t *ktest_suite_vfs(void) { return &suite; }
