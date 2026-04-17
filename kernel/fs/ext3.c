/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ext3.c — native read-only ext2/ext3-compatible filesystem backend.
 */

#include "ext3.h"
#include "vfs.h"
#include "blkdev.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include <stdint.h>

#define EXT3_SUPER_OFFSET       1024u
#define EXT3_SUPER_MAGIC        0xEF53u
#define EXT3_ROOT_INO           2u
#define EXT3_NAME_MAX           255u
#define EXT3_N_BLOCKS           15u
#define EXT3_NDIR_BLOCKS        12u
#define EXT3_FT_REG_FILE        1u
#define EXT3_FT_DIR             2u
#define EXT3_FT_SYMLINK         7u
#define EXT3_S_IFMT             0xF000u
#define EXT3_S_IFREG            0x8000u
#define EXT3_S_IFDIR            0x4000u
#define EXT3_S_IFLNK            0xA000u
#define EXT3_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT3_FEATURE_INCOMPAT_FILETYPE  0x0002u
#define EXT3_FEATURE_INCOMPAT_RECOVER   0x0004u
#define EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT3_FEATURE_RO_COMPAT_LARGE_FILE   0x0002u
#define JBD_MAGIC                 0xC03B3998u
#define JBD_DESCRIPTOR_BLOCK      1u
#define JBD_COMMIT_BLOCK          2u
#define JBD_SUPERBLOCK_V1         3u
#define JBD_SUPERBLOCK_V2         4u
#define JBD_REVOKE_BLOCK          5u
#define JBD_FLAG_ESCAPE           1u
#define JBD_FLAG_SAME_UUID        2u
#define JBD_FLAG_LAST_TAG         8u
#define JBD_FEATURE_INCOMPAT_REVOKE 0x00000001u
#define EXT3_REPLAY_MAX_BLOCKS    32u
#define EXT3_REPLAY_MAX_TAGS      64u
#define EXT3_REPLAY_MAX_REVOKES   128u

typedef struct {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t r_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[16];
    char     last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t  prealloc_blocks;
    uint8_t  prealloc_dir_blocks;
    uint16_t reserved_gdt_blocks;
    uint8_t  journal_uuid[16];
    uint32_t journal_inum;
    uint32_t journal_dev;
    uint32_t last_orphan;
} __attribute__((packed)) ext3_super_t;

typedef struct {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t  reserved[12];
} __attribute__((packed)) ext3_group_desc_t;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[EXT3_N_BLOCKS];
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint8_t  osd2[12];
} __attribute__((packed)) ext3_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} __attribute__((packed)) ext3_dirent_t;

static const blkdev_ops_t *g_dev;
static ext3_super_t g_super;
static ext3_group_desc_t g_bg;
static uint32_t g_block_size;
static uint32_t g_sectors_per_block;

typedef struct {
    uint32_t fs_block;
    uint8_t *data;
} ext3_overlay_block_t;

static ext3_overlay_block_t g_overlay[EXT3_REPLAY_MAX_BLOCKS];
static uint32_t g_overlay_count;

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           p[3];
}

static int ext3_read_bytes(uint32_t byte_off, uint8_t *buf, uint32_t count)
{
    uint8_t sec[BLKDEV_SECTOR_SIZE];
    uint32_t done = 0;

    if (!g_dev || !buf)
        return -1;
    while (done < count) {
        uint32_t off = byte_off + done;
        uint32_t lba = off / BLKDEV_SECTOR_SIZE;
        uint32_t sec_off = off % BLKDEV_SECTOR_SIZE;
        uint32_t chunk = BLKDEV_SECTOR_SIZE - sec_off;
        if (chunk > count - done)
            chunk = count - done;
        if (g_dev->read_sector(lba, sec) != 0)
            return -1;
        k_memcpy(buf + done, sec + sec_off, chunk);
        done += chunk;
    }
    return 0;
}

static int ext3_read_disk_block(uint32_t block, uint8_t *buf)
{
    uint32_t lba;

    if (!g_dev || !buf || g_block_size == 0)
        return -1;
    lba = block * g_sectors_per_block;
    for (uint32_t i = 0; i < g_sectors_per_block; i++) {
        if (g_dev->read_sector(lba + i,
                               buf + i * BLKDEV_SECTOR_SIZE) != 0)
            return -1;
    }
    return 0;
}

