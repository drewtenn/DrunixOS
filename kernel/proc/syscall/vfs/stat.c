/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * stat.c - Linux stat, statx, and statfs syscall support.
 *
 * This file owns Linux metadata packing and stat-family syscall cases.
 */

#include "../syscall_internal.h"
#include "../syscall_linux.h"
#include "arch.h"
#include "blkdev.h"
#include "kheap.h"
#include "kstring.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdint.h>

static void linux_fill_stat64(uint8_t *st,
                              uint32_t mode,
                              uint32_t nlink,
                              uint32_t size,
                              uint32_t mtime,
                              uint32_t rdev_major,
                              uint32_t rdev_minor,
                              uint64_t ino)
{
	uint32_t blocks;

	k_memset(st, 0, 144u);
	blocks = (size + 511u) / 512u;

	linux_put_u64(st, 0u, 1u);     /* st_dev */
	linux_put_u32(st, 16u, mode);  /* st_mode */
	linux_put_u32(st, 20u, nlink); /* st_nlink */
	linux_put_u32(st, 24u, 0u);    /* st_uid */
	linux_put_u32(st, 28u, 0u);    /* st_gid */
	linux_put_u64(st, 32u, linux_encode_dev(rdev_major, rdev_minor));
	linux_put_u64(st, 44u, size);   /* st_size */
	linux_put_u32(st, 52u, 4096u);  /* st_blksize */
	linux_put_u64(st, 56u, blocks); /* st_blocks */
	linux_put_u64(st, 88u, ino);    /* st_ino */
	linux_put_u64(st, 96u, mtime);  /* st_atim.tv_sec */
	linux_put_u64(st, 112u, mtime); /* st_mtim.tv_sec */
	linux_put_u64(st, 128u, mtime); /* st_ctim.tv_sec */
}

static void linux_fill_stat_arm64(uint8_t *st, const linux_fd_stat_t *meta)
{
	uint32_t blocks;

	k_memset(st, 0, 128u);
	blocks = (meta->size + 511u) / 512u;

	linux_put_u64(st, 0u, 1u); /* st_dev */
	linux_put_u64(st, 8u, meta->ino);
	linux_put_u32(st, 16u, meta->mode);
	linux_put_u32(st, 20u, meta->nlink);
	linux_put_u32(st, 24u, 0u); /* st_uid */
	linux_put_u32(st, 28u, 0u); /* st_gid */
	linux_put_u64(st, 32u, linux_encode_dev(meta->rdev_major, meta->rdev_minor));
	linux_put_u64(st, 48u, meta->size);
	linux_put_u32(st, 56u, 4096u); /* st_blksize */
	linux_put_u64(st, 64u, blocks);
	linux_put_u64(st, 72u, meta->mtime);  /* st_atim.tv_sec */
	linux_put_u64(st, 88u, meta->mtime);  /* st_mtim.tv_sec */
	linux_put_u64(st, 104u, meta->mtime); /* st_ctim.tv_sec */
}

static void linux_fill_statx_timestamp(uint8_t *stx, uint32_t off, uint32_t sec)
{
	linux_put_u64(stx, off, sec);      /* tv_sec */
	linux_put_u32(stx, off + 8u, 0u);  /* tv_nsec */
	linux_put_u32(stx, off + 12u, 0u); /* __reserved */
}

static void linux_fill_statx(uint8_t *stx, const linux_fd_stat_t *meta)
{
	uint32_t blocks;

	k_memset(stx, 0, 256u);
	blocks = (meta->size + 511u) / 512u;

	linux_put_u32(stx, 0u, LINUX_STATX_BASIC_STATS); /* stx_mask */
	linux_put_u32(stx, 4u, 4096u);                   /* stx_blksize */
	linux_put_u32(stx, 16u, meta->nlink);            /* stx_nlink */
	linux_put_u32(stx, 20u, 0u);                     /* stx_uid */
	linux_put_u32(stx, 24u, 0u);                     /* stx_gid */
	linux_put_u32(stx, 28u, meta->mode);             /* stx_mode */
	linux_put_u64(stx, 32u, meta->ino);              /* stx_ino */
	linux_put_u64(stx, 40u, meta->size);             /* stx_size */
	linux_put_u64(stx, 48u, blocks);                 /* stx_blocks */
	linux_fill_statx_timestamp(stx, 64u, meta->mtime);
	linux_fill_statx_timestamp(stx, 96u, meta->mtime);
	linux_fill_statx_timestamp(stx, 112u, meta->mtime);
	linux_put_u32(stx, 128u, meta->rdev_major); /* stx_rdev_major */
	linux_put_u32(stx, 132u, meta->rdev_minor); /* stx_rdev_minor */
	linux_put_u32(stx, 136u, 1u);               /* stx_dev_major */
	linux_put_u32(stx, 140u, 0u);               /* stx_dev_minor */
}

