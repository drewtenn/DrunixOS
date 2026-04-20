/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ext3.c — native ext2/ext3-compatible filesystem backend.
 */

#include "ext3.h"
#include "vfs.h"
#include "blkdev.h"
#include "bcache.h"
#include "clock.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include <stdint.h>

#define EXT3_SUPER_OFFSET 1024u
#define EXT3_SUPER_MAGIC 0xEF53u
#define EXT3_ROOT_INO 2u
#define EXT3_NAME_MAX 255u
#define EXT3_N_BLOCKS 15u
#define EXT3_NDIR_BLOCKS 12u
#define EXT3_FT_REG_FILE 1u
#define EXT3_FT_DIR 2u
#define EXT3_FT_SYMLINK 7u
#define EXT3_S_IFMT 0xF000u
#define EXT3_S_IFREG 0x8000u
#define EXT3_S_IFDIR 0x4000u
#define EXT3_S_IFLNK 0xA000u
#define EXT3_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT3_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT3_FEATURE_INCOMPAT_RECOVER 0x0004u
#define EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT3_FEATURE_RO_COMPAT_LARGE_FILE 0x0002u
#define JBD_MAGIC 0xC03B3998u
#define JBD_DESCRIPTOR_BLOCK 1u
#define JBD_COMMIT_BLOCK 2u
#define JBD_SUPERBLOCK_V1 3u
#define JBD_SUPERBLOCK_V2 4u
#define JBD_REVOKE_BLOCK 5u
#define JBD_FLAG_ESCAPE 1u
#define JBD_FLAG_SAME_UUID 2u
#define JBD_FLAG_LAST_TAG 8u
#define JBD_FEATURE_INCOMPAT_REVOKE 0x00000001u
#define EXT3_REPLAY_MAX_BLOCKS 32u
#define EXT3_REPLAY_MAX_TAGS 64u
#define EXT3_REPLAY_MAX_REVOKES 128u
#define EXT3_TX_MAX_BLOCKS 128u

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
	uint8_t uuid[16];
	char volume_name[16];
	char last_mounted[64];
	uint32_t algorithm_usage_bitmap;
	uint8_t prealloc_blocks;
	uint8_t prealloc_dir_blocks;
	uint16_t reserved_gdt_blocks;
	uint8_t journal_uuid[16];
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
	uint8_t reserved[12];
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
	uint8_t osd2[12];
} __attribute__((packed)) ext3_inode_t;

typedef struct {
	uint32_t inode;
	uint16_t rec_len;
	uint8_t name_len;
	uint8_t file_type;
} __attribute__((packed)) ext3_dirent_t;

static const blkdev_ops_t *g_dev;
static ext3_super_t g_super;
static ext3_group_desc_t g_bg;
static uint32_t g_block_size;
static uint32_t g_sectors_per_block;
static uint32_t g_bgdt_block;
static uint32_t g_writable;
static uint32_t g_needs_recovery;
static uint32_t g_block_alloc_cursor;
static uint32_t g_inode_alloc_cursor;
static uint32_t g_tx_counts_dirty;

typedef struct {
	uint32_t fs_block;
	uint8_t *data;
} ext3_overlay_block_t;

typedef struct {
	uint32_t fs_block;
	uint8_t *data;
} ext3_tx_block_t;

static ext3_overlay_block_t g_overlay[EXT3_REPLAY_MAX_BLOCKS];
static uint32_t g_overlay_count;
static ext3_tx_block_t g_tx[EXT3_TX_MAX_BLOCKS];
static uint32_t g_tx_count;
static uint32_t g_tx_depth;
static uint32_t g_tx_failed;
static ext3_super_t g_tx_super_before;
static uint32_t g_tx_has_snapshot;

static uint16_t be16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
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

static int
ext3_write_bytes(uint32_t byte_off, const uint8_t *buf, uint32_t count)
{
	uint8_t sec[BLKDEV_SECTOR_SIZE];
	uint32_t done = 0;
	uint32_t first_block;
	uint32_t last_block;

	if (!g_dev || !g_dev->write_sector || !buf || count == 0)
		return -1;
	while (done < count) {
		uint32_t off = byte_off + done;
		uint32_t lba = off / BLKDEV_SECTOR_SIZE;
		uint32_t sec_off = off % BLKDEV_SECTOR_SIZE;
		uint32_t chunk = BLKDEV_SECTOR_SIZE - sec_off;
		if (chunk > count - done)
			chunk = count - done;
		if (chunk != BLKDEV_SECTOR_SIZE) {
			if (g_dev->read_sector(lba, sec) != 0)
				return -1;
		}
		k_memcpy(sec + sec_off, buf + done, chunk);
		if (g_dev->write_sector(lba, sec) != 0)
			return -1;
		done += chunk;
	}

	/* This function touched the device directly, bypassing the block cache.
     * Any 4 KB block whose contents we just changed on disk must be dropped
     * from the cache, otherwise a later bcache_read of the same range would
     * return pre-write bytes. The cache is keyed by the starting LBA of
     * each 4 KB range, so walk in 8-sector strides. */
	first_block = (byte_off / BCACHE_BLOCK_SIZE) * BCACHE_SECS_PER_BLK;
	last_block =
	    ((byte_off + count - 1u) / BCACHE_BLOCK_SIZE) * BCACHE_SECS_PER_BLK;
	for (uint32_t lba = first_block; lba <= last_block;
	     lba += BCACHE_SECS_PER_BLK)
		bcache_invalidate(g_dev, lba);
	return 0;
}

static int ext3_read_disk_block(uint32_t block, uint8_t *buf)
{
	if (!g_dev || !buf || g_block_size == 0)
		return -1;
	return bcache_read(g_dev, block * g_sectors_per_block, buf);
}

static int ext3_write_disk_block(uint32_t block, const uint8_t *buf)
{
	if (!g_dev || !g_dev->write_sector || !buf || g_block_size == 0)
		return -1;
	return bcache_write(g_dev, block * g_sectors_per_block, buf);
}

static int ext3_write_block(uint32_t block, const uint8_t *buf);

static int ext3_write_zeroed_block(uint32_t block)
{
	uint8_t *z = (uint8_t *)kmalloc(g_block_size);
	int rc;

	if (!z)
		return -1;
	k_memset(z, 0, g_block_size);
	rc = ext3_write_block(block, z);
	kfree(z);
	return rc;
}

static int ext3_tx_find(uint32_t fs_block)
{
	for (uint32_t i = 0; i < g_tx_count; i++) {
		if (g_tx[i].fs_block == fs_block)
			return (int)i;
	}
	return -1;
}

static void ext3_tx_reset(void)
{
	for (uint32_t i = 0; i < g_tx_count; i++) {
		if (g_tx[i].data)
			kfree(g_tx[i].data);
		g_tx[i].fs_block = 0;
		g_tx[i].data = 0;
	}
	g_tx_count = 0;
	g_tx_depth = 0;
	g_tx_failed = 0;
	g_tx_has_snapshot = 0;
	g_tx_counts_dirty = 0;
}

static int ext3_read_block(uint32_t block, uint8_t *buf)
{
	int tx_idx;

	if (!buf)
		return -1;
	tx_idx = ext3_tx_find(block);
	if (tx_idx >= 0) {
		k_memcpy(buf, g_tx[tx_idx].data, g_block_size);
		return 0;
	}
	for (uint32_t i = 0; i < g_overlay_count; i++) {
		if (g_overlay[i].fs_block == block) {
			k_memcpy(buf, g_overlay[i].data, g_block_size);
			return 0;
		}
	}
	return ext3_read_disk_block(block, buf);
}

static int ext3_write_block(uint32_t block, const uint8_t *buf)
{
	uint8_t *copy;
	int idx;

	if (!buf)
		return -1;
	if (g_tx_depth == 0)
		return ext3_write_disk_block(block, buf);

	idx = ext3_tx_find(block);
	if (idx >= 0) {
		k_memcpy(g_tx[idx].data, buf, g_block_size);
		return 0;
	}

	if (g_tx_count >= EXT3_TX_MAX_BLOCKS) {
		klog("EXT3", "journal transaction block limit hit");
		g_tx_failed = 1;
		return -1;
	}

	copy = (uint8_t *)kmalloc(g_block_size);
	if (!copy) {
		g_tx_failed = 1;
		return -1;
	}
	k_memcpy(copy, buf, g_block_size);
	g_tx[g_tx_count].fs_block = block;
	g_tx[g_tx_count].data = copy;
	g_tx_count++;
	return 0;
}

static int ext3_can_mutate(void)
{
	return g_writable && g_overlay_count == 0 && g_tx_failed == 0;
}