static int ext3_read_block(uint32_t block, uint8_t *buf)
{
    if (!buf)
        return -1;
    for (uint32_t i = 0; i < g_overlay_count; i++) {
        if (g_overlay[i].fs_block == block) {
            k_memcpy(buf, g_overlay[i].data, g_block_size);
            return 0;
        }
    }
    return ext3_read_disk_block(block, buf);
}

static int ext3_overlay_put(uint32_t fs_block, const uint8_t *data)
{
    uint8_t *copy;

    if (!data || fs_block == 0)
        return -1;
    for (uint32_t i = 0; i < g_overlay_count; i++) {
        if (g_overlay[i].fs_block == fs_block) {
            k_memcpy(g_overlay[i].data, data, g_block_size);
            return 0;
        }
    }
    if (g_overlay_count >= EXT3_REPLAY_MAX_BLOCKS) {
        klog("EXT3", "journal replay overlay exhausted");
        return -1;
    }
    copy = (uint8_t *)kmalloc(g_block_size);
    if (!copy) {
        klog("EXT3", "journal replay overlay allocation failed");
        return -1;
    }
    k_memcpy(copy, data, g_block_size);
    g_overlay[g_overlay_count].fs_block = fs_block;
    g_overlay[g_overlay_count].data = copy;
    g_overlay_count++;
    return 0;
}

static int ext3_read_inode(uint32_t ino, ext3_inode_t *out)
{
    uint8_t *blk;
    uint32_t idx;
    uint32_t off;
    uint32_t block;
    uint32_t block_off;

    if (!out || ino == 0 || ino > g_super.inodes_count)
        return -1;
    idx = (ino - 1u) % g_super.inodes_per_group;
    off = idx * g_super.inode_size;
    block = g_bg.inode_table + off / g_block_size;
    block_off = off % g_block_size;

    blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk)
        return -1;
    if (ext3_read_block(block, blk) != 0) {
        kfree(blk);
        return -1;
    }
    k_memcpy(out, blk + block_off, sizeof(*out));
    kfree(blk);
    return 0;
}

static uint32_t ext3_inode_size(const ext3_inode_t *in)
{
    uint32_t size = in->size;

    if ((in->mode & EXT3_S_IFMT) == EXT3_S_IFREG && in->dir_acl != 0) {
        /* Drunix is 32-bit; reject files whose high size word is non-zero. */
        return 0xFFFFFFFFu;
    }
    return size;
}

static uint32_t ext3_block_index(const ext3_inode_t *in, uint32_t logical)
{
    uint8_t *blk = 0;
    uint32_t ptrs = g_block_size / 4u;
    uint32_t result = 0;

    if (logical < EXT3_NDIR_BLOCKS)
        return in->block[logical];
    logical -= EXT3_NDIR_BLOCKS;

    blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk)
        return 0;

    if (logical < ptrs) {
        if (in->block[12] != 0 && ext3_read_block(in->block[12], blk) == 0)
            result = ((uint32_t *)blk)[logical];
        kfree(blk);
        return result;
    }
    logical -= ptrs;

    if (logical < ptrs * ptrs) {
        uint32_t l1 = logical / ptrs;
        uint32_t l2 = logical % ptrs;
        uint32_t l1_block = 0;
        if (in->block[13] == 0 || ext3_read_block(in->block[13], blk) != 0)
            goto done;
        l1_block = ((uint32_t *)blk)[l1];
        if (l1_block == 0 || ext3_read_block(l1_block, blk) != 0)
            goto done;
        result = ((uint32_t *)blk)[l2];
        goto done;
    }
    logical -= ptrs * ptrs;

    if (logical < ptrs * ptrs * ptrs) {
        uint32_t l1 = logical / (ptrs * ptrs);
        uint32_t rem = logical % (ptrs * ptrs);
        uint32_t l2 = rem / ptrs;
        uint32_t l3 = rem % ptrs;
        uint32_t l1_block = 0;
        uint32_t l2_block = 0;
        if (in->block[14] == 0 || ext3_read_block(in->block[14], blk) != 0)
            goto done;
        l1_block = ((uint32_t *)blk)[l1];
        if (l1_block == 0 || ext3_read_block(l1_block, blk) != 0)
            goto done;
        l2_block = ((uint32_t *)blk)[l2];
        if (l2_block == 0 || ext3_read_block(l2_block, blk) != 0)
            goto done;
        result = ((uint32_t *)blk)[l3];
    }

