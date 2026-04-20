/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * mutation.c - VFS create, remove, link, rename, readlink, and stat.
 */

#include "internal.h"
#include "kheap.h"
#include "procfs.h"
#include "sysfs.h"

static int vfs_mutation_mount(const char *path,
                              char **norm_out,
                              const vfs_mount_t **mnt_out,
                              const char **rel_out)
{
	vfs_lookup_ctx_t ctx;

	ctx.norm = 0;
	if (!norm_out || !mnt_out || !rel_out)
		return -1;
	if (vfs_mutation_lookup_ctx_init(&ctx, path) != 0) {
		vfs_lookup_ctx_clear(&ctx);
		return -1;
	}

	*norm_out = ctx.norm;
	*mnt_out = ctx.mnt;
	*rel_out = ctx.rel;
	ctx.norm = 0;
	vfs_lookup_ctx_clear(&ctx);
	return 0;
}

int vfs_mkdir(const char *path)
{
	char *norm = 0;
	const vfs_mount_t *mnt = 0;
	const char *rel = 0;
	int rc;

	if (vfs_mutation_mount(path, &norm, &mnt, &rel) != 0)
		return -1;
	if (mnt->kind != VFS_MOUNT_KIND_FS || !mnt->ops->mkdir) {
		kfree(norm);
		return -1;
	}

	rc = mnt->ops->mkdir(mnt->ops->ctx, rel);
	kfree(norm);
	return rc;
}

int vfs_create(const char *path)
{
	char *norm = 0;
	const vfs_mount_t *mnt = 0;
	const char *rel = 0;
	int rc;

	if (vfs_mutation_mount(path, &norm, &mnt, &rel) != 0)
		return -1;
	if (mnt->kind != VFS_MOUNT_KIND_FS || !mnt->ops->create) {
		kfree(norm);
		return -1;
	}

	rc = mnt->ops->create(mnt->ops->ctx, rel);
	kfree(norm);
	return rc;
}

int vfs_unlink(const char *path)
{
	char *norm = 0;
	const vfs_mount_t *mnt = 0;
	const char *rel = 0;
	int rc;

	if (vfs_mutation_mount(path, &norm, &mnt, &rel) != 0)
		return -1;
	if (mnt->kind != VFS_MOUNT_KIND_FS || !mnt->ops->unlink) {
		kfree(norm);
		return -1;
	}

	rc = mnt->ops->unlink(mnt->ops->ctx, rel);
	kfree(norm);
	return rc;
}

int vfs_rmdir(const char *path)
{
	char *norm = 0;
	const vfs_mount_t *mnt = 0;
	const char *rel = 0;
	int rc;

	if (vfs_mutation_mount(path, &norm, &mnt, &rel) != 0)
		return -1;
	if (mnt->kind != VFS_MOUNT_KIND_FS || !mnt->ops->rmdir) {
		kfree(norm);
		return -1;
	}

	rc = mnt->ops->rmdir(mnt->ops->ctx, rel);
	kfree(norm);
	return rc;
}

int vfs_rename(const char *oldpath, const char *newpath)
{
	char *oldnorm = 0;
	char *newnorm = 0;
	const vfs_mount_t *oldmnt = 0;
	const vfs_mount_t *newmnt = 0;
	const char *oldrel = 0;
	const char *newrel = 0;
	int rc = -1;

	if (vfs_mutation_mount(oldpath, &oldnorm, &oldmnt, &oldrel) != 0)
		return -1;
	if (vfs_mutation_mount(newpath, &newnorm, &newmnt, &newrel) != 0) {
		kfree(oldnorm);
		return -1;
	}

	if (oldmnt != newmnt || oldmnt->kind != VFS_MOUNT_KIND_FS ||
	    !oldmnt->ops->rename)
		goto out;

	rc = oldmnt->ops->rename(oldmnt->ops->ctx, oldrel, newrel);

out:
	kfree(newnorm);
	kfree(oldnorm);
	return rc;
}

int vfs_link(const char *oldpath, const char *newpath, uint32_t follow)
{
	char *oldnorm = 0;
	char *newnorm = 0;
	const vfs_mount_t *oldmnt = 0;
	const vfs_mount_t *newmnt = 0;
	const char *oldrel = 0;
	const char *newrel = 0;
	int rc = -1;

	if (vfs_mutation_mount(oldpath, &oldnorm, &oldmnt, &oldrel) != 0)
		return -1;
	if (vfs_mutation_mount(newpath, &newnorm, &newmnt, &newrel) != 0) {
		kfree(oldnorm);
		return -1;
	}

	if (oldmnt != newmnt || oldmnt->kind != VFS_MOUNT_KIND_FS ||
	    !oldmnt->ops->link)
		goto out;

	rc = oldmnt->ops->link(oldmnt->ops->ctx, oldrel, newrel, follow);

out:
	kfree(newnorm);
	kfree(oldnorm);
	return rc;
}

