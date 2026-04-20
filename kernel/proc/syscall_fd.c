/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_fd.c - Linux fd, pipe, and byte I/O syscalls.
 *
 * Owns read/write/readv/writev, poll/select, close/dup/fcntl, pipe/pipe2,
 * lseek/_llseek, sendfile64, and helpers that operate on open fd slots.
 */

#include "syscall.h"
#include "syscall_internal.h"
#include "syscall_linux.h"
#include "blkdev.h"
#include "chardev.h"
#include "desktop.h"
#include "klog.h"
#include "pipe.h"
#include "process.h"
#include "procfs.h"
#include "sched.h"
#include "tty.h"
#include "uaccess.h"
#include "vfs.h"
#include <stdint.h>

#define TTY_IO_CHUNK                                                           \
	((TTY_CANON_BUF_SIZE > TTY_RAW_BUF_SIZE) ? TTY_CANON_BUF_SIZE              \
	                                         : TTY_RAW_BUF_SIZE)

static uint32_t syscall_read_blockdev(process_t *cur,
                                      file_handle_t *fh,
                                      uint32_t user_buf,
                                      uint32_t count)
{
	const blkdev_ops_t *dev;
	uint8_t sec[BLKDEV_SECTOR_SIZE];
	uint32_t copied = 0;
	uint32_t size;
	uint32_t offset;
	uint32_t to_read;

	if (!cur || !fh || count == 0)
		return 0;

	dev = blkdev_get(fh->u.blockdev.name);
	if (!dev || !dev->read_sector)
		return (uint32_t)-1;

	size = fh->u.blockdev.size;
	offset = fh->u.blockdev.offset;
	if (offset >= size)
		return 0;

	to_read = size - offset;
	if (count < to_read)
		to_read = count;
	if (to_read == 0)
		return 0;

	if (uaccess_prepare(cur, user_buf, to_read, 1) != 0)
		return (uint32_t)-1;

	while (copied < to_read) {
		uint32_t abs_off = offset + copied;
		uint32_t lba = abs_off / BLKDEV_SECTOR_SIZE;
		uint32_t sec_off = abs_off % BLKDEV_SECTOR_SIZE;
		uint32_t chunk = BLKDEV_SECTOR_SIZE - sec_off;
		int rc;

		if (chunk > to_read - copied)
			chunk = to_read - copied;

		rc = dev->read_sector(lba, sec);
		if (rc != 0)
			goto partial_fail;
		if (uaccess_copy_to_user(
		        cur, user_buf + copied, sec + sec_off, chunk) != 0)
			goto partial_fail;
		copied += chunk;
	}

	fh->u.blockdev.offset = offset + copied;
	return copied;

partial_fail:
	if (copied != 0)
		fh->u.blockdev.offset = offset + copied;
	return copied ? copied : (uint32_t)-1;
}

