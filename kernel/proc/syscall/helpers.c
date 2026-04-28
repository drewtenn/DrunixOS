/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * helpers.c - shared syscall fd and console helpers.
 *
 * This file owns helper routines that are used by multiple syscall domains:
 * fd-table allocation/lookup/close and console-output routing through the GUI
 * desktop or legacy VGA console.
 */

#include "syscall_internal.h"
#include "console/runtime.h"
#include "pipe.h"
#include "process.h"
#include "pty.h"
#include "sched.h"
#include "tty.h"
#include "vfs.h"
#include "wmdev.h"
#include <stdint.h>

/*
 * fd_alloc: find the lowest free fd slot in the process's table.
 * Returns the slot index (0–MAX_FDS-1) or -1 if the table is full.
 * Note: slots 0/1/2 are pre-populated by process_create(), so in the
 * normal case the first free slot will be 3.
 */
file_handle_t *proc_fd_entries(process_t *proc)
{
	if (!proc)
		return 0;
	return proc && proc->files ? proc->files->open_files : proc->open_files;
}

int fd_alloc(process_t *proc)
{
	file_handle_t *files = proc_fd_entries(proc);

	if (!files)
		return -1;

	for (unsigned i = 0; i < MAX_FDS; i++) {
		if (files[i].type == FD_TYPE_NONE)
			return (int)i;
	}
	return -1;
}

int syscall_fd_is_console_output(const file_handle_t *fh)
{
	if (!fh)
		return 0;
	return fh->type == FD_TYPE_TTY && fh->writable;
}

int syscall_write_console_bytes(process_t *cur, const char *buf, uint32_t len)
{
	return console_runtime_write_process_output(cur, buf, len);
}

/*
 * fd_close_one: close a single fd slot.
 *
 * - DUFS files: flush the inode if writable.
 * - Pipe ends: decrement the appropriate refcount; free the pipe buffer
 *   once both read_open and write_open reach zero.
 */
void fd_close_one(process_t *proc, unsigned fd)
{
	file_handle_t *fh = &proc_fd_entries(proc)[fd];

	if (fh->type == FD_TYPE_FILE && fh->writable)
		vfs_flush(fh->u.file.ref);

	if (fh->type == FD_TYPE_PIPE_READ) {
		pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
		if (pb) {
			if (pb->read_open > 0)
				pb->read_open--;
			sched_wake_all(&pb->waiters);
			if (pb->read_open == 0 && pb->write_open == 0)
				pipe_free((int)fh->u.pipe.pipe_idx);
		}
	}

	if (fh->type == FD_TYPE_PIPE_WRITE) {
		pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
		if (pb) {
			if (pb->write_open > 0)
				pb->write_open--;
			sched_wake_all(&pb->waiters);
			if (pb->read_open == 0 && pb->write_open == 0)
				pipe_free((int)fh->u.pipe.pipe_idx);
		}
	}

	if (fh->type == FD_TYPE_PTY_MASTER)
		pty_release_master(fh->u.pty.pty_idx);
	else if (fh->type == FD_TYPE_PTY_SLAVE)
		pty_release_slave(fh->u.pty.pty_idx);
	if (fh->type == FD_TYPE_WM)
		wmdev_close(fh->u.wm.conn_id);

	fh->type = FD_TYPE_NONE;
	fh->writable = 0;
	fh->access_mode = 0;
	fh->append = 0;
	fh->cloexec = 0;
	fh->nonblock = 0;
}

#ifdef KTEST_ENABLED
int syscall_console_write_for_test(process_t *proc,
                                   const char *buf,
                                   uint32_t len)
{
	return syscall_write_console_bytes(proc, buf, len);
}
#endif
