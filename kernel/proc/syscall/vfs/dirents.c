/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * dirents.c - Linux getdents compatibility syscalls.
 *
 * This file owns getdents/getdents64 packing for fd-backed directories.
 */

#include "../syscall_internal.h"
#include "../syscall_linux.h"
#include "kheap.h"
#include "kstring.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdint.h>

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
