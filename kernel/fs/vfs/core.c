/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * core.c - VFS registry, mount tree, path helpers, and synthetic devfs.
 */

#include "internal.h"
#include "blkdev.h"
#include "kheap.h"
#include "kstring.h"
#include "procfs.h"
#include "sysfs.h"

vfs_reg_t vfs_table[VFS_MAX_FS];
vfs_mount_t vfs_mounts[VFS_MAX_MOUNTS];

const devfs_entry_t devfs_entries[] = {
    {"stdin", VFS_NODE_TTY, 0, 0},
    {"tty0", VFS_NODE_TTY, 0, 0},
    {"fb0", VFS_NODE_CHARDEV, 0, "fb0"},
    {"fb0info", VFS_NODE_CHARDEV, 0, "fb0info"},
    {"kbd", VFS_NODE_CHARDEV, 0, "kbd"},
    {"mouse", VFS_NODE_CHARDEV, 0, "mouse"},
    {"wm", VFS_NODE_CHARDEV, 0, "wm"},
};
const uint32_t devfs_entry_count =
    sizeof(devfs_entries) / sizeof(devfs_entries[0]);

static int devfs_fill_blockdev_node(const char *relpath, vfs_node_t *node_out)
{
	blkdev_info_t info;
	uint32_t count = blkdev_count();

	if (!relpath || !node_out)
		return -1;

	for (uint32_t i = 0; i < count; i++) {
		if (blkdev_info_at(i, &info) != 0)
			continue;
		if (k_strcmp(relpath, info.name) != 0)
			continue;

		node_out->type = VFS_NODE_BLOCKDEV;
		node_out->dev_id = i;
		node_out->size = info.sectors * info.sector_size;
		k_strncpy(node_out->dev_name, info.name, VFS_DEV_NAME_MAX - 1);
		node_out->dev_name[VFS_DEV_NAME_MAX - 1] = '\0';
		return 0;
	}

	return -1;
}

static int vfs_normalize_path(const char *path, char *out, uint32_t outsz)
{
	uint32_t w = 0;

	if (!out || outsz == 0)
		return -1;

	if (!path || path[0] == '\0') {
		out[0] = '\0';
		return 0;
	}

	while (*path) {
		const char *start;
		uint32_t len = 0;

		while (*path == '/')
			path++;
		if (*path == '\0')
			break;

		start = path;
		while (path[len] && path[len] != '/')
			len++;

		if (len == 1 && start[0] == '.') {
			path += len;
			continue;
		}

		if (len == 2 && start[0] == '.' && start[1] == '.') {
			if (w > 0) {
				while (w > 0 && out[w - 1] != '/')
					w--;
				if (w > 0)
					w--;
			}
			path += len;
			continue;
		}

		if (w != 0) {
			if (w + 1 >= outsz)
				return -1;
			out[w++] = '/';
		}
		if (w + len >= outsz)
			return -1;
		k_memcpy(out + w, start, len);
		w += len;
		path += len;
	}

	out[w] = '\0';
	return 0;
}

char *vfs_normalize_alloc(const char *path)
{
	uint32_t len = path ? k_strlen(path) : 0;
	char *norm = (char *)kmalloc(len + 1);

	if (!norm)
		return 0;
	if (vfs_normalize_path(path, norm, len + 1) != 0) {
		kfree(norm);
		return 0;
	}
	return norm;
}

int vfs_has_mounts(void)
{
	for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (vfs_mounts[i].in_use)
			return 1;
	}
	return 0;
}

int vfs_find_mount_exact(const char *norm_path)
{
	for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (!vfs_mounts[i].in_use)
			continue;
		if (k_strcmp(vfs_mounts[i].path, norm_path) == 0)
			return i;
	}
	return -1;
}

static int vfs_mount_matches(const vfs_mount_t *mnt, const char *norm_path)
{
	if (!mnt || !mnt->in_use)
		return 0;
	if (mnt->path_len == 0)
		return 1;
	if (k_strncmp(norm_path, mnt->path, mnt->path_len) != 0)
		return 0;
	return norm_path[mnt->path_len] == '\0' || norm_path[mnt->path_len] == '/';
}

int vfs_find_mount_for_path(const char *norm_path)
{
	int best = -1;
	uint32_t best_len = 0;

	for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (!vfs_mount_matches(&vfs_mounts[i], norm_path))
			continue;
		if (best < 0 || vfs_mounts[i].path_len > best_len) {
			best = i;
			best_len = vfs_mounts[i].path_len;
		}
	}
	return best;
}

const char *vfs_relpath(const vfs_mount_t *mnt, const char *norm_path)
{
	if (!mnt || !mnt->in_use)
		return 0;
	if (mnt->path_len == 0)
		return norm_path;
	if (norm_path[mnt->path_len] == '\0')
		return norm_path + mnt->path_len;
	return norm_path + mnt->path_len + 1;
}