done:
    kfree(blk);
    return result;
}

static int ext3_read(void *ctx, uint32_t inode_num, uint32_t offset,
                     uint8_t *buf, uint32_t count)
{
    ext3_inode_t in;
    uint8_t *blk;
    uint32_t size;
    uint32_t done = 0;

    (void)ctx;
    if (!buf || ext3_read_inode(inode_num, &in) != 0)
        return -1;
    size = ext3_inode_size(&in);
    if (size == 0xFFFFFFFFu)
        return -1;
    if (offset >= size || count == 0)
        return 0;
    if (count > size - offset)
        count = size - offset;

    blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk)
        return -1;
    while (done < count) {
        uint32_t cur = offset + done;
        uint32_t logical = cur / g_block_size;
        uint32_t block_off = cur % g_block_size;
        uint32_t phys = ext3_block_index(&in, logical);
        uint32_t chunk = g_block_size - block_off;

        if (chunk > count - done)
            chunk = count - done;
        if (phys == 0) {
            k_memset(buf + done, 0, chunk);
        } else {
            if (ext3_read_block(phys, blk) != 0) {
                kfree(blk);
                return done ? (int)done : -1;
            }
            k_memcpy(buf + done, blk + block_off, chunk);
        }
        done += chunk;
    }
    kfree(blk);
    return (int)done;
}

static int ext3_dir_lookup(uint32_t dir_ino, const char *name,
                           uint32_t *ino_out, uint8_t *type_out)
{
    ext3_inode_t dir;
    uint8_t *blk;
    uint32_t size;
    uint32_t pos = 0;
    uint32_t want_len;

    if (!name || !ino_out || ext3_read_inode(dir_ino, &dir) != 0)
        return -1;
    if ((dir.mode & EXT3_S_IFMT) != EXT3_S_IFDIR)
        return -1;
    size = ext3_inode_size(&dir);
    want_len = k_strlen(name);
    if (want_len > EXT3_NAME_MAX)
        return -1;

    blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk)
        return -1;

    while (pos < size) {
        uint32_t logical = pos / g_block_size;
        uint32_t phys = ext3_block_index(&dir, logical);
        uint32_t base = logical * g_block_size;
        uint32_t off = pos - base;

        if (phys == 0 || ext3_read_block(phys, blk) != 0)
            break;
        while (off + sizeof(ext3_dirent_t) <= g_block_size &&
               base + off < size) {
            ext3_dirent_t *de = (ext3_dirent_t *)(blk + off);
            if (de->rec_len < sizeof(ext3_dirent_t) ||
                off + de->rec_len > g_block_size)
                break;
            if (de->inode != 0 && de->name_len == want_len &&
                k_memcmp(blk + off + sizeof(ext3_dirent_t),
                         name, want_len) == 0) {
                *ino_out = de->inode;
                if (type_out)
                    *type_out = de->file_type;
                kfree(blk);
                return 0;
            }
            off += de->rec_len;
        }
        pos = base + g_block_size;
    }

    kfree(blk);
    return -1;
}

static int ext3_resolve(const char *path, uint32_t follow_final,
                        uint32_t *ino_out)
{
    uint32_t ino = EXT3_ROOT_INO;
    char part[EXT3_NAME_MAX + 1];
    uint32_t i = 0;

    if (!ino_out)
        return -1;
    if (!path || path[0] == '\0') {
        *ino_out = EXT3_ROOT_INO;
        return 0;
    }

    while (path[i]) {
        uint32_t n = 0;
        while (path[i] == '/')
            i++;
        if (!path[i])
            break;
        while (path[i] && path[i] != '/') {
            if (n >= EXT3_NAME_MAX)
                return -1;
            part[n++] = path[i++];
        }
        part[n] = '\0';
        if (ext3_dir_lookup(ino, part, &ino, 0) != 0)
            return -1;
    }

    (void)follow_final;
    *ino_out = ino;
    return 0;
}

static void ext3_stat_from_inode(const ext3_inode_t *in, vfs_stat_t *st)
{
    uint16_t kind = in->mode & EXT3_S_IFMT;

    st->type = kind == EXT3_S_IFDIR ? 2u :
               kind == EXT3_S_IFLNK ? 3u : 1u;
    st->size = ext3_inode_size(in);
    st->link_count = in->links_count;
    st->mtime = in->mtime;
}

