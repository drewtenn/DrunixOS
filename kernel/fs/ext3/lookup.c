/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * lookup.c - ext3 read, lookup, stat, and directory enumeration.
 *
 * This file owns read-only VFS operations, path resolution, symlink
 * following, stat packing, getdents, and readlink.
 */

#include "ext3_internal.h"
#include "vfs.h"
#include "kheap.h"
#include "kstring.h"
#include <stdint.h>

int ext3_read(void *ctx,
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

int ext3_dir_lookup(uint32_t dir_ino,
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

int ext3_resolve(const char *path, uint32_t follow_final, uint32_t *ino_out)
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

int ext3_stat(void *ctx, const char *path, vfs_stat_t *st)
{
	(void)ctx;
	return ext3_stat_common(path, st, 1);
}

int ext3_lstat(void *ctx, const char *path, vfs_stat_t *st)
{
	(void)ctx;
	return ext3_stat_common(path, st, 0);
}

int ext3_open(void *ctx,
              const char *path,
              uint32_t *inode_out,
              uint32_t *size_out)
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

int ext3_getdents(void *ctx, const char *path, char *buf, uint32_t bufsz)
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

int ext3_readlink(void *ctx, const char *path, char *buf, uint32_t bufsz)
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