void vfs_lookup_ctx_clear(vfs_lookup_ctx_t *ctx)
{
	if (!ctx)
		return;
	if (ctx->norm)
		kfree(ctx->norm);
	ctx->norm = 0;
	ctx->mount_idx = -1;
	ctx->mnt = 0;
	ctx->rel = 0;
}

/* Callers that keep the context after init must clear it, even on failure. */
int vfs_lookup_ctx_init(vfs_lookup_ctx_t *ctx, const char *path)
{
	if (!ctx)
		return -1;

	ctx->norm = vfs_normalize_alloc(path);
	ctx->mount_idx = -1;
	ctx->mnt = 0;
	ctx->rel = 0;

	if (!ctx->norm)
		return -1;

	ctx->mount_idx = vfs_find_mount_for_path(ctx->norm);
	if (ctx->mount_idx < 0)
		return -1;

	ctx->mnt = &vfs_mounts[ctx->mount_idx];
	ctx->rel = vfs_relpath(ctx->mnt, ctx->norm);
	return 0;
}

int vfs_mutation_lookup_ctx_init(vfs_lookup_ctx_t *ctx, const char *path)
{
	if (vfs_lookup_ctx_init(ctx, path) != 0) {
		vfs_lookup_ctx_clear(ctx);
		return -1;
	}
	if (ctx->norm[0] == '\0' || vfs_find_mount_exact(ctx->norm) >= 0) {
		vfs_lookup_ctx_clear(ctx);
		return -1;
	}
	return 0;
}

void vfs_dir_stat(vfs_stat_t *st)
{
	st->type = VFS_STAT_TYPE_DIR;
	st->size = 0;
	st->link_count = 1;
	st->mtime = 0;
}

void vfs_filelike_stat(vfs_stat_t *st)
{
	st->type = VFS_STAT_TYPE_FILE;
	st->size = 0;
	st->link_count = 1;
	st->mtime = 0;
}

void vfs_blockdev_stat(vfs_stat_t *st, uint32_t size)
{
	st->type = VFS_STAT_TYPE_BLOCKDEV;
	st->size = size;
	st->link_count = 1;
	st->mtime = 0;
}

static int devfs_fill_pty_node(const char *relpath, vfs_node_t *node_out)
{
	if (k_strcmp(relpath, "ptmx") == 0) {
		node_out->type = VFS_NODE_PTYMASTER;
		return 0;
	}
	if (relpath[0] == 'p' && relpath[1] == 't' && relpath[2] == 's') {
		const char *digits = relpath + 3;
		uint32_t n = 0;

		if (*digits == '\0')
			return -1;
		while (*digits) {
			if (*digits < '0' || *digits > '9')
				return -1;
			n = n * 10u + (uint32_t)(*digits - '0');
			digits++;
		}
		node_out->type = VFS_NODE_PTYSLAVE;
		node_out->dev_id = n;
		return 0;
	}
	return -1;
}

int devfs_fill_node(const char *relpath, vfs_node_t *node_out)
{
	if (!node_out)
		return -1;

	node_out->inode_num = 0;
	node_out->mount_id = 0;
	node_out->size = 0;
	node_out->dev_id = 0;
	node_out->dev_name[0] = '\0';
	node_out->proc_kind = 0;
	node_out->proc_pid = 0;
	node_out->proc_index = 0;

	if (!relpath || relpath[0] == '\0') {
		node_out->type = VFS_NODE_DIR;
		return 0;
	}

	for (uint32_t i = 0; i < sizeof(devfs_entries) / sizeof(devfs_entries[0]);
	     i++) {
		const devfs_entry_t *ent = &devfs_entries[i];
		if (k_strcmp(relpath, ent->name) != 0)
			continue;

		node_out->type = ent->type;
		node_out->dev_id = ent->dev_id;
		if (ent->dev_name) {
			k_strncpy(node_out->dev_name, ent->dev_name, VFS_DEV_NAME_MAX - 1);
			node_out->dev_name[VFS_DEV_NAME_MAX - 1] = '\0';
		}
		return 0;
	}

	if (devfs_fill_pty_node(relpath, node_out) == 0)
		return 0;

	if (devfs_fill_blockdev_node(relpath, node_out) == 0)
		return 0;

	return -1;
}

int devfs_stat(const char *relpath, vfs_stat_t *st)
{
	vfs_node_t node;

	if (devfs_fill_node(relpath, &node) != 0)
		return -1;
	if (node.type == VFS_NODE_DIR)
		vfs_dir_stat(st);
	else if (node.type == VFS_NODE_BLOCKDEV)
		vfs_blockdev_stat(st, node.size);
	else {
		vfs_filelike_stat(st);
	}
	return 0;
}