static int linux_blockdev_identity(const char *name,
                                   uint64_t *ino,
                                   uint32_t *major,
                                   uint32_t *minor)
{
	blkdev_info_t info;
	int idx;

	if (!name)
		return -1;
	idx = blkdev_find_index(name);
	if (idx < 0)
		return -1;
	if (blkdev_info_for_index((uint32_t)idx, &info) != 0)
		return -1;
	if (ino)
		*ino = 0x80000000u + (uint32_t)idx;
	if (major)
		*major = info.major;
	if (minor)
		*minor = info.minor;
	return 0;
}

static int
linux_fd_stat_metadata(process_t *cur, uint32_t fd, linux_fd_stat_t *meta)
{
	file_handle_t *fh;

	if (!cur || !meta || fd >= MAX_FDS)
		return -1;

	fh = &proc_fd_entries(cur)[fd];
	k_memset(meta, 0, sizeof(*meta));
	meta->nlink = 1;
	meta->size = 0;
	meta->mtime = arch_time_unix_seconds();
	meta->ino = fd + 1u;

	switch (fh->type) {
	case FD_TYPE_FILE:
		meta->mode = LINUX_S_IFREG | 0644u;
		meta->size = fh->u.file.size;
		meta->ino = fh->u.file.inode_num;
		break;
	case FD_TYPE_SYSFILE:
		meta->mode = LINUX_S_IFREG | 0444u;
		meta->size = fh->u.file.size;
		meta->ino = fh->u.file.inode_num;
		break;
	case FD_TYPE_PROCFILE:
		meta->mode = LINUX_S_IFREG | 0444u;
		meta->size = fh->u.proc.size;
		meta->ino = 0x70000000ull + fh->u.proc.kind * 1024u +
		            fh->u.proc.pid * 16u + fh->u.proc.index;
		break;
	case FD_TYPE_DIR:
		meta->mode = LINUX_S_IFDIR | 0755u;
		meta->nlink = 2;
		meta->ino = 0x60000000ull + fd;
		break;
	case FD_TYPE_CHARDEV:
	case FD_TYPE_TTY:
		meta->mode = LINUX_S_IFCHR | 0600u;
		break;
	case FD_TYPE_BLOCKDEV: {
		meta->mode = LINUX_S_IFBLK | 0444u;
		meta->size = fh->u.blockdev.size;
		(void)linux_blockdev_identity(fh->u.blockdev.name,
		                              &meta->ino,
		                              &meta->rdev_major,
		                              &meta->rdev_minor);
		break;
	}
	case FD_TYPE_PIPE_READ:
	case FD_TYPE_PIPE_WRITE:
		meta->mode = LINUX_S_IFIFO | 0600u;
		break;
	default:
		return -1;
	}

	return 0;
}

static void linux_metadata_from_vfs_stat(const vfs_stat_t *st,
                                         linux_fd_stat_t *meta)
{
	if (st->type == VFS_STAT_TYPE_DIR)
		meta->mode = LINUX_S_IFDIR | 0755u;
	else if (st->type == VFS_STAT_TYPE_SYMLINK)
		meta->mode = LINUX_S_IFLNK | 0777u;
	else if (st->type == VFS_STAT_TYPE_BLOCKDEV)
		meta->mode = LINUX_S_IFBLK | 0444u;
	else
		meta->mode = LINUX_S_IFREG | 0644u;
	meta->nlink = st->type == VFS_STAT_TYPE_DIR ? 2u : st->link_count;
	meta->size = st->size;
	meta->mtime = st->mtime;
	meta->ino = 1u;
}

static void linux_metadata_fix_blockdev_identity(const char *path,
                                                 const vfs_stat_t *st,
                                                 linux_fd_stat_t *meta)
{
	const char *base;

	if (!path || !st || !meta || st->type != VFS_STAT_TYPE_BLOCKDEV)
		return;

	base = k_strrchr(path, '/');
	base = base ? base + 1 : path;
	if (!base[0])
		return;

	(void)linux_blockdev_identity(
	    base, &meta->ino, &meta->rdev_major, &meta->rdev_minor);
}

