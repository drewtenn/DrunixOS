/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * procfs.c — synthetic /proc filesystem exposing live kernel state.
 */

#include "procfs.h"
#include "sched.h"
#include "module.h"
#include "mem_forensics.h"
#include "kheap.h"
#include "kprintf.h"
#include "kstring.h"
#include "klog.h"
#include <stdarg.h>
#include <stdint.h>

#define PROCFS_RENDER_CAP 16384u

extern int module_snapshot(module_info_t *out, uint32_t max)
    __attribute__((weak));

typedef struct {
	char *buf;
	uint32_t cap;
	uint32_t len;
} render_buf_t;

static void procfs_dir_stat(vfs_stat_t *st)
{
	st->type = 2;
	st->size = 0;
	st->link_count = 1;
	st->mtime = 0;
}

static void procfs_file_stat(vfs_stat_t *st, uint32_t size)
{
	st->type = 1;
	st->size = size;
	st->link_count = 1;
	st->mtime = 0;
}

static void procfs_emitf(render_buf_t *rb, const char *fmt, ...)
{
	va_list ap;
	uint32_t avail = (rb->cap > rb->len) ? rb->cap - rb->len : 0;
	int n;

	va_start(ap, fmt);
	n = k_vsnprintf(avail ? rb->buf + rb->len : 0, avail, fmt, ap);
	va_end(ap);

	if (n > 0)
		rb->len += (uint32_t)n;
}

static uint32_t procfs_rendered_size(uint32_t len)
{
	if (len >= PROCFS_RENDER_CAP)
		return PROCFS_RENDER_CAP - 1u;
	return len;
}