int vfs_append_dirent(
    char *buf, uint32_t bufsz, uint32_t *written, const char *name, int is_dir)
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

int vfs_mount_child_name(const char *dir_path,
                         const vfs_mount_t *mnt,
                         const char **name_out,
                         uint32_t *name_len_out)
{
	const char *name;
	uint32_t dir_len;

	if (!mnt || !mnt->in_use || mnt->path_len == 0)
		return 0;

	dir_len = k_strlen(dir_path);
	if (dir_len == 0) {
		name = mnt->path;
	} else {
		if (k_strncmp(mnt->path, dir_path, dir_len) != 0)
			return 0;
		if (mnt->path[dir_len] != '/')
			return 0;
		name = mnt->path + dir_len + 1;
	}

	if (*name == '\0' || k_strchr(name, '/') != 0)
		return 0;

	if (name_out)
		*name_out = name;
	if (name_len_out)
		*name_len_out = k_strlen(name);
	return 1;
}

int vfs_entry_shadowed(const char *dir_path, const char *entry)
{
	uint32_t entry_len = k_strlen(entry);

	if (entry_len > 0 && entry[entry_len - 1] == '/')
		entry_len--;

	for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
		const char *child_name;
		uint32_t child_len;

		if (!vfs_mount_child_name(
		        dir_path, &vfs_mounts[i], &child_name, &child_len))
			continue;
		if (child_len == entry_len &&
		    k_strncmp(child_name, entry, entry_len) == 0)
			return 1;
	}

	return 0;
}

int devfs_getdents(const char *relpath, char *buf, uint32_t bufsz)
{
	uint32_t written = 0;

	if (!buf)
		return -1;
	if (relpath && relpath[0] != '\0')
		return -1;

	for (uint32_t i = 0; i < sizeof(devfs_entries) / sizeof(devfs_entries[0]);
	     i++) {
		if (vfs_append_dirent(buf, bufsz, &written, devfs_entries[i].name, 0) !=
		    0)
			break;
	}
	for (uint32_t i = 0; i < blkdev_count(); i++) {
		blkdev_info_t info;

		if (blkdev_info_at(i, &info) != 0)
			continue;
		if (vfs_append_dirent(buf, bufsz, &written, info.name, 0) != 0)
			break;
	}
	return (int)written;
}

static const fs_ops_t *vfs_lookup_fs(const char *name, uint32_t *kind_out)
{
	if (k_strcmp(name, "devfs") == 0) {
		if (kind_out)
			*kind_out = VFS_MOUNT_KIND_DEVFS;
		return 0;
	}

	if (k_strcmp(name, "procfs") == 0) {
		if (kind_out)
			*kind_out = VFS_MOUNT_KIND_PROCFS;
		return 0;
	}

	if (k_strcmp(name, "sysfs") == 0) {
		if (kind_out)
			*kind_out = VFS_MOUNT_KIND_SYSFS;
		return 0;
	}

	for (int i = 0; i < VFS_MAX_FS; i++) {
		if (vfs_table[i].name[0] == '\0')
			continue;
		if (k_strcmp(vfs_table[i].name, name) == 0) {
			if (kind_out)
				*kind_out = VFS_MOUNT_KIND_FS;
			return vfs_table[i].ops;
		}
	}

	return 0;
}

static const char *vfs_default_source_for_kind(uint32_t kind,
                                               const char *fs_name)
{
	if (kind == VFS_MOUNT_KIND_PROCFS)
		return "proc";
	if (kind == VFS_MOUNT_KIND_SYSFS)
		return "sysfs";
	if (kind == VFS_MOUNT_KIND_DEVFS)
		return "devfs";
	return fs_name;
}

static const char *vfs_fstype_for_kind(uint32_t kind, const char *fs_name)
{
	if (kind == VFS_MOUNT_KIND_PROCFS)
		return "proc";
	if (kind == VFS_MOUNT_KIND_SYSFS)
		return "sysfs";
	if (kind == VFS_MOUNT_KIND_DEVFS)
		return "devfs";
	return fs_name;
}

static const char *vfs_options_for_kind(uint32_t kind)
{
	if (kind == VFS_MOUNT_KIND_DEVFS)
		return "rw,nosuid";
	if (kind == VFS_MOUNT_KIND_PROCFS || kind == VFS_MOUNT_KIND_SYSFS)
		return "rw,nosuid,nodev,noexec,relatime";
	return "rw";
}

void vfs_reset(void)
{
	k_memset(vfs_table, 0, sizeof(vfs_table));
	k_memset(vfs_mounts, 0, sizeof(vfs_mounts));
}