static int ext3_tx_begin(void);
static int ext3_tx_end(int rc);
static int ext3_load_bg(void);

static uint32_t ext3_dir_rec_len(uint32_t name_len)
{
	return (sizeof(ext3_dirent_t) + name_len + 3u) & ~3u;
}

static uint32_t ext3_bmap_test(const uint8_t *map, uint32_t bit)
{
	return (map[bit / 8u] & (uint8_t)(1u << (bit % 8u))) != 0;
}

static void ext3_bmap_set(uint8_t *map, uint32_t bit)
{
	map[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
}

static void ext3_bmap_clear(uint8_t *map, uint32_t bit)
{
	map[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
}

static int ext3_flush_super_image_raw(const ext3_super_t *super)
{
	if (!super)
		return -1;
	return ext3_write_bytes(
	    EXT3_SUPER_OFFSET, (const uint8_t *)super, sizeof(g_super));
}

static int ext3_flush_super_raw(void)
{
	return ext3_flush_super_image_raw(&g_super);
}

static int ext3_flush_super(void)
{
	uint8_t *blk;
	uint32_t block = EXT3_SUPER_OFFSET / g_block_size;
	uint32_t off = EXT3_SUPER_OFFSET % g_block_size;
	int rc;

	blk = (uint8_t *)kmalloc(g_block_size);
	if (!blk)
		return -1;
	if (ext3_read_block(block, blk) != 0) {
		kfree(blk);
		return -1;
	}
	k_memcpy(blk + off, &g_super, sizeof(g_super));
	rc = ext3_write_block(block, blk);
	kfree(blk);
	return rc;
}

static int ext3_flush_bg(void)
{
	uint8_t *blk = (uint8_t *)kmalloc(g_block_size);
	int rc;

	if (!blk)
		return -1;
	if (ext3_read_block(g_bgdt_block, blk) != 0) {
		kfree(blk);
		return -1;
	}
	k_memcpy(blk, &g_bg, sizeof(g_bg));
	rc = ext3_write_block(g_bgdt_block, blk);
	kfree(blk);
	return rc;
}

static int ext3_flush_counts(void)
{
	/* Inside a transaction, defer the super + BG writes to commit time.
     * A single mutating syscall may allocate or free many blocks, and each
     * flush rewrites two full blocks through the tx buffer — quadratic in
     * the number of allocations. Commit flushes once per transaction. */
	if (g_tx_depth > 0) {
		g_tx_counts_dirty = 1;
		return 0;
	}
	if (ext3_flush_super() != 0)
		return -1;
	if (ext3_flush_bg() != 0)
		return -1;
	return 0;
}

static int ext3_overlay_put(uint32_t fs_block, const uint8_t *data)
{
	uint8_t *copy;

	if (!data)
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

static void ext3_overlay_clear(void)
{
	for (uint32_t i = 0; i < g_overlay_count; i++) {
		if (g_overlay[i].data)
			kfree(g_overlay[i].data);
		g_overlay[i].fs_block = 0;
		g_overlay[i].data = 0;
	}
	g_overlay_count = 0;
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

static int ext3_write_inode(uint32_t ino, const ext3_inode_t *in)
{
	uint8_t *blk;
	uint32_t idx;
	uint32_t off;
	uint32_t block;
	uint32_t block_off;
	int rc;

	if (!in || ino == 0 || ino > g_super.inodes_count || !ext3_can_mutate())
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
	k_memcpy(blk + block_off, in, sizeof(*in));
	rc = ext3_write_block(block, blk);
	kfree(blk);
	return rc;
}

static void ext3_inode_blocks_add(ext3_inode_t *in, uint32_t fs_blocks)
{
	in->blocks += fs_blocks * g_sectors_per_block;
}

static void ext3_inode_blocks_sub(ext3_inode_t *in, uint32_t fs_blocks)
{
	uint32_t sectors = fs_blocks * g_sectors_per_block;
	in->blocks = in->blocks > sectors ? in->blocks - sectors : 0;
}

static uint32_t ext3_alloc_block(void)
{
	uint8_t *map;
	uint32_t block = 0;
	uint32_t first = g_super.first_data_block;
	uint32_t last = g_super.blocks_count;
	uint32_t start;
	uint32_t scanned = 0;
	uint32_t total;

	if (!ext3_can_mutate())
		return 0;
	if (first >= last)
		return 0;
	map = (uint8_t *)kmalloc(g_block_size);
	if (!map)
		return 0;
	if (ext3_read_block(g_bg.block_bitmap, map) != 0)
		goto done;

	total = last - first;
	start = g_block_alloc_cursor;
	if (start < first || start >= last)
		start = first;

	for (uint32_t b = start; scanned < total; scanned++) {
		if (!ext3_bmap_test(map, b)) {
			ext3_bmap_set(map, b);
			if (ext3_write_block(g_bg.block_bitmap, map) != 0) {
				ext3_bmap_clear(map, b);
				break;
			}
			if (g_super.free_blocks_count > 0)
				g_super.free_blocks_count--;
			if (g_bg.free_blocks_count > 0)
				g_bg.free_blocks_count--;
			if (ext3_flush_counts() != 0)
				break;
			if (ext3_write_zeroed_block(b) != 0)
				break;
			g_block_alloc_cursor = (b + 1u >= last) ? first : b + 1u;
			block = b;
			break;
		}
		b++;
		if (b >= last)
			b = first;
	}

done:
	kfree(map);
	return block;
}

static int ext3_free_block(uint32_t block)
{
	uint8_t *map;
	int rc = -1;

	if (!ext3_can_mutate() || block == 0 || block >= g_super.blocks_count)
		return -1;
	map = (uint8_t *)kmalloc(g_block_size);
	if (!map)
		return -1;
	if (ext3_read_block(g_bg.block_bitmap, map) != 0)
		goto done;
	if (!ext3_bmap_test(map, block)) {
		rc = 0;
		goto done;
	}
	ext3_bmap_clear(map, block);
	if (ext3_write_block(g_bg.block_bitmap, map) != 0)
		goto done;
	g_super.free_blocks_count++;
	g_bg.free_blocks_count++;
	if (block < g_block_alloc_cursor)
		g_block_alloc_cursor = block;
	rc = ext3_flush_counts();

done:
	kfree(map);
	return rc;
}

static uint32_t ext3_alloc_inode(uint32_t is_dir)
{
	uint8_t *map;
	uint32_t ino = 0;
	uint32_t first = g_super.first_ino ? g_super.first_ino : 11u;
	uint32_t last = g_super.inodes_count;
	uint32_t start;
	uint32_t scanned = 0;
	uint32_t total;

	if (!ext3_can_mutate())
		return 0;
	if (first > last)
		return 0;
	map = (uint8_t *)kmalloc(g_block_size);
	if (!map)
		return 0;
	if (ext3_read_block(g_bg.inode_bitmap, map) != 0)
		goto done;

	total = last - first + 1u;
	start = g_inode_alloc_cursor;
	if (start < first || start > last)
		start = first;

	for (uint32_t i = start; scanned < total; scanned++) {
		uint32_t bit = i - 1u;
		if (!ext3_bmap_test(map, bit)) {
			ext3_bmap_set(map, bit);
			if (ext3_write_block(g_bg.inode_bitmap, map) != 0)
				break;
			if (g_super.free_inodes_count > 0)
				g_super.free_inodes_count--;
			if (g_bg.free_inodes_count > 0)
				g_bg.free_inodes_count--;
			if (is_dir)
				g_bg.used_dirs_count++;
			if (ext3_flush_counts() != 0)
				break;
			g_inode_alloc_cursor = (i >= last) ? first : i + 1u;
			ino = i;
			break;
		}
		i++;
		if (i > last)
			i = first;
	}

done:
	kfree(map);
	return ino;
}

static int ext3_free_inode(uint32_t ino, uint32_t was_dir)
{
	uint8_t *map;
	int rc = -1;

	if (!ext3_can_mutate() || ino < g_super.first_ino ||
	    ino > g_super.inodes_count)
		return -1;
	map = (uint8_t *)kmalloc(g_block_size);
	if (!map)
		return -1;
	if (ext3_read_block(g_bg.inode_bitmap, map) != 0)
		goto done;
	if (!ext3_bmap_test(map, ino - 1u)) {
		rc = 0;
		goto done;
	}
	ext3_bmap_clear(map, ino - 1u);
	if (ext3_write_block(g_bg.inode_bitmap, map) != 0)
		goto done;
	g_super.free_inodes_count++;
	g_bg.free_inodes_count++;
	if (was_dir && g_bg.used_dirs_count > 0)
		g_bg.used_dirs_count--;
	if (ino < g_inode_alloc_cursor)
		g_inode_alloc_cursor = ino;
	rc = ext3_flush_counts();

done:
	kfree(map);
	return rc;
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

static uint32_t ext3_ensure_block(ext3_inode_t *in, uint32_t logical)
{
	uint8_t *blk;
	uint32_t ptrs = g_block_size / 4u;
	uint32_t block;

	if (!in || !ext3_can_mutate())
		return 0;
	if (logical < EXT3_NDIR_BLOCKS) {
		if (in->block[logical] == 0) {
			block = ext3_alloc_block();
			if (block == 0)
				return 0;
			in->block[logical] = block;
			ext3_inode_blocks_add(in, 1);
		}
		return in->block[logical];
	}

	logical -= EXT3_NDIR_BLOCKS;
	if (logical >= ptrs)
		return 0;

	blk = (uint8_t *)kmalloc(g_block_size);
	if (!blk)
		return 0;
	if (in->block[12] == 0) {
		block = ext3_alloc_block();
		if (block == 0) {
			kfree(blk);
			return 0;
		}
		in->block[12] = block;
		ext3_inode_blocks_add(in, 1);
	}
	if (ext3_read_block(in->block[12], blk) != 0) {
		kfree(blk);
		return 0;
	}
	block = ((uint32_t *)blk)[logical];
	if (block == 0) {
		block = ext3_alloc_block();
		if (block == 0) {
			kfree(blk);
			return 0;
		}
		((uint32_t *)blk)[logical] = block;
		ext3_inode_blocks_add(in, 1);
		if (ext3_write_block(in->block[12], blk) != 0) {
			kfree(blk);
			return 0;
		}
	}
	kfree(blk);
	return block;
}

static int ext3_free_inode_blocks(ext3_inode_t *in)
{
	uint8_t *blk = 0;

	if (!in || in->block[13] != 0 || in->block[14] != 0)
		return -1;

	for (uint32_t i = 0; i < EXT3_NDIR_BLOCKS; i++) {
		if (in->block[i] != 0) {
			if (ext3_free_block(in->block[i]) != 0)
				return -1;
			in->block[i] = 0;
			ext3_inode_blocks_sub(in, 1);
		}
	}

	if (in->block[12] != 0) {
		blk = (uint8_t *)kmalloc(g_block_size);
		if (!blk)
			return -1;
		if (ext3_read_block(in->block[12], blk) != 0) {
			kfree(blk);
			return -1;
		}
		for (uint32_t i = 0; i < g_block_size / 4u; i++) {
			uint32_t b = ((uint32_t *)blk)[i];
			if (b != 0) {
				if (ext3_free_block(b) != 0) {
					kfree(blk);
					return -1;
				}
				ext3_inode_blocks_sub(in, 1);
			}
		}
		kfree(blk);
		if (ext3_free_block(in->block[12]) != 0)
			return -1;
		in->block[12] = 0;
		ext3_inode_blocks_sub(in, 1);
	}

	in->size = 0;
	in->dir_acl = 0;
	return 0;
}

static int ext3_free_inode_payload(ext3_inode_t *in)
{
	if (!in)
		return -1;
	if ((in->mode & EXT3_S_IFMT) == EXT3_S_IFLNK &&
	    ext3_inode_size(in) <= 60u) {
		in->size = 0;
		in->dir_acl = 0;
		return 0;
	}
	return ext3_free_inode_blocks(in);
}

static int ext3_truncate_blocks(ext3_inode_t *in, uint32_t keep_blocks)
{
	uint8_t *blk = 0;
	uint32_t ptrs = g_block_size / 4u;
	uint32_t dirty = 0;

	if (!in || in->block[13] != 0 || in->block[14] != 0)
		return -1;

	for (uint32_t i = 0; i < EXT3_NDIR_BLOCKS; i++) {
		if (i >= keep_blocks && in->block[i] != 0) {
			if (ext3_free_block(in->block[i]) != 0)
				return -1;
			in->block[i] = 0;
			ext3_inode_blocks_sub(in, 1);
		}
	}

	if (in->block[12] == 0)
		return 0;

	blk = (uint8_t *)kmalloc(g_block_size);
	if (!blk)
		return -1;
	if (ext3_read_block(in->block[12], blk) != 0) {
		kfree(blk);
		return -1;
	}
	for (uint32_t i = 0; i < ptrs; i++) {
		uint32_t logical = EXT3_NDIR_BLOCKS + i;
		uint32_t b = ((uint32_t *)blk)[i];
		if (logical >= keep_blocks && b != 0) {
			if (ext3_free_block(b) != 0) {
				kfree(blk);
				return -1;
			}
			((uint32_t *)blk)[i] = 0;
			ext3_inode_blocks_sub(in, 1);
			dirty = 1;
		}
	}
	if (keep_blocks <= EXT3_NDIR_BLOCKS) {
		kfree(blk);
		if (ext3_free_block(in->block[12]) != 0)
			return -1;
		in->block[12] = 0;
		ext3_inode_blocks_sub(in, 1);
		return 0;
	}
	if (dirty && ext3_write_block(in->block[12], blk) != 0) {
		kfree(blk);
		return -1;
	}
	kfree(blk);
	return 0;
}

static int ext3_read(void *ctx,
                     uint32_t inode_num,
                     uint32_t offset,
                     uint8_t *buf,
                     uint32_t count)
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

static int ext3_dir_lookup(uint32_t dir_ino,
                           const char *name,
                           uint32_t *ino_out,
                           uint8_t *type_out)
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
			    k_memcmp(blk + off + sizeof(ext3_dirent_t), name, want_len) ==
			        0) {
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

static int ext3_read_symlink_target_ino(uint32_t ino, char *out, uint32_t outsz)
{
	ext3_inode_t in;
	uint32_t size;
	int n;

	if (!out || outsz == 0)
		return -1;
	if (ext3_read_inode(ino, &in) != 0 ||
	    (in.mode & EXT3_S_IFMT) != EXT3_S_IFLNK)
		return -1;
	size = ext3_inode_size(&in);
	if (size + 1u > outsz)
		return -1;
	if (size <= 60u) {
		k_memcpy(out, in.block, size);
		out[size] = '\0';
		return (int)size;
	}
	n = ext3_read(0, ino, 0, (uint8_t *)out, size);
	if (n != (int)size)
		return -1;
	out[size] = '\0';
	return n;
}

static int ext3_symlink_target_path(const char *link_path,
                                    const char *target,
                                    char *out,
                                    uint32_t outsz)
{
	const char *slash;
	uint32_t len;
	uint32_t parent_len;
	uint32_t target_len;

	if (!link_path || !target || !out || outsz == 0 || target[0] == '\0')
		return -1;
	if (target[0] == '/') {
		len = k_strlen(target);
		if (len + 1u > outsz)
			return -1;
		k_memcpy(out, target, len + 1u);
		return 0;
	}

	slash = k_strrchr(link_path, '/');
	if (!slash) {
		len = k_strlen(target);
		if (len + 1u > outsz)
			return -1;
		k_memcpy(out, target, len + 1u);
		return 0;
	}

	parent_len = (uint32_t)(slash - link_path);
	target_len = k_strlen(target);
	if (parent_len + 1u + target_len + 1u > outsz)
		return -1;
	k_memcpy(out, link_path, parent_len);
	out[parent_len] = '/';
	k_memcpy(out + parent_len + 1u, target, target_len + 1u);
	return 0;
}

static int ext3_symlink_target_path_with_tail(const char *link_path,
                                              const char *target,
                                              const char *tail,
                                              char *out,
                                              uint32_t outsz)
{
	uint32_t base_len;
	uint32_t tail_len;
	uint32_t need_sep = 1;

	if (ext3_symlink_target_path(link_path, target, out, outsz) != 0)
		return -1;
	while (tail && tail[0] == '/')
		tail++;
	if (!tail || tail[0] == '\0')
		return 0;

	base_len = k_strlen(out);
	tail_len = k_strlen(tail);
	if (base_len == 1 && out[0] == '/')
		need_sep = 0;
	if (base_len + need_sep + tail_len + 1u > outsz)
		return -1;
	if (need_sep)
		out[base_len++] = '/';
	k_memcpy(out + base_len, tail, tail_len + 1u);
	return 0;
}

static int ext3_resolve_follow(const char *path,
                               uint32_t follow_final,
                               uint32_t depth,
                               uint32_t *ino_out)
{
	uint32_t cur_dir = EXT3_ROOT_INO;
	const char *p;
	const char *path_start;

	if (!ino_out)
		return -1;
	if (!path || path[0] == '\0') {
		*ino_out = EXT3_ROOT_INO;
		return 0;
	}
	if (depth > 8)
		return -40;

	p = path;
	while (*p == '/')
		p++;
	if (*p == '\0') {
		*ino_out = EXT3_ROOT_INO;
		return 0;
	}
	path_start = p;

	for (;;) {
		const char *slash = 0;
		const char *tail = "";
		uint32_t comp_len;
		uint32_t child_ino;
		ext3_inode_t child;
		char part[EXT3_NAME_MAX + 1];

		while (*p == '/')
			p++;
		if (*p == '\0') {
			*ino_out = cur_dir;
			return 0;
		}
		for (const char *q = p; *q; q++) {
			if (*q == '/') {
				slash = q;
				break;
			}
		}
		comp_len = slash ? (uint32_t)(slash - p) : k_strlen(p);
		if (comp_len == 0 || comp_len > EXT3_NAME_MAX)
			return -1;
		k_memcpy(part, p, comp_len);
		part[comp_len] = '\0';

		if (ext3_dir_lookup(cur_dir, part, &child_ino, 0) != 0 ||
		    ext3_read_inode(child_ino, &child) != 0)
			return -1;

		if ((child.mode & EXT3_S_IFMT) == EXT3_S_IFLNK &&
		    (slash || follow_final)) {
			char *link_path = (char *)kmalloc(4096);
			char *target = (char *)kmalloc(4096);
			char *next = (char *)kmalloc(4096);
			uint32_t link_len;
			int rc;

			if (!link_path || !target || !next) {
				if (link_path)
					kfree(link_path);
				if (target)
					kfree(target);
				if (next)
					kfree(next);
				return -1;
			}

			link_len = slash ? (uint32_t)(slash - path_start)
			                 : (uint32_t)((p + comp_len) - path_start);
			if (link_len == 0 || link_len + 1u > 4096u) {
				kfree(link_path);
				kfree(target);
				kfree(next);
				return -1;
			}
			k_memcpy(link_path, path_start, link_len);
			link_path[link_len] = '\0';
			if (slash)
				tail = slash + 1;

			if (ext3_read_symlink_target_ino(child_ino, target, 4096) < 0 ||
			    ext3_symlink_target_path_with_tail(
			        link_path, target, tail, next, 4096) != 0) {
				kfree(link_path);
				kfree(target);
				kfree(next);
				return -1;
			}
			rc = ext3_resolve_follow(next, follow_final, depth + 1u, ino_out);
			kfree(link_path);
			kfree(target);
			kfree(next);
			return rc;
		}

		if (!slash) {
			*ino_out = child_ino;
			return 0;
		}
		if ((child.mode & EXT3_S_IFMT) != EXT3_S_IFDIR)
			return -1;
		cur_dir = child_ino;
		p = slash + 1;
	}
}

static int
ext3_resolve(const char *path, uint32_t follow_final, uint32_t *ino_out)
{
	return ext3_resolve_follow(path, follow_final, 0, ino_out);
}

static void ext3_stat_from_inode(const ext3_inode_t *in, vfs_stat_t *st)
{
	uint16_t kind = in->mode & EXT3_S_IFMT;

	st->type = kind == EXT3_S_IFDIR ? 2u : kind == EXT3_S_IFLNK ? 3u : 1u;
	st->size = ext3_inode_size(in);
	st->link_count = in->links_count;
	st->mtime = in->mtime;
}

static int ext3_stat_common(const char *path, vfs_stat_t *st, uint32_t follow)
{
	uint32_t ino;
	ext3_inode_t in;
	int rc;

	if (!st)
		return -1;
	rc = ext3_resolve(path, follow, &ino);
	if (rc != 0)
		return rc;
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

static int
ext3_open(void *ctx, const char *path, uint32_t *inode_out, uint32_t *size_out)
{
	uint32_t ino;
	ext3_inode_t in;
	int rc;

	(void)ctx;
	if (!inode_out || !size_out)
		return -1;
	rc = ext3_resolve(path, 1, &ino);
	if (rc != 0)
		return rc;
	if (ext3_read_inode(ino, &in) != 0)
		return -1;
	if ((in.mode & EXT3_S_IFMT) != EXT3_S_IFREG)
		return -1;
	*inode_out = ino;
	*size_out = ext3_inode_size(&in);
	return 0;
}

static int ext3_getdents(void *ctx, const char *path, char *buf, uint32_t bufsz)
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
			const char *name =
			    (const char *)(blk + off + sizeof(ext3_dirent_t));
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

static int ext3_readlink(void *ctx, const char *path, char *buf, uint32_t bufsz)
{
	uint32_t ino;
	ext3_inode_t in;
	uint32_t size;
	int rc;

	(void)ctx;
	if (!buf || bufsz == 0)
		return -22;
	rc = ext3_resolve(path, 0, &ino);
	if (rc != 0)
		return rc == -40 ? -40 : -2;
	if (ext3_read_inode(ino, &in) != 0)
		return -1;
	if ((in.mode & EXT3_S_IFMT) != EXT3_S_IFLNK)
		return -22;
	size = ext3_inode_size(&in);
	if (size > bufsz)
		size = bufsz;
	if (size <= 60u) {
		k_memcpy(buf, in.block, size);
		return (int)size;
	}
	return ext3_read(ctx, ino, 0, (uint8_t *)buf, size);
}

static int ext3_split_parent(const char *path, uint32_t *dir_ino, char *leaf)
{
	char *parent;
	uint32_t len;
	uint32_t slash = 0xFFFFFFFFu;
	uint32_t leaf_len;
	int rc;

	if (!path || !path[0] || !dir_ino || !leaf)
		return -1;
	len = k_strlen(path);
	while (len > 0 && path[len - 1u] == '/')
		len--;
	if (len == 0)
		return -1;
	for (uint32_t i = 0; i < len; i++) {
		if (path[i] == '/')
			slash = i;
	}
	leaf_len = (slash == 0xFFFFFFFFu) ? len : len - slash - 1u;
	if (leaf_len == 0 || leaf_len > EXT3_NAME_MAX)
		return -1;
	k_memcpy(leaf, path + (slash == 0xFFFFFFFFu ? 0 : slash + 1u), leaf_len);
	leaf[leaf_len] = '\0';

	if (slash == 0xFFFFFFFFu) {
		*dir_ino = EXT3_ROOT_INO;
		return 0;
	}

	parent = (char *)kmalloc(len + 1u);
	if (!parent)
		return -1;
	k_memcpy(parent, path, slash);
	parent[slash] = '\0';
	rc = ext3_resolve(parent, 1, dir_ino);
	kfree(parent);
	return rc;
}

static void ext3_write_dirent(uint8_t *dst,
                              uint32_t ino,
                              const char *name,
                              uint8_t type,
                              uint16_t rec_len)
{
	ext3_dirent_t *de = (ext3_dirent_t *)dst;
	uint32_t len = k_strlen(name);

	de->inode = ino;
	de->rec_len = rec_len;
	de->name_len = (uint8_t)len;
	de->file_type = type;
	k_memcpy(dst + sizeof(ext3_dirent_t), name, len);
}

static int ext3_dir_add(uint32_t dir_ino,
                        const char *name,
                        uint32_t child_ino,
                        uint8_t file_type)
{
	ext3_inode_t dir;
	uint8_t *blk;
	uint32_t name_len;
	uint32_t need;
	uint32_t size;
	uint32_t pos = 0;
	uint32_t slot_phys = 0;
	uint32_t slot_off = 0;
	uint32_t slot_kind = 0; /* 1 = reuse empty, 2 = split spare */
	uint32_t slot_rec = 0;

	if (!ext3_can_mutate() || !name || !name[0] ||
	    ext3_read_inode(dir_ino, &dir) != 0 ||
	    (dir.mode & EXT3_S_IFMT) != EXT3_S_IFDIR)
		return -1;

	name_len = k_strlen(name);
	if (name_len > EXT3_NAME_MAX)
		return -1;
	need = ext3_dir_rec_len(name_len);
	size = ext3_inode_size(&dir);

	blk = (uint8_t *)kmalloc(g_block_size);
	if (!blk)
		return -1;

	/* Single pass: check for name collision and remember the first usable
     * slot. The old code ran ext3_dir_lookup first (full pass) and then
     * scanned again to place the entry. */
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
			uint32_t actual;
			uint32_t spare;

			if (de->rec_len < sizeof(ext3_dirent_t) ||
			    off + de->rec_len > g_block_size)
				break;
			if (de->inode != 0 && de->name_len == name_len &&
			    k_memcmp(blk + off + sizeof(ext3_dirent_t), name, name_len) ==
			        0)
				goto fail;

			if (slot_kind == 0) {
				if (de->inode == 0 && de->rec_len >= need) {
					slot_phys = phys;
					slot_off = base + off;
					slot_rec = de->rec_len;
					slot_kind = 1;
				} else {
					actual = ext3_dir_rec_len(de->name_len);
					spare = de->rec_len > actual ? de->rec_len - actual : 0;
					if (de->inode != 0 && spare >= need) {
						slot_phys = phys;
						slot_off = base + off;
						slot_rec = spare;
						slot_kind = 2;
					}
				}
			}
			off += de->rec_len;
		}
		pos = base + g_block_size;
	}

	if (slot_kind != 0) {
		uint32_t logical = slot_off / g_block_size;
		uint32_t off = slot_off - logical * g_block_size;

		if (ext3_read_block(slot_phys, blk) != 0)
			goto fail;
		if (slot_kind == 1) {
			ext3_write_dirent(
			    blk + off, child_ino, name, file_type, (uint16_t)slot_rec);
		} else {
			ext3_dirent_t *de = (ext3_dirent_t *)(blk + off);
			uint32_t actual = ext3_dir_rec_len(de->name_len);
			de->rec_len = (uint16_t)actual;
			ext3_write_dirent(blk + off + actual,
			                  child_ino,
			                  name,
			                  file_type,
			                  (uint16_t)slot_rec);
		}
		if (ext3_write_block(slot_phys, blk) != 0)
			goto fail;
		dir.mtime = dir.ctime = clock_unix_time();
		if (ext3_write_inode(dir_ino, &dir) != 0)
			goto fail;
		kfree(blk);
		return 0;
	}

	{
		uint32_t logical = size / g_block_size;
		uint32_t phys = ext3_ensure_block(&dir, logical);
		if (phys == 0)
			goto fail;
		k_memset(blk, 0, g_block_size);
		ext3_write_dirent(
		    blk, child_ino, name, file_type, (uint16_t)g_block_size);
		if (ext3_write_block(phys, blk) != 0)
			goto fail;
		dir.size = size + g_block_size;
		dir.mtime = dir.ctime = clock_unix_time();
		if (ext3_write_inode(dir_ino, &dir) != 0)
			goto fail;
	}

	kfree(blk);
	return 0;

fail:
	kfree(blk);
	return -1;
}

static int ext3_dir_remove(uint32_t dir_ino, const char *name)
{
	ext3_inode_t dir;
	uint8_t *blk;
	uint32_t size;
	uint32_t pos = 0;
	uint32_t want_len;

	if (!ext3_can_mutate() || !name || ext3_read_inode(dir_ino, &dir) != 0 ||
	    (dir.mode & EXT3_S_IFMT) != EXT3_S_IFDIR)
		return -1;
	want_len = k_strlen(name);
	blk = (uint8_t *)kmalloc(g_block_size);
	if (!blk)
		return -1;
	size = ext3_inode_size(&dir);
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
			    k_memcmp(blk + off + sizeof(ext3_dirent_t), name, want_len) ==
			        0) {
				de->inode = 0;
				if (ext3_write_block(phys, blk) != 0) {
					kfree(blk);
					return -1;
				}
				dir.mtime = dir.ctime = clock_unix_time();
				if (ext3_write_inode(dir_ino, &dir) != 0) {
					kfree(blk);
					return -1;
				}
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

static int ext3_write_body(void *ctx,
                           uint32_t inode_num,
                           uint32_t offset,
                           const uint8_t *buf,
                           uint32_t count)
{
	ext3_inode_t in;
	uint8_t *blk;
	uint32_t done = 0;

	(void)ctx;
	if (!ext3_can_mutate() || !buf || ext3_read_inode(inode_num, &in) != 0 ||
	    (in.mode & EXT3_S_IFMT) != EXT3_S_IFREG)
		return -1;
	if (count == 0)
		return 0;

	blk = (uint8_t *)kmalloc(g_block_size);
	if (!blk)
		return -1;
	while (done < count) {
		uint32_t cur = offset + done;
		uint32_t logical = cur / g_block_size;
		uint32_t block_off = cur % g_block_size;
		uint32_t chunk = g_block_size - block_off;
		uint32_t phys;

		if (chunk > count - done)
			chunk = count - done;
		phys = ext3_ensure_block(&in, logical);
		if (phys == 0)
			break;
		if (chunk != g_block_size) {
			if (ext3_read_block(phys, blk) != 0)
				break;
		} else {
			k_memset(blk, 0, g_block_size);
		}
		k_memcpy(blk + block_off, buf + done, chunk);
		if (ext3_write_block(phys, blk) != 0)
			break;
		done += chunk;
	}
	kfree(blk);

	if (done == 0)
		return -1;
	if (offset + done > ext3_inode_size(&in))
		in.size = offset + done;
	in.mtime = in.ctime = clock_unix_time();
	if (ext3_write_inode(inode_num, &in) != 0)
		return -1;
	return (int)done;
}

static int ext3_write(void *ctx,
                      uint32_t inode_num,
                      uint32_t offset,
                      const uint8_t *buf,
                      uint32_t count)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_write_body(ctx, inode_num, offset, buf, count);
	return ext3_tx_end(rc);
}

static int ext3_zero_range_body(void *ctx,
                                uint32_t inode_num,
                                uint32_t start,
                                uint32_t end)
{
	uint8_t zeros[128];
	uint32_t off = start;

	if (end <= start)
		return 0;
	k_memset(zeros, 0, sizeof(zeros));
	while (off < end) {
		uint32_t chunk = end - off;

		if (chunk > sizeof(zeros))
			chunk = sizeof(zeros);
		if (ext3_write_body(ctx, inode_num, off, zeros, chunk) != (int)chunk)
			return -1;
		off += chunk;
	}
	return 0;
}

static int ext3_truncate_body(void *ctx, uint32_t inode_num, uint32_t size)
{
	ext3_inode_t in;
	uint32_t old_size;
	uint32_t keep_blocks;

	(void)ctx;
	if (!ext3_can_mutate() || ext3_read_inode(inode_num, &in) != 0 ||
	    (in.mode & EXT3_S_IFMT) != EXT3_S_IFREG)
		return -1;
	old_size = ext3_inode_size(&in);
	if (old_size == size)
		return 0;
	if (size > old_size)
		return ext3_zero_range_body(ctx, inode_num, old_size, size);

	if (ext3_zero_range_body(ctx, inode_num, size, old_size) != 0)
		return -1;
	if (ext3_read_inode(inode_num, &in) != 0)
		return -1;

	keep_blocks = (size + g_block_size - 1u) / g_block_size;
	if (ext3_truncate_blocks(&in, keep_blocks) != 0)
		return -1;
	in.size = size;
	in.mtime = in.ctime = clock_unix_time();
	return ext3_write_inode(inode_num, &in);
}

static int ext3_truncate(void *ctx, uint32_t inode_num, uint32_t size)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_truncate_body(ctx, inode_num, size);
	return ext3_tx_end(rc);
}

static int ext3_create_body(void *ctx, const char *path)
{
	uint32_t dir_ino;
	uint32_t existing;
	char leaf[EXT3_NAME_MAX + 1];
	ext3_inode_t in;
	uint32_t ino;
	uint32_t now;

	(void)ctx;
	if (!ext3_can_mutate() || ext3_split_parent(path, &dir_ino, leaf) != 0)
		return -1;
	if (ext3_dir_lookup(dir_ino, leaf, &existing, 0) == 0) {
		if (ext3_read_inode(existing, &in) != 0 ||
		    (in.mode & EXT3_S_IFMT) != EXT3_S_IFREG)
			return -1;
		if (ext3_free_inode_payload(&in) != 0)
			return -1;
		in.size = 0;
		in.mtime = in.ctime = clock_unix_time();
		if (ext3_write_inode(existing, &in) != 0)
			return -1;
		return (int)existing;
	}

	ino = ext3_alloc_inode(0);
	if (ino == 0)
		return -1;
	k_memset(&in, 0, sizeof(in));
	now = clock_unix_time();
	in.mode = EXT3_S_IFREG | 0644u;
	in.atime = in.ctime = in.mtime = now;
	in.links_count = 1;
	if (ext3_write_inode(ino, &in) != 0) {
		ext3_free_inode(ino, 0);
		return -1;
	}
	if (ext3_dir_add(dir_ino, leaf, ino, EXT3_FT_REG_FILE) != 0) {
		ext3_free_inode(ino, 0);
		return -1;
	}
	return (int)ino;
}

static int ext3_create(void *ctx, const char *path)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_create_body(ctx, path);
	return ext3_tx_end(rc);
}

static int ext3_unlink_body(void *ctx, const char *path)
{
	uint32_t dir_ino;
	uint32_t ino;
	uint8_t type;
	char leaf[EXT3_NAME_MAX + 1];
	ext3_inode_t in;

	(void)ctx;
	if (!ext3_can_mutate() || ext3_split_parent(path, &dir_ino, leaf) != 0 ||
	    ext3_dir_lookup(dir_ino, leaf, &ino, &type) != 0 ||
	    ext3_read_inode(ino, &in) != 0)
		return -1;
	if ((in.mode & EXT3_S_IFMT) == EXT3_S_IFDIR)
		return -1;

	if (in.links_count > 0)
		in.links_count--;
	if (in.links_count == 0) {
		if (ext3_free_inode_payload(&in) != 0)
			return -1;
		in.dtime = clock_unix_time();
		if (ext3_write_inode(ino, &in) != 0)
			return -1;
		if (ext3_free_inode(ino, 0) != 0)
			return -1;
	} else if (ext3_write_inode(ino, &in) != 0) {
		return -1;
	}
	(void)type;
	return ext3_dir_remove(dir_ino, leaf);
}

static int ext3_unlink(void *ctx, const char *path)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_unlink_body(ctx, path);
	return ext3_tx_end(rc);
}

static int ext3_mkdir_body(void *ctx, const char *path)
{
	uint32_t parent_ino;
	uint32_t ino;
	uint32_t block;
	uint32_t ignored;
	uint32_t now;
	char leaf[EXT3_NAME_MAX + 1];
	ext3_inode_t dir;
	ext3_inode_t parent;
	uint8_t *blk;

	(void)ctx;
	if (!ext3_can_mutate() || ext3_split_parent(path, &parent_ino, leaf) != 0 ||
	    ext3_dir_lookup(parent_ino, leaf, &ignored, 0) == 0)
		return -1;

	ino = ext3_alloc_inode(1);
	if (ino == 0)
		return -1;
	block = ext3_alloc_block();
	if (block == 0) {
		ext3_free_inode(ino, 1);
		return -1;
	}

	blk = (uint8_t *)kmalloc(g_block_size);
	if (!blk) {
		ext3_free_block(block);
		ext3_free_inode(ino, 1);
		return -1;
	}
	k_memset(blk, 0, g_block_size);
	ext3_write_dirent(
	    blk, ino, ".", EXT3_FT_DIR, (uint16_t)ext3_dir_rec_len(1));
	ext3_write_dirent(blk + ext3_dir_rec_len(1),
	                  parent_ino,
	                  "..",
	                  EXT3_FT_DIR,
	                  (uint16_t)(g_block_size - ext3_dir_rec_len(1)));
	if (ext3_write_block(block, blk) != 0) {
		kfree(blk);
		ext3_free_block(block);
		ext3_free_inode(ino, 1);
		return -1;
	}
	kfree(blk);

	k_memset(&dir, 0, sizeof(dir));
	now = clock_unix_time();
	dir.mode = EXT3_S_IFDIR | 0755u;
	dir.size = g_block_size;
	dir.atime = dir.ctime = dir.mtime = now;
	dir.links_count = 2;
	dir.blocks = g_sectors_per_block;
	dir.block[0] = block;
	if (ext3_write_inode(ino, &dir) != 0) {
		ext3_free_block(block);
		ext3_free_inode(ino, 1);
		return -1;
	}
	if (ext3_dir_add(parent_ino, leaf, ino, EXT3_FT_DIR) != 0) {
		ext3_free_block(block);
		ext3_free_inode(ino, 1);
		return -1;
	}
	if (ext3_read_inode(parent_ino, &parent) == 0) {
		parent.links_count++;
		parent.ctime = parent.mtime = now;
		ext3_write_inode(parent_ino, &parent);
	}
	return 0;
}

static int ext3_dir_is_empty(uint32_t ino)
{
	ext3_inode_t dir;
	uint8_t *blk;
	uint32_t size;
	uint32_t pos = 0;

	if (ext3_read_inode(ino, &dir) != 0 ||
	    (dir.mode & EXT3_S_IFMT) != EXT3_S_IFDIR)
		return 0;
	size = ext3_inode_size(&dir);
	blk = (uint8_t *)kmalloc(g_block_size);
	if (!blk)
		return 0;
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
			const char *name =
			    (const char *)(blk + off + sizeof(ext3_dirent_t));
			if (de->rec_len < sizeof(ext3_dirent_t) ||
			    off + de->rec_len > g_block_size) {
				kfree(blk);
				return 0;
			}
			if (de->inode != 0 && !(de->name_len == 1 && name[0] == '.') &&
			    !(de->name_len == 2 && name[0] == '.' && name[1] == '.')) {
				kfree(blk);
				return 0;
			}
			off += de->rec_len;
		}
		pos = base + g_block_size;
	}
	kfree(blk);
	return 1;
}

