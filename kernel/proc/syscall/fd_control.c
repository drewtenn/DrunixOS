/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * fd_control.c - Linux fd control, polling, pipe, dup, and seek syscalls.
 *
 * This file owns poll/select, close/dup/fcntl, pipe/pipe2, and
 * lseek/_llseek helpers for open file descriptors.
 */

#include "syscall_internal.h"
#include "syscall_linux.h"
#include "klog.h"
#include "pipe.h"
#include "process.h"
#include "procfs.h"
#include "pty.h"
#include "sched.h"
#include "uaccess.h"
#include "vfs.h"
#include "wmdev.h"
#include <stdint.h>

static uint32_t linux_poll_revents(process_t *cur, int32_t fd, uint32_t events)
{
	file_handle_t *fh;
	uint32_t rev = 0;

	if (!cur || fd < 0 || (uint32_t)fd >= MAX_FDS)
		return 0;
	fh = &proc_fd_entries(cur)[(uint32_t)fd];
	if (fh->type == FD_TYPE_NONE)
		return 0x0020u; /* POLLNVAL */

	if (events & LINUX_POLLOUT) {
		if (fh->type == FD_TYPE_PIPE_WRITE || fh->type == FD_TYPE_WM ||
		    fh->writable)
			rev |= LINUX_POLLOUT;
	}

	if (events & LINUX_POLLIN) {
		if ((fh->type == FD_TYPE_FILE || fh->type == FD_TYPE_SYSFILE) &&
		    fh->u.file.offset < fh->u.file.size)
			rev |= LINUX_POLLIN;
		else if (fh->type == FD_TYPE_BLOCKDEV &&
		         fh->u.blockdev.offset < fh->u.blockdev.size)
			rev |= LINUX_POLLIN;
		else if (fh->type == FD_TYPE_PROCFILE)
			rev |= LINUX_POLLIN;
		else if (fh->type == FD_TYPE_PIPE_READ) {
			pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
			if (pb && (pb->count > 0 || pb->write_open == 0))
				rev |= LINUX_POLLIN;
		} else if (fh->type == FD_TYPE_TTY) {
			if (tty_read_available((int)fh->u.tty.tty_idx) > 0)
				rev |= LINUX_POLLIN;
		} else if (fh->type == FD_TYPE_WM) {
			if (wmdev_event_available(fh->u.wm.conn_id) ||
			    wmdev_server_msg_available(fh->u.wm.conn_id))
				rev |= LINUX_POLLIN;
		}
	}

	return rev;
}

static void fd_bump_open_ref(file_handle_t *fh)
{
	pipe_buf_t *pb;

	if (!fh)
		return;
	if (fh->type == FD_TYPE_PIPE_READ) {
		pb = pipe_get((int)fh->u.pipe.pipe_idx);
		if (pb)
			pb->read_open++;
	} else if (fh->type == FD_TYPE_PIPE_WRITE) {
		pb = pipe_get((int)fh->u.pipe.pipe_idx);
		if (pb)
			pb->write_open++;
	}
	if (fh->type == FD_TYPE_PTY_MASTER)
		pty_get_master(fh->u.pty.pty_idx);
	else if (fh->type == FD_TYPE_PTY_SLAVE)
		pty_get_slave(fh->u.pty.pty_idx);
	else if (fh->type == FD_TYPE_WM)
		(void)wmdev_retain(fh->u.wm.conn_id);
}

static int fd_duplicate_from(process_t *proc,
                             uint32_t oldfd,
                             uint32_t minfd,
                             uint32_t cloexec)
{
	uint32_t fd;

	if (!proc || oldfd >= MAX_FDS || minfd >= MAX_FDS)
		return -1;
	if (proc_fd_entries(proc)[oldfd].type == FD_TYPE_NONE)
		return -1;

	for (fd = minfd; fd < MAX_FDS; fd++) {
		if (proc_fd_entries(proc)[fd].type == FD_TYPE_NONE) {
			proc_fd_entries(proc)[fd] = proc_fd_entries(proc)[oldfd];
			proc_fd_entries(proc)[fd].cloexec = cloexec ? 1u : 0u;
			fd_bump_open_ref(&proc_fd_entries(proc)[fd]);
			return (int)fd;
		}
	}
	return -1;
}

static uint32_t linux_fd_status_flags(const file_handle_t *fh)
{
	uint32_t flags;

	if (!fh)
		return 0;
	flags = fh->access_mode & LINUX_O_ACCMODE;
	if (fh->type == FD_TYPE_FILE && fh->append)
		flags |= LINUX_O_APPEND;
	if (fh->nonblock)
		flags |= LINUX_O_NONBLOCK;
	return flags;
}