int vfs_symlink(const char *target, const char *linkpath)
{
	char *norm = 0;
	const vfs_mount_t *mnt = 0;
	const char *rel = 0;
	int rc;

	if (!target || target[0] == '\0')
		return -1;
	if (vfs_mutation_mount(linkpath, &norm, &mnt, &rel) != 0)
		return -1;
	if (mnt->kind != VFS_MOUNT_KIND_FS || !mnt->ops->symlink) {
		kfree(norm);
		return -1;
	}

	rc = mnt->ops->symlink(mnt->ops->ctx, target, rel);
	kfree(norm);
	return rc;
}

int vfs_readlink(const char *path, char *buf, uint32_t bufsz)
{
	vfs_lookup_ctx_t ctx;
	int rc;

	ctx.norm = 0;
	if (!buf || bufsz == 0)
		return -22;
	if (vfs_lookup_ctx_init(&ctx, path) != 0) {
		rc = (!ctx.norm || ctx.norm[0] == '\0') ? -22 : -2;
		vfs_lookup_ctx_clear(&ctx);
		return rc;
	}
	if (ctx.norm[0] == '\0' || vfs_find_mount_exact(ctx.norm) >= 0) {
		vfs_lookup_ctx_clear(&ctx);
		return -22;
	}
	if (ctx.mnt->kind != VFS_MOUNT_KIND_FS || !ctx.mnt->ops->readlink) {
		vfs_lookup_ctx_clear(&ctx);
		return -22;
	}

	rc = ctx.mnt->ops->readlink(ctx.mnt->ops->ctx, ctx.rel, buf, bufsz);
	vfs_lookup_ctx_clear(&ctx);
	return rc;
}

static int vfs_stat_common(const char *path, vfs_stat_t *st, uint32_t follow)
{
	vfs_lookup_ctx_t ctx;
	int rc;

	ctx.norm = 0;
	if (!st)
		return -1;

	if (vfs_lookup_ctx_init(&ctx, path) != 0) {
		if (ctx.norm && ctx.norm[0] == '\0' && vfs_has_mounts()) {
			vfs_dir_stat(st);
			vfs_lookup_ctx_clear(&ctx);
			return 0;
		}
		vfs_lookup_ctx_clear(&ctx);
		return -1;
	}

	if (ctx.norm[0] == '\0' || vfs_find_mount_exact(ctx.norm) >= 0) {
		vfs_dir_stat(st);
		vfs_lookup_ctx_clear(&ctx);
		return 0;
	}

	if (ctx.mnt->kind == VFS_MOUNT_KIND_DEVFS) {
		rc = devfs_stat(ctx.rel, st);
		vfs_lookup_ctx_clear(&ctx);
		return rc;
	}

	if (ctx.mnt->kind == VFS_MOUNT_KIND_PROCFS) {
		rc = procfs_stat(ctx.rel, st);
		vfs_lookup_ctx_clear(&ctx);
		return rc;
	}

	if (ctx.mnt->kind == VFS_MOUNT_KIND_SYSFS) {
		rc = sysfs_stat(ctx.rel, st);
		vfs_lookup_ctx_clear(&ctx);
		return rc;
	}

	if (!follow && ctx.mnt->ops->lstat) {
		rc = ctx.mnt->ops->lstat(ctx.mnt->ops->ctx, ctx.rel, st);
		vfs_lookup_ctx_clear(&ctx);
		return rc;
	}

	if (ctx.mnt->ops->stat) {
		rc = ctx.mnt->ops->stat(ctx.mnt->ops->ctx, ctx.rel, st);
		vfs_lookup_ctx_clear(&ctx);
		return rc;
	}

	{
		uint32_t ino;
		uint32_t sz;

		rc = ctx.mnt->ops->open
		         ? ctx.mnt->ops->open(ctx.mnt->ops->ctx, ctx.rel, &ino, &sz)
		         : -1;
		if (rc == 0) {
			vfs_filelike_stat(st);
			st->size = sz;
		}
		vfs_lookup_ctx_clear(&ctx);
		return rc;
	}
}

int vfs_stat(const char *path, vfs_stat_t *st)
{
	return vfs_stat_common(path, st, 1);
}

int vfs_lstat(const char *path, vfs_stat_t *st)
{
	return vfs_stat_common(path, st, 0);
}
