/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * tty.c - Linux tty, termios, and ioctl syscalls.
 *
 * Owns tty fd lookup, Linux termios packing/unpacking, ioctl compatibility,
 * and Drunix foreground process-group tty controls.
 */

#include "syscall_internal.h"
#include "syscall_linux.h"
#include "console/runtime.h"
#include "kstring.h"
#include "pipe.h"
#include "process.h"
#include "procfs.h"
#include "pty.h"
#include "sched.h"
#include "tty.h"
#include "uaccess.h"
#include <stdint.h>

tty_t *syscall_tty_from_fd(process_t *cur, uint32_t fd, uint32_t *tty_idx_out)
{
	file_handle_t *fh;
	tty_t *tty;

	if (!cur || fd >= MAX_FDS)
		return 0;

	fh = &proc_fd_entries(cur)[fd];
	if (fh->type != FD_TYPE_TTY)
		return 0;

	tty = tty_get((int)fh->u.tty.tty_idx);
	if (!tty)
		return 0;

	if (tty_idx_out)
		*tty_idx_out = fh->u.tty.tty_idx;
	return tty;
}

static uint32_t linux_termios_lflag_from_tty(const tty_t *tty)
{
	uint32_t lflag = 0;

	if (!tty)
		return 0;
	if (tty->termios.c_lflag & ISIG)
		lflag |= LINUX_ISIG;
	if (tty->termios.c_lflag & ICANON)
		lflag |= LINUX_ICANON;
	if (tty->termios.c_lflag & ECHO)
		lflag |= LINUX_ECHO;
	if (tty->termios.c_lflag & ECHOE)
		lflag |= LINUX_ECHOE;
	return lflag;
}

static void linux_termios_to_tty(tty_t *tty, const uint8_t termios[60])
{
	uint32_t iflag;
	uint32_t oflag;
	uint32_t cflag;
	uint32_t lflag;
	termios_t out;
	uint32_t i;

	if (!tty || !termios)
		return;

	iflag = linux_get_u32(termios, 0u);
	oflag = linux_get_u32(termios, 4u);
	cflag = linux_get_u32(termios, 8u);
	lflag = linux_get_u32(termios, 12u);
	k_memset(&out, 0, sizeof(out));
	out.c_iflag = 0;
	if (iflag & LINUX_ICRNL)
		out.c_iflag |= ICRNL;
	out.c_oflag = 0;
	if (oflag & LINUX_OPOST)
		out.c_oflag |= OPOST;
	if (oflag & LINUX_ONLCR)
		out.c_oflag |= ONLCR;
	out.c_cflag = 0;
	if (cflag & LINUX_CREAD)
		out.c_cflag |= CREAD;
	if ((cflag & LINUX_CS8) == LINUX_CS8)
		out.c_cflag |= CS8;
	out.c_lflag = 0;
	if (lflag & LINUX_ISIG)
		out.c_lflag |= ISIG;
	if (lflag & LINUX_ICANON)
		out.c_lflag |= ICANON;
	if (lflag & LINUX_ECHO)
		out.c_lflag |= ECHO;
	if (lflag & LINUX_ECHOE)
		out.c_lflag |= ECHOE;
	for (i = 0; i < NCCS && 17u + i < 60u; i++)
		out.c_cc[i] = termios[17u + i];
	tty->termios = out;
}

static void linux_termios_from_tty(const tty_t *tty, uint8_t termios[60])
{
	k_memset(termios, 0, 60u);
	if (tty && (tty->termios.c_iflag & ICRNL))
		linux_put_u32(termios, 0u, LINUX_ICRNL);
	if (tty && (tty->termios.c_oflag & (OPOST | ONLCR))) {
		uint32_t oflag = 0;
		if (tty->termios.c_oflag & OPOST)
			oflag |= LINUX_OPOST;
		if (tty->termios.c_oflag & ONLCR)
			oflag |= LINUX_ONLCR;
		linux_put_u32(termios, 4u, oflag);
	}
	{
		uint32_t cflag = LINUX_B38400;
		if (!tty || (tty->termios.c_cflag & CS8))
			cflag |= LINUX_CS8;
		if (!tty || (tty->termios.c_cflag & CREAD))
			cflag |= LINUX_CREAD;
		linux_put_u32(termios, 8u, cflag);
	}
	linux_put_u32(termios, 12u, linux_termios_lflag_from_tty(tty));
	if (tty) {
		uint32_t i;
		for (i = 0; i < NCCS && 17u + i < 60u; i++)
			termios[17u + i] = tty->termios.c_cc[i];
	} else {
		termios[17u + LINUX_VTIME] = 0;
		termios[17u + LINUX_VMIN] = 1;
	}
}