static int ext3_mkdir(void *ctx, const char *path)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_mkdir_body(ctx, path);
	return ext3_tx_end(rc);
}

static int ext3_rmdir_body(void *ctx, const char *path)
{
	uint32_t parent_ino;
	uint32_t ino;
	char leaf[EXT3_NAME_MAX + 1];
	ext3_inode_t dir;
	ext3_inode_t parent;

	(void)ctx;
	if (!ext3_can_mutate() || ext3_split_parent(path, &parent_ino, leaf) != 0 ||
	    ext3_dir_lookup(parent_ino, leaf, &ino, 0) != 0 ||
	    !ext3_dir_is_empty(ino) || ext3_read_inode(ino, &dir) != 0)
		return -1;
	if (ext3_free_inode_blocks(&dir) != 0)
		return -1;
	dir.links_count = 0;
	dir.dtime = clock_unix_time();
	if (ext3_write_inode(ino, &dir) != 0)
		return -1;
	if (ext3_free_inode(ino, 1) != 0)
		return -1;
	if (ext3_dir_remove(parent_ino, leaf) != 0)
		return -1;
	if (ext3_read_inode(parent_ino, &parent) == 0 && parent.links_count > 0) {
		parent.links_count--;
		parent.ctime = parent.mtime = clock_unix_time();
		ext3_write_inode(parent_ino, &parent);
	}
	return 0;
}

