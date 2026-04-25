/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * fd.c - Linux fd, pipe, and byte I/O syscalls.
 *
 * Owns read/write/readv/writev, poll/select, close/dup/fcntl, pipe/pipe2,
 * lseek/_llseek, sendfile64, and helpers that operate on open fd slots.
 */

#include "syscall_internal.h"
#include "syscall_linux.h"
#include "arch.h"
#include "blkdev.h"
#include "chardev.h"
#include "console/runtime.h"
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

typedef uint32_t (*syscall_fd_io_op_t)(uint32_t fd,
                                       uint32_t user_buf,
                                       uint32_t count);

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
		uintptr_t console_batch = 0;
		uint32_t written = 0;

		if (uaccess_prepare(cur, user_buf, count, 0) != 0)
			return (uint32_t)-1;

		console_batch = console_runtime_begin_process_output(cur);

		while (written < count) {
			uint32_t chunk = count - written;
			if (chunk > USER_IO_CHUNK)
				chunk = USER_IO_CHUNK;
			if (uaccess_copy_from_user(cur, kbuf, user_buf + written, chunk) !=
			    0) {
				console_runtime_end_process_output(console_batch);
				return written ? written : (uint32_t)-1;
			}
			syscall_write_console_bytes(cur, (const char *)kbuf, chunk);
			written += chunk;
		}
		console_runtime_end_process_output(console_batch);
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
				if (pb->read_open == 0 ||
				    sched_process_has_unblocked_signal(cur))
					return copied ? copied : (uint32_t)-1;
				sched_block(&pb->waiters);
			}

			for (uint32_t i = 0; i < chunk; i++) {
				while (pb->count == PIPE_BUF_SIZE) {
					if (fh->nonblock)
						return copied ? copied : (uint32_t)-LINUX_EAGAIN;
					if (pb->read_open == 0 ||
					    sched_process_has_unblocked_signal(cur))
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
			arch_idle_wait();
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
			if (sched_process_has_unblocked_signal(cur))
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

static uint32_t syscall_iov_fd_op(uint32_t fd,
                                  uint32_t user_iov,
                                  uint32_t iovcnt,
                                  syscall_fd_io_op_t op)
{
	process_t *cur = sched_current();
	uint32_t total = 0;

	if (!cur || !op || fd >= MAX_FDS || user_iov == 0 || iovcnt > 1024u)
		return (uint32_t)-1;
	if (proc_fd_entries(cur)[fd].type == FD_TYPE_NONE)
		return (uint32_t)-1;

	for (uint32_t i = 0; i < iovcnt; i++) {
		uint32_t iov[2];
		uint32_t n;

		if (uaccess_copy_from_user(
		        cur, iov, user_iov + i * sizeof(iov), sizeof(iov)) != 0)
			return total ? total : (uint32_t)-1;
		if (iov[1] == 0)
			continue;

		n = op(fd, iov[0], iov[1]);
		if (n == (uint32_t)-1)
			return total ? total : (uint32_t)-1;
		total += n;
		if (n < iov[1])
			break;
	}
	return total;
}

static uint32_t syscall_iov64_fd_op(uint32_t fd,
                                    uint32_t user_iov,
                                    uint32_t iovcnt,
                                    syscall_fd_io_op_t op)
{
	process_t *cur = sched_current();
	uint32_t total = 0;

	if (!cur || !op || fd >= MAX_FDS || user_iov == 0 || iovcnt > 1024u)
		return (uint32_t)-1;
	if (proc_fd_entries(cur)[fd].type == FD_TYPE_NONE)
		return (uint32_t)-1;

	for (uint32_t i = 0; i < iovcnt; i++) {
		uint64_t iov[2];
		uint32_t n;

		if (uaccess_copy_from_user(
		        cur, iov, user_iov + i * sizeof(iov), sizeof(iov)) != 0)
			return total ? total : (uint32_t)-1;
		if (iov[0] > UINT32_MAX || iov[1] > UINT32_MAX)
			return total ? total : (uint32_t)-1;
		if (iov[1] == 0)
			continue;

		n = op(fd, (uint32_t)iov[0], (uint32_t)iov[1]);
		if (n == (uint32_t)-1)
			return total ? total : (uint32_t)-1;
		total += n;
		if (n < (uint32_t)iov[1])
			break;
	}
	return total;
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
		return syscall_iov_fd_op(ebx, ecx, edx, syscall_write_fd);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_writev64(uint32_t ebx,
                                                uint32_t ecx,
                                                uint32_t edx)
{
	return syscall_iov64_fd_op(ebx, ecx, edx, syscall_write_fd);
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
		return syscall_iov_fd_op(ebx, ecx, edx, syscall_read_fd);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_readv64(uint32_t ebx,
                                               uint32_t ecx,
                                               uint32_t edx)
{
	return syscall_iov64_fd_op(ebx, ecx, edx, syscall_read_fd);
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
