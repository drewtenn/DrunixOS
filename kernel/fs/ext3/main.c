/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ext3.c — native ext2/ext3-compatible filesystem backend.
 */

#include "../ext3.h"
#include "ext3_internal.h"
#include "vfs.h"
#include "blkdev.h"
#include "bcache.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include <stdint.h>

const blkdev_ops_t *g_dev;
ext3_super_t g_super;
ext3_group_desc_t g_bg;
uint32_t g_block_size;
uint32_t g_sectors_per_block;
uint32_t g_bgdt_block;
uint32_t g_writable;
uint32_t g_needs_recovery;
uint32_t g_block_alloc_cursor;
uint32_t g_inode_alloc_cursor;
uint32_t g_tx_counts_dirty;
ext3_overlay_block_t g_overlay[EXT3_REPLAY_MAX_BLOCKS];
uint32_t g_overlay_count;
ext3_tx_block_t g_tx[EXT3_TX_MAX_BLOCKS];
uint32_t g_tx_count;
uint32_t g_tx_depth;
uint32_t g_tx_failed;
ext3_super_t g_tx_super_before;
uint32_t g_tx_has_snapshot;

uint16_t be16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

int ext3_read_bytes(uint32_t byte_off, uint8_t *buf, uint32_t count)
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

int ext3_write_bytes(uint32_t byte_off, const uint8_t *buf, uint32_t count)
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

int ext3_read_disk_block(uint32_t block, uint8_t *buf)
{
	if (!g_dev || !buf || g_block_size == 0)
		return -1;
	return bcache_read(g_dev, block * g_sectors_per_block, buf);
}

int ext3_write_disk_block(uint32_t block, const uint8_t *buf)
{
	if (!g_dev || !g_dev->write_sector || !buf || g_block_size == 0)
		return -1;
	return bcache_write(g_dev, block * g_sectors_per_block, buf);
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