static int syscall_seek_handle(process_t *cur,
                               uint32_t fd,
                               int64_t offset,
                               uint32_t whence,
                               uint64_t *new_offset_out)
{
	file_handle_t *fh;
	uint32_t size = 0;
	uint32_t current = 0;
	int64_t base;
	int64_t new_off;

	if (!cur || fd >= MAX_FDS)
		return -LINUX_EBADF;
	fh = &proc_fd_entries(cur)[fd];
	if (fh->type != FD_TYPE_FILE && fh->type != FD_TYPE_SYSFILE &&
	    fh->type != FD_TYPE_PROCFILE)
		return -LINUX_EBADF;

	if (fh->type == FD_TYPE_FILE || fh->type == FD_TYPE_SYSFILE) {
		size = fh->u.file.size;
		current = fh->u.file.offset;
	} else {
		if (procfs_file_size(
		        fh->u.proc.kind, fh->u.proc.pid, fh->u.proc.index, &size) != 0)
			return -1;
		fh->u.proc.size = size;
		current = fh->u.proc.offset;
	}

	switch (whence) {
	case 0:
		base = 0;
		break; /* SEEK_SET */
	case 1:
		base = (int64_t)current;
		break; /* SEEK_CUR */
	case 2:
		base = (int64_t)size;
		break; /* SEEK_END */
	default:
		return -LINUX_EINVAL;
	}

	new_off = base + offset;
	if (new_off < 0 || new_off > UINT32_MAX)
		return -1;

	if (fh->type == FD_TYPE_FILE || fh->type == FD_TYPE_SYSFILE)
		fh->u.file.offset = (uint32_t)new_off;
	else
		fh->u.proc.offset = (uint32_t)new_off;
	if (new_offset_out)
		*new_offset_out = (uint64_t)new_off;
	return 0;
}
uint32_t SYSCALL_NOINLINE syscall_case_close(uint32_t ebx)
{
	{
		/*
         * ebx = fd to close.
         * Flushes writable DUFS files, then frees the slot.
         */
		if (ebx >= MAX_FDS)
			return (uint32_t)-1;

		process_t *cur = sched_current();
		if (!cur)
			return (uint32_t)-1;

		if (proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
			return (uint32_t)-1;

		fd_close_one(cur, ebx);
		return 0;
	}
}

static uint32_t poll_timeout_ticks(int32_t timeout_ms)
{
	uint32_t ms;
	uint32_t seconds;
	uint32_t remainder_ms;
	uint32_t ticks;
	uint32_t remainder_ticks;

	if (timeout_ms <= 0)
		return 0;
	ms = (uint32_t)timeout_ms;
	seconds = ms / 1000u;
	remainder_ms = ms % 1000u;
	if (seconds > 0xFFFFFFFFu / SCHED_HZ)
		return 0xFFFFFFFFu;
	ticks = seconds * SCHED_HZ;
	remainder_ticks = (remainder_ms * SCHED_HZ + 999u) / 1000u;
	if (0xFFFFFFFFu - ticks < remainder_ticks)
		return 0xFFFFFFFFu;
	ticks += remainder_ticks;
	return ticks == 0 ? 1u : ticks;
}

#ifdef KTEST_ENABLED
static void (*g_poll_wait_hook_for_test)(uint32_t deadline_tick);

void syscall_poll_set_wait_hook_for_test(void (*hook)(uint32_t deadline_tick))
{
	g_poll_wait_hook_for_test = hook;
}
#endif

static void poll_wait_until(uint32_t deadline_tick)
{
#ifdef KTEST_ENABLED
	if (g_poll_wait_hook_for_test) {
		g_poll_wait_hook_for_test(deadline_tick);
		return;
	}
#endif
	sched_block_until(deadline_tick);
}

static uint32_t poll_scan_user_fds(process_t *cur,
                                   uint32_t user_fds,
                                   uint32_t nfds,
                                   int *error_out)
{
	uint32_t ready = 0;

	if (error_out)
		*error_out = 0;
	for (uint32_t i = 0; i < nfds; i++) {
		uint8_t pfd[8];
		int32_t fd;
		uint32_t events;
		uint32_t revents;

		if (uaccess_copy_from_user(
		        cur, pfd, user_fds + i * sizeof(pfd), sizeof(pfd)) != 0) {
			if (error_out)
				*error_out = -1;
			return 0;
		}
		fd = (int32_t)linux_get_u32(pfd, 0u);
		events = linux_get_u16(pfd, 4u);
		revents = linux_poll_revents(cur, fd, events);
		linux_put_u16(pfd, 6u, revents);
		if (uaccess_copy_to_user(
		        cur, user_fds + i * sizeof(pfd), pfd, sizeof(pfd)) != 0) {
			if (error_out)
				*error_out = -1;
			return 0;
		}
		if (revents)
			ready++;
	}

	return ready;
}

uint32_t SYSCALL_NOINLINE
syscall_case_poll(uint32_t ebx, uint32_t ecx, uint32_t edx)
{
	{
		/*
         * Linux i386 poll(struct pollfd *fds, nfds_t nfds, int timeout).
         * Readiness is rescanned on each scheduler tick while a timeout is
         * active.  fd-specific wait queues can replace the tick wait later,
         * but this preserves poll's visible timeout contract.
         */
		process_t *cur = sched_current();
		int32_t timeout_ms = (int32_t)edx;
		uint32_t deadline = 0;
		uint32_t timeout_ticks = 0;
		int err = 0;

		if (!cur || ecx > 1024u)
			return (uint32_t)-1;
		if (ecx != 0 && ebx == 0)
			return (uint32_t)-LINUX_EFAULT;
		if (ecx != 0 && ebx > 0xFFFFFFFFu - (ecx - 1u) * 8u)
			return (uint32_t)-LINUX_EFAULT;

		if (timeout_ms > 0) {
			timeout_ticks = poll_timeout_ticks(timeout_ms);
			deadline = sched_ticks() + timeout_ticks;
		}

		for (;;) {
			uint32_t ready = poll_scan_user_fds(cur, ebx, ecx, &err);

			if (err != 0)
				return (uint32_t)-1;
			if (ready != 0 || timeout_ms == 0)
				return ready;
			if (sched_process_has_unblocked_signal(cur))
				return (uint32_t)-1;
			if (timeout_ms > 0) {
				uint32_t now = sched_ticks();
				uint32_t next = now + 1u;

				if ((int32_t)(deadline - now) <= 0)
					return 0;
				if ((int32_t)(deadline - next) < 0)
					next = deadline;
				poll_wait_until(next);
			} else {
				poll_wait_until(sched_ticks() + 1u);
			}
		}
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_fcntl64(uint32_t ebx,
                                               uint32_t ecx,
                                               uint32_t edx)
{
	{
		/*
         * Minimal Linux fcntl64 for libc/app startup: fd flags, status flags,
         * and fd duplication.  Close-on-exec is accepted but not tracked yet.
         */
		process_t *cur = sched_current();
		int dup_fd;

		if (!cur || ebx >= MAX_FDS)
			return (uint32_t)-1;
		if (proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
			return (uint32_t)-1;

		switch (ecx) {
		case LINUX_F_DUPFD:
			dup_fd = fd_duplicate_from(cur, ebx, edx, 0);
			return dup_fd < 0 ? (uint32_t)-1 : (uint32_t)dup_fd;
		case LINUX_F_DUPFD_CLOEXEC:
			dup_fd = fd_duplicate_from(cur, ebx, edx, 1);
			return dup_fd < 0 ? (uint32_t)-1 : (uint32_t)dup_fd;
		case LINUX_F_GETFD:
			return proc_fd_entries(cur)[ebx].cloexec ? LINUX_FD_CLOEXEC : 0u;
		case LINUX_F_SETFD:
			proc_fd_entries(cur)[ebx].cloexec =
			    (edx & LINUX_FD_CLOEXEC) ? 1u : 0u;
			return 0;
		case LINUX_F_GETFL:
			return linux_fd_status_flags(&proc_fd_entries(cur)[ebx]);
		case LINUX_F_SETFL:
			proc_fd_entries(cur)[ebx].append =
			    (proc_fd_entries(cur)[ebx].type == FD_TYPE_FILE &&
			     (edx & LINUX_O_APPEND))
			        ? 1u
			        : 0u;
			proc_fd_entries(cur)[ebx].nonblock =
			    (edx & LINUX_O_NONBLOCK) ? 1u : 0u;
			return 0;
		default:
			return (uint32_t)-1;
		}
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_newselect(uint32_t ebx,
                                                 uint32_t ecx,
                                                 uint32_t edx,
                                                 uint32_t esi)
{
	{
		/*
         * Linux i386 _newselect(nfds, readfds, writefds, exceptfds, timeout).
         * Supports nfds <= 32 and reports immediate readiness for the same
         * fd types as poll().
         */
		process_t *cur = sched_current();
		uint32_t in_read = 0;
		uint32_t in_write = 0;
		uint32_t out_read = 0;
		uint32_t out_write = 0;
		uint32_t ready = 0;

		if (!cur || ebx > 32u)
			return (uint32_t)-1;
		if (ecx &&
		    uaccess_copy_from_user(cur, &in_read, ecx, sizeof(in_read)) != 0)
			return (uint32_t)-1;
		if (edx &&
		    uaccess_copy_from_user(cur, &in_write, edx, sizeof(in_write)) != 0)
			return (uint32_t)-1;

		for (uint32_t fd = 0; fd < ebx; fd++) {
			uint32_t bit = 1u << fd;

			if ((in_read & bit) &&
			    (linux_poll_revents(cur, (int32_t)fd, LINUX_POLLIN) &
			     LINUX_POLLIN)) {
				out_read |= bit;
				ready++;
			}
			if ((in_write & bit) &&
			    (linux_poll_revents(cur, (int32_t)fd, LINUX_POLLOUT) &
			     LINUX_POLLOUT)) {
				out_write |= bit;
				ready++;
			}
		}

		if (ecx &&
		    uaccess_copy_to_user(cur, ecx, &out_read, sizeof(out_read)) != 0)
			return (uint32_t)-1;
		if (edx &&
		    uaccess_copy_to_user(cur, edx, &out_write, sizeof(out_write)) != 0)
			return (uint32_t)-1;
		if (esi) {
			uint32_t zero = 0;
			if (uaccess_copy_to_user(cur, esi, &zero, sizeof(zero)) != 0)
				return (uint32_t)-1;
		}
		return ready;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_pipe(uint32_t ebx)
{
	{
		/*
         * ebx = pointer to int[2] in user space.
         *
         * Allocates a kernel pipe ring buffer and installs two fds into the
         * calling process's table: fds[0] is the read end, fds[1] the write
         * end.  The pipe is reference-counted so that fd_close_one() can
         * free the buffer once both ends are closed across all processes.
         *
         * Returns 0 on success, -1 if the pipe table is full or the fd
         * table is full.
         */
		process_t *cur = sched_current();
		if (!cur)
			return (uint32_t)-1;
		if (ebx == 0)
			return (uint32_t)-LINUX_EFAULT;

		int pipe_idx = pipe_alloc();
		if (pipe_idx < 0) {
			klog("PIPE", "pipe table full");
			return (uint32_t)-1;
		}

		int rfd = fd_alloc(cur);
		if (rfd < 0) {
			pipe_free(pipe_idx);
			klog("PIPE", "fd table full (read end)");
			return (uint32_t)-1;
		}
		proc_fd_entries(cur)[rfd].type = FD_TYPE_PIPE_READ;
		proc_fd_entries(cur)[rfd].writable = 0;
		proc_fd_entries(cur)[rfd].access_mode = 0;
		proc_fd_entries(cur)[rfd].append = 0;
		proc_fd_entries(cur)[rfd].cloexec = 0;
		proc_fd_entries(cur)[rfd].nonblock = 0;
		proc_fd_entries(cur)[rfd].u.pipe.pipe_idx = (uint32_t)pipe_idx;

		int wfd = fd_alloc(cur);
		if (wfd < 0) {
			fd_close_one(cur, (unsigned)rfd);
			klog("PIPE", "fd table full (write end)");
			return (uint32_t)-1;
		}
		proc_fd_entries(cur)[wfd].type = FD_TYPE_PIPE_WRITE;
		proc_fd_entries(cur)[wfd].writable = 1;
		proc_fd_entries(cur)[wfd].access_mode = LINUX_O_WRONLY;
		proc_fd_entries(cur)[wfd].append = 0;
		proc_fd_entries(cur)[wfd].cloexec = 0;
		proc_fd_entries(cur)[wfd].nonblock = 0;
		proc_fd_entries(cur)[wfd].u.pipe.pipe_idx = (uint32_t)pipe_idx;

		{
			int user_fds[2];
			user_fds[0] = rfd;
			user_fds[1] = wfd;
			if (uaccess_copy_to_user(cur, ebx, user_fds, sizeof(user_fds)) !=
			    0) {
				fd_close_one(cur, (unsigned)wfd);
				fd_close_one(cur, (unsigned)rfd);
				return (uint32_t)-1;
			}
		}
		return 0;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_pipe2(uint32_t eax,
                                             uint32_t ebx,
                                             uint32_t ecx,
                                             uint32_t edx,
                                             uint32_t esi,
                                             uint32_t edi,
                                             uint32_t ebp)
{
	uint32_t supported = LINUX_O_CLOEXEC | LINUX_O_NONBLOCK;
	process_t *cur;
	int fds[2];
	uint32_t rc;

	(void)eax;
	(void)edx;
	(void)esi;
	(void)edi;
	(void)ebp;

	if ((ecx & ~supported) != 0)
		return (uint32_t)-LINUX_EINVAL;
	rc = syscall_case_pipe(ebx);
	if (rc != 0)
		return rc;
	cur = sched_current();
	if (!cur || uaccess_copy_from_user(cur, fds, ebx, sizeof(fds)) != 0)
		return (uint32_t)-1;
	for (int i = 0; i < 2; i++) {
		if (fds[i] < 0 || (uint32_t)fds[i] >= MAX_FDS)
			return (uint32_t)-1;
		proc_fd_entries(cur)[fds[i]].cloexec =
		    (ecx & LINUX_O_CLOEXEC) ? 1u : 0u;
		proc_fd_entries(cur)[fds[i]].nonblock =
		    (ecx & LINUX_O_NONBLOCK) ? 1u : 0u;
	}
	return 0;
}

uint32_t SYSCALL_NOINLINE syscall_case_dup(uint32_t ebx)
{
	{
		/*
         * Linux i386 dup(oldfd).  Return the lowest available descriptor that
         * refers to the same open file description.
         */
		process_t *cur = sched_current();
		int fd;

		fd = fd_duplicate_from(cur, ebx, 0, 0);
		return fd < 0 ? (uint32_t)-LINUX_EBADF : (uint32_t)fd;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_dup2(uint32_t ebx, uint32_t ecx)
{
	{
		/*
         * ebx = old_fd, ecx = new_fd.
         *
         * Duplicates old_fd to new_fd.  If new_fd is already open it is
         * closed first.  The duplicated open resource refcount is bumped.
         * If old_fd == new_fd returns new_fd immediately (no-op).
         *
         * Returns new_fd on success, -1 on error.
         */
		if (ebx >= MAX_FDS || ecx >= MAX_FDS)
			return (uint32_t)-LINUX_EBADF;

		process_t *cur = sched_current();
		if (!cur)
			return (uint32_t)-1;

		if (proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
			return (uint32_t)-LINUX_EBADF;

		if (ebx == ecx)
			return ecx; /* dup2(fd, fd) is a documented no-op */

		/* Close the destination if it is already open. */
		if (proc_fd_entries(cur)[ecx].type != FD_TYPE_NONE)
			fd_close_one(cur, ecx);

		/* Copy the handle. */
		proc_fd_entries(cur)[ecx] = proc_fd_entries(cur)[ebx];
		proc_fd_entries(cur)[ecx].cloexec = 0;

		fd_bump_open_ref(&proc_fd_entries(cur)[ecx]);

		return ecx;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_lseek(uint32_t ebx,
                                             uint32_t ecx,
                                             uint32_t edx)
{
	{
		/*
         * ebx = fd, ecx = offset (signed), edx = whence.
         * Repositions the file offset of an open fd.
         *   SEEK_SET (0) — set offset to ecx
         *   SEEK_CUR (1) — set offset to current + ecx
         *   SEEK_END (2) — set offset to file_size + ecx
         * Returns the new offset, or (uint32_t)-1 on error.
         */
		process_t *cur = sched_current();
		uint64_t new_off = 0;
		int rc;

		rc = syscall_seek_handle(cur, ebx, (int32_t)ecx, edx, &new_off);
		if (rc != 0)
			return (uint32_t)rc;
		return (uint32_t)new_off;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_llseek(
    uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	{
		/*
         * Linux i386 _llseek(fd, high, low, loff_t *result, whence).
         */
		process_t *cur = sched_current();
		uint64_t raw_off = ((uint64_t)ecx << 32) | (uint64_t)edx;
		int64_t signed_off = (int64_t)raw_off;
		uint32_t result[2];
		uint64_t new_off = 0;
		int rc;

		if (!cur || esi == 0)
			return (uint32_t)-1;
		rc = syscall_seek_handle(cur, ebx, signed_off, edi, &new_off);
		if (rc != 0)
			return (uint32_t)rc;
		result[0] = (uint32_t)new_off;
		result[1] = (uint32_t)(new_off >> 32);
		if (uaccess_copy_to_user(cur, esi, result, sizeof(result)) != 0)
			return (uint32_t)-1;
		return 0;
	}
}