static uint32_t syscall_write_fd(uint32_t fd, uint32_t user_buf, uint32_t count)
{
	process_t *cur;
	file_handle_t *fh;

	if (fd >= MAX_FDS)
		return (uint32_t)-LINUX_EBADF;

	cur = sched_current();
	if (!cur)
		return (uint32_t)-1;

	fh = &proc_fd_entries(cur)[fd];

	if (fh->type == FD_TYPE_BLOCKDEV)
		return (uint32_t)-1;

	if (syscall_fd_is_console_output(fh)) {
		uint8_t kbuf[USER_IO_CHUNK];
		desktop_state_t *batch_desktop;
		int use_console_batch;
		uint32_t written = 0;

		if (uaccess_prepare(cur, user_buf, count, 0) != 0)
			return (uint32_t)-1;

		batch_desktop = desktop_is_active() ? desktop_global() : 0;
		use_console_batch =
		    batch_desktop &&
		    syscall_desktop_should_route_console_output(batch_desktop, cur);
		if (use_console_batch)
			desktop_begin_console_batch(batch_desktop);

		while (written < count) {
			uint32_t chunk = count - written;
			if (chunk > USER_IO_CHUNK)
				chunk = USER_IO_CHUNK;
			if (uaccess_copy_from_user(cur, kbuf, user_buf + written, chunk) !=
			    0) {
				if (use_console_batch)
					desktop_end_console_batch(batch_desktop);
				return written ? written : (uint32_t)-1;
			}
			syscall_write_console_bytes(cur, (const char *)kbuf, chunk);
			written += chunk;
		}
		if (use_console_batch)
			desktop_end_console_batch(batch_desktop);
		return written;
	}

	if (fh->type == FD_TYPE_PIPE_WRITE) {
		pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
		uint8_t kbuf[USER_IO_CHUNK];
		uint32_t copied = 0;

		if (!pb || pb->read_open == 0)
			return (uint32_t)-1;

		if (uaccess_prepare(cur, user_buf, count, 0) != 0)
			return (uint32_t)-1;

		while (copied < count) {
			uint32_t chunk = count - copied;
			if (chunk > USER_IO_CHUNK)
				chunk = USER_IO_CHUNK;
			if (uaccess_copy_from_user(cur, kbuf, user_buf + copied, chunk) !=
			    0)
				return copied ? copied : (uint32_t)-1;

			while (pb->count == PIPE_BUF_SIZE) {
				if (fh->nonblock)
					return copied ? copied : (uint32_t)-LINUX_EAGAIN;
				if (pb->read_open == 0 || cur->sig_pending)
					return copied ? copied : (uint32_t)-1;
				sched_block(&pb->waiters);
			}

			for (uint32_t i = 0; i < chunk; i++) {
				while (pb->count == PIPE_BUF_SIZE) {
					if (fh->nonblock)
						return copied ? copied : (uint32_t)-LINUX_EAGAIN;
					if (pb->read_open == 0 || cur->sig_pending)
						return copied ? copied : (uint32_t)-1;
					sched_block(&pb->waiters);
				}
				pb->buf[pb->write_idx] = kbuf[i];
				pb->write_idx = (pb->write_idx + 1) % PIPE_BUF_SIZE;
				pb->count++;
				copied++;
			}
		}
		sched_wake_all(&pb->waiters);
		return copied;
	}

	if (fh->type == FD_TYPE_FILE || fh->type == FD_TYPE_SYSFILE) {
		uint8_t kbuf[USER_IO_CHUNK];
		uint32_t written = 0;

		if (!fh->writable)
			return (uint32_t)-LINUX_EBADF;
		if (count == 0)
			return 0;

		if (uaccess_prepare(cur, user_buf, count, 0) != 0)
			return (uint32_t)-1;

		if (fh->append)
			fh->u.file.offset = fh->u.file.size;

		while (written < count) {
			uint32_t chunk = count - written;
			int n;

			if (chunk > USER_IO_CHUNK)
				chunk = USER_IO_CHUNK;
			if (uaccess_copy_from_user(cur, kbuf, user_buf + written, chunk) !=
			    0)
				return written ? written : (uint32_t)-1;

			n = vfs_write(
			    fh->u.file.ref, fh->u.file.offset + written, kbuf, chunk);
			if (n < 0)
				return written ? written : (uint32_t)-1;

			written += (uint32_t)n;
			if ((uint32_t)n < chunk)
				break;
		}

		fh->u.file.offset += written;
		if (fh->u.file.offset > fh->u.file.size)
			fh->u.file.size = fh->u.file.offset;
		return written;
	}

	return (uint32_t)-1;
}

