/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_vfs.c - Linux path, metadata, and directory syscalls.
 *
 * This file owns pathname resolution, current-working-directory helpers,
 * open/create/unlink/rename/link syscalls, stat/statx/statfs packing,
 * truncate, readlink, getcwd/chdir, and getdents compatibility.
 */

#include "syscall.h"
#include "syscall_internal.h"
#include "syscall_linux.h"
#include "blkdev.h"
#include "clock.h"
#include "fs.h"
#include "kheap.h"
#include "klog.h"
#include "kprintf.h"
#include "kstring.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdint.h>

/*
 * kcwd_resolve: build a full VFS path for `name` relative to the calling
 * process's current working directory.
 *
 * Paths are root-relative with no leading slash ("dir/file").
 * Resolution rules:
 *   - name starts with '/'  → absolute; strip the leading slash and use the rest.
 *   - cwd is empty (root)   → use name unchanged.
 *   - otherwise             → prepend cwd + '/' + name.
 *
 * Unlike the old shell convention (only bare names are relative), ANY path
 * that does not start with '/' is resolved relative to cwd.  This matches
 * POSIX behaviour: "a/b" in cwd "x" resolves to "x/a/b".
 *
 * out must have room for at least outsz bytes including the NUL terminator.
 */
static void
kcwd_resolve(const char *cwd, const char *name, char *out, int outsz)
{
	if (!name) {
		out[0] = '\0';
		return;
	}

	if (k_strcmp(name, ".") == 0) {
		k_snprintf(out, (uint32_t)outsz, "%s", cwd);
	} else if (name[0] == '.' && name[1] == '/') {
		if (cwd[0] == '\0')
			k_snprintf(out, (uint32_t)outsz, "%s", name + 2);
		else
			k_snprintf(out, (uint32_t)outsz, "%s/%s", cwd, name + 2);
	} else if (name[0] == '/')
		k_snprintf(
		    out, (uint32_t)outsz, "%s", name + 1); /* strip leading '/' */
	else if (cwd[0] == '\0')
		k_snprintf(out, (uint32_t)outsz, "%s", name); /* at root: use as-is */
	else
		k_snprintf(out, (uint32_t)outsz, "%s/%s", cwd, name); /* prepend cwd */
}

/*
 * cwd accessors: route reads/writes through fs_state when present, otherwise
 * fall back to the inline proc->cwd buffer (used by early boot processes
 * created before fs_state was allocated, and by the test harness).
 *
 * The null-proc sentinel differs by form: the const reader returns "" so
 * callers can safely pass it to k_snprintf/format strings without a null
 * check; the mutable form returns 0 because there is no safe buffer to hand
 * back — callers must check before writing.
 */
const char *syscall_process_cwd(const process_t *proc)
{
	if (!proc)
		return "";
	return proc->fs_state ? proc->fs_state->cwd : proc->cwd;
}

static char *syscall_process_cwd_mut(process_t *proc)
{
	if (!proc)
		return 0;
	return proc->fs_state ? proc->fs_state->cwd : proc->cwd;
}

static void syscall_mirror_legacy_cwd(process_t *proc)
{
	const char *cwd;

	if (!proc || !proc->fs_state)
		return;
	cwd = proc->fs_state->cwd;
	k_strncpy(proc->cwd, cwd, sizeof(proc->cwd) - 1u);
	proc->cwd[sizeof(proc->cwd) - 1u] = '\0';
}

void syscall_set_process_cwd(process_t *proc, const char *cwd)
{
	if (!proc || !cwd)
		return;

	if (proc->fs_state) {
		k_strncpy(proc->fs_state->cwd, cwd, sizeof(proc->fs_state->cwd) - 1u);
		proc->fs_state->cwd[sizeof(proc->fs_state->cwd) - 1u] = '\0';
		syscall_mirror_legacy_cwd(proc);
	} else {
		k_strncpy(proc->cwd, cwd, sizeof(proc->cwd) - 1u);
		proc->cwd[sizeof(proc->cwd) - 1u] = '\0';
	}
}
char *syscall_alloc_path_scratch(void)
{
	return (char *)kmalloc(SYSCALL_PATH_MAX);
}

char *
copy_user_string_alloc(process_t *proc, uint32_t user_ptr, uint32_t max_len)
{
	char *buf;

	if (!proc || user_ptr == 0 || max_len == 0)
		return 0;

	buf = (char *)kmalloc(max_len);
	if (!buf)
		return 0;

	if (uaccess_copy_string_from_user(proc, buf, max_len, user_ptr) != 0) {
		kfree(buf);
		return 0;
	}

	return buf;
}

int resolve_user_path(process_t *proc,
                      uint32_t user_ptr,
                      char *resolved,
                      uint32_t resolved_sz)
{
	char *raw;

	if (!proc || !resolved || resolved_sz == 0 || user_ptr == 0)
		return -1;

	raw = copy_user_string_alloc(proc, user_ptr, resolved_sz);
	if (!raw)
		return -1;

	kcwd_resolve(syscall_process_cwd(proc), raw, resolved, (int)resolved_sz);
	kfree(raw);
	return 0;
}

int resolve_user_path_at(process_t *proc,
                         uint32_t dirfd,
                         uint32_t user_ptr,
                         char *resolved,
                         uint32_t resolved_sz)
{
	char *raw;

	if (!proc || !resolved || resolved_sz == 0 || user_ptr == 0)
		return -1;

	raw = copy_user_string_alloc(proc, user_ptr, resolved_sz);
	if (!raw)
		return -1;
	if (raw[0] == '\0') {
		kfree(raw);
		return -1;
	}

	if (raw[0] == '/' || dirfd == LINUX_AT_FDCWD) {
		kcwd_resolve(
		    syscall_process_cwd(proc), raw, resolved, (int)resolved_sz);
	} else {
		file_handle_t *fh;

		if (dirfd >= MAX_FDS) {
			kfree(raw);
			return -LINUX_EBADF;
		}
		fh = &proc_fd_entries(proc)[dirfd];
		if (fh->type != FD_TYPE_DIR) {
			kfree(raw);
			return -LINUX_EBADF;
		}
		kcwd_resolve(fh->u.dir.path, raw, resolved, (int)resolved_sz);
	}

	kfree(raw);
	return 0;
}

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
	meta->mtime = clock_unix_time();
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