static void linux_metadata_fix_sysfile_identity(const char *path,
                                                linux_fd_stat_t *meta)
{
	vfs_node_t node;

	if (!path || !meta)
		return;
	if (vfs_resolve(path, &node) != 0 || node.type != VFS_NODE_SYSFILE)
		return;

	meta->mode = LINUX_S_IFREG | 0444u;
	meta->size = node.size;
	meta->ino = node.inode_num;
}

static int linux_path_stat_metadata(process_t *cur,
                                    uint32_t user_path,
                                    linux_fd_stat_t *meta)
{
	char *rpath;
	vfs_stat_t st;

	if (!cur || !meta || user_path == 0)
		return -1;

	k_memset(meta, 0, sizeof(*meta));
	rpath = syscall_alloc_path_scratch();
	if (!rpath)
		return -1;
	if (resolve_user_path(cur, user_path, rpath, SYSCALL_PATH_MAX) != 0) {
		kfree(rpath);
		return -1;
	}
	if (vfs_stat(rpath, &st) != 0) {
		kfree(rpath);
		return -1;
	}

	linux_metadata_from_vfs_stat(&st, meta);
	linux_metadata_fix_sysfile_identity(rpath, meta);
	linux_metadata_fix_blockdev_identity(rpath, &st, meta);

	kfree(rpath);
	return 0;
}

static int linux_path_lstat_metadata(process_t *cur,
                                     uint32_t user_path,
                                     linux_fd_stat_t *meta)
{
	char *rpath;
	vfs_stat_t st;

	if (!cur || !meta || user_path == 0)
		return -1;

	k_memset(meta, 0, sizeof(*meta));
	rpath = syscall_alloc_path_scratch();
	if (!rpath)
		return -1;
	if (resolve_user_path(cur, user_path, rpath, SYSCALL_PATH_MAX) != 0) {
		kfree(rpath);
		return -1;
	}
	if (vfs_lstat(rpath, &st) != 0) {
		kfree(rpath);
		return -1;
	}

	linux_metadata_from_vfs_stat(&st, meta);
	if (st.type != VFS_STAT_TYPE_SYMLINK)
		linux_metadata_fix_sysfile_identity(rpath, meta);
	linux_metadata_fix_blockdev_identity(rpath, &st, meta);

	kfree(rpath);
	return 0;
}

static int linux_path_stat_metadata_at_flags(process_t *cur,
                                             uint32_t dirfd,
                                             uint32_t user_path,
                                             linux_fd_stat_t *meta,
                                             uint32_t nofollow)
{
	char *raw;
	char *rpath;
	vfs_stat_t st;

	if (!cur || !meta || user_path == 0)
		return -1;

	k_memset(meta, 0, sizeof(*meta));
	raw = copy_user_string_alloc(cur, user_path, SYSCALL_PATH_MAX);
	if (!raw)
		return -1;

	rpath = syscall_alloc_path_scratch();
	if (!rpath) {
		kfree(raw);
		return -1;
	}

	if (raw[0] == '/' || dirfd == LINUX_AT_FDCWD) {
		kcwd_resolve(syscall_process_cwd(cur), raw, rpath, SYSCALL_PATH_MAX);
	} else {
		file_handle_t *fh;

		if (dirfd >= MAX_FDS) {
			kfree(rpath);
			kfree(raw);
			return -1;
		}
		fh = &proc_fd_entries(cur)[dirfd];
		if (fh->type != FD_TYPE_DIR) {
			kfree(rpath);
			kfree(raw);
			return -1;
		}
		kcwd_resolve(fh->u.dir.path, raw, rpath, SYSCALL_PATH_MAX);
	}

	if ((nofollow ? vfs_lstat(rpath, &st) : vfs_stat(rpath, &st)) != 0) {
		kfree(rpath);
		kfree(raw);
		return -1;
	}

	linux_metadata_from_vfs_stat(&st, meta);
	if (st.type != VFS_STAT_TYPE_SYMLINK)
		linux_metadata_fix_sysfile_identity(rpath, meta);
	linux_metadata_fix_blockdev_identity(rpath, &st, meta);

	kfree(rpath);
	kfree(raw);
	return 0;
}

static int linux_path_stat_metadata_at(process_t *cur,
                                       uint32_t dirfd,
                                       uint32_t user_path,
                                       linux_fd_stat_t *meta)
{
	return linux_path_stat_metadata_at_flags(cur, dirfd, user_path, meta, 0);
}