static uint32_t syscall_read_fd(uint32_t fd, uint32_t user_buf, uint32_t count)
{
	process_t *cur;
	file_handle_t *fh;

	if (fd >= MAX_FDS)
		return (uint32_t)-1;

	cur = sched_current();
	if (!cur)
		return (uint32_t)-1;

	fh = &proc_fd_entries(cur)[fd];

	if (count == 0) {
		switch (fh->type) {
		case FD_TYPE_BLOCKDEV:
		case FD_TYPE_TTY:
		case FD_TYPE_CHARDEV:
		case FD_TYPE_PIPE_READ:
		case FD_TYPE_FILE:
		case FD_TYPE_SYSFILE:
			return 0;
		default:
			return (uint32_t)-1;
		}
	}

	if (fh->type == FD_TYPE_BLOCKDEV)
		return syscall_read_blockdev(cur, fh, user_buf, count);

	if (fh->type == FD_TYPE_TTY) {
		char kbuf[TTY_IO_CHUNK];
		uint32_t chunk = count > TTY_IO_CHUNK ? TTY_IO_CHUNK : count;
		int n;

		if (uaccess_prepare(cur, user_buf, chunk, 1) != 0)
			return (uint32_t)-1;
		n = tty_read((int)fh->u.tty.tty_idx, kbuf, chunk);
		if (n > 0 &&
		    uaccess_copy_to_user(cur, user_buf, kbuf, (uint32_t)n) != 0)
			return (uint32_t)-1;
		return (uint32_t)n;
	}

	if (fh->type == FD_TYPE_CHARDEV) {
		const chardev_ops_t *dev = chardev_get(fh->u.chardev.name);
		char c = 0;

		if (!dev)
			return (uint32_t)-1;
		if (uaccess_prepare(cur, user_buf, 1, 1) != 0)
			return (uint32_t)-1;
		while ((c = dev->read_char()) == 0)
			__asm__ volatile("pause");
		if (uaccess_copy_to_user(cur, user_buf, &c, 1) != 0)
			return (uint32_t)-1;
		return 1;
	}

	if (fh->type == FD_TYPE_PIPE_READ) {
		pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
		uint32_t to_read;
		uint8_t kbuf[USER_IO_CHUNK];
		uint32_t copied = 0;

		if (!pb)
			return (uint32_t)-1;
		while (pb->count == 0) {
			if (pb->write_open == 0)
				return 0;
			if (fh->nonblock)
				return (uint32_t)-LINUX_EAGAIN;
			if (cur->sig_pending)
				return (uint32_t)-1;
			sched_block(&pb->waiters);
		}

		to_read = (count < pb->count) ? count : pb->count;
		if (uaccess_prepare(cur, user_buf, to_read, 1) != 0)
			return (uint32_t)-1;

		while (copied < to_read) {
			uint32_t chunk = to_read - copied;
			if (chunk > USER_IO_CHUNK)
				chunk = USER_IO_CHUNK;
			for (uint32_t i = 0; i < chunk; i++) {
				kbuf[i] = pb->buf[pb->read_idx];
				pb->read_idx = (pb->read_idx + 1) % PIPE_BUF_SIZE;
			}
			if (uaccess_copy_to_user(cur, user_buf + copied, kbuf, chunk) != 0)
				return copied ? copied : (uint32_t)-1;
			copied += chunk;
		}
		pb->count -= to_read;
		sched_wake_all(&pb->waiters);
		return to_read;
	}

	if (fh->type == FD_TYPE_FILE || fh->type == FD_TYPE_SYSFILE) {
		uint32_t remaining;
		uint32_t to_read;
		uint32_t copied = 0;
		uint32_t file_off = fh->u.file.offset;
		uint8_t kbuf[USER_IO_CHUNK];

		if (fh->u.file.offset >= fh->u.file.size)
			return 0;
		remaining = fh->u.file.size - fh->u.file.offset;
		to_read = (count < remaining) ? count : remaining;
		if (to_read == 0)
			return 0;
		if (uaccess_prepare(cur, user_buf, to_read, 1) != 0)
			return (uint32_t)-1;

		while (copied < to_read) {
			uint32_t chunk = to_read - copied;
			int n;

			if (chunk > USER_IO_CHUNK)
				chunk = USER_IO_CHUNK;
			n = vfs_read(fh->u.file.ref, file_off + copied, kbuf, chunk);
			if (n < 0) {
				klog("READ", "vfs_read failed");
				return copied ? copied : (uint32_t)-1;
			}
			if (n == 0)
				break;
			if (uaccess_copy_to_user(
			        cur, user_buf + copied, kbuf, (uint32_t)n) != 0)
				return copied ? copied : (uint32_t)-1;
			copied += (uint32_t)n;
			if ((uint32_t)n < chunk)
				break;
		}

		fh->u.file.offset = file_off + copied;
		return copied;
	}

	if (fh->type == FD_TYPE_PROCFILE) {
		uint32_t size = 0;
		uint8_t kbuf[USER_IO_CHUNK];
		uint32_t copied = 0;
		uint32_t to_read;

		if (procfs_file_size(
		        fh->u.proc.kind, fh->u.proc.pid, fh->u.proc.index, &size) != 0)
			return (uint32_t)-1;

		fh->u.proc.size = size;
		if (fh->u.proc.offset >= size)
			return 0;

		to_read = size - fh->u.proc.offset;
		if (count < to_read)
			to_read = count;
		if (to_read == 0)
			return 0;
		if (uaccess_prepare(cur, user_buf, to_read, 1) != 0)
			return (uint32_t)-1;

		while (copied < to_read) {
			uint32_t chunk = to_read - copied;
			int n;

			if (chunk > USER_IO_CHUNK)
				chunk = USER_IO_CHUNK;
			n = procfs_read_file(fh->u.proc.kind,
			                     fh->u.proc.pid,
			                     fh->u.proc.index,
			                     fh->u.proc.offset + copied,
			                     (char *)kbuf,
			                     chunk);
			if (n < 0)
				return copied ? copied : (uint32_t)-1;
			if (n == 0)
				break;
			if (uaccess_copy_to_user(
			        cur, user_buf + copied, kbuf, (uint32_t)n) != 0)
				return copied ? copied : (uint32_t)-1;
			copied += (uint32_t)n;
		}

		fh->u.proc.offset += copied;
		return copied;
	}

	return (uint32_t)-1;
}

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
		if (fh->type == FD_TYPE_PIPE_WRITE || fh->writable)
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
		}
	}

	return rev;
}