static uint32_t linux_dirent64_reclen(uint32_t name_len)
{
	uint32_t len = 19u + name_len + 1u;
	return (len + 7u) & ~7u;
}

static uint32_t linux_dirent_reclen(uint32_t name_len)
{
	uint32_t len = 10u + name_len + 2u;
	return (len + 3u) & ~3u;
}

static int linux_fill_getdents(process_t *cur,
                               file_handle_t *fh,
                               uint32_t user_buf,
                               uint32_t count)
{
	char *names;
	uint8_t *out;
	int names_len;
	uint32_t entry = 0;
	uint32_t written = 0;
	uint32_t pos = 0;
	int full = 0;

	if (!cur || !fh || fh->type != FD_TYPE_DIR || user_buf == 0 || count == 0)
		return -1;

	names = (char *)kmalloc(LINUX_DIRENT_NAME_SCRATCH);
	out = (uint8_t *)kmalloc(count);
	if (!names || !out) {
		if (names)
			kfree(names);
		if (out)
			kfree(out);
		return -1;
	}

	names_len = vfs_getdents(fh->u.dir.path[0] ? fh->u.dir.path : 0,
	                         names,
	                         LINUX_DIRENT_NAME_SCRATCH);
	if (names_len < 0) {
		kfree(out);
		kfree(names);
		return -1;
	}

	for (uint32_t dot = 0; dot < 2u; dot++) {
		const char *name = dot ? ".." : ".";
		uint32_t name_len = dot ? 2u : 1u;
		uint32_t reclen;

		if (entry++ < fh->u.dir.index)
			continue;

		reclen = linux_dirent_reclen(name_len);
		if (written + reclen > count) {
			full = 1;
			break;
		}

		k_memset(out + written, 0, reclen);
		linux_put_u32(out, written + 0u, entry);
		linux_put_u32(out, written + 4u, entry);
		linux_put_u16(out, written + 8u, reclen);
		for (uint32_t i = 0; i < name_len; i++)
			out[written + 10u + i] = (uint8_t)name[i];
		out[written + reclen - 1u] = (uint8_t)LINUX_DT_DIR;

		written += reclen;
		fh->u.dir.index++;
	}

	while (!full && pos < (uint32_t)names_len) {
		char *name = names + pos;
		uint32_t raw_len = (uint32_t)k_strlen(name);
		uint32_t name_len = raw_len;
		uint32_t type = LINUX_DT_REG;
		uint32_t reclen;

		pos += raw_len + 1u;
		if (raw_len == 0)
			continue;
		if (entry++ < fh->u.dir.index)
			continue;

		if (name_len > 0 && name[name_len - 1u] == '/') {
			type = LINUX_DT_DIR;
			name_len--;
		}

		reclen = linux_dirent_reclen(name_len);
		if (written + reclen > count)
			break;

		k_memset(out + written, 0, reclen);
		linux_put_u32(out, written + 0u, entry);
		linux_put_u32(out, written + 4u, entry);
		linux_put_u16(out, written + 8u, reclen);
		for (uint32_t i = 0; i < name_len; i++)
			out[written + 10u + i] = (uint8_t)name[i];
		out[written + reclen - 1u] = (uint8_t)type;

		written += reclen;
		fh->u.dir.index++;
	}

	if (written != 0 &&
	    uaccess_copy_to_user(cur, user_buf, out, written) != 0) {
		kfree(out);
		kfree(names);
		return -1;
	}

	kfree(out);
	kfree(names);
	return (int)written;
}

static int linux_fill_getdents64(process_t *cur,
                                 file_handle_t *fh,
                                 uint32_t user_buf,
                                 uint32_t count)
{
	char *names;
	uint8_t *out;
	int names_len;
	uint32_t entry = 0;
	uint32_t written = 0;
	uint32_t pos = 0;
	int full = 0;

	if (!cur || !fh || fh->type != FD_TYPE_DIR || user_buf == 0 || count == 0)
		return -1;

	names = (char *)kmalloc(LINUX_DIRENT_NAME_SCRATCH);
	out = (uint8_t *)kmalloc(count);
	if (!names || !out) {
		if (names)
			kfree(names);
		if (out)
			kfree(out);
		return -1;
	}

	names_len = vfs_getdents(fh->u.dir.path[0] ? fh->u.dir.path : 0,
	                         names,
	                         LINUX_DIRENT_NAME_SCRATCH);
	if (names_len < 0) {
		kfree(out);
		kfree(names);
		return -1;
	}

	for (uint32_t dot = 0; dot < 2u; dot++) {
		const char *name = dot ? ".." : ".";
		uint32_t name_len = dot ? 2u : 1u;
		uint32_t reclen;

		if (entry++ < fh->u.dir.index)
			continue;

		reclen = linux_dirent64_reclen(name_len);
		if (written + reclen > count) {
			full = 1;
			break;
		}

		k_memset(out + written, 0, reclen);
		linux_put_u64(out, written + 0u, (uint64_t)entry);
		linux_put_u64(out, written + 8u, (uint64_t)entry);
		linux_put_u16(out, written + 16u, reclen);
		out[written + 18u] = (uint8_t)LINUX_DT_DIR;
		for (uint32_t i = 0; i < name_len; i++)
			out[written + 19u + i] = (uint8_t)name[i];

		written += reclen;
		fh->u.dir.index++;
	}

	while (!full && pos < (uint32_t)names_len) {
		char *name = names + pos;
		uint32_t raw_len = (uint32_t)k_strlen(name);
		uint32_t name_len = raw_len;
		uint32_t type = LINUX_DT_REG;
		uint32_t reclen;

		pos += raw_len + 1u;
		if (raw_len == 0)
			continue;
		if (entry++ < fh->u.dir.index)
			continue;

		if (name_len > 0 && name[name_len - 1u] == '/') {
			type = LINUX_DT_DIR;
			name_len--;
		}

		reclen = linux_dirent64_reclen(name_len);
		if (written + reclen > count)
			break;

		k_memset(out + written, 0, reclen);
		linux_put_u64(out, written + 0u, (uint64_t)entry);
		linux_put_u64(out, written + 8u, (uint64_t)entry);
		linux_put_u16(out, written + 16u, reclen);
		out[written + 18u] = (uint8_t)type;
		for (uint32_t i = 0; i < name_len; i++)
			out[written + 19u + i] = (uint8_t)name[i];

		written += reclen;
		fh->u.dir.index++;
	}

	if (written != 0 &&
	    uaccess_copy_to_user(cur, user_buf, out, written) != 0) {
		kfree(out);
		kfree(names);
		return -1;
	}

	kfree(out);
	kfree(names);
	return (int)written;
}