static int
linux_copy_statfs64(process_t *cur, uint32_t user_buf, uint32_t user_size)
{
	uint8_t st[84];
	uint32_t copy_size;

	if (!cur || user_buf == 0 || user_size < sizeof(st))
		return -22;

	k_memset(st, 0, sizeof(st));
	linux_put_u32(st, 0u, 0x4452554Eu); /* f_type: "DRUN" */
	linux_put_u32(st, 4u, 4096u);       /* f_bsize */
	linux_put_u64(st, 8u, 12800u);      /* f_blocks */
	linux_put_u64(st, 16u, 6400u);      /* f_bfree */
	linux_put_u64(st, 24u, 6400u);      /* f_bavail */
	linux_put_u64(st, 32u, 4096u);      /* f_files */
	linux_put_u64(st, 40u, 2048u);      /* f_ffree */
	linux_put_u32(st, 56u, 255u);       /* f_namelen */
	linux_put_u32(st, 60u, 4096u);      /* f_frsize */

	copy_size = user_size < sizeof(st) ? user_size : sizeof(st);
	return uaccess_copy_to_user(cur, user_buf, st, copy_size) == 0 ? 0 : -1;
}

static int linux_copy_statfs_arm64(process_t *cur, uint32_t user_buf)
{
	uint8_t st[120];

	if (!cur || user_buf == 0)
		return -22;

	k_memset(st, 0, sizeof(st));
	linux_put_u64(st, 0u, 0x4452554Eu); /* f_type: "DRUN" */
	linux_put_u64(st, 8u, 4096u);       /* f_bsize */
	linux_put_u64(st, 16u, 12800u);     /* f_blocks */
	linux_put_u64(st, 24u, 6400u);      /* f_bfree */
	linux_put_u64(st, 32u, 6400u);      /* f_bavail */
	linux_put_u64(st, 40u, 4096u);      /* f_files */
	linux_put_u64(st, 48u, 2048u);      /* f_ffree */
	linux_put_u64(st, 64u, 255u);       /* f_namelen */
	linux_put_u64(st, 72u, 4096u);      /* f_frsize */
	return uaccess_copy_to_user(cur, user_buf, st, sizeof(st)) == 0 ? 0 : -1;
}

static uint32_t syscall_stat64_path_common(uint32_t user_path,
                                           uint32_t user_stat,
                                           uint32_t nofollow)
{
	process_t *cur = sched_current();
	uint8_t st64[144];
	linux_fd_stat_t meta;
	int rc;

	if (!cur || user_stat == 0)
		return (uint32_t)-1;
	rc = nofollow ? linux_path_lstat_metadata(cur, user_path, &meta)
	              : linux_path_stat_metadata(cur, user_path, &meta);
	if (rc != 0)
		return (uint32_t)-LINUX_ENOENT;

	linux_fill_stat64(st64,
	                  meta.mode,
	                  meta.nlink,
	                  meta.size,
	                  meta.mtime,
	                  meta.rdev_major,
	                  meta.rdev_minor,
	                  meta.ino);
	if (uaccess_copy_to_user(cur, user_stat, st64, sizeof(st64)) != 0)
		return (uint32_t)-1;
	return 0;
}

static uint32_t syscall_fstat64(uint32_t fd, uint32_t user_stat)
{
	process_t *cur = sched_current();
	uint8_t st64[144];
	linux_fd_stat_t meta;

	if (!cur || user_stat == 0 || linux_fd_stat_metadata(cur, fd, &meta) != 0)
		return (uint32_t)-1;

	linux_fill_stat64(st64,
	                  meta.mode,
	                  meta.nlink,
	                  meta.size,
	                  meta.mtime,
	                  meta.rdev_major,
	                  meta.rdev_minor,
	                  meta.ino);
	if (uaccess_copy_to_user(cur, user_stat, st64, sizeof(st64)) != 0)
		return (uint32_t)-1;
	return 0;
}