static uint32_t syscall_ioctl(uint32_t fd, uint32_t request, uint32_t argp)
{
	process_t *cur = sched_current();
	file_handle_t *fh;

	if (!cur || fd >= MAX_FDS)
		return (uint32_t)-1;
	fh = &proc_fd_entries(cur)[fd];
	if (fh->type == FD_TYPE_NONE)
		return (uint32_t)-1;

	switch (request) {
	case LINUX_TIOCGWINSZ: {
		uint16_t ws[4];

		if (argp == 0)
			return (uint32_t)-1;
		console_runtime_winsize(&ws[0], &ws[1]);
		ws[2] = 0;
		ws[3] = 0;
		if (uaccess_copy_to_user(cur, argp, ws, sizeof(ws)) != 0)
			return (uint32_t)-1;
		return 0;
	}
	case LINUX_FIONREAD: {
		uint32_t available = 0;

		if (argp == 0)
			return (uint32_t)-1;
		if (fh->type == FD_TYPE_PIPE_READ) {
			pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
			if (!pb)
				return (uint32_t)-1;
			available = pb->count;
		} else if (fh->type == FD_TYPE_FILE || fh->type == FD_TYPE_SYSFILE) {
			if (fh->u.file.offset < fh->u.file.size)
				available = fh->u.file.size - fh->u.file.offset;
		} else if (fh->type == FD_TYPE_PROCFILE) {
			uint32_t size = 0;
			if (procfs_file_size(
			        fh->u.proc.kind, fh->u.proc.pid, fh->u.proc.index, &size) !=
			    0)
				return (uint32_t)-1;
			if (fh->u.proc.offset < size)
				available = size - fh->u.proc.offset;
		} else if (fh->type == FD_TYPE_TTY) {
			available = tty_read_available((int)fh->u.tty.tty_idx);
		} else if (fh->type == FD_TYPE_PTY_MASTER) {
			available = pty_master_read_available(fh->u.pty.pty_idx);
		} else if (fh->type == FD_TYPE_PTY_SLAVE) {
			available = pty_slave_read_available(fh->u.pty.pty_idx);
		}
		if (uaccess_copy_to_user(cur, argp, &available, sizeof(available)) != 0)
			return (uint32_t)-1;
		return 0;
	}
	case LINUX_TIOCGPTN: {
		uint32_t pty_idx;

		if (argp == 0 || fh->type != FD_TYPE_PTY_MASTER)
			return (uint32_t)-1;
		pty_idx = fh->u.pty.pty_idx;
		if (uaccess_copy_to_user(cur, argp, &pty_idx, sizeof(pty_idx)) != 0)
			return (uint32_t)-1;
		return 0;
	}
	case LINUX_TCGETS: {
		uint8_t termios[60];
		tty_t *tty;

		if (argp == 0)
			return (uint32_t)-1;
		tty = syscall_tty_from_fd(cur, fd, 0);
		if (!tty)
			return (uint32_t)-1;
		linux_termios_from_tty(tty, termios);
		if (uaccess_copy_to_user(cur, argp, termios, sizeof(termios)) != 0)
			return (uint32_t)-1;
		return 0;
	}
	case LINUX_TCSETS:
	case LINUX_TCSETSW:
	case LINUX_TCSETSF: {
		uint8_t termios[60];
		tty_t *tty;

		if (argp == 0)
			return (uint32_t)-1;
		tty = syscall_tty_from_fd(cur, fd, 0);
		if (!tty)
			return (uint32_t)-1;
		if (uaccess_copy_from_user(cur, termios, argp, sizeof(termios)) != 0)
			return (uint32_t)-1;
		if (request == LINUX_TCSETSF) {
			tty->raw_head = tty->raw_tail = 0;
			tty->canon_len = 0;
			tty->canon_ready = 0;
		}
		linux_termios_to_tty(tty, termios);
		return 0;
	}
	default:
		return (uint32_t)-1;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_ioctl(uint32_t ebx,
                                             uint32_t ecx,
                                             uint32_t edx)
{
	{
		/*
         * Linux i386 ioctl(fd, request, argp).  Implement the terminal probes
         * used by static musl/BusyBox and fail unsupported requests cleanly.
         */
		return syscall_ioctl(ebx, ecx, edx);
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_tcgetattr(uint32_t ebx,
                                                        uint32_t ecx)
{
	{
		/* ebx = fd, ecx = termios_t* (user pointer) */
		process_t *cur = sched_current();
		tty_t *tty = syscall_tty_from_fd(cur, ebx, 0);
		if (!tty)
			return (uint32_t)-1;
		if (uaccess_copy_to_user(
		        cur, ecx, &tty->termios, sizeof(tty->termios)) != 0)
			return (uint32_t)-1;
		return 0;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_tcsetattr(uint32_t ebx,
                                                        uint32_t ecx,
                                                        uint32_t edx)
{
	{
		/* ebx = fd, ecx = action (TCSANOW=0 / TCSAFLUSH=2), edx = termios_t* */
		process_t *cur = sched_current();
		tty_t *tty = syscall_tty_from_fd(cur, ebx, 0);
		termios_t new_termios;
		if (!tty)
			return (uint32_t)-1;
		if (uaccess_copy_from_user(
		        cur, &new_termios, edx, sizeof(new_termios)) != 0)
			return (uint32_t)-1;
		if (ecx == TCSAFLUSH) {
			/* Discard unread input */
			tty->raw_head = tty->raw_tail = 0;
			tty->canon_len = 0;
			tty->canon_ready = 0;
		}
		tty->termios = new_termios;
		return 0;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_tcsetpgrp(uint32_t ebx,
                                                        uint32_t ecx)
{
	{
		/* ebx = fd, ecx = pgid → 0 or -1 */
		process_t *cur = sched_current();
		uint32_t tty_idx;
		tty_t *tty = syscall_tty_from_fd(cur, ebx, &tty_idx);
		if (!tty)
			return (uint32_t)-1;
		if (ecx == 0)
			return (uint32_t)-1;
		if (cur->tty_id != tty_idx)
			return (uint32_t)-1;
		if (tty->ctrl_sid == 0)
			tty->ctrl_sid = cur->sid;
		if (tty->ctrl_sid != cur->sid)
			return (uint32_t)-1;
		if (!sched_session_has_pgid(tty->ctrl_sid, ecx))
			return (uint32_t)-1;
		tty->fg_pgid = ecx;
		return 0;
	}
}

uint32_t SYSCALL_NOINLINE syscall_case_drunix_tcgetpgrp(uint32_t ebx)
{
	{
		/* ebx = fd → fg_pgid or -1 */
		process_t *cur = sched_current();
		tty_t *tty = syscall_tty_from_fd(cur, ebx, 0);
		if (!tty)
			return (uint32_t)-1;
		return tty->fg_pgid;
	}
}
