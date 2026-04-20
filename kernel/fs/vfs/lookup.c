/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * lookup.c - VFS path resolution, file refs, I/O, and getdents.
 */

#include "internal.h"
#include "kheap.h"
#include "kstring.h"
#include "procfs.h"
#include "sysfs.h"

int vfs_resolve(const char *path, vfs_node_t *node_out)
{
	char *norm = vfs_normalize_alloc(path);
	int mount_idx;
	const vfs_mount_t *mnt;
	const char *rel;
	vfs_stat_t st;
	int rc;

	if (!norm || !node_out)
		return -1;

	node_out->type = VFS_NODE_NONE;
	node_out->inode_num = 0;
	node_out->mount_id = 0;
	node_out->size = 0;
	node_out->dev_id = 0;
	node_out->dev_name[0] = '\0';
	node_out->proc_kind = 0;
	node_out->proc_pid = 0;
	node_out->proc_index = 0;

	if (norm[0] == '\0') {
		if (!vfs_has_mounts()) {
			kfree(norm);
			return -1;
		}
		node_out->type = VFS_NODE_DIR;
		kfree(norm);
		return 0;
	}

	if (vfs_find_mount_exact(norm) >= 0) {
		node_out->type = VFS_NODE_DIR;
		kfree(norm);
		return 0;
	}

	mount_idx = vfs_find_mount_for_path(norm);
	if (mount_idx < 0) {
		kfree(norm);
		return -1;
	}

	mnt = &vfs_mounts[mount_idx];
	rel = vfs_relpath(mnt, norm);

	if (mnt->kind == VFS_MOUNT_KIND_DEVFS) {
		int rc = devfs_fill_node(rel, node_out);
		kfree(norm);
		return rc;
	}

	if (mnt->kind == VFS_MOUNT_KIND_PROCFS) {
		int rc = procfs_fill_node(rel, node_out);
		kfree(norm);
		return rc;
	}

	if (mnt->kind == VFS_MOUNT_KIND_SYSFS) {
		int rc = sysfs_fill_node(rel, node_out);

		if (rc == 0 && node_out->type == VFS_NODE_SYSFILE)
			node_out->mount_id = (uint32_t)mount_idx + 1u;
		kfree(norm);
		return rc;
	}

	if (mnt->ops->stat) {
		rc = mnt->ops->stat(mnt->ops->ctx, rel, &st);
		if (rc == 0 && st.type == 2) {
			node_out->type = VFS_NODE_DIR;
			kfree(norm);
			return 0;
		}
		if (rc < -1) {
			kfree(norm);
			return rc;
		}
	}

	rc = -1;
	if (!mnt->ops->open ||
	    (rc = mnt->ops->open(
	         mnt->ops->ctx, rel, &node_out->inode_num, &node_out->size)) != 0) {
		kfree(norm);
		return rc < -1 ? rc : -1;
	}

	node_out->type = VFS_NODE_FILE;
	node_out->mount_id = (uint32_t)mount_idx + 1u;
	kfree(norm);
	return 0;
}

int vfs_open_file(const char *path, vfs_file_ref_t *ref_out, uint32_t *size_out)
{
	vfs_node_t node;

	if (!ref_out || !size_out)
		return -1;
	if (vfs_resolve(path, &node) != 0 ||
	    (node.type != VFS_NODE_FILE && node.type != VFS_NODE_SYSFILE))
		return -1;

	ref_out->mount_id = node.mount_id;
	ref_out->inode_num = node.inode_num;
	*size_out = node.size;
	return 0;
}

static const fs_ops_t *vfs_ops_for_file_ref(vfs_file_ref_t ref)
{
	uint32_t idx;

	if (ref.mount_id == 0)
		return 0;
	idx = ref.mount_id - 1u;
	if (idx >= VFS_MAX_MOUNTS)
		return 0;
	if (!vfs_mounts[idx].in_use || vfs_mounts[idx].kind != VFS_MOUNT_KIND_FS)
		return 0;
	return vfs_mounts[idx].ops;
}

