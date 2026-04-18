/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * fs.c — DUFS filesystem implementation for on-disk inode and directory operations.
 */

#include "fs.h"
#include "vfs.h"
#include "blkdev.h"
#include "bcache.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include "clock.h"
#include <stdint.h>

/* ── Mount state ────────────────────────────────────────────────────────── */

typedef struct {
    const char         *blkdev_name;
    const blkdev_ops_t *blkdev;
    dufs_super_t        super;
    uint8_t             inode_bitmap[4096];  /* 32,768 inode bits */
    uint8_t             block_bitmap[4096];  /* 32,768 block bits */
} dufs_mount_t;

static dufs_mount_t g_dufs_hd0 = { .blkdev_name = "hd0" };
static dufs_mount_t g_dufs_hd1 = { .blkdev_name = "hd1" };
static dufs_mount_t *g_dufs = &g_dufs_hd0;

#define g_blkdev       (g_dufs->blkdev)
#define g_super        (g_dufs->super)
#define g_inode_bitmap (g_dufs->inode_bitmap)
#define g_block_bitmap (g_dufs->block_bitmap)

static void dufs_select(dufs_mount_t *mnt)
{
    if (mnt)
        g_dufs = mnt;
}

int dufs_use_device(const char *blkdev_name)
{
    if (!blkdev_name)
        return -1;
    if (k_strcmp(blkdev_name, "hd0") == 0) {
        dufs_select(&g_dufs_hd0);
        return 0;
    }
    if (k_strcmp(blkdev_name, "hd1") == 0) {
        dufs_select(&g_dufs_hd1);
        return 0;
    }
    return -1;
}

/* ── Generic bitmap helpers ─────────────────────────────────────────────── */

static int bmap_test(const uint8_t *bm, uint32_t bit)
{
    return (bm[bit >> 3] >> (bit & 7)) & 1;
}