static void fd_bump_pipe_ref(file_handle_t *fh)
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
			fd_bump_pipe_ref(&proc_fd_entries(proc)[fd]);
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

static uint32_t syscall_sendfile64(process_t *cur,
                                   uint32_t out_fd,
                                   uint32_t in_fd,
                                   uint32_t offset_ptr,
                                   uint32_t count)
{
	file_handle_t *out;
	file_handle_t *in;
	uint32_t off_words[2] = {0, 0};
	uint32_t read_off;
	uint32_t size;
	uint32_t copied = 0;
	uint8_t kbuf[USER_IO_CHUNK];

	if (!cur || out_fd >= MAX_FDS || in_fd >= MAX_FDS)
		return (uint32_t)-LINUX_EBADF;
	out = &proc_fd_entries(cur)[out_fd];
	in = &proc_fd_entries(cur)[in_fd];
	if (!syscall_fd_is_console_output(out) &&
	    (out->type != FD_TYPE_FILE || !out->writable))
		return (uint32_t)-LINUX_EBADF;
	if (in->type != FD_TYPE_FILE && in->type != FD_TYPE_SYSFILE &&
	    in->type != FD_TYPE_PROCFILE)
		return (uint32_t)-LINUX_EBADF;

	if (offset_ptr) {
		if (uaccess_copy_from_user(
		        cur, off_words, offset_ptr, sizeof(off_words)) != 0)
			return (uint32_t)-1;
		if (off_words[1] != 0)
			return (uint32_t)-1;
		read_off = off_words[0];
	} else if (in->type == FD_TYPE_FILE || in->type == FD_TYPE_SYSFILE) {
		read_off = in->u.file.offset;
	} else {
		read_off = in->u.proc.offset;
	}

	if (in->type == FD_TYPE_FILE || in->type == FD_TYPE_SYSFILE) {
		size = in->u.file.size;
	} else {
		if (procfs_file_size(
		        in->u.proc.kind, in->u.proc.pid, in->u.proc.index, &size) != 0)
			return (uint32_t)-1;
		in->u.proc.size = size;
	}

	while (copied < count && read_off < size) {
		uint32_t chunk = count - copied;
		int n;

		if (chunk > USER_IO_CHUNK)
			chunk = USER_IO_CHUNK;
		if (chunk > size - read_off)
			chunk = size - read_off;

		if (in->type == FD_TYPE_FILE || in->type == FD_TYPE_SYSFILE) {
			n = vfs_read(in->u.file.ref, read_off, kbuf, chunk);
		} else {
			n = procfs_read_file(in->u.proc.kind,
			                     in->u.proc.pid,
			                     in->u.proc.index,
			                     read_off,
			                     (char *)kbuf,
			                     chunk);
		}
		if (n < 0)
			return copied ? copied : (uint32_t)-1;
		if (n == 0)
			break;
		if (syscall_fd_is_console_output(out)) {
			if (syscall_write_console_bytes(
			        cur, (const char *)kbuf, (uint32_t)n) != n)
				return copied ? copied : (uint32_t)-1;
			read_off += (uint32_t)n;
			copied += (uint32_t)n;
		} else {
			int written;

			if (out->append)
				out->u.file.offset = out->u.file.size;
			written = vfs_write(
			    out->u.file.ref, out->u.file.offset, kbuf, (uint32_t)n);
			if (written < 0)
				return copied ? copied : (uint32_t)-1;
			if (written == 0)
				break;
			out->u.file.offset += (uint32_t)written;
			if (out->u.file.offset > out->u.file.size)
				out->u.file.size = out->u.file.offset;
			read_off += (uint32_t)written;
			copied += (uint32_t)written;
			if (written < n)
				break;
		}
		if ((uint32_t)n < chunk)
			break;
	}

	if (offset_ptr) {
		off_words[0] = read_off;
		off_words[1] = 0;
		if (uaccess_copy_to_user(
		        cur, offset_ptr, off_words, sizeof(off_words)) != 0)
			return copied ? copied : (uint32_t)-1;
	} else if (in->type == FD_TYPE_FILE || in->type == FD_TYPE_SYSFILE) {
		in->u.file.offset = read_off;
	} else {
		in->u.proc.offset = read_off;
	}
	return copied;
}