static int ext3_stat_common(const char *path, vfs_stat_t *st, uint32_t follow)
{
    uint32_t ino;
    ext3_inode_t in;

    (void)follow;
    if (!st || ext3_resolve(path, follow, &ino) != 0)
        return -1;
    if (ext3_read_inode(ino, &in) != 0)
        return -1;
    ext3_stat_from_inode(&in, st);
    return 0;
}

static int ext3_stat(void *ctx, const char *path, vfs_stat_t *st)
{
    (void)ctx;
    return ext3_stat_common(path, st, 1);
}

static int ext3_lstat(void *ctx, const char *path, vfs_stat_t *st)
{
    (void)ctx;
    return ext3_stat_common(path, st, 0);
}

static int ext3_open(void *ctx, const char *path, uint32_t *inode_out,
                     uint32_t *size_out)
{
    uint32_t ino;
    ext3_inode_t in;

    (void)ctx;
    if (!inode_out || !size_out || ext3_resolve(path, 1, &ino) != 0)
        return -1;
    if (ext3_read_inode(ino, &in) != 0)
        return -1;
    if ((in.mode & EXT3_S_IFMT) != EXT3_S_IFREG)
        return -1;
    *inode_out = ino;
    *size_out = ext3_inode_size(&in);
    return 0;
}

static int ext3_getdents(void *ctx, const char *path, char *buf,
                         uint32_t bufsz)
{
    uint32_t dir_ino;
    ext3_inode_t dir;
    uint8_t *blk;
    uint32_t size;
    uint32_t pos = 0;
    uint32_t written = 0;

    (void)ctx;
    if (!buf || ext3_resolve(path, 1, &dir_ino) != 0 ||
        ext3_read_inode(dir_ino, &dir) != 0 ||
        (dir.mode & EXT3_S_IFMT) != EXT3_S_IFDIR)
        return -1;
    size = ext3_inode_size(&dir);

    blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk)
        return -1;
    while (pos < size) {
        uint32_t logical = pos / g_block_size;
        uint32_t phys = ext3_block_index(&dir, logical);
        uint32_t base = logical * g_block_size;
        uint32_t off = pos - base;

        if (phys == 0 || ext3_read_block(phys, blk) != 0)
            break;
        while (off + sizeof(ext3_dirent_t) <= g_block_size &&
               base + off < size) {
            ext3_dirent_t *de = (ext3_dirent_t *)(blk + off);
            const char *name = (const char *)(blk + off + sizeof(ext3_dirent_t));
            uint32_t len;
            if (de->rec_len < sizeof(ext3_dirent_t) ||
                off + de->rec_len > g_block_size)
                break;
            if (de->inode != 0 && de->name_len > 0 &&
                !(de->name_len == 1 && name[0] == '.') &&
                !(de->name_len == 2 && name[0] == '.' && name[1] == '.')) {
                len = de->name_len;
                if (written + len + (de->file_type == EXT3_FT_DIR ? 2u : 1u) >
                    bufsz)
                    goto done;
                k_memcpy(buf + written, name, len);
                written += len;
                if (de->file_type == EXT3_FT_DIR)
                    buf[written++] = '/';
                buf[written++] = '\0';
            }
            off += de->rec_len;
        }
        pos = base + g_block_size;
    }
done:
    kfree(blk);
    return (int)written;
}

static int ext3_readlink(void *ctx, const char *path, char *buf,
                         uint32_t bufsz)
{
    uint32_t ino;
    ext3_inode_t in;
    uint32_t size;

    (void)ctx;
    if (!buf || bufsz == 0 || ext3_resolve(path, 0, &ino) != 0 ||
        ext3_read_inode(ino, &in) != 0 ||
        (in.mode & EXT3_S_IFMT) != EXT3_S_IFLNK)
        return -1;
    size = ext3_inode_size(&in);
    if (size > bufsz)
        size = bufsz;
    if (size <= 60u) {
        k_memcpy(buf, in.block, size);
        return (int)size;
    }
    return ext3_read(ctx, ino, 0, (uint8_t *)buf, size);
}

static int ext3_readonly_path(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return -30;
}