static void bmap_set(uint8_t *bm, uint32_t bit)
{
    bm[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void bmap_clr(uint8_t *bm, uint32_t bit)
{
    bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}


/* ── Block I/O helpers ──────────────────────────────────────────────────── */

/*
 * read_block / write_block: read or write one 4096-byte data block,
 * which spans 8 consecutive 512-byte ATA sectors starting at `lba`.
 * Reads are routed through the shared buffer cache; inode-table and
 * superblock sectors are accessed directly and never appear as cached
 * blocks. DUFS has no journal, so writes go through write-through:
 * metadata durability matches the pre-cache behaviour, and any cached
 * copy is kept coherent with what hits the platter.
 */
static int read_block(uint32_t lba, uint8_t *buf)
{
    return bcache_read(g_blkdev, lba, buf);
}

static int write_block(uint32_t lba, const uint8_t *buf)
{
    return bcache_write_through(g_blkdev, lba, buf);
}

/* Write a zeroed 4096-byte block — used when allocating new directory or
 * indirect blocks so all entries start as zero (meaning "empty"). */
static int write_zeroed_block(uint32_t lba)
{
    uint8_t *z = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
    if (!z) return -1;
    for (uint32_t i = 0; i < DUFS_BLOCK_SIZE; i++) z[i] = 0;
    int rc = write_block(lba, z);
    kfree(z);
    return rc;
}

/* ── Bitmap flush helpers ────────────────────────────────────────────────── */

static int flush_inode_bitmap(void)
{
    return write_block(g_super.inode_bitmap_lba, g_inode_bitmap);
}

static int flush_block_bitmap(void)
{
    return write_block(g_super.block_bitmap_lba, g_block_bitmap);
}

/* ── Inode table I/O ────────────────────────────────────────────────────── */

/*
 * Inode I/O still uses single 512-byte sector reads: 4 inodes per sector.
 * Sector = inode_table_lba + num/4; slot = num % 4.
 */
static int inode_read(uint32_t num, dufs_inode_t *out)
{
    uint8_t buf[512];
    uint32_t sector = g_super.inode_table_lba + num / DUFS_INODES_PER_SECTOR;
    uint32_t slot   = num % DUFS_INODES_PER_SECTOR;
    if (g_blkdev->read_sector(sector, buf) != 0) return -1;
    *out = ((dufs_inode_t *)buf)[slot];
    return 0;
}

static int inode_write(uint32_t num, const dufs_inode_t *in)
{
    uint8_t buf[512];
    uint32_t sector = g_super.inode_table_lba + num / DUFS_INODES_PER_SECTOR;
    uint32_t slot   = num % DUFS_INODES_PER_SECTOR;
    if (g_blkdev->read_sector(sector, buf) != 0) return -1;
    ((dufs_inode_t *)buf)[slot] = *in;
    return g_blkdev->write_sector(sector, buf);
}

/* ── Inode allocation ───────────────────────────────────────────────────── */

static uint32_t inode_alloc(void)
{
    for (uint32_t i = 2; i < DUFS_INODE_COUNT; i++) {
        if (!bmap_test(g_inode_bitmap, i)) {
            bmap_set(g_inode_bitmap, i);
            return i;
        }
    }
    return 0;
}

static void inode_free(uint32_t num)
{
    if (num >= 2 && num < DUFS_INODE_COUNT)
        bmap_clr(g_inode_bitmap, num);
}

/* ── Data block allocation ──────────────────────────────────────────────── */

/*
 * Allocate the first free data block and return its LBA (the first of 8
 * consecutive sectors).  Returns 0 on failure (LBA 0 is always unused).
 *
 * Block bitmap bit i corresponds to the block starting at
 * data_lba + i * DUFS_SECS_PER_BLOCK.
 */
static uint32_t block_alloc(void)
{
    uint32_t max_bits = (g_super.total_sectors - g_super.data_lba) / DUFS_SECS_PER_BLOCK;
    if (max_bits > 4096u * 8u) max_bits = 4096u * 8u;
    for (uint32_t i = 0; i < max_bits; i++) {
        if (!bmap_test(g_block_bitmap, i)) {
            bmap_set(g_block_bitmap, i);
            return g_super.data_lba + i * DUFS_SECS_PER_BLOCK;
        }
    }
    return 0;
}

static void block_free(uint32_t lba)
{
    if (lba < g_super.data_lba) return;
    uint32_t bit = (lba - g_super.data_lba) / DUFS_SECS_PER_BLOCK;
    if (bit < 4096u * 8u)
        bmap_clr(g_block_bitmap, bit);
}

/* ── Block-pointer resolution ───────────────────────────────────────────── */

/*
 * Return the LBA for logical block block_idx within inode.
 * Returns 0 if the block has not been allocated yet.
 * Handles direct (0–11), single-indirect (12–1035), and
 * double-indirect (1036–1049611) tiers.
 */
static uint32_t block_map(const dufs_inode_t *inode, uint32_t block_idx)
{
    /* Direct — no heap needed. */
    if (block_idx < DUFS_DIRECT_BLOCKS)
        return inode->direct[block_idx];

    block_idx -= DUFS_DIRECT_BLOCKS;

    /* Indirect tiers share one heap buffer; the L1 LBA is saved into a local
     * so the buffer can be reused for the second read in the double-indirect
     * path without needing a second simultaneous allocation. */
    uint8_t *buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
    if (!buf) return 0;
    uint32_t result = 0;

    /* Single-indirect. */
    if (block_idx < DUFS_INDIR_ENTRIES) {
        if (inode->indirect == 0) goto done;
        if (read_block(inode->indirect, buf) != 0) goto done;
        result = ((uint32_t *)buf)[block_idx];
        goto done;
    }
    block_idx -= DUFS_INDIR_ENTRIES;

    /* Double-indirect. */
    if (block_idx < (uint32_t)DUFS_INDIR_ENTRIES * DUFS_INDIR_ENTRIES) {
        if (inode->double_indirect == 0) goto done;
        if (read_block(inode->double_indirect, buf) != 0) goto done;
        uint32_t l1_idx = block_idx / DUFS_INDIR_ENTRIES;
        uint32_t l2_idx = block_idx % DUFS_INDIR_ENTRIES;
        uint32_t l1_lba = ((uint32_t *)buf)[l1_idx]; /* save before reuse */
        if (l1_lba == 0) goto done;
        if (read_block(l1_lba, buf) != 0) goto done;
        result = ((uint32_t *)buf)[l2_idx];
    }

done:
    kfree(buf);
    return result;
}

/*
 * Ensure logical block block_idx in inode is allocated.  If it is not,
 * allocate a new block (and any needed indirect table blocks) and update
 * the inode in memory.  The caller must call inode_write() after this.
 *
 * Returns the LBA of the block on success, or 0 on failure.
 */
static uint32_t block_ensure(dufs_inode_t *inode, uint32_t block_idx)
{
    /* Direct. */
    if (block_idx < DUFS_DIRECT_BLOCKS) {
        if (inode->direct[block_idx] == 0) {
            uint32_t lba = block_alloc();
            if (lba == 0) return 0;
            if (write_zeroed_block(lba) != 0) { block_free(lba); return 0; }
            inode->direct[block_idx] = lba;
            inode->block_count++;
        }
        return inode->direct[block_idx];
    }
    block_idx -= DUFS_DIRECT_BLOCKS;

    /* Single-indirect. */
    if (block_idx < DUFS_INDIR_ENTRIES) {
        if (inode->indirect == 0) {
            uint32_t indir_lba = block_alloc();
            if (indir_lba == 0) return 0;
            if (write_zeroed_block(indir_lba) != 0) { block_free(indir_lba); return 0; }
            inode->indirect = indir_lba;
            inode->block_count++;
        }

        uint8_t *buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
        if (!buf) return 0;
        if (read_block(inode->indirect, buf) != 0) { kfree(buf); return 0; }
        uint32_t *entries = (uint32_t *)buf;

        if (entries[block_idx] == 0) {
            uint32_t lba = block_alloc();
            if (lba == 0) { kfree(buf); return 0; }
            if (write_zeroed_block(lba) != 0) { block_free(lba); kfree(buf); return 0; }
            entries[block_idx] = lba;
            inode->block_count++;
            if (write_block(inode->indirect, buf) != 0) { kfree(buf); return 0; }
        }
        uint32_t result = entries[block_idx];
        kfree(buf);
        return result;
    }
    block_idx -= DUFS_INDIR_ENTRIES;

    /* Double-indirect.  One buffer is reused for both the L0 and L1 table
     * reads: the L1 LBA is extracted and saved into a local variable before
     * the buffer is overwritten with the L1 table contents. */
    if (block_idx < (uint32_t)DUFS_INDIR_ENTRIES * DUFS_INDIR_ENTRIES) {
        /* Allocate L0 (double-indirect table) if needed. */
        if (inode->double_indirect == 0) {
            uint32_t l0_lba = block_alloc();
            if (l0_lba == 0) return 0;
            if (write_zeroed_block(l0_lba) != 0) { block_free(l0_lba); return 0; }
            inode->double_indirect = l0_lba;
            inode->block_count++;
        }

        uint32_t l1_idx = block_idx / DUFS_INDIR_ENTRIES;
        uint32_t l2_idx = block_idx % DUFS_INDIR_ENTRIES;

        uint8_t *buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
        if (!buf) return 0;

        if (read_block(inode->double_indirect, buf) != 0) { kfree(buf); return 0; }
        uint32_t *l0 = (uint32_t *)buf;

        /* Allocate L1 (single-indirect table) if needed. */
        if (l0[l1_idx] == 0) {
            uint32_t l1_lba = block_alloc();
            if (l1_lba == 0) { kfree(buf); return 0; }
            if (write_zeroed_block(l1_lba) != 0) { block_free(l1_lba); kfree(buf); return 0; }
            l0[l1_idx] = l1_lba;
            inode->block_count++;
            if (write_block(inode->double_indirect, buf) != 0) { kfree(buf); return 0; }
        }
        uint32_t l1_lba = l0[l1_idx]; /* save before buf is reused */

        /* Reuse buf for the L1 table. */
        if (read_block(l1_lba, buf) != 0) { kfree(buf); return 0; }
        uint32_t *l1 = (uint32_t *)buf;

        if (l1[l2_idx] == 0) {
            uint32_t lba = block_alloc();
            if (lba == 0) { kfree(buf); return 0; }
            if (write_zeroed_block(lba) != 0) { block_free(lba); kfree(buf); return 0; }
            l1[l2_idx] = lba;
            inode->block_count++;
            if (write_block(l1_lba, buf) != 0) { kfree(buf); return 0; }
        }
        uint32_t result = l1[l2_idx];
        kfree(buf);
        return result;
    }

    return 0; /* beyond double-indirect */
}

/* ── Directory operations ───────────────────────────────────────────────── */

/*
 * Scan all data blocks of the directory at dir_ino for an entry matching
 * name.  Returns the child inode number on success, or 0 if not found.
 */
static uint32_t dir_lookup(uint32_t dir_ino, const char *name)
{
    dufs_inode_t dir;
    if (inode_read(dir_ino, &dir) != 0) {
        klog("LOOKUP", "inode_read failed");
        return 0;
    }

    uint8_t *buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
    if (!buf) return 0;
    uint32_t found = 0;

    for (uint32_t blk = 0; blk < dir.block_count; blk++) {
        uint32_t lba = block_map(&dir, blk);
        if (lba == 0) continue;

        if (read_block(lba, buf) != 0) continue;

        dufs_dirent_t *entries = (dufs_dirent_t *)buf;
        for (uint32_t e = 0; e < DUFS_DIRENTS_PER_BLOCK; e++) {
            if (entries[e].inode == 0) continue;
            if (k_strncmp(entries[e].name, name, DUFS_MAX_NAME) == 0) {
                found = entries[e].inode;
                goto done;
            }
        }
    }
done:
    kfree(buf);
    return found;
}

/*
 * Add (name, child_ino) to the directory at dir_ino.  Finds an empty slot
 * in an existing block, or allocates a new block if all current slots are
 * occupied.
 * Returns 0 on success, -1 on error.
 */
static int dir_add(uint32_t dir_ino, const char *name, uint32_t child_ino)
{
    dufs_inode_t dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;

    uint8_t *buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
    if (!buf) return -1;

    /* Search existing blocks for an empty slot. */
    for (uint32_t blk = 0; blk < dir.block_count; blk++) {
        uint32_t lba = block_map(&dir, blk);
        if (lba == 0) continue;

        if (read_block(lba, buf) != 0) continue;
        dufs_dirent_t *entries = (dufs_dirent_t *)buf;
        for (uint32_t e = 0; e < DUFS_DIRENTS_PER_BLOCK; e++) {
            if (entries[e].inode != 0) continue;
            /* Empty slot found: fill it. */
            uint32_t i = 0;
            while (i < DUFS_MAX_NAME - 1 && name[i]) {
                entries[e].name[i] = name[i];
                i++;
            }
            while (i < DUFS_MAX_NAME) entries[e].name[i++] = '\0';
            entries[e].inode = child_ino;
            dir.mtime = clock_unix_time();
            if (inode_write(dir_ino, &dir) != 0) {
                kfree(buf);
                return -1;
            }
            int rc = write_block(lba, buf);
            kfree(buf);
            return rc;
        }
    }

    /* No empty slot: allocate a new directory block, reusing the buffer. */
    uint32_t new_lba = block_ensure(&dir, dir.block_count);
    if (new_lba == 0) { kfree(buf); return -1; }

    uint32_t i;
    for (i = 0; i < DUFS_BLOCK_SIZE; i++) buf[i] = 0;

    dufs_dirent_t *entries = (dufs_dirent_t *)buf;
    i = 0;
    while (i < DUFS_MAX_NAME - 1 && name[i]) {
        entries[0].name[i] = name[i];
        i++;
    }
    while (i < DUFS_MAX_NAME) entries[0].name[i++] = '\0';
    entries[0].inode = child_ino;

    dir.mtime = clock_unix_time();
    if (write_block(new_lba, buf) != 0) { kfree(buf); return -1; }
    kfree(buf);

    /* Persist the updated directory inode (new block_count / new pointer). */
    return inode_write(dir_ino, &dir);
}

/*
 * Remove the entry named name from the directory at dir_ino.
 * Returns the child inode number of the removed entry, or 0 if not found.
 */
static uint32_t dir_remove(uint32_t dir_ino, const char *name)
{
    dufs_inode_t dir;
    if (inode_read(dir_ino, &dir) != 0) return 0;

    uint8_t *buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
    if (!buf) return 0;
    uint32_t found = 0;

    for (uint32_t blk = 0; blk < dir.block_count; blk++) {
        uint32_t lba = block_map(&dir, blk);
        if (lba == 0) continue;

        if (read_block(lba, buf) != 0) continue;
        dufs_dirent_t *entries = (dufs_dirent_t *)buf;
        for (uint32_t e = 0; e < DUFS_DIRENTS_PER_BLOCK; e++) {
            if (entries[e].inode == 0) continue;
            if (k_strncmp(entries[e].name, name, DUFS_MAX_NAME) != 0) continue;

            found              = entries[e].inode;
            entries[e].inode   = 0;
            entries[e].name[0] = '\0';
            dir.mtime = clock_unix_time();
            if (inode_write(dir_ino, &dir) != 0) {
                kfree(buf);
                return 0;
            }
            write_block(lba, buf);
            goto done;
        }
    }
done:
    kfree(buf);
    return found;
}

/* Count non-empty entries in a directory (used by fs_rmdir to check emptiness). */
static int dir_count_entries(uint32_t dir_ino)
{
    dufs_inode_t dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;

    uint8_t *buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
    if (!buf) return -1;

    int count = 0;
    for (uint32_t blk = 0; blk < dir.block_count; blk++) {
        uint32_t lba = block_map(&dir, blk);
        if (lba == 0) continue;

        if (read_block(lba, buf) != 0) continue;
        dufs_dirent_t *entries = (dufs_dirent_t *)buf;
        for (uint32_t e = 0; e < DUFS_DIRENTS_PER_BLOCK; e++) {
            if (entries[e].inode != 0) count++;
        }
    }
    kfree(buf);
    return count;
}

/* ── Path resolution ────────────────────────────────────────────────────── */

static int read_symlink_target(uint32_t ino, char *out, uint32_t outsz);
static int symlink_target_path_with_tail(const char *link_path,
                                         const char *target,
                                         const char *tail,
                                         char *out, uint32_t outsz);

static int path_resolve_follow(const char *path, uint32_t *dir_ino_out,
                               char *leaf_out, uint32_t depth)
{
    if (!path || path[0] == '\0') return -1;
    if (depth > 8) return -40;

    /* Skip any leading slash so "/a/b" is treated the same as "a/b". */
    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') return -1; /* bare "/" has no leaf */

    const char *path_start = p;
    uint32_t cur_dir = 1; /* start at root */

    for (;;) {
        /* Find the next slash in the remaining path. */
        const char *slash = 0;
        for (const char *q = p; *q; q++)
            if (*q == '/') { slash = q; break; }

        if (!slash) {
            /* p is the final leaf component — no more directories to walk. */
            uint32_t i = 0;
            while (i < DUFS_MAX_NAME - 1 && p[i]) {
                leaf_out[i] = p[i];
                i++;
            }
            if (i == 0) return -1; /* empty leaf */
            leaf_out[i] = '\0';
            *dir_ino_out = cur_dir;
            return 0;
        }

        /* Extract the next intermediate component. */
        uint32_t comp_len = (uint32_t)(slash - p);
        if (comp_len == 0 || comp_len >= DUFS_MAX_NAME) return -1;

        char comp[DUFS_MAX_NAME];
        for (uint32_t i = 0; i < comp_len; i++) comp[i] = p[i];
        comp[comp_len] = '\0';

        /* Look it up and verify it is a directory. */
        uint32_t ino = dir_lookup(cur_dir, comp);
        if (ino == 0) return -1;

        dufs_inode_t dinode;
        if (inode_read(ino, &dinode) != 0) return -1;
        if (dinode.type == DUFS_TYPE_SYMLINK) {
            char *link_path = (char *)kmalloc(4096);
            char *target = (char *)kmalloc(4096);
            char *next = (char *)kmalloc(4096);
            int rc;

            if (!link_path || !target || !next) {
                if (link_path) kfree(link_path);
                if (target) kfree(target);
                if (next) kfree(next);
                return -1;
            }

            uint32_t link_len = (uint32_t)(slash - path_start);
            if (link_len == 0 || link_len + 1u > 4096u) {
                kfree(link_path);
                kfree(target);
                kfree(next);
                return -1;
            }
            k_memcpy(link_path, path_start, link_len);
            link_path[link_len] = '\0';

            if (read_symlink_target(ino, target, 4096) < 0 ||
                symlink_target_path_with_tail(link_path, target,
                                              slash + 1, next, 4096) != 0) {
                kfree(link_path);
                kfree(target);
                kfree(next);
                return -1;
            }
            rc = path_resolve_follow(next, dir_ino_out, leaf_out, depth + 1u);
            kfree(link_path);
            kfree(target);
            kfree(next);
            return rc;
        }
        if (dinode.type != DUFS_TYPE_DIR) return -1;

        cur_dir = ino;
        p = slash + 1;
        if (*p == '\0') return -1; /* trailing slash — no leaf */
    }
}

/*
 * Walk an arbitrary-depth path and return the containing directory inode
 * number and the final component (leaf name). Intermediate symlinks are
 * followed with a Linux-style depth cap; the final component is left as-is
 * for callers such as lstat(), readlink(), and linkat() without follow.
 */
static int path_resolve(const char *path, uint32_t *dir_ino_out, char *leaf_out)
{
    return path_resolve_follow(path, dir_ino_out, leaf_out, 0);
}

static int symlink_target_path(const char *link_path, const char *target,
                               char *out, uint32_t outsz)
{
    if (!link_path || !target || !out || outsz == 0 || target[0] == '\0')
        return -1;

    if (target[0] == '/') {
        uint32_t len = k_strlen(target);
        if (len + 1u > outsz)
            return -1;
        k_memcpy(out, target, len + 1u);
        return 0;
    }

    const char *slash = k_strrchr(link_path, '/');
    if (!slash) {
        uint32_t len = k_strlen(target);
        if (len + 1u > outsz)
            return -1;
        k_memcpy(out, target, len + 1u);
        return 0;
    }

    uint32_t parent_len = (uint32_t)(slash - link_path);
    uint32_t target_len = k_strlen(target);
    if (parent_len + 1u + target_len + 1u > outsz)
        return -1;

    k_memcpy(out, link_path, parent_len);
    out[parent_len] = '/';
    k_memcpy(out + parent_len + 1u, target, target_len + 1u);
    return 0;
}

static int symlink_target_path_with_tail(const char *link_path,
                                         const char *target,
                                         const char *tail,
                                         char *out, uint32_t outsz)
{
    if (symlink_target_path(link_path, target, out, outsz) != 0)
        return -1;
    if (!tail || tail[0] == '\0')
        return 0;

    uint32_t base_len = k_strlen(out);
    uint32_t tail_len = k_strlen(tail);
    uint32_t need_sep = 1;

    if (base_len == 1 && out[0] == '/')
        need_sep = 0;
    if (base_len + need_sep + tail_len + 1u > outsz)
        return -1;
    if (need_sep)
        out[base_len++] = '/';
    k_memcpy(out + base_len, tail, tail_len + 1u);
    return 0;
}

static int read_symlink_target(uint32_t ino, char *out, uint32_t outsz)
{
    dufs_inode_t inode;
    int n;

    if (!out || outsz == 0)
        return -1;
    if (inode_read(ino, &inode) != 0 || inode.type != DUFS_TYPE_SYMLINK)
        return -1;
    if (inode.size + 1u > outsz)
        return -1;

    n = fs_read(ino, 0, (uint8_t *)out, inode.size);
    if (n != (int)inode.size)
        return -1;
    out[inode.size] = '\0';
    return (int)inode.size;
}

static void stat_from_inode(const dufs_inode_t *inode, vfs_stat_t *st)
{
    st->type       = inode->type;
    st->size       = inode->size;
    st->link_count = inode->link_count;
    st->mtime      = inode->mtime;
}

/* ── Free all blocks belonging to an inode ──────────────────────────────── */

static void free_inode_blocks(dufs_inode_t *inode)
{
    for (uint32_t i = 0; i < DUFS_DIRECT_BLOCKS; i++) {
        if (inode->direct[i] != 0) {
            block_free(inode->direct[i]);
            inode->direct[i] = 0;
        }
    }
    if (inode->indirect != 0) {
        uint8_t *buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
        if (buf && read_block(inode->indirect, buf) == 0) {
            uint32_t *entries = (uint32_t *)buf;
            for (uint32_t i = 0; i < DUFS_INDIR_ENTRIES; i++) {
                if (entries[i] != 0) block_free(entries[i]);
            }
        }
        kfree(buf);
        block_free(inode->indirect);
        inode->indirect = 0;
    }
    if (inode->double_indirect != 0) {
        /* Two buffers are needed simultaneously: l0buf holds the L0 pointer
         * table for the entire loop while l1buf is reused per L1 table. */
        uint8_t *l0buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
        uint8_t *l1buf = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
        if (l0buf && l1buf && read_block(inode->double_indirect, l0buf) == 0) {
            uint32_t *l0 = (uint32_t *)l0buf;
            for (uint32_t i = 0; i < DUFS_INDIR_ENTRIES; i++) {
                if (l0[i] == 0) continue;
                if (read_block(l0[i], l1buf) == 0) {
                    uint32_t *l1 = (uint32_t *)l1buf;
                    for (uint32_t j = 0; j < DUFS_INDIR_ENTRIES; j++) {
                        if (l1[j] != 0) block_free(l1[j]);
                    }
                }
                block_free(l0[i]);
            }
        }
        kfree(l0buf);
        kfree(l1buf);
        block_free(inode->double_indirect);
        inode->double_indirect = 0;
    }
    inode->block_count = 0;
    inode->size = 0;
}

/* ── Zero a struct ──────────────────────────────────────────────────────── */

static void zero_inode(dufs_inode_t *inode)
{
    uint8_t *p = (uint8_t *)inode;
    for (uint32_t i = 0; i < sizeof(dufs_inode_t); i++) p[i] = 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int fs_init(void)
{
    if (!g_dufs)
        dufs_select(&g_dufs_hd0);

    g_blkdev = blkdev_get(g_dufs->blkdev_name);
    if (!g_blkdev) return -1;

    uint8_t buf[512];
    if (g_blkdev->read_sector(1, buf) != 0) return -1;

    /* Copy superblock into g_super. */
    uint32_t i;
    for (i = 0; i < sizeof(dufs_super_t); i++)
        ((uint8_t *)&g_super)[i] = buf[i];

    if (g_super.magic != DUFS_MAGIC) {
        klog_hex("FS", "bad magic", g_super.magic);
        return -2;
    }

    klog_uint("FS", "data_lba",         g_super.data_lba);
    klog_uint("FS", "inode_bitmap_lba", g_super.inode_bitmap_lba);
    klog_uint("FS", "block_bitmap_lba", g_super.block_bitmap_lba);
    klog_uint("FS", "inode_table_lba",  g_super.inode_table_lba);

    /* Load bitmaps — each is one 4096-byte block (8 sectors). */
    if (read_block(g_super.inode_bitmap_lba, g_inode_bitmap) != 0) return -1;
    if (read_block(g_super.block_bitmap_lba, g_block_bitmap) != 0) return -1;

    return 0;
}

static int fs_open_follow(const char *path, uint32_t *inode_out,
                          uint32_t *size_out, uint32_t depth)
{
    uint32_t dir_ino;
    char leaf[DUFS_MAX_NAME];

    if (depth > 8)
        return -40;
    if (path_resolve(path, &dir_ino, leaf) != 0) return -1;

    uint32_t ino = dir_lookup(dir_ino, leaf);
    if (ino == 0) {
        klog("OPEN", "dir_lookup: not found");
        return -1;
    }

    dufs_inode_t inode;
    if (inode_read(ino, &inode) != 0) return -1;
    if (inode.type == DUFS_TYPE_SYMLINK) {
        char *target = (char *)kmalloc(4096);
        char *next = (char *)kmalloc(4096);
        int rc;

        if (!target || !next) {
            if (target) kfree(target);
            if (next) kfree(next);
            return -1;
        }
        if (read_symlink_target(ino, target, 4096) < 0 ||
            symlink_target_path(path, target, next, 4096) != 0) {
            kfree(target);
            kfree(next);
            return -1;
        }
        rc = fs_open_follow(next, inode_out, size_out, depth + 1u);
        kfree(target);
        kfree(next);
        return rc;
    }
    if (inode.type != DUFS_TYPE_FILE) {
        klog_uint("OPEN", "wrong type", inode.type);
        return -1;
    }

    *inode_out = ino;
    *size_out  = inode.size;
    return 0;
}

int fs_open(const char *path, uint32_t *inode_out, uint32_t *size_out)
{
    return fs_open_follow(path, inode_out, size_out, 0);
}

int fs_read(uint32_t inode_num, uint32_t offset, uint8_t *buf, uint32_t count)
{
    if (count == 0) return 0;

    dufs_inode_t inode;
    if (inode_read(inode_num, &inode) != 0) return -1;
    if (offset >= inode.size) return 0; /* EOF */
    if (offset + count > inode.size)
        count = inode.size - offset;

    uint8_t *blk = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
    if (!blk) return -1;

    uint32_t total_read = 0;
    while (total_read < count) {
        uint32_t cur_off   = offset + total_read;
        uint32_t block_idx = cur_off / DUFS_BLOCK_SIZE;
        uint32_t block_off = cur_off % DUFS_BLOCK_SIZE;

        uint32_t lba = block_map(&inode, block_idx);
        if (lba == 0) break; /* sparse hole or allocation error */

        if (read_block(lba, blk) != 0) break;

        uint32_t avail = DUFS_BLOCK_SIZE - block_off;
        uint32_t chunk = count - total_read;
        if (chunk > avail) chunk = avail;

        for (uint32_t i = 0; i < chunk; i++)
            buf[total_read + i] = blk[block_off + i];

        total_read += chunk;
    }
    kfree(blk);
    return (int)total_read;
}

int fs_write(uint32_t inode_num, uint32_t offset,
             const uint8_t *buf, uint32_t count)
{
    if (count == 0) return 0;

    dufs_inode_t inode;
    if (inode_read(inode_num, &inode) != 0) return -1;

    uint32_t written      = 0;
    int      bitmap_dirty = 0;

    uint8_t *blk = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
    if (!blk) return -1;

    while (written < count) {
        uint32_t cur_off   = offset + written;
        uint32_t block_idx = cur_off / DUFS_BLOCK_SIZE;
        uint32_t block_off = cur_off % DUFS_BLOCK_SIZE;

        uint32_t lba = block_ensure(&inode, block_idx);
        if (lba == 0) break;
        bitmap_dirty = 1;

        /* Read-modify-write: preserve bytes outside the written range. */
        if (read_block(lba, blk) != 0) break;

        uint32_t space = DUFS_BLOCK_SIZE - block_off;
        uint32_t chunk = count - written;
        if (chunk > space) chunk = space;

        for (uint32_t i = 0; i < chunk; i++)
            blk[block_off + i] = buf[written + i];

        if (write_block(lba, blk) != 0) break;

        written += chunk;

        uint32_t end = cur_off + chunk;
        if (end > inode.size) inode.size = end;
    }
    kfree(blk);

    if (written > 0)
        inode.mtime = clock_unix_time();

    if (inode_write(inode_num, &inode) != 0) return -1;
    if (bitmap_dirty) flush_block_bitmap();

    return (int)written;
}

int fs_truncate(uint32_t inode_num, uint32_t size)
{
    dufs_inode_t inode;

    if (inode_read(inode_num, &inode) != 0)
        return -1;
    if (inode.type != DUFS_TYPE_FILE)
        return -1;
    if (inode.size == size)
        return 0;

    if (size > inode.size) {
        uint8_t zero = 0;
        return fs_write(inode_num, size - 1u, &zero, 1u) == 1 ? 0 : -1;
    }

    inode.size = size;
    inode.mtime = clock_unix_time();
    return inode_write(inode_num, &inode);
}

int fs_create(const char *path)
{
    uint32_t dir_ino;
    char leaf[DUFS_MAX_NAME];
    if (path_resolve(path, &dir_ino, leaf) != 0) return -1;

    /* If the file already exists, truncate it. */
    uint32_t existing = dir_lookup(dir_ino, leaf);
    if (existing != 0) {
        dufs_inode_t inode;
        if (inode_read(existing, &inode) != 0) return -1;
        if (inode.type != DUFS_TYPE_FILE) return -1;
        free_inode_blocks(&inode);
        inode.mtime = clock_unix_time();
        if (inode_write(existing, &inode) != 0) return -1;
        flush_block_bitmap();
        return (int)existing;
    }

    /* New file: allocate an inode. */
    uint32_t new_ino = inode_alloc();
    if (new_ino == 0) return -1;

    dufs_inode_t inode;
    zero_inode(&inode);
    inode.type       = DUFS_TYPE_FILE;
    inode.link_count = 1;
    inode.mtime      = clock_unix_time();
    inode.atime      = inode.mtime;

    if (inode_write(new_ino, &inode) != 0) {
        inode_free(new_ino);
        flush_inode_bitmap();
        return -1;
    }

    if (dir_add(dir_ino, leaf, new_ino) != 0) {
        inode_free(new_ino);
        flush_inode_bitmap();
        flush_block_bitmap();
        return -1;
    }

    flush_inode_bitmap();
    flush_block_bitmap();
    return (int)new_ino;
}

int fs_flush_inode(uint32_t inode_num)
{
    (void)inode_num;
    return 0;
}

int fs_unlink(const char *path)
{
    if (!path || path[0] == '\0') return -1;

    uint32_t dir_ino;
    char leaf[DUFS_MAX_NAME];
    if (path_resolve(path, &dir_ino, leaf) != 0) return -1;

    uint32_t child_ino = dir_lookup(dir_ino, leaf);
    if (child_ino == 0) return -1;

    dufs_inode_t inode;
    if (inode_read(child_ino, &inode) != 0) return -1;
    if (inode.type != DUFS_TYPE_FILE && inode.type != DUFS_TYPE_SYMLINK)
        return -1;

    if (inode.link_count > 0) inode.link_count--;
    if (inode.link_count == 0) {
        free_inode_blocks(&inode);
        inode_free(child_ino);
        inode_write(child_ino, &inode);
        flush_inode_bitmap();
        flush_block_bitmap();
    } else {
        inode_write(child_ino, &inode);
    }

    if (dir_remove(dir_ino, leaf) == 0) return -1;
    return 0;
}

int fs_mkdir(const char *path)
{
    if (!path || path[0] == '\0') return -1;

    uint32_t dir_ino;
    char leaf[DUFS_MAX_NAME];
    if (path_resolve(path, &dir_ino, leaf) != 0) return -1;

    /* Reject duplicates. */
    if (dir_lookup(dir_ino, leaf) != 0) return -1;

    uint32_t new_ino = inode_alloc();
    if (new_ino == 0) return -1;

    dufs_inode_t inode;
    zero_inode(&inode);
    inode.type       = DUFS_TYPE_DIR;
    inode.link_count = 1;
    inode.mtime      = clock_unix_time();
    inode.atime      = inode.mtime;

    if (inode_write(new_ino, &inode) != 0) {
        inode_free(new_ino);
        flush_inode_bitmap();
        return -1;
    }

    if (dir_add(dir_ino, leaf, new_ino) != 0) {
        inode_free(new_ino);
        flush_inode_bitmap();
        flush_block_bitmap();
        return -1;
    }

    flush_inode_bitmap();
    flush_block_bitmap();
    return 0;
}

int fs_rmdir(const char *path)
{
    if (!path || path[0] == '\0') return -1;

    uint32_t par_ino;
    char leaf[DUFS_MAX_NAME];
    if (path_resolve(path, &par_ino, leaf) != 0) return -1;

    uint32_t dir_ino = dir_lookup(par_ino, leaf);
    if (dir_ino == 0) return -1;

    dufs_inode_t inode;
    if (inode_read(dir_ino, &inode) != 0) return -1;
    if (inode.type != DUFS_TYPE_DIR) return -1;

    /* Refuse to remove a non-empty directory. */
    if (dir_count_entries(dir_ino) > 0) return -1;

    free_inode_blocks(&inode);
    inode_free(dir_ino);
    inode_write(dir_ino, &inode);
    flush_inode_bitmap();
    flush_block_bitmap();

    if (dir_remove(par_ino, leaf) == 0) return -1;
    return 0;
}

int fs_rename(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath || oldpath[0] == '\0' || newpath[0] == '\0')
        return -1;

    uint32_t old_dir_ino;
    char old_leaf[DUFS_MAX_NAME];
    if (path_resolve(oldpath, &old_dir_ino, old_leaf) != 0) return -1;

    uint32_t new_dir_ino;
    char new_leaf[DUFS_MAX_NAME];
    if (path_resolve(newpath, &new_dir_ino, new_leaf) != 0) return -1;

    uint32_t src_ino = dir_lookup(old_dir_ino, old_leaf);
    if (src_ino == 0) return -1;

    dufs_inode_t src_inode;
    if (inode_read(src_ino, &src_inode) != 0) return -1;

    /* Handle collision at destination. */
    uint32_t dst_ino = dir_lookup(new_dir_ino, new_leaf);
    if (dst_ino != 0) {
        if (dst_ino == src_ino) return 0; /* same inode — no-op */

        dufs_inode_t dst_inode;
        if (inode_read(dst_ino, &dst_inode) != 0) return -1;
        if (dst_inode.type == DUFS_TYPE_DIR) return -1; /* cannot replace a dir */

        if (dst_inode.link_count > 0) dst_inode.link_count--;
        if (dst_inode.link_count == 0) {
            free_inode_blocks(&dst_inode);
            inode_free(dst_ino);
            inode_write(dst_ino, &dst_inode);
            flush_inode_bitmap();
            flush_block_bitmap();
        }
        dir_remove(new_dir_ino, new_leaf);
    }

    if (dir_add(new_dir_ino, new_leaf, src_ino) != 0) return -1;
    dir_remove(old_dir_ino, old_leaf);
    return 0;
}

static int fs_link_source_inode(const char *path, uint32_t follow,
                                uint32_t depth, uint32_t *ino_out)
{
    uint32_t old_dir_ino;
    char old_leaf[DUFS_MAX_NAME];
    int rc;

    if (depth > 8) return -40;
    if (path_resolve(path, &old_dir_ino, old_leaf) != 0) return -1;

    uint32_t src_ino = dir_lookup(old_dir_ino, old_leaf);
    if (src_ino == 0) return -1;

    dufs_inode_t inode;
    if (inode_read(src_ino, &inode) != 0) return -1;
    if (follow && inode.type == DUFS_TYPE_SYMLINK) {
        char *target = (char *)kmalloc(4096);
        char *next = (char *)kmalloc(4096);

        if (!target || !next) {
            if (target) kfree(target);
            if (next) kfree(next);
            return -1;
        }
        if (read_symlink_target(src_ino, target, 4096) < 0 ||
            symlink_target_path(path, target, next, 4096) != 0) {
            kfree(target);
            kfree(next);
            return -1;
        }
        rc = fs_link_source_inode(next, follow, depth + 1u, ino_out);
        kfree(target);
        kfree(next);
        return rc;
    }

    *ino_out = src_ino;
    return 0;
}

int fs_link(const char *oldpath, const char *newpath, uint32_t follow)
{
    if (!oldpath || !newpath || oldpath[0] == '\0' || newpath[0] == '\0')
        return -1;

    uint32_t src_ino;
    int rc = fs_link_source_inode(oldpath, follow, 0, &src_ino);
    if (rc != 0) return rc;

    dufs_inode_t inode;
    if (inode_read(src_ino, &inode) != 0) return -1;
    if (inode.type == DUFS_TYPE_DIR) return -1;

    uint32_t new_dir_ino;
    char new_leaf[DUFS_MAX_NAME];
    if (path_resolve(newpath, &new_dir_ino, new_leaf) != 0) return -1;
    if (dir_lookup(new_dir_ino, new_leaf) != 0) return -1;

    if (dir_add(new_dir_ino, new_leaf, src_ino) != 0) return -1;
    inode.link_count++;
    inode.mtime = clock_unix_time();
    if (inode_write(src_ino, &inode) != 0) return -1;
    return 0;
}

int fs_symlink(const char *target, const char *linkpath)
{
    if (!target || !linkpath || target[0] == '\0' || linkpath[0] == '\0')
        return -1;

    uint32_t dir_ino;
    char leaf[DUFS_MAX_NAME];
    if (path_resolve(linkpath, &dir_ino, leaf) != 0) return -1;
    if (dir_lookup(dir_ino, leaf) != 0) return -1;

    uint32_t target_len = k_strlen(target);
    if (target_len == 0 || target_len >= 4096u)
        return -1;

    uint32_t new_ino = inode_alloc();
    if (new_ino == 0) return -1;

    dufs_inode_t inode;
    zero_inode(&inode);
    inode.type       = DUFS_TYPE_SYMLINK;
    inode.link_count = 1;
    inode.mtime      = clock_unix_time();
    inode.atime      = inode.mtime;

    if (inode_write(new_ino, &inode) != 0) {
        inode_free(new_ino);
        flush_inode_bitmap();
        return -1;
    }

    if (fs_write(new_ino, 0, (const uint8_t *)target, target_len) !=
        (int)target_len) {
        inode_read(new_ino, &inode);
        free_inode_blocks(&inode);
        inode_free(new_ino);
        inode_write(new_ino, &inode);
        flush_inode_bitmap();
        flush_block_bitmap();
        return -1;
    }

    if (dir_add(dir_ino, leaf, new_ino) != 0) {
        inode_read(new_ino, &inode);
        free_inode_blocks(&inode);
        inode_free(new_ino);
        inode_write(new_ino, &inode);
        flush_inode_bitmap();
        flush_block_bitmap();
        return -1;
    }

    flush_inode_bitmap();
    flush_block_bitmap();
    return 0;
}

int fs_readlink(const char *path, char *buf, uint32_t bufsz)
{
    if (!path || !buf || bufsz == 0)
        return -22;

    uint32_t dir_ino;
    char leaf[DUFS_MAX_NAME];
    if (path_resolve(path, &dir_ino, leaf) != 0) return -2;

    uint32_t ino = dir_lookup(dir_ino, leaf);
    if (ino == 0) return -2;

    dufs_inode_t inode;
    if (inode_read(ino, &inode) != 0) return -2;
    if (inode.type != DUFS_TYPE_SYMLINK) return -22;

    uint32_t n = inode.size;
    if (n > bufsz)
        n = bufsz;
    if (n == 0)
        return 0;
    int rc = fs_read(ino, 0, (uint8_t *)buf, n);
    return rc == (int)n ? (int)n : -1;
}

int fs_list(const char *path, char *buf, uint32_t bufsz)
{
    uint32_t dir_ino;

    if (path == 0 || path[0] == '\0') {
        dir_ino = 1; /* root */
    } else {
        uint32_t par_ino;
        char leaf[DUFS_MAX_NAME];
        if (path_resolve(path, &par_ino, leaf) != 0) return -1;

        dir_ino = dir_lookup(par_ino, leaf);
        if (dir_ino == 0) return -1;

        dufs_inode_t dinode;
        if (inode_read(dir_ino, &dinode) != 0) return -1;
        if (dinode.type != DUFS_TYPE_DIR) return -1;
    }

    dufs_inode_t dir;
    if (inode_read(dir_ino, &dir) != 0) return -1;

    uint8_t *block = (uint8_t *)kmalloc(DUFS_BLOCK_SIZE);
    if (!block) return -1;

    uint32_t written = 0;
    for (uint32_t blk = 0; blk < dir.block_count; blk++) {
        uint32_t lba = block_map(&dir, blk);
        if (lba == 0) continue;

        if (read_block(lba, block) != 0) continue;
        dufs_dirent_t *entries = (dufs_dirent_t *)block;

        for (uint32_t e = 0; e < DUFS_DIRENTS_PER_BLOCK; e++) {
            if (entries[e].inode == 0) continue;

            /* Copy the name. */
            uint32_t j = 0;
            while (j < DUFS_MAX_NAME && entries[e].name[j]) {
                if (written >= bufsz - 2) goto done;
                buf[written++] = entries[e].name[j++];
            }

            /* Append '/' for directory entries so callers can detect type. */
            dufs_inode_t child;
            if (inode_read(entries[e].inode, &child) == 0 &&
                child.type == DUFS_TYPE_DIR) {
                if (written < bufsz - 1)
                    buf[written++] = '/';
            }

            if (written < bufsz)
                buf[written++] = '\0';
        }
    }
done:
    kfree(block);
    return (int)written;
}

static int fs_stat_common(const char *path, vfs_stat_t *st,
                          uint32_t follow, uint32_t depth)
{
    uint32_t dir_ino;
    char leaf[DUFS_MAX_NAME];

    if (depth > 8)
        return -40;
    if (path_resolve(path, &dir_ino, leaf) != 0) return -1;

    uint32_t ino = dir_lookup(dir_ino, leaf);
    if (ino == 0) return -1;

    dufs_inode_t inode;
    if (inode_read(ino, &inode) != 0) return -1;
    if (follow && inode.type == DUFS_TYPE_SYMLINK) {
        char *target = (char *)kmalloc(4096);
        char *next = (char *)kmalloc(4096);
        int rc;

        if (!target || !next) {
            if (target) kfree(target);
            if (next) kfree(next);
            return -1;
        }
        if (read_symlink_target(ino, target, 4096) < 0 ||
            symlink_target_path(path, target, next, 4096) != 0) {
            kfree(target);
            kfree(next);
            return -1;
        }
        rc = fs_stat_common(next, st, follow, depth + 1u);
        kfree(target);
        kfree(next);
        return rc;
    }

    stat_from_inode(&inode, st);
    return 0;
}

int fs_stat(const char *path, vfs_stat_t *st)
{
    return fs_stat_common(path, st, 1, 0);
}

int fs_lstat(const char *path, vfs_stat_t *st)
{
    return fs_stat_common(path, st, 0, 0);
}

/* ── VFS registration ───────────────────────────────────────────────────── */

static dufs_mount_t *dufs_mount_from_ctx(void *ctx)
{
    return ctx ? (dufs_mount_t *)ctx : &g_dufs_hd0;
}

static int dufs_vfs_init(void *ctx)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_init();
}

static int dufs_vfs_open(void *ctx, const char *path,
                         uint32_t *inode_out, uint32_t *size_out)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_open(path, inode_out, size_out);
}