static int ext3_rmdir(void *ctx, const char *path)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_rmdir_body(ctx, path);
	return ext3_tx_end(rc);
}

static uint8_t ext3_file_type_from_mode(uint16_t mode)
{
	switch (mode & EXT3_S_IFMT) {
	case EXT3_S_IFDIR:
		return EXT3_FT_DIR;
	case EXT3_S_IFLNK:
		return EXT3_FT_SYMLINK;
	default:
		return EXT3_FT_REG_FILE;
	}
}

static int ext3_unlink_existing_nondir(uint32_t dir_ino, const char *leaf)
{
	uint32_t ino;
	ext3_inode_t in;

	if (ext3_dir_lookup(dir_ino, leaf, &ino, 0) != 0)
		return 0;
	if (ext3_read_inode(ino, &in) != 0 ||
	    (in.mode & EXT3_S_IFMT) == EXT3_S_IFDIR)
		return -1;
	if (in.links_count > 0)
		in.links_count--;
	if (in.links_count == 0) {
		if (ext3_free_inode_payload(&in) != 0)
			return -1;
		in.dtime = clock_unix_time();
		if (ext3_write_inode(ino, &in) != 0)
			return -1;
		if (ext3_free_inode(ino, 0) != 0)
			return -1;
	} else if (ext3_write_inode(ino, &in) != 0) {
		return -1;
	}
	return ext3_dir_remove(dir_ino, leaf);
}