static int ext3_readonly_rename(void *ctx, const char *oldpath,
                                const char *newpath)
{
    (void)ctx;
    (void)oldpath;
    (void)newpath;
    return -30;
}

static int ext3_readonly_link(void *ctx, const char *oldpath,
                              const char *newpath, uint32_t follow)
{
    (void)ctx;
    (void)oldpath;
    (void)newpath;
    (void)follow;
    return -30;
}

static int ext3_readonly_symlink(void *ctx, const char *target,
                                 const char *linkpath)
{
    (void)ctx;
    (void)target;
    (void)linkpath;
    return -30;
}

static int ext3_readonly_write(void *ctx, uint32_t inode_num, uint32_t offset,
                               const uint8_t *buf, uint32_t count)
{
    (void)ctx;
    (void)inode_num;
    (void)offset;
    (void)buf;
    (void)count;
    return -30;
}

static int ext3_readonly_truncate(void *ctx, uint32_t inode_num,
                                  uint32_t size)
{
    (void)ctx;
    (void)inode_num;
    (void)size;
    return -30;
}

typedef struct {
    uint32_t fs_block;
    uint32_t journal_block;
    uint32_t flags;
} jbd_tag_t;

static uint32_t jbd_next(uint32_t cur, uint32_t first, uint32_t maxlen)
{
    cur++;
    if (cur >= maxlen)
        cur = first;
    return cur;
}

static int ext3_journal_read_block(const ext3_inode_t *journal,
                                   uint32_t logical, uint8_t *buf)
{
    uint32_t phys = ext3_block_index(journal, logical);

    if (phys == 0)
        return -1;
    return ext3_read_disk_block(phys, buf);
}

static int jbd_revoked(uint32_t block, const uint32_t *revokes,
                       uint32_t revoke_count)
{
    for (uint32_t i = 0; i < revoke_count; i++) {
        if (revokes[i] == block)
            return 1;
    }
    return 0;
}

static int jbd_apply_transaction(const ext3_inode_t *journal,
                                 const jbd_tag_t *tags,
                                 uint32_t tag_count,
                                 const uint32_t *revokes,
                                 uint32_t revoke_count)
{
    uint8_t *data = (uint8_t *)kmalloc(g_block_size);

    if (!data)
        return -1;
    for (uint32_t i = 0; i < tag_count; i++) {
        if (jbd_revoked(tags[i].fs_block, revokes, revoke_count))
            continue;
        if (ext3_journal_read_block(journal, tags[i].journal_block, data) != 0) {
            kfree(data);
            return -1;
        }
        if (tags[i].flags & JBD_FLAG_ESCAPE) {
            data[0] = 0xC0;
            data[1] = 0x3B;
            data[2] = 0x39;
            data[3] = 0x98;
        }
        if (ext3_overlay_put(tags[i].fs_block, data) != 0) {
            kfree(data);
            return -1;
        }
    }
    kfree(data);
    return 0;
}

static int jbd_parse_descriptor(const uint8_t *blk, uint32_t pos,
                                uint32_t first, uint32_t maxlen,
                                jbd_tag_t *tags, uint32_t *tag_count_out,
                                uint32_t *after_data_out)
{
    uint32_t off = 12u;
    uint32_t data_pos = jbd_next(pos, first, maxlen);
    uint32_t count = 0;

    while (off + 8u <= g_block_size) {
        uint32_t fs_block = be32(blk + off);
        uint32_t flags = be32(blk + off + 4u);
        uint32_t tag_size = (flags & JBD_FLAG_SAME_UUID) ? 8u : 24u;

        if (fs_block == 0 || count >= EXT3_REPLAY_MAX_TAGS ||
            off + tag_size > g_block_size) {
            klog("EXT3", "corrupt journal descriptor");
            return -1;
        }
        tags[count].fs_block = fs_block;
        tags[count].journal_block = data_pos;
        tags[count].flags = flags;
        count++;
        data_pos = jbd_next(data_pos, first, maxlen);
        off += tag_size;
        if (flags & JBD_FLAG_LAST_TAG)
            break;
    }

    *tag_count_out = count;
    *after_data_out = data_pos;
    return 0;
}

