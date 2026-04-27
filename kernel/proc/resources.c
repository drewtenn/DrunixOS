/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "resources.h"
#include "kheap.h"
#include "kstring.h"
#include "pipe.h"
#include "pty.h"
#include "sched.h"
#include "vfs.h"

static void *alloc_zero(uint32_t size)
{
	void *ptr = kmalloc(size);
	if (ptr)
		k_memset(ptr, 0, size);
	return ptr;
}

static void proc_fd_table_bump_open_refs(proc_fd_table_t *files)
{
	if (!files)
		return;

	for (unsigned i = 0; i < MAX_FDS; i++) {
		file_handle_t *fh = &files->open_files[i];

		if (fh->type == FD_TYPE_PIPE_READ) {
			pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
			if (pb)
				pb->read_open++;
		} else if (fh->type == FD_TYPE_PIPE_WRITE) {
			pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
			if (pb)
				pb->write_open++;
		}
		if (fh->type == FD_TYPE_PTY_MASTER)
			pty_get_master(fh->u.pty.pty_idx);
		else if (fh->type == FD_TYPE_PTY_SLAVE)
			pty_get_slave(fh->u.pty.pty_idx);
	}
}

int proc_resource_init_fresh(process_t *proc)
{
	if (!proc)
		return -1;

	proc->as = (proc_address_space_t *)alloc_zero(sizeof(*proc->as));
	proc->files = (proc_fd_table_t *)alloc_zero(sizeof(*proc->files));
	proc->fs_state = (proc_fs_state_t *)alloc_zero(sizeof(*proc->fs_state));
	proc->sig_actions =
	    (proc_sig_actions_t *)alloc_zero(sizeof(*proc->sig_actions));

	if (!proc->as || !proc->files || !proc->fs_state || !proc->sig_actions) {
		proc_resource_put_all(proc);
		return -1;
	}

	proc->as->refs = 1;
	proc->files->refs = 1;
	proc->fs_state->refs = 1;
	proc->sig_actions->refs = 1;
	return 0;
}

void proc_resource_mirror_from_process(process_t *proc)
{
	if (!proc)
		return;

	if (proc->as) {
		proc->as->pd_phys = proc->pd_phys;
		proc->as->entry = proc->entry;
		proc->as->user_stack = proc->user_stack;
		proc->as->heap_start = proc->heap_start;
		proc->as->brk = proc->brk;
		proc->as->image_start = proc->image_start;
		proc->as->image_end = proc->image_end;
		proc->as->stack_low_limit = proc->stack_low_limit;
		proc->as->vma_count = proc->vma_count;
		k_memcpy(proc->as->vmas, proc->vmas, sizeof(proc->vmas));
		k_memcpy(proc->as->name, proc->name, sizeof(proc->name));
		k_memcpy(proc->as->psargs, proc->psargs, sizeof(proc->psargs));
	}

	if (proc->files)
		k_memcpy(proc->files->open_files,
		         proc->open_files,
		         sizeof(proc->open_files));

	if (proc->fs_state) {
		proc->fs_state->umask = proc->umask;
		k_memcpy(proc->fs_state->cwd, proc->cwd, sizeof(proc->cwd));
	}

	if (proc->sig_actions) {
		for (int i = 0; i < NSIG; i++)
			proc->sig_actions->handlers[i] = proc->sig_handlers[i];
	}
}

int proc_fd_table_dup(proc_fd_table_t **out, const proc_fd_table_t *src)
{
	if (!out || !src)
		return -1;

	proc_fd_table_t *dup = (proc_fd_table_t *)alloc_zero(sizeof(*dup));
	if (!dup)
		return -1;

	k_memcpy(dup, src, sizeof(*dup));
	dup->refs = 1;
	proc_fd_table_bump_open_refs(dup);
	*out = dup;
	return 0;
}

int proc_fs_state_dup(proc_fs_state_t **out, const proc_fs_state_t *src)
{
	if (!out || !src)
		return -1;

	proc_fs_state_t *dup = (proc_fs_state_t *)alloc_zero(sizeof(*dup));
	if (!dup)
		return -1;

	k_memcpy(dup, src, sizeof(*dup));
	dup->refs = 1;
	*out = dup;
	return 0;
}

int proc_sig_actions_dup(proc_sig_actions_t **out,
                         const proc_sig_actions_t *src)
{
	if (!out || !src)
		return -1;

	proc_sig_actions_t *dup = (proc_sig_actions_t *)alloc_zero(sizeof(*dup));
	if (!dup)
		return -1;

	k_memcpy(dup, src, sizeof(*dup));
	dup->refs = 1;
	*out = dup;
	return 0;
}

int proc_resource_clone_for_fork(process_t *child, const process_t *parent)
{
	if (!child || !parent || !parent->as || !parent->files ||
	    !parent->fs_state || !parent->sig_actions)
		return -1;

	child->as = (proc_address_space_t *)alloc_zero(sizeof(*child->as));
	child->files = 0;
	child->fs_state = 0;
	child->sig_actions = 0;
	if (!child->as)
		return -1;

	k_memcpy(child->as, parent->as, sizeof(*child->as));
	child->as->refs = 1;
	child->as->pd_phys = child->pd_phys;

	if (proc_fd_table_dup(&child->files, parent->files) != 0 ||
	    proc_fs_state_dup(&child->fs_state, parent->fs_state) != 0 ||
	    proc_sig_actions_dup(&child->sig_actions, parent->sig_actions) != 0) {
		proc_resource_put_all(child);
		return -1;
	}

	return 0;
}