uint32_t SYSCALL_NOINLINE syscall_case_write(uint32_t ebx,
                                             uint32_t ecx,
                                             uint32_t edx)
{
	{
		/*
         * ebx = fd
         * ecx = pointer to byte buffer in user virtual space
         * edx = number of bytes to write
         *
         * Dispatches on fd type:
         *   writable TTY    → active desktop shell or VGA console
         *   FD_TYPE_FILE    → fs_write() into the DUFS inode
         *
         * Returns the number of bytes written, or -1 on error.
         */
		return syscall_write_fd(ebx, ecx, edx);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_writev(uint32_t ebx,
                                              uint32_t ecx,
                                              uint32_t edx)
{
	{
		/*
         * Linux i386 writev(fd, iov, iovcnt).  Each iovec is two 32-bit
         * words: base pointer, byte length.
         */
		process_t *cur = sched_current();
		uint32_t total = 0;

		if (!cur || ebx >= MAX_FDS || ecx == 0 || edx > 1024u)
			return (uint32_t)-1;
		if (proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
			return (uint32_t)-1;

		for (uint32_t i = 0; i < edx; i++) {
			uint32_t iov[2];
			uint32_t n;

			if (uaccess_copy_from_user(
			        cur, iov, ecx + i * sizeof(iov), sizeof(iov)) != 0)
				return total ? total : (uint32_t)-1;
			if (iov[1] == 0)
				continue;

			n = syscall_handler(SYS_WRITE, ebx, iov[0], iov[1], 0, 0, 0);
			if (n == (uint32_t)-1)
				return total ? total : (uint32_t)-1;
			total += n;
			if (n < iov[1])
				break;
		}
		return total;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_readv(uint32_t ebx,
                                             uint32_t ecx,
                                             uint32_t edx)
{
	{
		/*
         * Linux i386 readv(fd, iov, iovcnt).  Each iovec is two 32-bit
         * words: base pointer, byte length.
         */
		process_t *cur = sched_current();
		uint32_t total = 0;

		if (!cur || ebx >= MAX_FDS || ecx == 0 || edx > 1024u)
			return (uint32_t)-1;
		if (proc_fd_entries(cur)[ebx].type == FD_TYPE_NONE)
			return (uint32_t)-1;

		for (uint32_t i = 0; i < edx; i++) {
			uint32_t iov[2];
			uint32_t n;

			if (uaccess_copy_from_user(
			        cur, iov, ecx + i * sizeof(iov), sizeof(iov)) != 0)
				return total ? total : (uint32_t)-1;
			if (iov[1] == 0)
				continue;

			n = syscall_handler(SYS_READ, ebx, iov[0], iov[1], 0, 0, 0);
			if (n == (uint32_t)-1)
				return total ? total : (uint32_t)-1;
			total += n;
			if (n < iov[1])
				break;
		}
		return total;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_read(uint32_t ebx,
                                            uint32_t ecx,
                                            uint32_t edx)
{
	{
		/*
         * ebx = fd
         * ecx = pointer to output buffer in user space
         * edx = max bytes to read
         *
         * Dispatches on fd type:
         *   FD_TYPE_CHARDEV → spin-wait on chardev ring buffer (e.g. keyboard)
         *   FD_TYPE_FILE    → fs_read() from DUFS inode at current offset
         *
         * Returns bytes read, 0 at EOF, -1 on error.
         */
		return syscall_read_fd(ebx, ecx, edx);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_sendfile64(uint32_t ebx,
                                                  uint32_t ecx,
                                                  uint32_t edx,
                                                  uint32_t esi)
{
	/*
         * Linux i386 sendfile64(out_fd, in_fd, offset64 *, count).
         * The BusyBox fast path uses this to copy regular files to stdout.
         */
	return syscall_sendfile64(sched_current(), ebx, ecx, edx, esi);
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

uint32_t SYSCALL_NOINLINE syscall_case_poll(uint32_t ebx, uint32_t ecx)
{
	{
		/*
         * Linux i386 poll(struct pollfd *fds, nfds_t nfds, int timeout).
         * Return immediately with readiness for the simple fd types Drunix
         * supports.  This is enough for BusyBox terminal/stdout probes.
         */
		process_t *cur = sched_current();
		uint32_t ready = 0;

		if (!cur || ecx > 1024u)
			return (uint32_t)-1;
		if (ecx != 0 && ebx == 0)
			return (uint32_t)-LINUX_EFAULT;

		for (uint32_t i = 0; i < ecx; i++) {
			uint8_t pfd[8];
			int32_t fd;
			uint32_t events;
			uint32_t revents;

			if (uaccess_copy_from_user(
			        cur, pfd, ebx + i * sizeof(pfd), sizeof(pfd)) != 0)
				return (uint32_t)-1;
			fd = (int32_t)linux_get_u32(pfd, 0u);
			events = linux_get_u16(pfd, 4u);
			revents = linux_poll_revents(cur, fd, events);
			linux_put_u16(pfd, 6u, revents);
			if (uaccess_copy_to_user(
			        cur, ebx + i * sizeof(pfd), pfd, sizeof(pfd)) != 0)
				return (uint32_t)-1;
			if (revents)
				ready++;
		}
		return ready;
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
         * closed first.  Both ends of a pipe have their refcount bumped.
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

		/* Bump the pipe refcount for the duplicated end. */
		if (proc_fd_entries(cur)[ecx].type == FD_TYPE_PIPE_READ) {
			pipe_buf_t *pb =
			    pipe_get((int)proc_fd_entries(cur)[ecx].u.pipe.pipe_idx);
			if (pb)
				pb->read_open++;
		} else if (proc_fd_entries(cur)[ecx].type == FD_TYPE_PIPE_WRITE) {
			pipe_buf_t *pb =
			    pipe_get((int)proc_fd_entries(cur)[ecx].u.pipe.pipe_idx);
			if (pb)
				pb->write_open++;
		}

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