static int jbd_parse_revoke(const uint8_t *blk, uint32_t seq,
                            uint32_t cur_seq, uint32_t *revokes,
                            uint32_t *revoke_count)
{
    uint32_t bytes = be32(blk + 12u);
    uint32_t off = 16u;

    if (seq != cur_seq)
        return 0;
    if (bytes > g_block_size)
        return -1;
    while (off + 4u <= bytes) {
        if (*revoke_count >= EXT3_REPLAY_MAX_REVOKES) {
            klog("EXT3", "journal revoke table exhausted");
            return -1;
        }
        revokes[*revoke_count] = be32(blk + off);
        (*revoke_count)++;
        off += 4u;
    }
    return 0;
}

static int ext3_replay_journal(void)
{
    ext3_inode_t journal;
    uint8_t *blk;
    jbd_tag_t *tags;
    uint32_t *revokes;
    uint32_t tag_count = 0;
    uint32_t revoke_count = 0;
    uint32_t cur_seq = 0;
    uint32_t active = 0;
    uint32_t blocksize;
    uint32_t maxlen;
    uint32_t first;
    uint32_t start;
    uint32_t pos;
    uint32_t scanned = 0;
    uint32_t incompat;

    if ((g_super.feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) == 0)
        return 0;
    if ((g_super.feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) == 0)
        return 0;
    if (g_super.journal_dev != 0 || g_super.journal_inum == 0) {
        klog("EXT3", "external or missing journal unsupported");
        return -1;
    }
    if (ext3_read_inode(g_super.journal_inum, &journal) != 0 ||
        (journal.mode & EXT3_S_IFMT) != EXT3_S_IFREG) {
        klog("EXT3", "journal inode invalid");
        return -1;
    }

    tags = (jbd_tag_t *)kmalloc(sizeof(jbd_tag_t) * EXT3_REPLAY_MAX_TAGS);
    revokes = (uint32_t *)kmalloc(sizeof(uint32_t) * EXT3_REPLAY_MAX_REVOKES);
    blk = (uint8_t *)kmalloc(g_block_size);
    if (!tags || !revokes || !blk) {
        if (tags) kfree(tags);
        if (revokes) kfree(revokes);
        if (blk) kfree(blk);
        return -1;
    }
    if (ext3_journal_read_block(&journal, 0, blk) != 0) {
        kfree(tags);
        kfree(revokes);
        kfree(blk);
        return -1;
    }
    if (be32(blk) != JBD_MAGIC ||
        (be32(blk + 4u) != JBD_SUPERBLOCK_V1 &&
         be32(blk + 4u) != JBD_SUPERBLOCK_V2)) {
        klog("EXT3", "journal superblock invalid");
        kfree(tags);
        kfree(revokes);
        kfree(blk);
        return -1;
    }

    blocksize = be32(blk + 12u);
    maxlen = be32(blk + 16u);
    first = be32(blk + 20u);
    start = be32(blk + 28u);
    incompat = be32(blk + 40u);
    if (blocksize != g_block_size || first == 0 || first >= maxlen ||
        maxlen == 0 || start >= maxlen) {
        klog("EXT3", "journal layout unsupported");
        kfree(tags);
        kfree(revokes);
        kfree(blk);
        return -1;
    }
    if ((incompat & ~JBD_FEATURE_INCOMPAT_REVOKE) != 0 ||
        be32(blk + 36u) != 0 || be32(blk + 44u) != 0) {
        klog("EXT3", "journal features unsupported");
        kfree(tags);
        kfree(revokes);
        kfree(blk);
        return -1;
    }
    if (start == 0) {
        kfree(tags);
        kfree(revokes);
        kfree(blk);
        return 0;
    }

    pos = start;
    while (scanned < maxlen - first) {
        uint32_t magic;
        uint32_t type;
        uint32_t seq;

        if (ext3_journal_read_block(&journal, pos, blk) != 0) {
            kfree(tags);
            kfree(revokes);
            kfree(blk);
            return -1;
        }
        magic = be32(blk);
        type = be32(blk + 4u);
        seq = be32(blk + 8u);
        if (magic != JBD_MAGIC)
            break;

        if (type == JBD_DESCRIPTOR_BLOCK) {
            uint32_t after_data;
            if (jbd_parse_descriptor(blk, pos, first, maxlen, tags,
                                     &tag_count, &after_data) != 0) {
                kfree(blk);
                kfree(tags);
                kfree(revokes);
                return -1;
            }
            revoke_count = 0;
            cur_seq = seq;
            active = 1;
            while (pos != after_data) {
                pos = jbd_next(pos, first, maxlen);
                scanned++;
            }
            continue;
        }

        if (type == JBD_REVOKE_BLOCK) {
            if (active &&
                jbd_parse_revoke(blk, seq, cur_seq, revokes,
                                 &revoke_count) != 0) {
                kfree(blk);
                kfree(tags);
                kfree(revokes);
                return -1;
            }
        } else if (type == JBD_COMMIT_BLOCK) {
            if (active && seq == cur_seq) {
                if (jbd_apply_transaction(&journal, tags, tag_count,
                                          revokes, revoke_count) != 0) {
                    kfree(blk);
                    kfree(tags);
                    kfree(revokes);
                    return -1;
                }
                active = 0;
                tag_count = 0;
                revoke_count = 0;
            }
        } else if (type != JBD_SUPERBLOCK_V1 && type != JBD_SUPERBLOCK_V2) {
            break;
        }

        pos = jbd_next(pos, first, maxlen);
        scanned++;
    }

    klog_uint("EXT3", "journal replay blocks", g_overlay_count);
    kfree(tags);
    kfree(revokes);
    kfree(blk);
    return 0;
}