static int linux_path_exists(process_t *cur, uint32_t user_path)
{
	char *rpath;
	vfs_stat_t st;
	int rc;

	if (!cur || user_path == 0)
		return -1;
	rpath = syscall_alloc_path_scratch();
	if (!rpath)
		return -1;
	if (resolve_user_path(cur, user_path, rpath, SYSCALL_PATH_MAX) != 0) {
		kfree(rpath);
		return -1;
	}
	rc = vfs_stat(rpath, &st);
	kfree(rpath);
	return rc == 0 ? 0 : -2;
}

static int
linux_path_exists_at(process_t *cur, uint32_t dirfd, uint32_t user_path)
{
	char *rpath;
	vfs_stat_t st;
	int rc;

	if (!cur || user_path == 0)
		return -1;
	rpath = syscall_alloc_path_scratch();
	if (!rpath)
		return -1;
	if (resolve_user_path_at(cur, dirfd, user_path, rpath, SYSCALL_PATH_MAX) !=
	    0) {
		kfree(rpath);
		return -1;
	}
	rc = vfs_stat(rpath, &st);
	kfree(rpath);
	return rc == 0 ? 0 : -2;
}

static int
linux_truncate_path(process_t *cur, uint32_t user_path, uint64_t length)
{
	char *rpath;
	vfs_file_ref_t ref;
	uint32_t size = 0;
	int rc;

	if (!cur || user_path == 0)
		return -1;
	if (length > 0xFFFFFFFFull)
		return -22;

	rpath = syscall_alloc_path_scratch();
	if (!rpath)
		return -1;
	if (resolve_user_path(cur, user_path, rpath, SYSCALL_PATH_MAX) != 0) {
		kfree(rpath);
		return -1;
	}

	rc = vfs_open_file(rpath, &ref, &size);
	kfree(rpath);
	if (rc != 0)
		return -2;
	(void)size;
	return vfs_truncate(ref, (uint32_t)length) == 0 ? 0 : -1;
}

