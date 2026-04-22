/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * mutation.c - ext3 create, write, link, rename, and removal operations.
 *
 * This file owns mutating VFS operations and directory entry updates.
 */

#include "ext3_internal.h"
#include "vfs.h"
#include "arch.h"
#include "kheap.h"
#include "kstring.h"
#include <stdint.h>

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
		dir.mtime = dir.ctime = arch_time_unix_seconds();
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
		dir.mtime = dir.ctime = arch_time_unix_seconds();
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
				dir.mtime = dir.ctime = arch_time_unix_seconds();
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
	in.mtime = in.ctime = arch_time_unix_seconds();
	if (ext3_write_inode(inode_num, &in) != 0)
		return -1;
	return (int)done;
}

int ext3_write(void *ctx,
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
	in.mtime = in.ctime = arch_time_unix_seconds();
	return ext3_write_inode(inode_num, &in);
}

int ext3_truncate(void *ctx, uint32_t inode_num, uint32_t size)
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
		in.mtime = in.ctime = arch_time_unix_seconds();
		if (ext3_write_inode(existing, &in) != 0)
			return -1;
		return (int)existing;
	}

	ino = ext3_alloc_inode(0);
	if (ino == 0)
		return -1;
	k_memset(&in, 0, sizeof(in));
	now = arch_time_unix_seconds();
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

int ext3_create(void *ctx, const char *path)
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
		in.dtime = arch_time_unix_seconds();
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

int ext3_unlink(void *ctx, const char *path)
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
	now = arch_time_unix_seconds();
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

int ext3_mkdir(void *ctx, const char *path)
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
	dir.dtime = arch_time_unix_seconds();
	if (ext3_write_inode(ino, &dir) != 0)
		return -1;
	if (ext3_free_inode(ino, 1) != 0)
		return -1;
	if (ext3_dir_remove(parent_ino, leaf) != 0)
		return -1;
	if (ext3_read_inode(parent_ino, &parent) == 0 && parent.links_count > 0) {
		parent.links_count--;
		parent.ctime = parent.mtime = arch_time_unix_seconds();
		ext3_write_inode(parent_ino, &parent);
	}
	return 0;
}

int ext3_rmdir(void *ctx, const char *path)
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
		in.dtime = arch_time_unix_seconds();
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

int ext3_rename(void *ctx, const char *oldpath, const char *newpath)
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
	in.ctime = arch_time_unix_seconds();
	return ext3_write_inode(old_ino, &in);
}

int ext3_link(void *ctx,
              const char *oldpath,
              const char *newpath,
              uint32_t follow)
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
	now = arch_time_unix_seconds();
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

int ext3_symlink(void *ctx, const char *target, const char *linkpath)
{
	int rc;

	if (ext3_tx_begin() != 0)
		return -1;
	rc = ext3_symlink_body(ctx, target, linkpath);
	return ext3_tx_end(rc);
}