static int ext3_init(void *ctx)
{
    uint8_t *blk;
    uint32_t bgdt_block;
    uint32_t allowed_incompat;
    uint32_t allowed_ro;

    (void)ctx;
    g_dev = blkdev_get("hd0");
    if (!g_dev)
        return -1;
    if (ext3_read_bytes(EXT3_SUPER_OFFSET, (uint8_t *)&g_super,
                        sizeof(g_super)) != 0)
        return -1;
    if (g_super.magic != EXT3_SUPER_MAGIC) {
        klog_hex("EXT3", "bad magic", g_super.magic);
        return -2;
    }

    g_block_size = 1024u << g_super.log_block_size;
    if (g_block_size != 1024u && g_block_size != 2048u &&
        g_block_size != 4096u) {
        klog_uint("EXT3", "unsupported block size", g_block_size);
        return -1;
    }
    g_sectors_per_block = g_block_size / BLKDEV_SECTOR_SIZE;
    if (g_super.inode_size < sizeof(ext3_inode_t))
        return -1;

    allowed_incompat = EXT3_FEATURE_INCOMPAT_FILETYPE |
                       EXT3_FEATURE_INCOMPAT_RECOVER;
    if ((g_super.feature_incompat & ~allowed_incompat) != 0) {
        klog_hex("EXT3", "unsupported incompat",
                 g_super.feature_incompat & ~allowed_incompat);
        return -1;
    }
    allowed_ro = EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER |
                 EXT3_FEATURE_RO_COMPAT_LARGE_FILE;
    if ((g_super.feature_ro_compat & ~allowed_ro) != 0) {
        klog_hex("EXT3", "unsupported ro compat",
                 g_super.feature_ro_compat & ~allowed_ro);
        return -1;
    }

    bgdt_block = (g_block_size == 1024u) ? 2u : 1u;
    blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk)
        return -1;
    if (ext3_read_block(bgdt_block, blk) != 0) {
        kfree(blk);
        return -1;
    }
    k_memcpy(&g_bg, blk, sizeof(g_bg));
    kfree(blk);

    if (ext3_replay_journal() != 0)
        return -1;

    klog_uint("EXT3", "block size", g_block_size);
    klog_uint("EXT3", "inode table", g_bg.inode_table);
    return 0;
}

static const fs_ops_t ext3_ops = {
    .init     = ext3_init,
    .open     = ext3_open,
    .getdents = ext3_getdents,
    .create   = ext3_readonly_path,
    .unlink   = ext3_readonly_path,
    .mkdir    = ext3_readonly_path,
    .rmdir    = ext3_readonly_path,
    .rename   = ext3_readonly_rename,
    .link     = ext3_readonly_link,
    .symlink  = ext3_readonly_symlink,
    .readlink = ext3_readlink,
    .stat     = ext3_stat,
    .lstat    = ext3_lstat,
    .read     = ext3_read,
    .write    = ext3_readonly_write,
    .truncate = ext3_readonly_truncate,
    .flush    = 0,
};

void ext3_register(void)
{
    vfs_register("ext3", &ext3_ops);
}
