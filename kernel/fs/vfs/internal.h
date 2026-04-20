/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * internal.h - private VFS mount-tree structures and helper contracts.
 *
 * Shared by the VFS implementation files under kernel/fs/vfs/. Public VFS
 * callers should include ../vfs.h through the normal kernel include path.
 */

#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include "../vfs.h"
#include <stdint.h>

#define VFS_MOUNT_PATH_MAX 128

typedef struct {
	char name[VFS_FS_NAME_MAX];
	const fs_ops_t *ops;
} vfs_reg_t;

typedef enum {
	VFS_MOUNT_KIND_NONE = 0,
	VFS_MOUNT_KIND_FS = 1,
	VFS_MOUNT_KIND_DEVFS = 2,
	VFS_MOUNT_KIND_PROCFS = 3,
	VFS_MOUNT_KIND_SYSFS = 4,
} vfs_mount_kind_t;

typedef struct {
	uint32_t in_use;
	uint32_t kind;
	const fs_ops_t *ops;
	char source[VFS_MOUNT_SOURCE_MAX];
	char fstype[VFS_FS_NAME_MAX];
	char options[48];
	char path[VFS_MOUNT_PATH_MAX];
	uint32_t path_len;
} vfs_mount_t;

typedef struct {
	char *norm;
	int mount_idx;
	const vfs_mount_t *mnt;
	const char *rel;
} vfs_lookup_ctx_t;

typedef struct {
	const char *name;
	uint32_t type;
	uint32_t dev_id;
	const char *dev_name;
} devfs_entry_t;

extern vfs_reg_t vfs_table[VFS_MAX_FS];
extern vfs_mount_t vfs_mounts[VFS_MAX_MOUNTS];
extern const devfs_entry_t devfs_entries[];
extern const uint32_t devfs_entry_count;

char *vfs_normalize_alloc(const char *path);
int vfs_has_mounts(void);
int vfs_find_mount_exact(const char *norm_path);
int vfs_find_mount_for_path(const char *norm_path);
const char *vfs_relpath(const vfs_mount_t *mnt, const char *norm_path);
void vfs_lookup_ctx_clear(vfs_lookup_ctx_t *ctx);
int vfs_lookup_ctx_init(vfs_lookup_ctx_t *ctx, const char *path);
int vfs_mutation_lookup_ctx_init(vfs_lookup_ctx_t *ctx, const char *path);
void vfs_dir_stat(vfs_stat_t *st);
void vfs_filelike_stat(vfs_stat_t *st);
void vfs_blockdev_stat(vfs_stat_t *st, uint32_t size);
int devfs_fill_node(const char *relpath, vfs_node_t *node_out);
int devfs_stat(const char *relpath, vfs_stat_t *st);
int vfs_append_dirent(
    char *buf, uint32_t bufsz, uint32_t *written, const char *name, int is_dir);
int vfs_mount_child_name(const char *dir_path,
                         const vfs_mount_t *mnt,
                         const char **name_out,
                         uint32_t *name_len_out);
int vfs_entry_shadowed(const char *dir_path, const char *entry);
int devfs_getdents(const char *relpath, char *buf, uint32_t bufsz);

#endif