static uint32_t syscall_statx(uint32_t dirfd,
                              uint32_t user_path,
                              uint32_t flags,
                              uint32_t mask,
                              uint32_t user_statx)
{
	process_t *cur = sched_current();
	uint8_t stx[256];
	linux_fd_stat_t meta;
	char first;

	(void)mask;
	if (!cur || user_path == 0 || user_statx == 0)
		return (uint32_t)-1;
	if ((flags & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_NO_AUTOMOUNT |
	               LINUX_AT_EMPTY_PATH | LINUX_AT_STATX_SYNC_TYPE)) != 0)
		return (uint32_t)-1;
	if (uaccess_copy_from_user(cur, &first, user_path, sizeof(first)) != 0)
		return (uint32_t)-1;
	if (first == '\0' && (flags & LINUX_AT_EMPTY_PATH) != 0) {
		if (linux_fd_stat_metadata(cur, dirfd, &meta) != 0)
			return (uint32_t)-1;
	} else {
		if (linux_path_stat_metadata_at_flags(
		        cur,
		        dirfd,
		        user_path,
		        &meta,
		        (flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0) != 0)
			return (uint32_t)-2; /* ENOENT */
	}

	linux_fill_statx(stx, &meta);
	if (uaccess_copy_to_user(cur, user_statx, stx, sizeof(stx)) != 0)
		return (uint32_t)-1;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_utimensat(uint32_t ebx,
                                                 uint32_t ecx,
                                                 uint32_t esi)
{
	{
		process_t *cur = sched_current();
		linux_fd_stat_t meta;
		int rc;

		if (!cur)
			return (uint32_t)-1;
		if ((esi & ~LINUX_AT_SYMLINK_NOFOLLOW) != 0)
			return (uint32_t)-22;
		if (ecx == 0) {
			if (ebx >= MAX_FDS ||
			    proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
				return (uint32_t)-1;
			return 0;
		}
		rc = linux_path_stat_metadata_at(cur, ebx, ecx, &meta);
		return rc == 0 ? 0 : (uint32_t)-2;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_stat(uint32_t ebx, uint32_t ecx)
{
	{
		/*
         * ebx = pointer to null-terminated path in user space.
         * ecx = pointer to vfs_stat_t in user space (kernel writes result here).
         *
         * Path is resolved relative to the process cwd.
         * Works for both regular files and directories.
         * Returns 0 on success, (uint32_t)-1 if the path is not found.
         */
		process_t *cur = sched_current();
		char *rpath = syscall_alloc_path_scratch();
		if (!rpath)
			return (uint32_t)-1;
		if (!cur || resolve_user_path(cur, ebx, rpath, SYSCALL_PATH_MAX) != 0) {
			kfree(rpath);
			return (uint32_t)-1;
		}
		vfs_stat_t st;
		uint32_t ret = (uint32_t)vfs_stat(rpath, &st);
		if ((int32_t)ret >= 0 &&
		    uaccess_copy_to_user(cur, ecx, &st, sizeof(st)) != 0)
			ret = (uint32_t)-1;
		kfree(rpath);
		return ret;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_stat64_lstat64(uint32_t nofollow,
                                                      uint32_t ebx,
                                                      uint32_t ecx)
{
	/*
         * Linux i386 stat64/lstat64(path, struct stat64 *).
         * stat64 follows symlinks; lstat64 reports the link inode itself.
         */
	return syscall_stat64_path_common(ebx, ecx, nofollow);
}

uint32_t SYSCALL_NOINLINE syscall_case_fstatat64(uint32_t ebx,
                                                 uint32_t ecx,
                                                 uint32_t edx,
                                                 uint32_t esi)
{
	{
		process_t *cur = sched_current();
		uint8_t st64[144];
		linux_fd_stat_t meta;
		char first;

		if (!cur || ecx == 0 || edx == 0)
			return (uint32_t)-1;
		if ((esi & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_NO_AUTOMOUNT |
		             LINUX_AT_EMPTY_PATH)) != 0)
			return (uint32_t)-LINUX_EINVAL;
		if (uaccess_copy_from_user(cur, &first, ecx, sizeof(first)) != 0)
			return (uint32_t)-1;
		if (first == '\0') {
			if ((esi & LINUX_AT_EMPTY_PATH) == 0)
				return (uint32_t)-2;
			if (linux_fd_stat_metadata(cur, ebx, &meta) != 0)
				return (uint32_t)-1;
		} else {
			if (linux_path_stat_metadata_at_flags(
			        cur,
			        ebx,
			        ecx,
			        &meta,
			        (esi & LINUX_AT_SYMLINK_NOFOLLOW) != 0) != 0)
				return (uint32_t)-2;
		}

		linux_fill_stat64(st64,
		                  meta.mode,
		                  meta.nlink,
		                  meta.size,
		                  meta.mtime,
		                  meta.rdev_major,
		                  meta.rdev_minor,
		                  meta.ino);
		if (uaccess_copy_to_user(cur, edx, st64, sizeof(st64)) != 0)
			return (uint32_t)-1;
		return 0;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_fstat64(uint32_t ebx, uint32_t ecx)
{
	/*
         * ebx = fd, ecx = struct stat64 *.
         *
         * Linux i386 musl uses fstat64 for stdio and file metadata.  The
         * layout here matches musl's i386 struct stat (144 bytes).
         */
	return syscall_fstat64(ebx, ecx);
}

uint32_t SYSCALL_NOINLINE syscall_case_fstatat_arm64(uint32_t ebx,
                                                     uint32_t ecx,
                                                     uint32_t edx,
                                                     uint32_t esi)
{
	process_t *cur = sched_current();
	uint8_t st[128];
	linux_fd_stat_t meta;
	char first;

	if (!cur || ecx == 0 || edx == 0)
		return (uint32_t)-1;
	if ((esi & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_NO_AUTOMOUNT |
	             LINUX_AT_EMPTY_PATH)) != 0)
		return (uint32_t)-LINUX_EINVAL;
	if (uaccess_copy_from_user(cur, &first, ecx, sizeof(first)) != 0)
		return (uint32_t)-1;
	if (first == '\0') {
		if ((esi & LINUX_AT_EMPTY_PATH) == 0)
			return (uint32_t)-LINUX_ENOENT;
		if (linux_fd_stat_metadata(cur, ebx, &meta) != 0)
			return (uint32_t)-1;
	} else {
		if (linux_path_stat_metadata_at_flags(
		        cur,
		        ebx,
		        ecx,
		        &meta,
		        (esi & LINUX_AT_SYMLINK_NOFOLLOW) != 0) != 0)
			return (uint32_t)-LINUX_ENOENT;
	}

	linux_fill_stat_arm64(st, &meta);
	if (uaccess_copy_to_user(cur, edx, st, sizeof(st)) != 0)
		return (uint32_t)-1;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_fstat_arm64(uint32_t ebx, uint32_t ecx)
{
	process_t *cur = sched_current();
	uint8_t st[128];
	linux_fd_stat_t meta;

	if (!cur || ecx == 0 || linux_fd_stat_metadata(cur, ebx, &meta) != 0)
		return (uint32_t)-1;

	linux_fill_stat_arm64(st, &meta);
	if (uaccess_copy_to_user(cur, ecx, st, sizeof(st)) != 0)
		return (uint32_t)-1;
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_statx(
    uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	/*
         * Linux i386 statx:
         *   ebx = dirfd, ecx = path, edx = flags, esi = mask, edi = statx *.
         *
         * musl implements fstat(fd, &st) with statx(fd, "", AT_EMPTY_PATH,
         * ...).  BusyBox ls also uses path-based statx with AT_FDCWD.
         */
	return syscall_statx(ebx, ecx, edx, esi, edi);
}

uint32_t SYSCALL_NOINLINE syscall_case_statfs64(uint32_t ebx,
                                                uint32_t ecx,
                                                uint32_t edx)
{
	{
		process_t *cur = sched_current();
		int rc = linux_path_exists(cur, ebx);

		if (rc != 0)
			return (uint32_t)rc;
		rc = linux_copy_statfs64(cur, edx, ecx);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_fstatfs64(uint32_t ebx,
                                                 uint32_t ecx,
                                                 uint32_t edx)
{
	{
		process_t *cur = sched_current();
		int rc;

		if (!cur || ebx >= MAX_FDS ||
		    proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
			return (uint32_t)-LINUX_EBADF;
		rc = linux_copy_statfs64(cur, edx, ecx);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_statfs_arm64(uint32_t ebx, uint32_t ecx)
{
	process_t *cur = sched_current();
	int rc = linux_path_exists(cur, ebx);

	if (rc != 0)
		return (uint32_t)rc;
	rc = linux_copy_statfs_arm64(cur, ecx);
	return rc == 0 ? 0 : (uint32_t)rc;
}

uint32_t SYSCALL_NOINLINE syscall_case_fstatfs_arm64(uint32_t ebx, uint32_t ecx)
{
	process_t *cur = sched_current();
	int rc;

	if (!cur || ebx >= MAX_FDS || proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
		return (uint32_t)-LINUX_EBADF;
	rc = linux_copy_statfs_arm64(cur, ecx);
	return rc == 0 ? 0 : (uint32_t)rc;
}