static int dufs_vfs_list(void *ctx, const char *path, char *buf,
                         uint32_t bufsz)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_list(path, buf, bufsz);
}

static int dufs_vfs_create(void *ctx, const char *path)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_create(path);
}

static int dufs_vfs_unlink(void *ctx, const char *path)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_unlink(path);
}

static int dufs_vfs_mkdir(void *ctx, const char *path)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_mkdir(path);
}

static int dufs_vfs_rmdir(void *ctx, const char *path)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_rmdir(path);
}

static int dufs_vfs_rename(void *ctx, const char *oldpath,
                           const char *newpath)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_rename(oldpath, newpath);
}

static int dufs_vfs_link(void *ctx, const char *oldpath,
                         const char *newpath, uint32_t follow)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_link(oldpath, newpath, follow);
}

static int dufs_vfs_symlink(void *ctx, const char *target,
                            const char *linkpath)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_symlink(target, linkpath);
}

static int dufs_vfs_readlink(void *ctx, const char *path, char *buf,
                             uint32_t bufsz)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_readlink(path, buf, bufsz);
}

static int dufs_vfs_stat(void *ctx, const char *path, vfs_stat_t *st)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_stat(path, st);
}

static int dufs_vfs_lstat(void *ctx, const char *path, vfs_stat_t *st)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_lstat(path, st);
}