void proc_resource_get_all(process_t *proc)
{
	if (!proc)
		return;

	if (proc->as)
		proc->as->refs++;
	if (proc->files)
		proc->files->refs++;
	if (proc->fs_state)
		proc->fs_state->refs++;
	if (proc->sig_actions)
		proc->sig_actions->refs++;
}

void proc_fd_table_close_all(proc_fd_table_t *files)
{
	if (!files)
		return;

	for (unsigned i = 0; i < MAX_FDS; i++) {
		file_handle_t *fh = &files->open_files[i];

		if (fh->type == FD_TYPE_NONE)
			continue;

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
		} else if (fh->type == FD_TYPE_PIPE_WRITE) {
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

		fh->type = FD_TYPE_NONE;
		fh->writable = 0;
		fh->access_mode = 0;
		fh->append = 0;
	}
}

void proc_resource_put_all(process_t *proc)
{
	if (!proc)
		return;

	if (proc->as) {
		proc_address_space_t *as = proc->as;
		if (as->refs > 0)
			as->refs--;
		if (as->refs == 0) {
			if (proc->pd_phys == as->pd_phys)
				process_release_user_space(proc);
			kfree(as);
			proc->as = 0;
		}
	}

	if (proc->files) {
		proc_fd_table_t *files = proc->files;
		if (files->refs > 0)
			files->refs--;
		if (files->refs == 0) {
			proc_fd_table_close_all(files);
			kfree(files);
			proc->files = 0;
		}
	}

	if (proc->fs_state) {
		proc_fs_state_t *fs_state = proc->fs_state;
		if (fs_state->refs > 0)
			fs_state->refs--;
		if (fs_state->refs == 0) {
			kfree(fs_state);
			proc->fs_state = 0;
		}
	}

	if (proc->sig_actions) {
		proc_sig_actions_t *sig_actions = proc->sig_actions;
		if (sig_actions->refs > 0)
			sig_actions->refs--;
		if (sig_actions->refs == 0) {
			kfree(sig_actions);
			proc->sig_actions = 0;
		}
	}
}

void proc_resource_put_files(process_t *proc)
{
	if (!proc || !proc->files)
		return;

	proc_fd_table_t *files = proc->files;
	if (files->refs > 0)
		files->refs--;
	if (files->refs == 0) {
		proc_fd_table_close_all(files);
		kfree(files);
	}
	proc->files = 0;
}

/*
 * execve resource release helpers.
 *
 * syscall_execve() builds a brand-new process_t for the replacement image and
 * then must drop the outgoing process's per-process resources before
 * sched_exec_current() swaps the two descriptors. We split the drop so the
 * outgoing page directory and kernel stack stay mapped until
 * process_exec_cleanup() runs under the new descriptor:
 *
 *   proc_resource_put_exec_nonfiles()
 *     Drops the address-space, fs-state, and sig-actions refs only. It does
 *     NOT call process_release_user_space(), because exec_cur->pd_phys and
 *     exec_cur->kstack_bottom must still be valid when we pass them into
 *     process_build_exec_frame() / sched_exec_current(). The user-space pages
 *     get freed later by process_exec_cleanup(), which runs on the NEW
 *     kernel stack after the switch.
 *
 *   proc_resource_put_exec_owner()
 *     Also drops the fd table. Pipe refcounts balance out across exec:
 *     process_create_file() inherits the fd table and bumps pipe refs +1 per
 *     open pipe end, and the proc_fd_table_close_all() inside this helper
 *     balances them -1.
 *
 * In the normal exec path every ref count starts at 1 (the outgoing process
 * is the sole owner of its resources), so these puts are effectively free
 * paths — but the refs>0 guards keep them safe if a future CLONE_VM thread
 * ever reaches execve with shared resources.
 */
void proc_resource_put_exec_nonfiles(process_t *proc)
{
	if (!proc)
		return;

	if (proc->as) {
		proc_address_space_t *as = proc->as;
		if (as->refs > 0)
			as->refs--;
		if (as->refs == 0)
			kfree(as);
		proc->as = 0;
	}

	if (proc->fs_state) {
		proc_fs_state_t *fs_state = proc->fs_state;
		if (fs_state->refs > 0)
			fs_state->refs--;
		if (fs_state->refs == 0)
			kfree(fs_state);
		proc->fs_state = 0;
	}

	if (proc->sig_actions) {
		proc_sig_actions_t *sig_actions = proc->sig_actions;
		if (sig_actions->refs > 0)
			sig_actions->refs--;
		if (sig_actions->refs == 0)
			kfree(sig_actions);
		proc->sig_actions = 0;
	}
}

void proc_resource_put_exec_owner(process_t *proc)
{
	if (!proc)
		return;

	proc_resource_put_exec_nonfiles(proc);

	proc_resource_put_files(proc);
}