static int ext3_rename_body(void *ctx, const char *oldpath, const char *newpath)
{
	uint32_t old_dir;
	uint32_t new_dir;
	uint32_t old_ino;
	uint8_t old_type;
	char old_leaf[EXT3_NAME_MAX + 1];
	char new_leaf[EXT3_NAME_MAX + 1];

	(void)ctx;
	if (!ext3_can_mutate() ||
	    ext3_split_parent(oldpath, &old_dir, old_leaf) != 0 ||
	    ext3_split_parent(newpath, &new_dir, new_leaf) != 0 ||
	    ext3_dir_lookup(old_dir, old_leaf, &old_ino, &old_type) != 0)
		return -1;
	if (old_dir == new_dir && k_strcmp(old_leaf, new_leaf) == 0)
		return 0;
	if (ext3_unlink_existing_nondir(new_dir, new_leaf) != 0)
		return -1;
	if (ext3_dir_add(new_dir, new_leaf, old_ino, old_type) != 0)
		return -1;
	return ext3_dir_remove(old_dir, old_leaf);
}

static int ext3_rename(void *ctx, const char *oldpath, const char *newpath)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_rename_body(ctx, oldpath, newpath);
	return ext3_tx_end(rc);
}

static int ext3_link_body(void *ctx,
                          const char *oldpath,
                          const char *newpath,
                          uint32_t follow)
{
	uint32_t new_dir;
	uint32_t old_ino;
	uint32_t existing;
	char new_leaf[EXT3_NAME_MAX + 1];
	ext3_inode_t in;

	(void)ctx;
	if (!ext3_can_mutate() ||
	    ext3_resolve(oldpath, follow ? 1u : 0u, &old_ino) != 0 ||
	    ext3_split_parent(newpath, &new_dir, new_leaf) != 0 ||
	    ext3_dir_lookup(new_dir, new_leaf, &existing, 0) == 0 ||
	    ext3_read_inode(old_ino, &in) != 0 ||
	    (in.mode & EXT3_S_IFMT) == EXT3_S_IFDIR)
		return -1;
	if (ext3_dir_add(
	        new_dir, new_leaf, old_ino, ext3_file_type_from_mode(in.mode)) != 0)
		return -1;
	in.links_count++;
	in.ctime = clock_unix_time();
	return ext3_write_inode(old_ino, &in);
}