int vfs_register(const char *name, const fs_ops_t *ops)
{
	if (!name || !ops)
		return -1;

	for (int i = 0; i < VFS_MAX_FS; i++) {
		if (vfs_table[i].name[0] == '\0')
			continue;
		if (k_strcmp(vfs_table[i].name, name) == 0)
			return vfs_table[i].ops == ops ? 0 : -1;
	}

	for (int i = 0; i < VFS_MAX_FS; i++) {
		if (vfs_table[i].name[0] != '\0')
			continue;
		k_strncpy(vfs_table[i].name, name, VFS_FS_NAME_MAX - 1);
		vfs_table[i].name[VFS_FS_NAME_MAX - 1] = '\0';
		vfs_table[i].ops = ops;
		return 0;
	}

	return -1;
}

int vfs_mount(const char *mount_path, const char *fs_name)
{
	return vfs_mount_with_source(mount_path, fs_name, 0);
}

int vfs_mount_with_source(const char *mount_path,
                          const char *fs_name,
                          const char *source)
{
	char norm[VFS_MOUNT_PATH_MAX];
	char parent[VFS_MOUNT_PATH_MAX];
	uint32_t kind = VFS_MOUNT_KIND_NONE;
	const fs_ops_t *ops;
	vfs_stat_t st;

	if (!fs_name)
		return -1;
	if (vfs_normalize_path(mount_path, norm, sizeof(norm)) != 0)
		return -1;
	if (vfs_find_mount_exact(norm) >= 0)
		return -1;

	ops = vfs_lookup_fs(fs_name, &kind);
	if (!ops && kind != VFS_MOUNT_KIND_DEVFS && kind != VFS_MOUNT_KIND_PROCFS &&
	    kind != VFS_MOUNT_KIND_SYSFS)
		return -1;

	if (norm[0] != '\0') {
		k_strncpy(parent, norm, sizeof(parent) - 1);
		parent[sizeof(parent) - 1] = '\0';
		{
			char *slash = k_strrchr(parent, '/');
			if (slash)
				*slash = '\0';
			else
				parent[0] = '\0';
		}

		if (vfs_stat(parent, &st) != 0 || st.type != 2)
			return -1;
	}

	if (kind == VFS_MOUNT_KIND_FS) {
		int rc = ops->init ? ops->init(ops->ctx) : 0;
		if (rc != 0)
			return rc;
	}

	for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (vfs_mounts[i].in_use)
			continue;
		vfs_mounts[i].in_use = 1;
		vfs_mounts[i].kind = kind;
		vfs_mounts[i].ops = ops;
		k_strncpy(vfs_mounts[i].source,
		          source ? source : vfs_default_source_for_kind(kind, fs_name),
		          sizeof(vfs_mounts[i].source) - 1);
		vfs_mounts[i].source[sizeof(vfs_mounts[i].source) - 1] = '\0';
		k_strncpy(vfs_mounts[i].fstype,
		          vfs_fstype_for_kind(kind, fs_name),
		          sizeof(vfs_mounts[i].fstype) - 1);
		vfs_mounts[i].fstype[sizeof(vfs_mounts[i].fstype) - 1] = '\0';
		k_strncpy(vfs_mounts[i].options,
		          vfs_options_for_kind(kind),
		          sizeof(vfs_mounts[i].options) - 1);
		vfs_mounts[i].options[sizeof(vfs_mounts[i].options) - 1] = '\0';
		k_strncpy(vfs_mounts[i].path, norm, sizeof(vfs_mounts[i].path) - 1);
		vfs_mounts[i].path[sizeof(vfs_mounts[i].path) - 1] = '\0';
		vfs_mounts[i].path_len = k_strlen(vfs_mounts[i].path);
		return 0;
	}

	return -1;
}

uint32_t vfs_mount_count(void)
{
	uint32_t count = 0;

	for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (vfs_mounts[i].in_use)
			count++;
	}
	return count;
}

int vfs_mount_info_at(uint32_t index, vfs_mount_info_t *out)
{
	uint32_t seen = 0;

	if (!out)
		return -1;

	for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (!vfs_mounts[i].in_use)
			continue;
		if (seen == index) {
			k_memset(out, 0, sizeof(*out));
			k_strncpy(
			    out->source, vfs_mounts[i].source, sizeof(out->source) - 1);
			if (vfs_mounts[i].path[0] == '\0') {
				out->path[0] = '/';
				out->path[1] = '\0';
			} else {
				out->path[0] = '/';
				k_strncpy(
				    out->path + 1, vfs_mounts[i].path, sizeof(out->path) - 2);
				out->path[sizeof(out->path) - 1] = '\0';
			}
			k_strncpy(
			    out->fstype, vfs_mounts[i].fstype, sizeof(out->fstype) - 1);
			k_strncpy(
			    out->options, vfs_mounts[i].options, sizeof(out->options) - 1);
			return 0;
		}
		seen++;
	}
	return -1;
}
