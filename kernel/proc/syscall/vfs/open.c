/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * open.c - Linux open/access and simple VFS compatibility syscalls.
 *
 * This file owns open/openat, access and metadata no-op compatibility,
 * plus helpers for installing VFS nodes into fd slots.
 */

#include "../syscall_internal.h"
#include "../syscall_linux.h"
#include "blkdev.h"
#include "fs.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include "process.h"
#include "pty.h"
#include "sched.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdint.h>

int linux_path_exists(process_t *cur, uint32_t user_path)
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
		proc_fd_entries(proc)[fd].u.chardev.offset = 0;
		return fd;

	case VFS_NODE_PTYMASTER: {
		int idx = pty_alloc_master();

		if (idx < 0) {
			proc_fd_entries(proc)[fd].type = FD_TYPE_NONE;
			return -1;
		}
		proc_fd_entries(proc)[fd].type = FD_TYPE_PTY_MASTER;
		proc_fd_entries(proc)[fd].u.pty.pty_idx = (uint32_t)idx;
		return fd;
	}

	case VFS_NODE_PTYSLAVE:
		if (pty_open_slave(node->dev_id) != 0) {
			proc_fd_entries(proc)[fd].type = FD_TYPE_NONE;
			return -1;
		}
		proc_fd_entries(proc)[fd].type = FD_TYPE_PTY_SLAVE;
		proc_fd_entries(proc)[fd].u.pty.pty_idx = node->dev_id;
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
	    node.type != VFS_NODE_CHARDEV && node.type != VFS_NODE_PTYMASTER &&
	    node.type != VFS_NODE_PTYSLAVE)
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
