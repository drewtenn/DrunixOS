/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * path.c - Linux cwd, path resolution, chdir, and getcwd syscalls.
 *
 * This file owns pathname resolution, current-working-directory helpers,
 * open/create/unlink/rename/link syscalls, stat/statx/statfs packing,
 * truncate, readlink, getcwd/chdir, and getdents compatibility.
 */

#include "../../syscall.h"
#include "../syscall_internal.h"
#include "../syscall_linux.h"
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
void kcwd_resolve(const char *cwd, const char *name, char *out, int outsz)
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