static int dufs_vfs_read(void *ctx, uint32_t inode_num, uint32_t offset,
                         uint8_t *buf, uint32_t count)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_read(inode_num, offset, buf, count);
}

static int dufs_vfs_write(void *ctx, uint32_t inode_num, uint32_t offset,
                          const uint8_t *buf, uint32_t count)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_write(inode_num, offset, buf, count);
}

static int dufs_vfs_truncate(void *ctx, uint32_t inode_num, uint32_t size)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_truncate(inode_num, size);
}

static int dufs_vfs_flush(void *ctx, uint32_t inode_num)
{
    dufs_select(dufs_mount_from_ctx(ctx));
    return fs_flush_inode(inode_num);
}

static const fs_ops_t dufs_ops = {
    .ctx      = &g_dufs_hd0,
    .init     = dufs_vfs_init,
    .open     = dufs_vfs_open,
    .getdents = dufs_vfs_list,
    .create   = dufs_vfs_create,
    .unlink   = dufs_vfs_unlink,
    .mkdir    = dufs_vfs_mkdir,
    .rmdir    = dufs_vfs_rmdir,
    .rename   = dufs_vfs_rename,
    .link     = dufs_vfs_link,
    .symlink  = dufs_vfs_symlink,
    .readlink = dufs_vfs_readlink,
    .stat     = dufs_vfs_stat,
    .lstat    = dufs_vfs_lstat,
    .read     = dufs_vfs_read,
    .write    = dufs_vfs_write,
    .truncate = dufs_vfs_truncate,
    .flush    = dufs_vfs_flush,
};

static const fs_ops_t dufs1_ops = {
    .ctx      = &g_dufs_hd1,
    .init     = dufs_vfs_init,
    .open     = dufs_vfs_open,
    .getdents = dufs_vfs_list,
    .create   = dufs_vfs_create,
    .unlink   = dufs_vfs_unlink,
    .mkdir    = dufs_vfs_mkdir,
    .rmdir    = dufs_vfs_rmdir,
    .rename   = dufs_vfs_rename,
    .link     = dufs_vfs_link,
    .symlink  = dufs_vfs_symlink,
    .readlink = dufs_vfs_readlink,
    .stat     = dufs_vfs_stat,
    .lstat    = dufs_vfs_lstat,
    .read     = dufs_vfs_read,
    .write    = dufs_vfs_write,
    .truncate = dufs_vfs_truncate,
    .flush    = dufs_vfs_flush,
};

void dufs_register(void) {
    vfs_register("dufs", &dufs_ops);
    vfs_register("dufs1", &dufs1_ops);
}
