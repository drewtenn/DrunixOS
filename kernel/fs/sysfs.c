/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * sysfs.c — synthetic /sys tree exposing block-device metadata.
 */

#include "sysfs.h"
#include "blkdev.h"
#include "kprintf.h"
#include "kstring.h"
#include <stdint.h>

#define SYSFS_INODE_KIND_SHIFT  24u
#define SYSFS_INODE_DISK_SHIFT   12u
#define SYSFS_INODE_PART_SHIFT    0u
#define SYSFS_FILE_TEXT_MAX      32u

typedef enum {
    SYSFS_INODE_DISK_SIZE = 1,
    SYSFS_INODE_DISK_DEV = 2,
    SYSFS_INODE_DISK_TYPE = 3,
    SYSFS_INODE_PART_SIZE = 4,
    SYSFS_INODE_PART_DEV = 5,
    SYSFS_INODE_PART_TYPE = 6,
    SYSFS_INODE_PARTITION = 7,
    SYSFS_INODE_START = 8,
} sysfs_inode_kind_t;

static uint32_t sysfs_make_inode(uint32_t kind, uint32_t disk_index,
                                 uint32_t part_index)
{
    return (kind << SYSFS_INODE_KIND_SHIFT) |
           ((disk_index & 0x0FFFu) << SYSFS_INODE_DISK_SHIFT) |
           ((part_index & 0x0FFFu) << SYSFS_INODE_PART_SHIFT);
}

static uint32_t sysfs_inode_kind(uint32_t inode)
{
    return inode >> SYSFS_INODE_KIND_SHIFT;
}

static uint32_t sysfs_inode_disk(uint32_t inode)
{
    return (inode >> SYSFS_INODE_DISK_SHIFT) & 0x0FFFu;
}

static uint32_t sysfs_inode_part(uint32_t inode)
{
    return (inode >> SYSFS_INODE_PART_SHIFT) & 0x0FFFu;
}

static void sysfs_clear_node(vfs_node_t *node_out)
{
    if (!node_out)
        return;

    node_out->type = VFS_NODE_NONE;
    node_out->inode_num = 0;
    node_out->mount_id = 0;
    node_out->size = 0;
    node_out->dev_id = 0;
    node_out->dev_name[0] = '\0';
    node_out->proc_kind = 0;
    node_out->proc_pid = 0;
    node_out->proc_index = 0;
}

static int sysfs_append_dirent(char *buf, uint32_t bufsz, uint32_t *written,
                               const char *name, int is_dir)
{
    uint32_t len = k_strlen(name);

    if (!buf || !written || !name)
        return -1;
    if (*written + len + (is_dir ? 2u : 1u) > bufsz)
        return -1;

    k_memcpy(buf + *written, name, len);
    *written += len;
    if (is_dir)
        buf[(*written)++] = '/';
    buf[(*written)++] = '\0';
    return 0;
}