static int
ext3_link(void *ctx, const char *oldpath, const char *newpath, uint32_t follow)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_link_body(ctx, oldpath, newpath, follow);
	return ext3_tx_end(rc);
}

static int
ext3_symlink_body(void *ctx, const char *target, const char *linkpath)
{
	uint32_t dir_ino;
	uint32_t existing;
	uint32_t ino;
	uint32_t len;
	uint32_t now;
	char leaf[EXT3_NAME_MAX + 1];
	ext3_inode_t in;

	(void)ctx;
	if (!ext3_can_mutate() || !target || target[0] == '\0' ||
	    ext3_split_parent(linkpath, &dir_ino, leaf) != 0 ||
	    ext3_dir_lookup(dir_ino, leaf, &existing, 0) == 0)
		return -1;
	len = k_strlen(target);
	if (len > 60u)
		return -1;

	ino = ext3_alloc_inode(0);
	if (ino == 0)
		return -1;
	k_memset(&in, 0, sizeof(in));
	now = clock_unix_time();
	in.mode = EXT3_S_IFLNK | 0777u;
	in.atime = in.ctime = in.mtime = now;
	in.links_count = 1;
	in.size = len;
	k_memcpy(in.block, target, len);
	if (ext3_write_inode(ino, &in) != 0) {
		ext3_free_inode(ino, 0);
		return -1;
	}
	if (ext3_dir_add(dir_ino, leaf, ino, EXT3_FT_SYMLINK) != 0) {
		ext3_free_inode(ino, 0);
		return -1;
	}
	return 0;
}