static int procfs_append_dirent(
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

static int
procfs_parse_u32(const char *s, uint32_t *value_out, const char **end_out)
{
	uint32_t value = 0;
	int digits = 0;

	if (!s || !value_out)
		return -1;

	while (*s >= '0' && *s <= '9') {
		value = value * 10u + (uint32_t)(*s - '0');
		s++;
		digits++;
	}

	if (digits == 0)
		return -1;

	*value_out = value;
	if (end_out)
		*end_out = s;
	return 0;
}

static const process_t *procfs_lookup_process(uint32_t pid)
{
	const task_group_t *group = sched_find_group(pid, 1);

	if (!group)
		return 0;
	return sched_find_process(task_group_leader_tid(group), 1);
}

static int procfs_parse_path(const char *relpath,
                             uint32_t *node_type_out,
                             uint32_t *kind_out,
                             uint32_t *pid_out,
                             uint32_t *index_out)
{
	uint32_t pid = 0;
	uint32_t index = 0;
	const char *slash;
	const process_t *proc;

	if (!relpath || relpath[0] == '\0') {
		if (node_type_out)
			*node_type_out = VFS_NODE_DIR;
		if (kind_out)
			*kind_out = PROCFS_FILE_NONE;
		if (pid_out)
			*pid_out = 0;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (k_strcmp(relpath, "kmsg") == 0) {
		if (node_type_out)
			*node_type_out = VFS_NODE_PROCFILE;
		if (kind_out)
			*kind_out = PROCFS_FILE_KMSG;
		if (pid_out)
			*pid_out = 0;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (k_strcmp(relpath, "modules") == 0) {
		if (node_type_out)
			*node_type_out = VFS_NODE_PROCFILE;
		if (kind_out)
			*kind_out = PROCFS_FILE_MODULES;
		if (pid_out)
			*pid_out = 0;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (k_strcmp(relpath, "mounts") == 0) {
		if (node_type_out)
			*node_type_out = VFS_NODE_PROCFILE;
		if (kind_out)
			*kind_out = PROCFS_FILE_MOUNTS;
		if (pid_out)
			*pid_out = 0;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (procfs_parse_u32(relpath, &pid, &slash) != 0)
		return -1;

	proc = procfs_lookup_process(pid);
	if (!proc)
		return -1;

	if (*slash == '\0') {
		if (node_type_out)
			*node_type_out = VFS_NODE_DIR;
		if (kind_out)
			*kind_out = PROCFS_FILE_NONE;
		if (pid_out)
			*pid_out = pid;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (*slash != '/')
		return -1;
	slash++;

	if (k_strcmp(slash, "status") == 0) {
		if (node_type_out)
			*node_type_out = VFS_NODE_PROCFILE;
		if (kind_out)
			*kind_out = PROCFS_FILE_STATUS;
		if (pid_out)
			*pid_out = pid;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (k_strcmp(slash, "maps") == 0) {
		if (node_type_out)
			*node_type_out = VFS_NODE_PROCFILE;
		if (kind_out)
			*kind_out = PROCFS_FILE_MAPS;
		if (pid_out)
			*pid_out = pid;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (k_strcmp(slash, "vmstat") == 0) {
		if (node_type_out)
			*node_type_out = VFS_NODE_PROCFILE;
		if (kind_out)
			*kind_out = PROCFS_FILE_VMSTAT;
		if (pid_out)
			*pid_out = pid;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (k_strcmp(slash, "fault") == 0) {
		if (node_type_out)
			*node_type_out = VFS_NODE_PROCFILE;
		if (kind_out)
			*kind_out = PROCFS_FILE_FAULT;
		if (pid_out)
			*pid_out = pid;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (k_strcmp(slash, "fd") == 0) {
		if (node_type_out)
			*node_type_out = VFS_NODE_DIR;
		if (kind_out)
			*kind_out = PROCFS_FILE_NONE;
		if (pid_out)
			*pid_out = pid;
		if (index_out)
			*index_out = 0;
		return 0;
	}

	if (k_strncmp(slash, "fd/", 3) == 0) {
		if (procfs_parse_u32(slash + 3, &index, &slash) != 0 || *slash != '\0')
			return -1;
		if (index >= MAX_FDS || proc->open_files[index].type == FD_TYPE_NONE)
			return -1;
		if (node_type_out)
			*node_type_out = VFS_NODE_PROCFILE;
		if (kind_out)
			*kind_out = PROCFS_FILE_FD;
		if (pid_out)
			*pid_out = pid;
		if (index_out)
			*index_out = index;
		return 0;
	}

	return -1;
}

static char procfs_state_code(uint32_t state)
{
	switch (state) {
	case PROC_READY:
		return 'R';
	case PROC_RUNNING:
		return 'R';
	case PROC_ZOMBIE:
		return 'Z';
	case PROC_BLOCKED:
		return 'S';
	case PROC_STOPPED:
		return 'T';
	default:
		return '?';
	}
}

static const char *procfs_state_name(uint32_t state)
{
	switch (state) {
	case PROC_READY:
		return "ready";
	case PROC_RUNNING:
		return "running";
	case PROC_ZOMBIE:
		return "zombie";
	case PROC_BLOCKED:
		return "blocked";
	case PROC_STOPPED:
		return "stopped";
	default:
		return "unused";
	}
}

static uint32_t procfs_open_fd_count(const process_t *proc)
{
	uint32_t count = 0;

	if (!proc)
		return 0;

	for (uint32_t i = 0; i < MAX_FDS; i++) {
		if (proc->open_files[i].type != FD_TYPE_NONE)
			count++;
	}

	return count;
}

static void procfs_render_status(render_buf_t *rb, const process_t *proc)
{
	uint32_t tgid = proc->tgid ? proc->tgid : proc->pid;

	procfs_emitf(rb, "Name:\t%s\n", proc->name[0] ? proc->name : "(unnamed)");
	procfs_emitf(rb,
	             "State:\t%c (%s)\n",
	             procfs_state_code(proc->state),
	             procfs_state_name(proc->state));
	procfs_emitf(rb, "Pid:\t%u\n", tgid);
	procfs_emitf(rb, "Tgid:\t%u\n", tgid);
	procfs_emitf(rb,
	             "Threads:\t%u\n",
	             proc->group ? task_group_live_count(proc->group) : 1u);
	procfs_emitf(rb, "PPid:\t%u\n", proc->parent_pid);
	procfs_emitf(rb, "PGid:\t%u\n", proc->pgid);
	procfs_emitf(rb, "Sid:\t%u\n", proc->sid);
	procfs_emitf(rb, "Tty:\ttty%u\n", proc->tty_id);
	procfs_emitf(
	    rb, "Image:\t0x%08x-0x%08x\n", proc->image_start, proc->image_end);
	procfs_emitf(rb, "Heap:\t0x%08x-0x%08x\n", proc->heap_start, proc->brk);
	procfs_emitf(
	    rb, "Stack:\t0x%08x-0x%08x\n", proc->stack_low_limit, USER_STACK_TOP);
	procfs_emitf(rb, "SigPnd:\t0x%08x\n", proc->sig_pending);
	procfs_emitf(rb, "SigBlk:\t0x%08x\n", proc->sig_blocked);
	procfs_emitf(rb, "FDs:\t%u\n", procfs_open_fd_count(proc));
	procfs_emitf(rb, "Cmd:\t%s\n", proc->psargs[0] ? proc->psargs : proc->name);
}

static const char *procfs_fd_kind_name(uint32_t kind)
{
	switch (kind) {
	case PROCFS_FILE_STATUS:
		return "status";
	case PROCFS_FILE_MAPS:
		return "maps";
	case PROCFS_FILE_VMSTAT:
		return "vmstat";
	case PROCFS_FILE_FAULT:
		return "fault";
	case PROCFS_FILE_FD:
		return "fd";
	case PROCFS_FILE_MODULES:
		return "modules";
	case PROCFS_FILE_KMSG:
		return "kmsg";
	case PROCFS_FILE_MOUNTS:
		return "mounts";
	default:
		return "unknown";
	}
}

static void
procfs_render_fd(render_buf_t *rb, const process_t *proc, uint32_t fd)
{
	const file_handle_t *fh;

	if (!proc || fd >= MAX_FDS)
		return;

	fh = &proc->open_files[fd];
	switch (fh->type) {
	case FD_TYPE_FILE:
		procfs_emitf(rb,
		             "type=file inode=%u size=%u offset=%u writable=%u\n",
		             fh->u.file.inode_num,
		             fh->u.file.size,
		             fh->u.file.offset,
		             fh->writable);
		break;
	case FD_TYPE_CHARDEV:
		procfs_emitf(rb,
		             "type=chardev name=%s writable=%u\n",
		             fh->u.chardev.name,
		             fh->writable);
		break;
	case FD_TYPE_PIPE_READ:
		procfs_emitf(rb, "type=pipe dir=read pipe=%u\n", fh->u.pipe.pipe_idx);
		break;
	case FD_TYPE_PIPE_WRITE:
		procfs_emitf(rb, "type=pipe dir=write pipe=%u\n", fh->u.pipe.pipe_idx);
		break;
	case FD_TYPE_TTY:
		procfs_emitf(rb,
		             "type=tty index=%u writable=%u\n",
		             fh->u.tty.tty_idx,
		             fh->writable);
		break;
	case FD_TYPE_PROCFILE:
		procfs_emitf(rb,
		             "type=procfs kind=%s pid=%u index=%u\n",
		             procfs_fd_kind_name(fh->u.proc.kind),
		             fh->u.proc.pid,
		             fh->u.proc.index);
		break;
	default:
		procfs_emitf(rb, "type=unknown\n");
		break;
	}
}

static void procfs_render_modules(render_buf_t *rb)
{
	module_info_t mods[MODULE_MAX_LOADED];
	int n;

	if (!module_snapshot)
		return;
	n = module_snapshot(mods, MODULE_MAX_LOADED);

	for (int i = 0; i < n; i++) {
		procfs_emitf(rb,
		             "%s %u 0x%08x live\n",
		             mods[i].name,
		             mods[i].size,
		             mods[i].base);
	}
}

static void procfs_render_mounts(render_buf_t *rb)
{
	vfs_mount_info_t info;

	for (uint32_t i = 0; i < vfs_mount_count(); i++) {
		if (vfs_mount_info_at(i, &info) != 0)
			continue;
		procfs_emitf(rb,
		             "%s %s %s %s 0 0\n",
		             info.source,
		             info.path[0] ? info.path : "/",
		             info.fstype,
		             info.options);
	}
}

static int procfs_render_file(uint32_t kind,
                              uint32_t pid,
                              uint32_t index,
                              char *buf,
                              uint32_t cap,
                              uint32_t *size_out)
{
	render_buf_t rb = {buf, cap, 0};
	const process_t *proc = 0;

	if (kind != PROCFS_FILE_MODULES && kind != PROCFS_FILE_KMSG &&
	    kind != PROCFS_FILE_MOUNTS) {
		proc = procfs_lookup_process(pid);
		if (!proc)
			return -1;
	}

	switch (kind) {
	case PROCFS_FILE_STATUS:
		procfs_render_status(&rb, proc);
		break;
	case PROCFS_FILE_MAPS:
		return mem_forensics_render_maps(proc, buf, cap, size_out);
	case PROCFS_FILE_VMSTAT:
		return mem_forensics_render_vmstat(proc, buf, cap, size_out);
	case PROCFS_FILE_FAULT:
		return mem_forensics_render_fault(proc, buf, cap, size_out);
	case PROCFS_FILE_FD:
		if (index >= MAX_FDS || proc->open_files[index].type == FD_TYPE_NONE)
			return -1;
		procfs_render_fd(&rb, proc, index);
		break;
	case PROCFS_FILE_MODULES:
		procfs_render_modules(&rb);
		break;
	case PROCFS_FILE_MOUNTS:
		procfs_render_mounts(&rb);
		break;
	case PROCFS_FILE_KMSG:
		if (klog_snapshot(buf, cap, size_out) != 0)
			return -1;
		return 0;
	default:
		return -1;
	}

	if (size_out)
		*size_out = procfs_rendered_size(rb.len);
	return 0;
}

int procfs_fill_node(const char *relpath, vfs_node_t *node_out)
{
	uint32_t node_type = 0;
	uint32_t kind = 0;
	uint32_t pid = 0;
	uint32_t index = 0;
	uint32_t size = 0;

	if (!node_out)
		return -1;

	if (procfs_parse_path(relpath, &node_type, &kind, &pid, &index) != 0)
		return -1;

	node_out->type = node_type;
	node_out->inode_num = 0;
	node_out->mount_id = 0;
	node_out->size = 0;
	node_out->dev_id = 0;
	node_out->dev_name[0] = '\0';
	node_out->proc_kind = kind;
	node_out->proc_pid = pid;
	node_out->proc_index = index;

	if (node_type == VFS_NODE_PROCFILE &&
	    procfs_file_size(kind, pid, index, &size) == 0)
		node_out->size = size;

	return 0;
}

int procfs_stat(const char *relpath, vfs_stat_t *st)
{
	uint32_t node_type = 0;
	uint32_t kind = 0;
	uint32_t pid = 0;
	uint32_t index = 0;
	uint32_t size = 0;

	if (!st)
		return -1;

	if (procfs_parse_path(relpath, &node_type, &kind, &pid, &index) != 0)
		return -1;

	if (node_type == VFS_NODE_DIR) {
		procfs_dir_stat(st);
		return 0;
	}

	if (procfs_file_size(kind, pid, index, &size) != 0)
		return -1;
	procfs_file_stat(st, size);
	return 0;
}

int procfs_getdents(const char *relpath, char *buf, uint32_t bufsz)
{
	uint32_t written = 0;
	uint32_t pid = 0;
	uint32_t index = 0;
	uint32_t node_type = 0;
	uint32_t kind = 0;
	char name[16];

	if (!buf)
		return -1;

	if (!relpath || relpath[0] == '\0') {
		uint32_t tgids[MAX_PROCS];
		int n = sched_snapshot_tgids(tgids, MAX_PROCS, 1);

		if (procfs_append_dirent(buf, bufsz, &written, "kmsg", 0) != 0)
			return (int)written;
		if (procfs_append_dirent(buf, bufsz, &written, "modules", 0) != 0)
			return (int)written;
		if (procfs_append_dirent(buf, bufsz, &written, "mounts", 0) != 0)
			return (int)written;

		for (int i = 0; i < n; i++) {
			k_snprintf(name, sizeof(name), "%u", tgids[i]);
			if (procfs_append_dirent(buf, bufsz, &written, name, 1) != 0)
				break;
		}
		return (int)written;
	}

	if (procfs_parse_path(relpath, &node_type, &kind, &pid, &index) != 0)
		return -1;

	if (node_type != VFS_NODE_DIR)
		return -1;

	if (pid == 0) {
		if (procfs_append_dirent(buf, bufsz, &written, "kmsg", 0) != 0)
			return (int)written;
		if (procfs_append_dirent(buf, bufsz, &written, "modules", 0) != 0)
			return (int)written;
		if (procfs_append_dirent(buf, bufsz, &written, "mounts", 0) != 0)
			return (int)written;
		return (int)written;
	}

	{
		const char *tail = k_strrchr(relpath, '/');
		tail = tail ? tail + 1 : relpath;
		if (k_strcmp(tail, "fd") == 0) {
			const process_t *proc = procfs_lookup_process(pid);
			if (!proc)
				return -1;

			for (uint32_t fd = 0; fd < MAX_FDS; fd++) {
				if (proc->open_files[fd].type == FD_TYPE_NONE)
					continue;
				k_snprintf(name, sizeof(name), "%u", fd);
				if (procfs_append_dirent(buf, bufsz, &written, name, 0) != 0)
					break;
			}
			return (int)written;
		}
	}

	if (procfs_append_dirent(buf, bufsz, &written, "status", 0) != 0)
		return (int)written;
	if (procfs_append_dirent(buf, bufsz, &written, "maps", 0) != 0)
		return (int)written;
	if (procfs_append_dirent(buf, bufsz, &written, "vmstat", 0) != 0)
		return (int)written;
	if (procfs_append_dirent(buf, bufsz, &written, "fault", 0) != 0)
		return (int)written;
	if (procfs_append_dirent(buf, bufsz, &written, "fd", 1) != 0)
		return (int)written;
	return (int)written;
}

int procfs_file_size(uint32_t kind,
                     uint32_t pid,
                     uint32_t index,
                     uint32_t *size_out)
{
	uint32_t size = 0;

	if (!size_out)
		return -1;

	if (procfs_render_file(kind, pid, index, 0, 0, &size) != 0)
		return -1;

	*size_out = size;
	return 0;
}

int procfs_read_file(uint32_t kind,
                     uint32_t pid,
                     uint32_t index,
                     uint32_t offset,
                     char *buf,
                     uint32_t count)
{
	char *render;
	uint32_t size = 0;

	if (!buf)
		return -1;
	render = (char *)kmalloc(PROCFS_RENDER_CAP);
	if (!render)
		return -1;

	if (procfs_render_file(
	        kind, pid, index, render, PROCFS_RENDER_CAP, &size) != 0) {
		kfree(render);
		return -1;
	}

	if (offset >= size) {
		kfree(render);
		return 0;
	}
	if (count > size - offset)
		count = size - offset;

	k_memcpy(buf, render + offset, count);
	kfree(render);
	return (int)count;
}
