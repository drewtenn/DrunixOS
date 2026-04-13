/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_vfs.c — in-kernel virtual filesystem layer unit tests.
 */

#include "ktest.h"
#include "vfs.h"
#include "procfs.h"
#include "sched.h"
#include "klog.h"
#include "kstring.h"

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

static int mock_root_open(const char *path, uint32_t *inode_out, uint32_t *size_out)
{
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

static int mock_root_getdents(const char *path, char *buf, uint32_t bufsz)
{
    uint32_t written = 0;

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

static int mock_root_stat(const char *path, vfs_stat_t *st)
{
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

static int mock_sub_open(const char *path, uint32_t *inode_out, uint32_t *size_out)
{
    if (k_strcmp(path, "childfile") == 0) {
        *inode_out = CHILD_FILE_INO;
        *size_out = CHILD_FILE_SIZE;
        return 0;
    }
    return -1;
}

static int mock_sub_getdents(const char *path, char *buf, uint32_t bufsz)
{
    static const char entries[] = "childfile\0";
    uint32_t n = (uint32_t)sizeof(entries);

    if (!buf)
        return -1;
    if (path && path[0] != '\0')
        return -1;

    if (n > bufsz)
        n = bufsz;
    k_memcpy(buf, entries, n);
    return (int)n;
}

static int mock_sub_stat(const char *path, vfs_stat_t *st)
{
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

static void test_open_no_mount(ktest_case_t *tc)
{
    uint32_t ino = 0, size = 0;
    vfs_reset();
    KTEST_EXPECT_TRUE(tc, vfs_open("anything", &ino, &size) < 0);
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
    uint32_t ino = 0, size = 0;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree(), 0u);
    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_open("mnt/childfile", &ino, &size), 0u);
    KTEST_EXPECT_EQ(tc, ino, CHILD_FILE_INO);
    KTEST_EXPECT_EQ(tc, size, CHILD_FILE_SIZE);
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
    vfs_stat_t st;

    KTEST_EXPECT_EQ(tc, (uint32_t)setup_mount_tree(), 0u);

    n = vfs_getdents("dev", buf, sizeof(buf));
    KTEST_EXPECT_TRUE(tc, n > 0);
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "stdin"));
    KTEST_EXPECT_TRUE(tc, has_entry(buf, n, "tty0"));

    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_stat("dev", &st), 0u);
    KTEST_EXPECT_EQ(tc, st.type, 2u);

    KTEST_EXPECT_EQ(tc, (uint32_t)vfs_resolve("dev/tty0", &node), 0u);
    KTEST_EXPECT_EQ(tc, node.type, (uint32_t)VFS_NODE_TTY);
    KTEST_EXPECT_EQ(tc, node.dev_id, 0u);
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
    klog("TESTLOG", "procfs-kmsg-visible");

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

/* ── Suite ──────────────────────────────────────────────────────────────── */

static ktest_case_t cases[] = {
    KTEST_CASE(test_open_no_mount),
    KTEST_CASE(test_getdents_no_mount),
    KTEST_CASE(test_create_no_mount),
    KTEST_CASE(test_unlink_no_mount),
    KTEST_CASE(test_cross_mount_open_prefers_child_mount),
    KTEST_CASE(test_root_listing_includes_mount_points_once),
    KTEST_CASE(test_mount_root_stat_reports_directory),
    KTEST_CASE(test_dev_namespace_lists_and_resolves_devices),
    KTEST_CASE(test_proc_namespace_lists_modules_and_pid_dirs),
    KTEST_CASE(test_proc_pid_directory_lists_status_maps_and_fd),
    KTEST_CASE(test_proc_fd_directory_lists_open_fds),
    KTEST_CASE(test_proc_kmsg_renders_retained_kernel_log),
};

static ktest_suite_t suite = KTEST_SUITE("vfs", cases);

ktest_suite_t *ktest_suite_vfs(void) { return &suite; }