int vfs_read(vfs_file_ref_t ref, uint32_t offset, uint8_t *buf, uint32_t count)
{
	const fs_ops_t *ops = vfs_ops_for_file_ref(ref);

	if (ref.mount_id != 0) {
		uint32_t idx = ref.mount_id - 1u;

		if (idx < VFS_MAX_MOUNTS && vfs_mounts[idx].in_use &&
		    vfs_mounts[idx].kind == VFS_MOUNT_KIND_SYSFS)
			return sysfs_read_file(ref.inode_num, offset, (char *)buf, count);
	}

	if (!ops || !ops->read)
		return -1;
	return ops->read(ops->ctx, ref.inode_num, offset, buf, count);
}

int vfs_write(vfs_file_ref_t ref,
              uint32_t offset,
              const uint8_t *buf,
              uint32_t count)
{
	const fs_ops_t *ops = vfs_ops_for_file_ref(ref);

	if (!ops || !ops->write)
		return -1;
	return ops->write(ops->ctx, ref.inode_num, offset, buf, count);
}

int vfs_truncate(vfs_file_ref_t ref, uint32_t size)
{
	const fs_ops_t *ops = vfs_ops_for_file_ref(ref);

	if (!ops || !ops->truncate)
		return -1;
	return ops->truncate(ops->ctx, ref.inode_num, size);
}

int vfs_flush(vfs_file_ref_t ref)
{
	const fs_ops_t *ops = vfs_ops_for_file_ref(ref);

	if (!ops || !ops->flush)
		return -1;
	return ops->flush(ops->ctx, ref.inode_num);
}

int vfs_getdents(const char *path, char *buf, uint32_t bufsz)
{
	char *norm = vfs_normalize_alloc(path);
	char *tmp = 0;
	uint32_t written = 0;
	int mount_idx = -1;

	if (!norm || !buf)
		return -1;
	if (norm[0] == '\0' && !vfs_has_mounts()) {
		kfree(norm);
		return -1;
	}

	mount_idx = vfs_find_mount_for_path(norm);
	if (mount_idx >= 0) {
		const vfs_mount_t *mnt = &vfs_mounts[mount_idx];
		const char *rel = vfs_relpath(mnt, norm);
		int n = 0;

		if (mnt->kind == VFS_MOUNT_KIND_DEVFS) {
			n = devfs_getdents(rel, buf, bufsz);
			if (n < 0) {
				kfree(norm);
				return -1;
			}
			written = (uint32_t)n;
		} else if (mnt->kind == VFS_MOUNT_KIND_PROCFS) {
			n = procfs_getdents(rel, buf, bufsz);
			if (n < 0) {
				kfree(norm);
				return -1;
			}
			written = (uint32_t)n;
		} else if (mnt->kind == VFS_MOUNT_KIND_SYSFS) {
			n = sysfs_getdents(rel, buf, bufsz);
			if (n < 0) {
				kfree(norm);
				return -1;
			}
			written = (uint32_t)n;
		} else {
			tmp = (char *)kmalloc(bufsz ? bufsz : 1u);
			if (!tmp) {
				kfree(norm);
				return -1;
			}

			if (norm[0] != '\0' && rel[0] == '\0')
				n = mnt->ops->getdents(mnt->ops->ctx, 0, tmp, bufsz);
			else
				n = mnt->ops->getdents(
				    mnt->ops->ctx, rel[0] ? rel : 0, tmp, bufsz);

			if (n < 0) {
				kfree(tmp);
				kfree(norm);
				return -1;
			}

			for (int i = 0; i < n;) {
				char *entry = tmp + i;
				uint32_t ent_len = k_strlen(entry);

				if (!vfs_entry_shadowed(norm, entry)) {
					if (written + ent_len + 1 > bufsz)
						break;
					k_memcpy(buf + written, entry, ent_len + 1);
					written += ent_len + 1;
				}

				i += (int)ent_len + 1;
			}

			kfree(tmp);
		}
	} else if (norm[0] != '\0') {
		kfree(norm);
		return -1;
	}

	for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
		const char *child_name;

		if (!vfs_mount_child_name(norm, &vfs_mounts[i], &child_name, 0))
			continue;
		if (vfs_append_dirent(buf, bufsz, &written, child_name, 1) != 0)
			break;
	}

	kfree(norm);
	return (int)written;
}