static int sysfs_copy_name(char *dst, uint32_t dstsz, const char *src,
                           uint32_t len)
{
    if (!dst || !src || dstsz == 0 || len + 1u > dstsz)
        return -1;
    k_memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

static int sysfs_lookup_disk(const char *name, uint32_t *index_out,
                             blkdev_info_t *info_out)
{
    int idx;
    blkdev_info_t info;

    if (!name)
        return -1;
    idx = blkdev_find_index(name);
    if (idx < 0)
        return -1;
    if (blkdev_info_for_index((uint32_t)idx, &info) != 0 ||
        info.kind != BLKDEV_KIND_DISK)
        return -1;

    if (index_out)
        *index_out = (uint32_t)idx;
    if (info_out)
        *info_out = info;
    return 0;
}

static int sysfs_lookup_partition(uint32_t disk_index, const char *name,
                                  uint32_t *index_out, blkdev_info_t *info_out)
{
    int idx;
    blkdev_info_t info;

    if (!name)
        return -1;
    idx = blkdev_find_index(name);
    if (idx < 0)
        return -1;
    if (blkdev_info_for_index((uint32_t)idx, &info) != 0 ||
        info.kind != BLKDEV_KIND_PART ||
        info.parent_index != disk_index)
        return -1;

    if (index_out)
        *index_out = (uint32_t)idx;
    if (info_out)
        *info_out = info;
    return 0;
}

static int sysfs_render_leaf_text(uint32_t inode_num, char *buf, uint32_t bufsz)
{
    char tmp[SYSFS_FILE_TEXT_MAX];
    blkdev_info_t disk;
    blkdev_info_t part;
    uint32_t kind = sysfs_inode_kind(inode_num);
    uint32_t disk_index = sysfs_inode_disk(inode_num);
    uint32_t part_index = sysfs_inode_part(inode_num);
    int n = -1;

    if (blkdev_info_for_index(disk_index, &disk) != 0 ||
        disk.kind != BLKDEV_KIND_DISK)
        return -1;

    switch ((sysfs_inode_kind_t)kind) {
    case SYSFS_INODE_DISK_SIZE:
        n = k_snprintf(tmp, sizeof(tmp), "%u\n", disk.sectors);
        break;
    case SYSFS_INODE_DISK_DEV:
        n = k_snprintf(tmp, sizeof(tmp), "%u:%u\n", disk.major, disk.minor);
        break;
    case SYSFS_INODE_DISK_TYPE:
        n = k_snprintf(tmp, sizeof(tmp), "disk\n");
        break;
    case SYSFS_INODE_PART_SIZE:
        if (blkdev_info_for_index(part_index, &part) != 0 ||
            part.kind != BLKDEV_KIND_PART ||
            part.parent_index != disk_index)
            return -1;
        n = k_snprintf(tmp, sizeof(tmp), "%u\n", part.sectors);
        break;
    case SYSFS_INODE_PART_DEV:
        if (blkdev_info_for_index(part_index, &part) != 0 ||
            part.kind != BLKDEV_KIND_PART ||
            part.parent_index != disk_index)
            return -1;
        n = k_snprintf(tmp, sizeof(tmp), "%u:%u\n", part.major, part.minor);
        break;
    case SYSFS_INODE_PART_TYPE:
        if (blkdev_info_for_index(part_index, &part) != 0 ||
            part.kind != BLKDEV_KIND_PART ||
            part.parent_index != disk_index)
            return -1;
        n = k_snprintf(tmp, sizeof(tmp), "part\n");
        break;
    case SYSFS_INODE_PARTITION:
        if (blkdev_info_for_index(part_index, &part) != 0 ||
            part.kind != BLKDEV_KIND_PART ||
            part.parent_index != disk_index)
            return -1;
        n = k_snprintf(tmp, sizeof(tmp), "%u\n", part.partition_number);
        break;
    case SYSFS_INODE_START:
        if (blkdev_info_for_index(part_index, &part) != 0 ||
            part.kind != BLKDEV_KIND_PART ||
            part.parent_index != disk_index)
            return -1;
        n = k_snprintf(tmp, sizeof(tmp), "%u\n", part.start_sector);
        break;
    default:
        return -1;
    }

    if (n < 0)
        return -1;

    if (buf && bufsz > 0) {
        uint32_t copy = (uint32_t)n < bufsz ? (uint32_t)n : bufsz;
        k_memcpy(buf, tmp, copy);
        return (int)copy;
    }

    return n;
}

static int sysfs_render_block_root(char *buf, uint32_t bufsz)
{
    uint32_t written = 0;
    blkdev_info_t disk;

    for (uint32_t i = 0; i < BLKDEV_MAX; i++) {
        if (blkdev_info_for_index(i, &disk) != 0 ||
            disk.kind != BLKDEV_KIND_DISK)
            continue;
        if (sysfs_append_dirent(buf, bufsz, &written, disk.name, 1) != 0)
            break;
    }
    return (int)written;
}

static int sysfs_render_disk_dir(uint32_t disk_index, char *buf, uint32_t bufsz)
{
    uint32_t written = 0;
    blkdev_info_t child;

    if (sysfs_append_dirent(buf, bufsz, &written, "size", 0) != 0)
        return (int)written;
    if (sysfs_append_dirent(buf, bufsz, &written, "dev", 0) != 0)
        return (int)written;
    if (sysfs_append_dirent(buf, bufsz, &written, "type", 0) != 0)
        return (int)written;

    for (uint32_t i = 0; i < BLKDEV_MAX; i++) {
        if (blkdev_info_for_index(i, &child) != 0 ||
            child.kind != BLKDEV_KIND_PART ||
            child.parent_index != disk_index)
            continue;
        if (sysfs_append_dirent(buf, bufsz, &written, child.name, 1) != 0)
            break;
    }
    return (int)written;
}

static int sysfs_render_partition_dir(char *buf, uint32_t bufsz)
{
    uint32_t written = 0;

    if (sysfs_append_dirent(buf, bufsz, &written, "size", 0) != 0)
        return (int)written;
    if (sysfs_append_dirent(buf, bufsz, &written, "dev", 0) != 0)
        return (int)written;
    if (sysfs_append_dirent(buf, bufsz, &written, "type", 0) != 0)
        return (int)written;
    if (sysfs_append_dirent(buf, bufsz, &written, "partition", 0) != 0)
        return (int)written;
    if (sysfs_append_dirent(buf, bufsz, &written, "start", 0) != 0)
        return (int)written;
    return (int)written;
}

static int sysfs_fill_disk_node(const char *relpath, vfs_node_t *node_out)
{
    uint32_t disk_index = 0;
    blkdev_info_t disk;

    if (k_strcmp(relpath, "block") == 0) {
        node_out->type = VFS_NODE_DIR;
        return 0;
    }

    if (k_strncmp(relpath, "block/", 6) != 0)
        return -1;
    relpath += 6;

    {
        const char *slash = k_strchr(relpath, '/');
        uint32_t disk_name_len = slash ? (uint32_t)(slash - relpath)
                                       : k_strlen(relpath);
        char disk_name[BLKDEV_NAME_MAX];

        if (sysfs_copy_name(disk_name, sizeof(disk_name), relpath,
                            disk_name_len) != 0)
            return -1;
        if (sysfs_lookup_disk(disk_name, &disk_index, &disk) != 0)
            return -1;

        if (!slash) {
            node_out->type = VFS_NODE_DIR;
            return 0;
        }

        relpath = slash + 1;
        slash = k_strchr(relpath, '/');
        if (!slash) {
            if (k_strcmp(relpath, "size") == 0) {
                node_out->type = VFS_NODE_SYSFILE;
                node_out->inode_num = sysfs_make_inode(SYSFS_INODE_DISK_SIZE,
                                                       disk_index, 0);
                node_out->size = (uint32_t)sysfs_render_leaf_text(node_out->inode_num,
                                                                  0, 0);
                return 0;
            }
            if (k_strcmp(relpath, "dev") == 0) {
                node_out->type = VFS_NODE_SYSFILE;
                node_out->inode_num = sysfs_make_inode(SYSFS_INODE_DISK_DEV,
                                                       disk_index, 0);
                node_out->size = (uint32_t)sysfs_render_leaf_text(node_out->inode_num,
                                                                  0, 0);
                return 0;
            }
            if (k_strcmp(relpath, "type") == 0) {
                node_out->type = VFS_NODE_SYSFILE;
                node_out->inode_num = sysfs_make_inode(SYSFS_INODE_DISK_TYPE,
                                                       disk_index, 0);
                node_out->size = (uint32_t)sysfs_render_leaf_text(node_out->inode_num,
                                                                  0, 0);
                return 0;
            }

            if (sysfs_lookup_partition(disk_index, relpath, 0, 0) == 0) {
                node_out->type = VFS_NODE_DIR;
                return 0;
            }
            return -1;
        }

        {
            uint32_t part_index = 0;
            blkdev_info_t part;
            uint32_t part_name_len = (uint32_t)(slash - relpath);
            char part_name[BLKDEV_NAME_MAX];

            if (sysfs_copy_name(part_name, sizeof(part_name), relpath,
                                part_name_len) != 0)
                return -1;
            if (sysfs_lookup_partition(disk_index, part_name, &part_index, &part) != 0)
                return -1;

            relpath = slash + 1;
            if (k_strcmp(relpath, "size") == 0) {
                node_out->type = VFS_NODE_SYSFILE;
                node_out->inode_num = sysfs_make_inode(SYSFS_INODE_PART_SIZE,
                                                       disk_index, part_index);
                node_out->size = (uint32_t)sysfs_render_leaf_text(node_out->inode_num,
                                                                  0, 0);
                return 0;
            }
            if (k_strcmp(relpath, "dev") == 0) {
                node_out->type = VFS_NODE_SYSFILE;
                node_out->inode_num = sysfs_make_inode(SYSFS_INODE_PART_DEV,
                                                       disk_index, part_index);
                node_out->size = (uint32_t)sysfs_render_leaf_text(node_out->inode_num,
                                                                  0, 0);
                return 0;
            }
            if (k_strcmp(relpath, "type") == 0) {
                node_out->type = VFS_NODE_SYSFILE;
                node_out->inode_num = sysfs_make_inode(SYSFS_INODE_PART_TYPE,
                                                       disk_index, part_index);
                node_out->size = (uint32_t)sysfs_render_leaf_text(node_out->inode_num,
                                                                  0, 0);
                return 0;
            }
            if (k_strcmp(relpath, "partition") == 0) {
                node_out->type = VFS_NODE_SYSFILE;
                node_out->inode_num = sysfs_make_inode(SYSFS_INODE_PARTITION,
                                                       disk_index, part_index);
                node_out->size = (uint32_t)sysfs_render_leaf_text(node_out->inode_num,
                                                                  0, 0);
                return 0;
            }
            if (k_strcmp(relpath, "start") == 0) {
                node_out->type = VFS_NODE_SYSFILE;
                node_out->inode_num = sysfs_make_inode(SYSFS_INODE_START,
                                                       disk_index, part_index);
                node_out->size = (uint32_t)sysfs_render_leaf_text(node_out->inode_num,
                                                                  0, 0);
                return 0;
            }
        }
    }

    return -1;
}

int sysfs_fill_node(const char *relpath, vfs_node_t *node_out)
{
    int rc;

    if (!node_out)
        return -1;
    sysfs_clear_node(node_out);

    rc = sysfs_fill_disk_node(relpath, node_out);
    if (rc != 0)
        return rc;

    return 0;
}

int sysfs_stat(const char *relpath, vfs_stat_t *st)
{
    vfs_node_t node;

    if (!st)
        return -1;
    if (sysfs_fill_node(relpath, &node) != 0)
        return -1;

    if (node.type == VFS_NODE_DIR) {
        st->type = VFS_STAT_TYPE_DIR;
        st->size = 0;
    } else if (node.type == VFS_NODE_SYSFILE) {
        st->type = VFS_STAT_TYPE_FILE;
        st->size = node.size;
    } else {
        return -1;
    }
    st->link_count = 1;
    st->mtime = 0;
    return 0;
}

int sysfs_getdents(const char *relpath, char *buf, uint32_t bufsz)
{
    if (!buf)
        return -1;
    if (!relpath || relpath[0] == '\0') {
        uint32_t written = 0;
        if (sysfs_append_dirent(buf, bufsz, &written, "block", 1) != 0)
            return -1;
        return (int)written;
    }
    if (k_strcmp(relpath, "block") == 0) {
        return sysfs_render_block_root(buf, bufsz);
    }
    if (k_strncmp(relpath, "block/", 6) != 0)
        return -1;

    {
        const char *tail = relpath + 6;
        const char *slash = k_strchr(tail, '/');
        uint32_t disk_name_len = slash ? (uint32_t)(slash - tail)
                                       : k_strlen(tail);
        char disk_name[BLKDEV_NAME_MAX];
        uint32_t disk_index = 0;
        blkdev_info_t disk;

        if (sysfs_copy_name(disk_name, sizeof(disk_name), tail,
                            disk_name_len) != 0)
            return -1;
        if (sysfs_lookup_disk(disk_name, &disk_index, &disk) != 0)
            return -1;

        if (!slash)
            return sysfs_render_disk_dir(disk_index, buf, bufsz);

        tail = slash + 1;
        slash = k_strchr(tail, '/');
        if (!slash) {
            if (sysfs_lookup_partition(disk_index, tail, 0, 0) == 0)
                return sysfs_render_partition_dir(buf, bufsz);
            return -1;
        }

        {
            uint32_t part_name_len = (uint32_t)(slash - tail);
            char part_name[BLKDEV_NAME_MAX];

            if (sysfs_copy_name(part_name, sizeof(part_name), tail,
                                part_name_len) != 0)
                return -1;
            if (sysfs_lookup_partition(disk_index, part_name, 0, 0) != 0)
                return -1;

            if (k_strcmp(slash + 1, "size") == 0 ||
                k_strcmp(slash + 1, "dev") == 0 ||
                k_strcmp(slash + 1, "type") == 0 ||
                k_strcmp(slash + 1, "partition") == 0 ||
                k_strcmp(slash + 1, "start") == 0)
                return -1;
            return -1;
        }
    }

    return -1;
}

int sysfs_file_size(uint32_t inode_num, uint32_t *size_out)
{
    int n = sysfs_render_leaf_text(inode_num, 0, 0);

    if (!size_out || n < 0)
        return -1;
    *size_out = (uint32_t)n;
    return 0;
}

int sysfs_read_file(uint32_t inode_num, uint32_t offset,
                    char *buf, uint32_t count)
{
    char tmp[SYSFS_FILE_TEXT_MAX];
    int n;

    if (!buf)
        return -1;
    n = sysfs_render_leaf_text(inode_num, tmp, sizeof(tmp));
    if (n < 0)
        return -1;
    if (offset >= (uint32_t)n)
        return 0;
    if (count > (uint32_t)n - offset)
        count = (uint32_t)n - offset;
    if (count == 0)
        return 0;
    k_memcpy(buf, tmp + offset, count);
    return (int)count;
}