static int ext3_symlink(void *ctx, const char *target, const char *linkpath)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_symlink_body(ctx, target, linkpath);
	return ext3_tx_end(rc);
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
                                   uint32_t logical,
                                   uint8_t *buf)
{
	uint32_t phys = ext3_block_index(journal, logical);

	if (phys == 0)
		return -1;
	return ext3_read_disk_block(phys, buf);
}

static int ext3_journal_write_block(const ext3_inode_t *journal,
                                    uint32_t logical,
                                    const uint8_t *buf)
{
	uint32_t phys = ext3_block_index(journal, logical);

	if (phys == 0 || !g_dev || !g_dev->write_sector)
		return -1;
	/* Journal blocks bypass the cache's write-back queue: the whole point of
     * the JBD protocol is that descriptor/data/commit blocks are on the
     * platter before we act on them. bcache_write_through writes the sectors
     * and keeps any existing cached copy coherent with the new contents. */
	return bcache_write_through(g_dev, phys * g_sectors_per_block, buf);
}

static void jbd_write_header(uint8_t *blk, uint32_t type, uint32_t seq)
{
	put_be32(blk, JBD_MAGIC);
	put_be32(blk + 4u, type);
	put_be32(blk + 8u, seq);
}

static int
jbd_update_super(const ext3_inode_t *journal, uint32_t start, uint32_t seq)
{
	uint8_t *blk = (uint8_t *)kmalloc(g_block_size);
	int rc;

	if (!blk)
		return -1;
	if (ext3_journal_read_block(journal, 0, blk) != 0) {
		kfree(blk);
		return -1;
	}
	if (seq != 0)
		put_be32(blk + 24u, seq);
	put_be32(blk + 28u, start);
	rc = ext3_journal_write_block(journal, 0, blk);
	kfree(blk);
	return rc;
}

static int jbd_write_transaction(const ext3_inode_t *journal,
                                 uint32_t seq,
                                 uint32_t maxlen)
{
	uint8_t *desc;
	uint8_t *data;
	uint32_t off = 12u;

	if (g_tx_count == 0)
		return 0;
	/* Bound against the JBD superblock's s_maxlen — the inode size can round
     * up past the journal's logical length on some mke2fs images, and writing
     * beyond maxlen stomps whatever follows the journal on disk. */
	if (maxlen < 3u || g_tx_count + 2u >= maxlen)
		return -1;
	if (12u + g_tx_count * 8u > g_block_size)
		return -1;

	desc = (uint8_t *)kmalloc(g_block_size);
	data = (uint8_t *)kmalloc(g_block_size);
	if (!desc || !data) {
		if (desc)
			kfree(desc);
		if (data)
			kfree(data);
		return -1;
	}

	k_memset(desc, 0, g_block_size);
	jbd_write_header(desc, JBD_DESCRIPTOR_BLOCK, seq);
	for (uint32_t i = 0; i < g_tx_count; i++) {
		uint32_t flags = JBD_FLAG_SAME_UUID;
		if (i + 1u == g_tx_count)
			flags |= JBD_FLAG_LAST_TAG;
		if (be32(g_tx[i].data) == JBD_MAGIC)
			flags |= JBD_FLAG_ESCAPE;
		put_be32(desc + off, g_tx[i].fs_block);
		put_be32(desc + off + 4u, flags);
		off += 8u;
	}
	if (ext3_journal_write_block(journal, 1u, desc) != 0) {
		kfree(desc);
		kfree(data);
		return -1;
	}

	for (uint32_t i = 0; i < g_tx_count; i++) {
		k_memcpy(data, g_tx[i].data, g_block_size);
		if (be32(data) == JBD_MAGIC)
			k_memset(data, 0, 4u);
		if (ext3_journal_write_block(journal, 2u + i, data) != 0) {
			kfree(desc);
			kfree(data);
			return -1;
		}
	}

	k_memset(desc, 0, g_block_size);
	jbd_write_header(desc, JBD_COMMIT_BLOCK, seq);
	if (ext3_journal_write_block(journal, 2u + g_tx_count, desc) != 0) {
		kfree(desc);
		kfree(data);
		return -1;
	}

	kfree(desc);
	kfree(data);
	return 0;
}

static int ext3_checkpoint_tx(void)
{
	for (uint32_t i = 0; i < g_tx_count; i++) {
		if (ext3_write_disk_block(g_tx[i].fs_block, g_tx[i].data) != 0)
			return -1;
	}
	return 0;
}

static int ext3_commit_tx(void)
{
	ext3_inode_t journal;
	ext3_super_t recover_super;
	uint8_t *jsb;
	uint32_t seq;
	uint32_t jmaxlen;

	if (g_tx_failed)
		return -1;
	/* Flush the deferred BG counter update into the tx buffer. The super is
     * flushed below anyway (for the RECOVER flag), so it already picks up
     * the updated free counts from g_super. */
	if (g_tx_counts_dirty) {
		if (ext3_flush_bg() != 0)
			return -1;
		g_tx_counts_dirty = 0;
	}
	if (g_tx_count == 0)
		return 0;
	if (g_super.journal_dev != 0 || g_super.journal_inum == 0)
		return -1;
	if (ext3_read_inode(g_super.journal_inum, &journal) != 0)
		return -1;

	jsb = (uint8_t *)kmalloc(g_block_size);
	if (!jsb)
		return -1;
	if (ext3_journal_read_block(&journal, 0, jsb) != 0) {
		kfree(jsb);
		return -1;
	}
	seq = be32(jsb + 24u);
	if (seq == 0)
		seq = 1u;
	jmaxlen = be32(jsb + 16u);
	kfree(jsb);
	if (jmaxlen == 0)
		return -1;

	recover_super = g_tx_has_snapshot ? g_tx_super_before : g_super;
	recover_super.feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
	if (ext3_flush_super_image_raw(&recover_super) != 0)
		return -1;

	g_super.feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
	if (ext3_flush_super() != 0)
		return -1;
	if (jbd_update_super(&journal, 1u, seq) != 0)
		return -1;
	if (jbd_write_transaction(&journal, seq, jmaxlen) != 0)
		return -1;
	if (ext3_checkpoint_tx() != 0)
		return -1;
	/* Checkpoint writes land in the cache as dirty entries. They must be on
     * the platter before we clear the journal's start field — otherwise a
     * crash here would leave the journal saying "no recovery needed" while
     * the home blocks still hold their pre-transaction contents. */
	if (bcache_sync(g_dev) != 0)
		return -1;
	if (jbd_update_super(&journal, 0u, seq + 1u) != 0)
		return -1;
	g_super.feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
	return ext3_flush_super_raw();
}

