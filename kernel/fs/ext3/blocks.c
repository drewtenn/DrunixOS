/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * blocks.c - ext3 block, inode, bitmap, and cache helpers.
 *
 * This file owns low-level block I/O, replay overlays, allocation maps,
 * inode persistence, and block mapping/truncation helpers.
 */

#include "ext3_internal.h"
#include "bcache.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include <stdint.h>

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

void ext3_tx_reset(void)
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

int ext3_read_block(uint32_t block, uint8_t *buf)
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

int ext3_write_block(uint32_t block, const uint8_t *buf)
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

int ext3_can_mutate(void)
{
	return g_writable && g_overlay_count == 0 && g_tx_failed == 0;
}

uint32_t ext3_dir_rec_len(uint32_t name_len)
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

int ext3_flush_super_image_raw(const ext3_super_t *super)
{
	if (!super)
		return -1;
	return ext3_write_bytes(
	    EXT3_SUPER_OFFSET, (const uint8_t *)super, sizeof(g_super));
}

int ext3_flush_super_raw(void)
{
	return ext3_flush_super_image_raw(&g_super);
}

int ext3_flush_super(void)
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

int ext3_flush_bg(void)
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

int ext3_overlay_put(uint32_t fs_block, const uint8_t *data)
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

void ext3_overlay_clear(void)
{
	for (uint32_t i = 0; i < g_overlay_count; i++) {
		if (g_overlay[i].data)
			kfree(g_overlay[i].data);
		g_overlay[i].fs_block = 0;
		g_overlay[i].data = 0;
	}
	g_overlay_count = 0;
}

int ext3_read_inode(uint32_t ino, ext3_inode_t *out)
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

int ext3_write_inode(uint32_t ino, const ext3_inode_t *in)
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

void ext3_inode_blocks_add(ext3_inode_t *in, uint32_t fs_blocks)
{
	in->blocks += fs_blocks * g_sectors_per_block;
}

void ext3_inode_blocks_sub(ext3_inode_t *in, uint32_t fs_blocks)
{
	uint32_t sectors = fs_blocks * g_sectors_per_block;
	in->blocks = in->blocks > sectors ? in->blocks - sectors : 0;
}

uint32_t ext3_alloc_block(void)
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

int ext3_free_block(uint32_t block)
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

uint32_t ext3_alloc_inode(uint32_t is_dir)
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

int ext3_free_inode(uint32_t ino, uint32_t was_dir)
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

uint32_t ext3_inode_size(const ext3_inode_t *in)
{
	uint32_t size = in->size;

	if ((in->mode & EXT3_S_IFMT) == EXT3_S_IFREG && in->dir_acl != 0) {
		/* Drunix is 32-bit; reject files whose high size word is non-zero. */
		return 0xFFFFFFFFu;
	}
	return size;
}

uint32_t ext3_block_index(const ext3_inode_t *in, uint32_t logical)
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

uint32_t ext3_ensure_block(ext3_inode_t *in, uint32_t logical)
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

int ext3_free_inode_blocks(ext3_inode_t *in)
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

int ext3_free_inode_payload(ext3_inode_t *in)
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

int ext3_truncate_blocks(ext3_inode_t *in, uint32_t keep_blocks)
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
