/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall_linux.h - Linux i386 ABI constants and layout helpers.
 *
 * Keep Linux-facing syscall numbers, flag bits, errno values, and packed
 * structure helpers here so domain modules can share ABI details without
 * depending on the central dispatcher.
 */

#ifndef SYSCALL_LINUX_H
#define SYSCALL_LINUX_H

#include <stdint.h>

#define LINUX_S_IFIFO 0010000u
#define LINUX_S_IFCHR 0020000u
#define LINUX_S_IFBLK 0060000u
#define LINUX_S_IFDIR 0040000u
#define LINUX_S_IFREG 0100000u
#define LINUX_S_IFLNK 0120000u
#define LINUX_DT_FIFO 1u
#define LINUX_DT_CHR 2u
#define LINUX_DT_DIR 4u
#define LINUX_DT_REG 8u
#define LINUX_AT_FDCWD 0xFFFFFF9Cu
#define LINUX_AT_SYMLINK_NOFOLLOW 0x0100u
#define LINUX_AT_REMOVEDIR 0x0200u
#define LINUX_AT_SYMLINK_FOLLOW 0x0400u
#define LINUX_AT_NO_AUTOMOUNT 0x0800u
#define LINUX_AT_EMPTY_PATH 0x1000u
#define LINUX_AT_STATX_SYNC_TYPE 0x6000u
#define LINUX_STATX_BASIC_STATS 0x000007FFu
#define LINUX_TCGETS 0x5401u
#define LINUX_TCSETS 0x5402u
#define LINUX_TCSETSW 0x5403u
#define LINUX_TCSETSF 0x5404u
#define LINUX_TIOCGWINSZ 0x5413u
#define LINUX_FIONREAD 0x541Bu
#define LINUX_ICRNL 0000400u
#define LINUX_OPOST 0000001u
#define LINUX_ONLCR 0000004u
#define LINUX_ISIG 0000001u
#define LINUX_ICANON 0000002u
#define LINUX_ECHO 0000010u
#define LINUX_ECHOE 0000020u
#define LINUX_CREAD 0000200u
#define LINUX_CS8 0000060u
#define LINUX_B38400 0000017u
#define LINUX_VTIME 5u
#define LINUX_VMIN 6u
#define LINUX_VINTR 0u
#define LINUX_VERASE 2u
#define LINUX_VEOF 4u
#define LINUX_VSUSP 10u
#define LINUX_VSTART 8u
#define LINUX_VSTOP 9u
#define LINUX_F_DUPFD 0u
#define LINUX_F_GETFD 1u
#define LINUX_F_SETFD 2u
#define LINUX_F_GETFL 3u
#define LINUX_F_SETFL 4u
#define LINUX_F_DUPFD_CLOEXEC 1030u
#define LINUX_FD_CLOEXEC 1u
#define LINUX_RLIMIT_NLIMITS 16u
#define LINUX_RLIMIT_STACK 3u
#define LINUX_O_WRONLY 01u
#define LINUX_O_RDWR 02u
#define LINUX_O_ACCMODE 03u
#define LINUX_O_CREAT 0100u
#define LINUX_O_EXCL 0200u
#define LINUX_O_TRUNC 01000u
#define LINUX_O_APPEND 02000u
#define LINUX_O_NONBLOCK 04000u
#define LINUX_O_CLOEXEC 02000000u
#define LINUX_O_DIRECTORY 0200000u
#define LINUX_POLLIN 0x0001u
#define LINUX_POLLOUT 0x0004u
#define LINUX_EPERM 1
#define LINUX_ENOENT 2
#define LINUX_ESRCH 3
#define LINUX_EBADF 9
#define LINUX_EAGAIN 11
#define LINUX_EFAULT 14
#define LINUX_EEXIST 17
#define LINUX_ENOTDIR 20
#define LINUX_EISDIR 21
#define LINUX_EINVAL 22
#define LINUX_ERANGE 34
#define LINUX_DIRENT_NAME_SCRATCH 4096u
#define SYSCALL_PATH_MAX 4096u
#define USER_IO_CHUNK 512u

typedef struct {
	uint32_t addr;
	uint32_t length;
	uint32_t prot;
	uint32_t flags;
	uint32_t fd;
	uint32_t offset;
} old_mmap_args_t;

typedef struct {
	uint32_t entry_number;
	uint32_t base_addr;
	uint32_t limit;
	uint32_t flags;
} linux_user_desc_t;

typedef struct {
	uint32_t mode;
	uint32_t nlink;
	uint32_t size;
	uint32_t mtime;
	uint32_t rdev_major;
	uint32_t rdev_minor;
	uint64_t ino;
} linux_fd_stat_t;

static inline void linux_put_u32(uint8_t *buf, uint32_t off, uint32_t value)
{
	buf[off + 0] = (uint8_t)(value & 0xFFu);
	buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
	buf[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
	buf[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

static inline void linux_put_u16(uint8_t *buf, uint32_t off, uint32_t value)
{
	buf[off + 0] = (uint8_t)(value & 0xFFu);
	buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
}

static inline uint32_t linux_get_u32(const uint8_t *buf, uint32_t off)
{
	return (uint32_t)buf[off + 0] | ((uint32_t)buf[off + 1] << 8) |
	       ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

static inline uint32_t linux_get_u16(const uint8_t *buf, uint32_t off)
{
	return (uint32_t)buf[off + 0] | ((uint32_t)buf[off + 1] << 8);
}

static inline void linux_put_u64(uint8_t *buf, uint32_t off, uint64_t value)
{
	linux_put_u32(buf, off, (uint32_t)value);
	linux_put_u32(buf, off + 4u, (uint32_t)(value >> 32));
}

static inline uint64_t linux_encode_dev(uint32_t major, uint32_t minor)
{
	return ((uint64_t)major << 8) | (uint64_t)minor;
}

#endif