static int linux_truncate_fd(process_t *cur, uint32_t fd, uint64_t length)
{
	file_handle_t *fh;

	if (!cur || fd >= MAX_FDS)
		return -LINUX_EBADF;
	if (length > 0xFFFFFFFFull)
		return -22;

	fh = &proc_fd_entries(cur)[fd];
	if (fh->type != FD_TYPE_FILE || !fh->writable)
		return -LINUX_EBADF;
	if (vfs_truncate(fh->u.file.ref, (uint32_t)length) != 0)
		return -1;

	for (uint32_t i = 0; i < MAX_FDS; i++) {
		file_handle_t *other = &proc_fd_entries(cur)[i];
		if (other->type == FD_TYPE_FILE &&
		    other->u.file.ref.mount_id == fh->u.file.ref.mount_id &&
		    other->u.file.ref.inode_num == fh->u.file.ref.inode_num) {
			other->u.file.size = (uint32_t)length;
			if (other->u.file.offset > other->u.file.size)
				other->u.file.offset = other->u.file.size;
		}
	}

	return 0;
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

static uint32_t syscall_stat64_path(uint32_t user_path, uint32_t user_stat)
{
	return syscall_stat64_path_common(user_path, user_stat, 0);
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

static int fd_install_vfs_node(process_t *proc,
                               const vfs_node_t *node,
                               uint32_t access_mode,
                               uint32_t writable,
                               uint32_t append,
                               uint32_t cloexec,
                               uint32_t nonblock)
{
	int fd;

	if (!proc || !node)
		return -1;

	fd = fd_alloc(proc);
	if (fd < 0)
		return -1;

	proc_fd_entries(proc)[fd].writable = writable;
	proc_fd_entries(proc)[fd].access_mode = access_mode & LINUX_O_ACCMODE;
	proc_fd_entries(proc)[fd].append = 0;
	proc_fd_entries(proc)[fd].cloexec = cloexec ? 1u : 0u;
	proc_fd_entries(proc)[fd].nonblock = nonblock ? 1u : 0u;

	switch (node->type) {
	case VFS_NODE_FILE:
		proc_fd_entries(proc)[fd].type = FD_TYPE_FILE;
		proc_fd_entries(proc)[fd].append = append ? 1u : 0u;
		proc_fd_entries(proc)[fd].u.file.ref.mount_id = node->mount_id;
		proc_fd_entries(proc)[fd].u.file.ref.inode_num = node->inode_num;
		proc_fd_entries(proc)[fd].u.file.inode_num = node->inode_num;
		proc_fd_entries(proc)[fd].u.file.size = node->size;
		proc_fd_entries(proc)[fd].u.file.offset = 0;
		return fd;

	case VFS_NODE_SYSFILE:
		if (writable) {
			proc_fd_entries(proc)[fd].type = FD_TYPE_NONE;
			proc_fd_entries(proc)[fd].writable = 0;
			proc_fd_entries(proc)[fd].access_mode = 0;
			proc_fd_entries(proc)[fd].cloexec = 0;
			proc_fd_entries(proc)[fd].nonblock = 0;
			return -1;
		}
		proc_fd_entries(proc)[fd].type = FD_TYPE_SYSFILE;
		proc_fd_entries(proc)[fd].append = 0;
		proc_fd_entries(proc)[fd].u.file.ref.mount_id = node->mount_id;
		proc_fd_entries(proc)[fd].u.file.ref.inode_num = node->inode_num;
		proc_fd_entries(proc)[fd].u.file.inode_num = node->inode_num;
		proc_fd_entries(proc)[fd].u.file.size = node->size;
		proc_fd_entries(proc)[fd].u.file.offset = 0;
		return fd;

	case VFS_NODE_DIR:
		proc_fd_entries(proc)[fd].type = FD_TYPE_DIR;
		proc_fd_entries(proc)[fd].u.dir.path[0] = '\0';
		proc_fd_entries(proc)[fd].u.dir.index = 0;
		return fd;

	case VFS_NODE_BLOCKDEV:
		if (writable) {
			proc_fd_entries(proc)[fd].type = FD_TYPE_NONE;
			proc_fd_entries(proc)[fd].writable = 0;
			proc_fd_entries(proc)[fd].access_mode = 0;
			proc_fd_entries(proc)[fd].cloexec = 0;
			proc_fd_entries(proc)[fd].nonblock = 0;
			return -1;
		}
		proc_fd_entries(proc)[fd].type = FD_TYPE_BLOCKDEV;
		k_strncpy(proc_fd_entries(proc)[fd].u.blockdev.name,
		          node->dev_name,
		          sizeof(proc_fd_entries(proc)[fd].u.blockdev.name) - 1);
		proc_fd_entries(proc)[fd]
		    .u.blockdev
		    .name[sizeof(proc_fd_entries(proc)[fd].u.blockdev.name) - 1] = '\0';
		proc_fd_entries(proc)[fd].u.blockdev.offset = 0;
		proc_fd_entries(proc)[fd].u.blockdev.size = node->size;
		return fd;

	case VFS_NODE_TTY:
		proc_fd_entries(proc)[fd].type = FD_TYPE_TTY;
		proc_fd_entries(proc)[fd].u.tty.tty_idx = node->dev_id;
		return fd;

	case VFS_NODE_PROCFILE:
		proc_fd_entries(proc)[fd].type = FD_TYPE_PROCFILE;
		proc_fd_entries(proc)[fd].u.proc.kind = node->proc_kind;
		proc_fd_entries(proc)[fd].u.proc.pid = node->proc_pid;
		proc_fd_entries(proc)[fd].u.proc.index = node->proc_index;
		proc_fd_entries(proc)[fd].u.proc.size = node->size;
		proc_fd_entries(proc)[fd].u.proc.offset = 0;
		return fd;

	case VFS_NODE_CHARDEV:
		proc_fd_entries(proc)[fd].type = FD_TYPE_CHARDEV;
		k_strncpy(proc_fd_entries(proc)[fd].u.chardev.name,
		          node->dev_name,
		          sizeof(proc_fd_entries(proc)[fd].u.chardev.name) - 1);
		proc_fd_entries(proc)[fd]
		    .u.chardev
		    .name[sizeof(proc_fd_entries(proc)[fd].u.chardev.name) - 1] = '\0';
		return fd;

	default:
		proc_fd_entries(proc)[fd].type = FD_TYPE_NONE;
		proc_fd_entries(proc)[fd].writable = 0;
		proc_fd_entries(proc)[fd].access_mode = 0;
		proc_fd_entries(proc)[fd].append = 0;
		proc_fd_entries(proc)[fd].cloexec = 0;
		proc_fd_entries(proc)[fd].nonblock = 0;
		return -1;
	}
}

static int
syscall_open_resolved_path(process_t *cur, const char *rpath, uint32_t flags)
{
	uint32_t accmode;
	uint32_t writable;
	uint32_t append;
	uint32_t cloexec;
	uint32_t nonblock;
	vfs_node_t node;
	int rc;
	int fd;

	if (!cur || !rpath)
		return -1;

	accmode = flags & LINUX_O_ACCMODE;
	if (accmode == (LINUX_O_WRONLY | LINUX_O_RDWR))
		return -1;
	writable = accmode != 0;
	append = (flags & LINUX_O_APPEND) != 0;
	cloexec = (flags & LINUX_O_CLOEXEC) != 0;
	nonblock = (flags & LINUX_O_NONBLOCK) != 0;

	if ((flags & LINUX_O_TRUNC) && !writable)
		return -LINUX_EINVAL;

	rc = vfs_resolve(rpath, &node);
	if (rc != 0) {
		if (rc < -1)
			return rc;
		if ((flags & LINUX_O_CREAT) == 0)
			return -2;
		if (flags & LINUX_O_DIRECTORY)
			return -2;
		if (vfs_create(rpath) < 0)
			return -1;
		if (vfs_resolve(rpath, &node) != 0)
			return -1;
	} else if ((flags & (LINUX_O_CREAT | LINUX_O_EXCL)) ==
	           (LINUX_O_CREAT | LINUX_O_EXCL)) {
		return -LINUX_EEXIST;
	} else if (flags & LINUX_O_TRUNC) {
		vfs_file_ref_t ref;
		uint32_t size = 0;

		if (node.type != VFS_NODE_FILE)
			return node.type == VFS_NODE_DIR ? -LINUX_EISDIR : -1;
		if (vfs_open_file(rpath, &ref, &size) != 0)
			return -1;
		if (vfs_truncate(ref, 0) != 0)
			return -1;
		node.size = 0;
	}

	if (node.type != VFS_NODE_FILE && node.type != VFS_NODE_DIR &&
	    node.type != VFS_NODE_BLOCKDEV && node.type != VFS_NODE_SYSFILE &&
	    node.type != VFS_NODE_PROCFILE && node.type != VFS_NODE_TTY &&
	    node.type != VFS_NODE_CHARDEV)
		return -1;

	if ((flags & LINUX_O_DIRECTORY) && node.type != VFS_NODE_DIR)
		return -LINUX_ENOTDIR;
	if (writable && node.type == VFS_NODE_DIR)
		return -LINUX_EISDIR;

	fd = fd_install_vfs_node(
	    cur, &node, accmode, writable, append, cloexec, nonblock);
	if (fd < 0)
		return -1;
	if (node.type == VFS_NODE_DIR) {
		k_strncpy(proc_fd_entries(cur)[fd].u.dir.path,
		          rpath,
		          sizeof(proc_fd_entries(cur)[fd].u.dir.path) - 1);
		proc_fd_entries(cur)[fd]
		    .u.dir.path[sizeof(proc_fd_entries(cur)[fd].u.dir.path) - 1] = '\0';
	}
	return fd;
}

typedef uint32_t (*syscall_one_path_op_t)(const char *path);
typedef uint32_t (*syscall_two_path_op_t)(const char *oldpath,
                                          const char *newpath);
typedef int (*syscall_path_resolver_t)(process_t *proc,
                                       uint32_t resolver_arg,
                                       uint32_t user_ptr,
                                       char *resolved,
                                       uint32_t resolved_sz);

typedef struct {
	syscall_path_resolver_t resolve;
	uint32_t resolver_arg;
} syscall_path_spec_t;
static uint32_t syscall_with_resolved_path(process_t *cur,
                                           uint32_t user_path,
                                           syscall_one_path_op_t op)
{
	char *rpath;
	uint32_t ret;

	if (!cur || !op)
		return (uint32_t)-1;

	rpath = syscall_alloc_path_scratch();
	if (!rpath)
		return (uint32_t)-1;

	if (resolve_user_path(cur, user_path, rpath, SYSCALL_PATH_MAX) != 0) {
		kfree(rpath);
		return (uint32_t)-1;
	}

	ret = op(rpath);
	kfree(rpath);
	return ret;
}

static uint32_t syscall_with_resolved_path_at(process_t *cur,
                                              uint32_t dirfd,
                                              uint32_t user_path,
                                              syscall_one_path_op_t op)
{
	char *rpath;
	uint32_t ret;

	if (!cur || !op)
		return (uint32_t)-1;

	rpath = syscall_alloc_path_scratch();
	if (!rpath)
		return (uint32_t)-1;

	ret = (uint32_t)resolve_user_path_at(
	    cur, dirfd, user_path, rpath, SYSCALL_PATH_MAX);
	if ((int32_t)ret != 0) {
		kfree(rpath);
		return ret;
	}

	ret = op(rpath);
	kfree(rpath);
	return ret;
}

static int syscall_resolve_user_path(process_t *proc,
                                     uint32_t resolver_arg,
                                     uint32_t user_ptr,
                                     char *resolved,
                                     uint32_t resolved_sz)
{
	(void)resolver_arg;
	return resolve_user_path(proc, user_ptr, resolved, resolved_sz);
}

static int syscall_resolve_user_path_at(process_t *proc,
                                        uint32_t resolver_arg,
                                        uint32_t user_ptr,
                                        char *resolved,
                                        uint32_t resolved_sz)
{
	return resolve_user_path_at(
	    proc, resolver_arg, user_ptr, resolved, resolved_sz);
}

static uint32_t
syscall_with_two_resolved_paths_common(process_t *cur,
                                       uint32_t old_user_path,
                                       uint32_t new_user_path,
                                       const syscall_path_spec_t *old_spec,
                                       const syscall_path_spec_t *new_spec,
                                       syscall_two_path_op_t op)
{
	char *oldpath;
	char *newpath;
	uint32_t ret;

	if (!cur || !old_spec || !new_spec || !old_spec->resolve ||
	    !new_spec->resolve || !op)
		return (uint32_t)-1;

	oldpath = syscall_alloc_path_scratch();
	if (!oldpath)
		return (uint32_t)-1;

	newpath = syscall_alloc_path_scratch();
	if (!newpath) {
		kfree(oldpath);
		return (uint32_t)-1;
	}

	ret = (uint32_t)old_spec->resolve(
	    cur, old_spec->resolver_arg, old_user_path, oldpath, SYSCALL_PATH_MAX);
	if ((int32_t)ret == 0)
		ret = (uint32_t)new_spec->resolve(cur,
		                                  new_spec->resolver_arg,
		                                  new_user_path,
		                                  newpath,
		                                  SYSCALL_PATH_MAX);
	if ((int32_t)ret != 0) {
		kfree(oldpath);
		kfree(newpath);
		return ret;
	}

	ret = op(oldpath, newpath);
	kfree(oldpath);
	kfree(newpath);
	return ret;
}

static uint32_t syscall_with_two_resolved_paths(process_t *cur,
                                                uint32_t old_user_path,
                                                uint32_t new_user_path,
                                                syscall_two_path_op_t op)
{
	syscall_path_spec_t old_spec = {
	    syscall_resolve_user_path,
	    0u,
	};
	syscall_path_spec_t new_spec = {
	    syscall_resolve_user_path,
	    0u,
	};

	return syscall_with_two_resolved_paths_common(
	    cur, old_user_path, new_user_path, &old_spec, &new_spec, op);
}

static uint32_t syscall_with_two_resolved_paths_at(process_t *cur,
                                                   uint32_t old_dirfd,
                                                   uint32_t old_user_path,
                                                   uint32_t new_dirfd,
                                                   uint32_t new_user_path,
                                                   syscall_two_path_op_t op)
{
	syscall_path_spec_t old_spec = {
	    syscall_resolve_user_path_at,
	    old_dirfd,
	};
	syscall_path_spec_t new_spec = {
	    syscall_resolve_user_path_at,
	    new_dirfd,
	};

	return syscall_with_two_resolved_paths_common(
	    cur, old_user_path, new_user_path, &old_spec, &new_spec, op);
}

static uint32_t syscall_vfs_mkdir_op(const char *path)
{
	return (uint32_t)vfs_mkdir(path);
}

static uint32_t syscall_vfs_rmdir_op(const char *path)
{
	return (uint32_t)vfs_rmdir(path);
}

static uint32_t syscall_vfs_unlink_op(const char *path)
{
	return vfs_unlink(path) == 0 ? 0 : (uint32_t)-LINUX_ENOENT;
}

static uint32_t syscall_vfs_rename_op(const char *oldpath, const char *newpath)
{
	return (uint32_t)vfs_rename(oldpath, newpath);
}

static uint32_t syscall_vfs_link_follow_op(const char *oldpath,
                                           const char *newpath)
{
	return (uint32_t)vfs_link(oldpath, newpath, 1u);
}

static uint32_t syscall_vfs_link_nofollow_op(const char *oldpath,
                                             const char *newpath)
{
	return (uint32_t)vfs_link(oldpath, newpath, 0u);
}

uint32_t SYSCALL_NOINLINE syscall_case_open(uint32_t ebx, uint32_t ecx)
{
	{
		/*
         * Linux i386 open(path, flags, mode).  Drunix only stores simple DUFS
         * metadata, but the access mode and create/truncate bits must follow
         * Linux enough for static BusyBox file utilities.
         */
		process_t *cur = sched_current();
		uint32_t flags = ecx;
		int fd;
		if (!cur)
			return (uint32_t)-1;

		char *rpath = syscall_alloc_path_scratch();
		if (!rpath)
			return (uint32_t)-1;
		if (resolve_user_path(cur, ebx, rpath, SYSCALL_PATH_MAX) != 0) {
			kfree(rpath);
			return (uint32_t)-1;
		}

		fd = syscall_open_resolved_path(cur, rpath, flags);
		kfree(rpath);
		return (uint32_t)fd;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_openat(uint32_t ebx,
                                              uint32_t ecx,
                                              uint32_t edx)
{
	{
		process_t *cur = sched_current();
		uint32_t flags = edx;
		char *rpath;
		int fd;
		int rc;

		if (!cur)
			return (uint32_t)-1;

		rpath = syscall_alloc_path_scratch();
		if (!rpath)
			return (uint32_t)-1;
		rc = resolve_user_path_at(cur, ebx, ecx, rpath, SYSCALL_PATH_MAX);
		if (rc != 0) {
			kfree(rpath);
			return (uint32_t)rc;
		}

		fd = syscall_open_resolved_path(cur, rpath, flags);
		kfree(rpath);
		return (uint32_t)fd;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_access(uint32_t ebx)
{
	{
		/*
         * Linux i386 access(path, mode).  DUFS does not carry Unix
         * permissions yet; existence is enough and uid 0 can access it.
         */
		process_t *cur = sched_current();
		int rc = linux_path_exists(cur, ebx);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_faccessat(uint32_t ebx,
                                                 uint32_t ecx,
                                                 uint32_t edx)
{
	{
		process_t *cur = sched_current();
		int rc;

		if ((edx & ~7u) != 0)
			return (uint32_t)-22;
		rc = linux_path_exists_at(cur, ebx, ecx);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_fchmodat(uint32_t ebx, uint32_t ecx)
{
	{
		process_t *cur = sched_current();
		int rc;

		rc = linux_path_exists_at(cur, ebx, ecx);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_fchownat(uint32_t ebx,
                                                uint32_t ecx,
                                                uint32_t edi)
{
	{
		process_t *cur = sched_current();
		int rc;

		if ((edi & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0)
			return (uint32_t)-22;
		if (ecx == 0) {
			if ((edi & LINUX_AT_EMPTY_PATH) == 0 || !cur || ebx >= MAX_FDS ||
			    proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
				return (uint32_t)-1;
			return 0;
		}
		rc = linux_path_exists_at(cur, ebx, ecx);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_futimesat(uint32_t ebx, uint32_t ecx)
{
	{
		process_t *cur = sched_current();
		int rc;

		if (ecx == 0) {
			if (!cur || ebx >= MAX_FDS ||
			    proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
				return (uint32_t)-1;
			return 0;
		}
		rc = linux_path_exists_at(cur, ebx, ecx);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_chmod(uint32_t ebx)
{
	{
		/*
         * Linux i386 chmod(path, mode).  Mode persistence is not represented
         * in DUFS yet, so accept chmod on existing paths as a compatibility
         * no-op.
         */
		process_t *cur = sched_current();
		int rc = linux_path_exists(cur, ebx);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_lchown_chown32(uint32_t ebx)
{
	{
		/*
         * Linux chown/lchown compatibility.  Drunix currently runs as uid 0
         * with no per-inode uid/gid fields, so owner changes on existing paths
         * are accepted as no-ops.
         */
		process_t *cur = sched_current();
		int rc = linux_path_exists(cur, ebx);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_sync(void)
{
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_umask(uint32_t ebx)
{
	{
		process_t *cur = sched_current();
		uint32_t old;

		if (!cur)
			return (uint32_t)-1;
		old = cur->umask;
		cur->umask = ebx & 0777u;
		return old;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_readlink(uint32_t ebx,
                                                uint32_t ecx,
                                                uint32_t edx)
{
	{
		process_t *cur = sched_current();
		char *rpath;
		char *target;
		int rc;

		if (!cur || ecx == 0 || edx == 0)
			return (uint32_t)-22;
		rpath = syscall_alloc_path_scratch();
		target = (char *)kmalloc(edx);
		if (!rpath || !target) {
			if (rpath)
				kfree(rpath);
			if (target)
				kfree(target);
			return (uint32_t)-1;
		}
		if (resolve_user_path(cur, ebx, rpath, SYSCALL_PATH_MAX) != 0) {
			kfree(rpath);
			kfree(target);
			return (uint32_t)-1;
		}
		rc = vfs_readlink(rpath, target, edx);
		if (rc > 0 && uaccess_copy_to_user(cur, ecx, target, (uint32_t)rc) != 0)
			rc = -1;
		kfree(rpath);
		kfree(target);
		return (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_readlinkat(uint32_t ebx,
                                                  uint32_t ecx,
                                                  uint32_t edx,
                                                  uint32_t esi)
{
	{
		process_t *cur = sched_current();
		char *rpath;
		char *target;
		int rc;

		if (!cur || edx == 0 || esi == 0)
			return (uint32_t)-22;
		rpath = syscall_alloc_path_scratch();
		target = (char *)kmalloc(esi);
		if (!rpath || !target) {
			if (rpath)
				kfree(rpath);
			if (target)
				kfree(target);
			return (uint32_t)-1;
		}
		if (resolve_user_path_at(cur, ebx, ecx, rpath, SYSCALL_PATH_MAX) != 0) {
			kfree(rpath);
			kfree(target);
			return (uint32_t)-1;
		}
		rc = vfs_readlink(rpath, target, esi);
		if (rc > 0 && uaccess_copy_to_user(cur, edx, target, (uint32_t)rc) != 0)
			rc = -1;
		kfree(rpath);
		kfree(target);
		return (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_truncate64(uint32_t ebx,
                                                  uint32_t ecx,
                                                  uint32_t edx)
{
	{
		process_t *cur = sched_current();
		uint64_t length = (uint64_t)ecx | ((uint64_t)edx << 32);
		int rc = linux_truncate_path(cur, ebx, length);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_ftruncate64(uint32_t ebx,
                                                   uint32_t ecx,
                                                   uint32_t edx)
{
	{
		process_t *cur = sched_current();
		uint64_t length = (uint64_t)ecx | ((uint64_t)edx << 32);
		int rc = linux_truncate_fd(cur, ebx, length);
		return rc == 0 ? 0 : (uint32_t)rc;
	}
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

uint32_t SYSCALL_NOINLINE syscall_case_creat(uint32_t ebx)
{
	{
		/*
         * ebx = pointer to null-terminated filename in user space.
         *
         * Creates a new file (or truncates an existing one) in DUFS, finds
         * a free slot in the fd table, and installs a writable FD_TYPE_FILE
         * handle.  Returns the fd on success, or -1 on error.
         */
		process_t *cur = sched_current();
		if (!cur)
			return (uint32_t)-1;

		char *rpath = syscall_alloc_path_scratch();
		if (!rpath)
			return (uint32_t)-1;
		if (resolve_user_path(cur, ebx, rpath, SYSCALL_PATH_MAX) != 0) {
			kfree(rpath);
			return (uint32_t)-1;
		}

		int ino_c = vfs_create(rpath);
		vfs_file_ref_t ref_c;
		uint32_t size_c = 0;
		if (ino_c < 0) {
			kfree(rpath);
			klog("CREATE", "vfs_create failed");
			return (uint32_t)-1;
		}
		if (vfs_open_file(rpath, &ref_c, &size_c) != 0) {
			kfree(rpath);
			return (uint32_t)-1;
		}
		kfree(rpath);

		int fd = fd_alloc(cur);
		if (fd < 0) {
			klog("CREATE", "fd table full");
			return (uint32_t)-1;
		}
		proc_fd_entries(cur)[fd].type = FD_TYPE_FILE;
		proc_fd_entries(cur)[fd].writable = 1;
		proc_fd_entries(cur)[fd].access_mode = LINUX_O_WRONLY;
		proc_fd_entries(cur)[fd].append = 0;
		proc_fd_entries(cur)[fd].cloexec = 0;
		proc_fd_entries(cur)[fd].nonblock = 0;
		proc_fd_entries(cur)[fd].u.file.ref = ref_c;
		proc_fd_entries(cur)[fd].u.file.inode_num = (uint32_t)ino_c;
		proc_fd_entries(cur)[fd].u.file.size = size_c;
		proc_fd_entries(cur)[fd].u.file.offset = 0;
		return (uint32_t)fd;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_unlink(uint32_t ebx)
{
	{
		process_t *cur = sched_current();

		return syscall_with_resolved_path(cur, ebx, syscall_vfs_unlink_op);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_unlinkat(uint32_t ebx,
                                                uint32_t ecx,
                                                uint32_t edx)
{
	{
		process_t *cur = sched_current();

		if ((edx & ~LINUX_AT_REMOVEDIR) != 0)
			return (uint32_t)-22;
		if (edx & LINUX_AT_REMOVEDIR)
			return syscall_with_resolved_path_at(
			    cur, ebx, ecx, syscall_vfs_rmdir_op);
		return syscall_with_resolved_path_at(
		    cur, ebx, ecx, syscall_vfs_unlink_op);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_mkdir(uint32_t ebx)
{
	{
		process_t *cur = sched_current();

		return syscall_with_resolved_path(cur, ebx, syscall_vfs_mkdir_op);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_mkdirat(uint32_t ebx, uint32_t ecx)
{
	{
		process_t *cur = sched_current();

		return syscall_with_resolved_path_at(
		    cur, ebx, ecx, syscall_vfs_mkdir_op);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_rmdir(uint32_t ebx)
{
	{
		process_t *cur = sched_current();

		return syscall_with_resolved_path(cur, ebx, syscall_vfs_rmdir_op);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_chdir(uint32_t ebx)
{
	{
		/*
         * ebx = pointer to null-terminated path in user space.
         *
         * Changes the calling process's current working directory.
         * Special forms handled before VFS validation:
         *   "" / "/"         → move to root (cwd = "").
         *   ".."             → strip the last component from cwd.
         * All other paths are resolved relative to the current cwd, then
         * validated as an existing directory via vfs_stat before being stored.
         * Returns 0 on success, -1 if the path is not a valid directory.
         */
		process_t *cur = sched_current();
		if (!cur)
			return (uint32_t)-1;
		if (ebx == 0)
			return (uint32_t)-LINUX_EFAULT;

		char *path = 0;
		char *cwd;

		if (ebx != 0) {
			path = copy_user_string_alloc(cur, ebx, SYSCALL_PATH_MAX);
			if (!path)
				return (uint32_t)-1;
		}
		cwd = syscall_process_cwd_mut(cur);
		if (!cwd) {
			kfree(path);
			return (uint32_t)-1;
		}

		/* Go to root. */
		if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
			syscall_set_process_cwd(cur, "");
			kfree(path);
			return 0;
		}

		/* Go up one level. */
		if (path[0] == '.' && path[1] == '.' && path[2] == '\0') {
			if (cwd[0] == '\0') {
				kfree(path);
				return 0;
			}
			int i = 0;
			while (cwd[i])
				i++;
			while (i > 0 && cwd[i - 1] != '/')
				i--;
			if (i > 0)
				i--;
			cwd[i] = '\0';
			syscall_mirror_legacy_cwd(cur);
			kfree(path);
			return 0;
		}

		/* Resolve path relative to cwd. */
		char *resolved = syscall_alloc_path_scratch();
		if (!resolved) {
			kfree(path);
			return (uint32_t)-1;
		}
		kcwd_resolve(
		    syscall_process_cwd(cur), path, resolved, SYSCALL_PATH_MAX);
		kfree(path);

		/* Trim any trailing slash. */
		int rlen = 0;
		while (resolved[rlen])
			rlen++;
		if (rlen > 0 && resolved[rlen - 1] == '/')
			resolved[--rlen] = '\0';

		/* Empty after trimming → root. */
		if (resolved[0] == '\0') {
			syscall_set_process_cwd(cur, "");
			kfree(resolved);
			return 0;
		}

		/* Validate: must exist and be a directory (type == 2). */
		vfs_stat_t st;
		if (vfs_stat(resolved, &st) != 0) {
			klog("CHDIR", "missing directory");
			kfree(resolved);
			return (uint32_t)-LINUX_ENOENT;
		}
		if (st.type != 2) {
			klog("CHDIR", "not a directory");
			kfree(resolved);
			return (uint32_t)-1;
		}

		/* Commit the new cwd. */
		syscall_set_process_cwd(cur, resolved);
		kfree(resolved);
		return 0;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_getcwd(uint32_t ebx, uint32_t ecx)
{
	{
		/*
         * ebx = pointer to output buffer in user space.
         * ecx = size of the buffer.
         *
         * Linux getcwd returns an absolute path.  Drunix stores cwd without a
         * leading slash internally, with an empty string meaning root.
         * Returns the byte count including the NUL terminator.
         */
		process_t *cur = sched_current();
		char *path;
		uint32_t len;

		if (!cur || ebx == 0 || ecx == 0)
			return (uint32_t)-1;
		path = syscall_alloc_path_scratch();
		if (!path)
			return (uint32_t)-1;
		const char *cwd = syscall_process_cwd(cur);
		if (cwd[0] == '\0')
			k_strncpy(path, "/", SYSCALL_PATH_MAX - 1u);
		else
			k_snprintf(path, SYSCALL_PATH_MAX, "/%s", cwd);
		path[SYSCALL_PATH_MAX - 1u] = '\0';

		len = k_strnlen(path, SYSCALL_PATH_MAX - 1u);
		if (len + 1u > ecx) {
			kfree(path);
			return (uint32_t)-LINUX_ERANGE;
		}
		if (uaccess_copy_to_user(cur, ebx, path, len + 1u) != 0) {
			kfree(path);
			return (uint32_t)-1;
		}
		kfree(path);
		return len + 1u;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_rename(uint32_t ebx, uint32_t ecx)
{
	process_t *cur = sched_current();

	return syscall_with_two_resolved_paths(
	    cur, ebx, ecx, syscall_vfs_rename_op);
}

uint32_t SYSCALL_NOINLINE syscall_case_renameat(uint32_t ebx,
                                                uint32_t ecx,
                                                uint32_t edx,
                                                uint32_t esi)
{
	process_t *cur = sched_current();

	return syscall_with_two_resolved_paths_at(
	    cur, ebx, ecx, edx, esi, syscall_vfs_rename_op);
}

uint32_t SYSCALL_NOINLINE syscall_case_linkat(
    uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	process_t *cur = sched_current();
	syscall_two_path_op_t op;

	if ((edi & ~LINUX_AT_SYMLINK_FOLLOW) != 0)
		return (uint32_t)-22;

	op = (edi & LINUX_AT_SYMLINK_FOLLOW) ? syscall_vfs_link_follow_op
	                                     : syscall_vfs_link_nofollow_op;
	return syscall_with_two_resolved_paths_at(cur, ebx, ecx, edx, esi, op);
}

uint32_t SYSCALL_NOINLINE syscall_case_symlinkat(uint32_t ebx,
                                                 uint32_t ecx,
                                                 uint32_t edx)
{
	{
		process_t *cur = sched_current();
		char *target;
		char *newpath;
		uint32_t ret;

		if (!cur)
			return (uint32_t)-1;
		target = copy_user_string_alloc(cur, ebx, SYSCALL_PATH_MAX);
		newpath = syscall_alloc_path_scratch();
		if (!target || !newpath) {
			if (target)
				kfree(target);
			if (newpath)
				kfree(newpath);
			return (uint32_t)-1;
		}
		if (target[0] == '\0' ||
		    resolve_user_path_at(cur, ecx, edx, newpath, SYSCALL_PATH_MAX) !=
		        0) {
			kfree(target);
			kfree(newpath);
			return (uint32_t)-1;
		}
		ret = (uint32_t)vfs_symlink(target, newpath);
		kfree(target);
		kfree(newpath);
		return ret;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_getdents(uint32_t ebx,
                                                uint32_t ecx,
                                                uint32_t edx)
{
	{
		/*
         * Linux i386 getdents(fd, dirp, count).  The older Drunix path-based
         * directory listing ABI is kept as SYS_DRUNIX_GETDENTS_PATH.
         */
		process_t *cur = sched_current();

		if (!cur || ebx >= MAX_FDS ||
		    proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
			return (uint32_t)-LINUX_EBADF;
		if (proc_fd_entries(cur)[ebx].type != FD_TYPE_DIR)
			return (uint32_t)-LINUX_ENOTDIR;
		return (uint32_t)linux_fill_getdents(
		    cur, &proc_fd_entries(cur)[ebx], ecx, edx);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_getdents_path(uint32_t ebx,
                                                            uint32_t ecx,
                                                            uint32_t edx)
{
	{
		/*
         * ebx = path pointer in user space (NULL = use process cwd)
         * ecx = pointer to output buffer in user space
         * edx = buffer size
         *
         * If ebx is NULL and the process cwd is non-empty, list the cwd.
         * Otherwise resolve the path relative to cwd.
         */
		process_t *cur = sched_current();
		char *upath = 0;
		char *rpath = syscall_alloc_path_scratch();
		char *kbuf;
		if (!rpath)
			return (uint32_t)-1;
		if (!cur) {
			kfree(rpath);
			return (uint32_t)-1;
		}
		if (ebx == 0) {
			/* NULL → list the process cwd (empty string lists root). */
			k_strncpy(rpath, syscall_process_cwd(cur), SYSCALL_PATH_MAX - 1u);
			rpath[SYSCALL_PATH_MAX - 1u] = '\0';
			upath = rpath;
		} else {
			upath = copy_user_string_alloc(cur, ebx, SYSCALL_PATH_MAX);
			if (!upath) {
				kfree(rpath);
				return (uint32_t)-1;
			}
			kcwd_resolve(
			    syscall_process_cwd(cur), upath, rpath, SYSCALL_PATH_MAX);
			kfree(upath);
			upath = rpath;
		}
		kbuf = (char *)kmalloc(edx ? edx : 1);
		if (!kbuf) {
			kfree(rpath);
			return (uint32_t)-1;
		}
		uint32_t ret =
		    (uint32_t)vfs_getdents(*upath ? upath : (const char *)0, kbuf, edx);
		if ((int32_t)ret >= 0 && uaccess_copy_to_user(cur, ecx, kbuf, ret) != 0)
			ret = (uint32_t)-1;
		kfree(kbuf);
		kfree(rpath);
		return ret;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_getdents64(uint32_t ebx,
                                                  uint32_t ecx,
                                                  uint32_t edx)
{
	{
		/*
         * Linux i386 getdents64(fd, dirp, count).  This is used by static
         * BusyBox/musl for directory iteration.
         */
		process_t *cur = sched_current();

		if (!cur || ebx >= MAX_FDS ||
		    proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
			return (uint32_t)-LINUX_EBADF;
		if (proc_fd_entries(cur)[ebx].type != FD_TYPE_DIR)
			return (uint32_t)-LINUX_ENOTDIR;
		return (uint32_t)linux_fill_getdents64(
		    cur, &proc_fd_entries(cur)[ebx], ecx, edx);
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

uint32_t SYSCALL_NOINLINE syscall_case_stat64_lstat64(uint32_t eax,
                                                      uint32_t ebx,
                                                      uint32_t ecx)
{
	/*
         * Linux i386 stat64/lstat64(path, struct stat64 *).
         * stat64 follows symlinks; lstat64 reports the link inode itself.
         */
	return syscall_stat64_path_common(ebx, ecx, eax == SYS_LSTAT64);
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