static int ext3_tx_begin(void)
{
	if (!g_writable || g_overlay_count != 0)
		return -1;
	if (g_tx_depth == 0) {
		ext3_tx_reset();
		g_tx_super_before = g_super;
		g_tx_has_snapshot = 1;
	}
	g_tx_depth++;
	return 0;
}

static int ext3_tx_end(int rc)
{
	int commit_rc = 0;

	if (g_tx_depth == 0)
		return rc;
	if (g_tx_depth > 1) {
		g_tx_depth--;
		return rc;
	}

	if (rc == 0 || rc > 0)
		commit_rc = ext3_commit_tx();
	if (commit_rc != 0) {
		/* Commit may have stopped at any step: before the descriptor, between
         * descriptor and commit record, mid-checkpoint, or after checkpoint
         * but before the final RECOVER-clear. Disk state is handled by replay
         * on the next mount, but in-memory g_super and g_bg have already
         * absorbed the aborted tx's allocations/frees. Restore them from the
         * snapshot (and reload g_bg from disk once the tx buffer is cleared)
         * and force the FS read-only so subsequent calls surface the failure
         * rather than proceeding from drifted free counts. */
		if (g_tx_has_snapshot)
			g_super = g_tx_super_before;
		rc = -1;
	}
	ext3_tx_reset();
	if (commit_rc != 0) {
		(void)ext3_load_bg();
		g_writable = 0;
		klog("EXT3", "commit failed, filesystem forced read-only");
	}
	return rc;
}

static int ext3_load_bg(void)
{
	uint8_t *blk = (uint8_t *)kmalloc(g_block_size);

	if (!blk)
		return -1;
	if (ext3_read_block(g_bgdt_block, blk) != 0) {
		kfree(blk);
		return -1;
	}
	k_memcpy(&g_bg, blk, sizeof(g_bg));
	kfree(blk);
	return 0;
}

static int ext3_checkpoint_replay(void)
{
	ext3_inode_t journal;

	if (g_overlay_count == 0)
		return 0;
	if (!g_dev->write_sector)
		return -1;
	for (uint32_t i = 0; i < g_overlay_count; i++) {
		if (ext3_write_disk_block(g_overlay[i].fs_block, g_overlay[i].data) !=
		    0)
			return -1;
	}
	/* Replay writes go through the cache like any other home-location
     * write. Flush them before advancing the journal superblock so that a
     * crash immediately after replay does not leave committed-but-not-home
     * blocks lost in dirty cache entries. */
	if (bcache_sync(g_dev) != 0)
		return -1;
	if (g_super.journal_inum == 0 ||
	    ext3_read_inode(g_super.journal_inum, &journal) != 0)
		return -1;
	if (jbd_update_super(&journal, 0u, 0u) != 0)
		return -1;
	g_super.feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
	if (ext3_flush_super_raw() != 0)
		return -1;
	ext3_overlay_clear();
	g_needs_recovery = 0;
	return ext3_load_bg();
}

static int
jbd_revoked(uint32_t block, const uint32_t *revokes, uint32_t revoke_count)
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
		if (ext3_journal_read_block(journal, tags[i].journal_block, data) !=
		    0) {
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

static int jbd_parse_descriptor(const uint8_t *blk,
                                uint32_t pos,
                                uint32_t first,
                                uint32_t maxlen,
                                jbd_tag_t *tags,
                                uint32_t *tag_count_out,
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

static int jbd_parse_revoke(const uint8_t *blk,
                            uint32_t seq,
                            uint32_t cur_seq,
                            uint32_t *revokes,
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
		if (tags)
			kfree(tags);
		if (revokes)
			kfree(revokes);
		if (blk)
			kfree(blk);
		return -1;
	}
	if (ext3_journal_read_block(&journal, 0, blk) != 0) {
		kfree(tags);
		kfree(revokes);
		kfree(blk);
		return -1;
	}
	if (be32(blk) != JBD_MAGIC || (be32(blk + 4u) != JBD_SUPERBLOCK_V1 &&
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
			if (jbd_parse_descriptor(
			        blk, pos, first, maxlen, tags, &tag_count, &after_data) !=
			    0) {
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
			if (active && jbd_parse_revoke(
			                  blk, seq, cur_seq, revokes, &revoke_count) != 0) {
				kfree(blk);
				kfree(tags);
				kfree(revokes);
				return -1;
			}
		} else if (type == JBD_COMMIT_BLOCK) {
			if (active && seq == cur_seq) {
				if (jbd_apply_transaction(
				        &journal, tags, tag_count, revokes, revoke_count) !=
				    0) {
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
	uint32_t allowed_incompat;
	uint32_t allowed_ro;

	(void)ctx;
	g_writable = 0;
	g_needs_recovery = 0;
	g_overlay_count = 0;
	g_dev = blkdev_get("sda1");
	if (!g_dev)
		return -1;
	if (ext3_read_bytes(
	        EXT3_SUPER_OFFSET, (uint8_t *)&g_super, sizeof(g_super)) != 0)
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
	if (g_super.blocks_count > g_super.blocks_per_group ||
	    g_super.inodes_count > g_super.inodes_per_group) {
		klog("EXT3", "multi-group filesystems unsupported");
		return -1;
	}

	allowed_incompat =
	    EXT3_FEATURE_INCOMPAT_FILETYPE | EXT3_FEATURE_INCOMPAT_RECOVER;
	if ((g_super.feature_incompat & ~allowed_incompat) != 0) {
		klog_hex("EXT3",
		         "unsupported incompat",
		         g_super.feature_incompat & ~allowed_incompat);
		return -1;
	}
	allowed_ro =
	    EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT3_FEATURE_RO_COMPAT_LARGE_FILE;
	if ((g_super.feature_ro_compat & ~allowed_ro) != 0) {
		klog_hex("EXT3",
		         "unsupported ro compat",
		         g_super.feature_ro_compat & ~allowed_ro);
		return -1;
	}

	g_needs_recovery =
	    (g_super.feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) != 0;
	g_bgdt_block = (g_block_size == 1024u) ? 2u : 1u;
	g_block_alloc_cursor = g_super.first_data_block;
	g_inode_alloc_cursor = g_super.first_ino ? g_super.first_ino : 11u;
	g_tx_counts_dirty = 0;
	if (ext3_load_bg() != 0)
		return -1;

	if (ext3_replay_journal() != 0)
		return -1;
	if (g_overlay_count != 0 && ext3_checkpoint_replay() != 0)
		klog("EXT3", "journal checkpoint failed; keeping writes disabled");

	if (!g_needs_recovery && g_overlay_count == 0 && g_dev->write_sector) {
		g_writable = 1;
	} else {
		klog("EXT3", "write support disabled for this image");
	}

	klog_uint("EXT3", "block size", g_block_size);
	klog_uint("EXT3", "inode table", g_bg.inode_table);
	return 0;
}

static const fs_ops_t ext3_ops = {
    .init = ext3_init,
    .open = ext3_open,
    .getdents = ext3_getdents,
    .create = ext3_create,
    .unlink = ext3_unlink,
    .mkdir = ext3_mkdir,
    .rmdir = ext3_rmdir,
    .rename = ext3_rename,
    .link = ext3_link,
    .symlink = ext3_symlink,
    .readlink = ext3_readlink,
    .stat = ext3_stat,
    .lstat = ext3_lstat,
    .read = ext3_read,
    .write = ext3_write,
    .truncate = ext3_truncate,
    .flush = 0,
};

void ext3_register(void)
{
	vfs_register("ext3", &ext3_ops);
}
