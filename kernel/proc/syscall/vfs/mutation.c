/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * mutation.c - Linux path mutation and link syscalls.
 *
 * This file owns readlink, truncate, create, unlink, mkdir, rmdir,
 * rename, hard-link, and symlink syscall cases.
 */

#include "../../syscall.h"
#include "../syscall_internal.h"
#include "../syscall_linux.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdint.h>

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
