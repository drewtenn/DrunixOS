/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall.c — INT 0x80 syscall dispatcher and kernel-side syscall implementations.
 */

#include "syscall.h"
#include "sched.h"
#include "process.h"
#include "gdt.h"
#include "pipe.h"
#include "blkdev.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "vfs.h"
#include "procfs.h"
#include "chardev.h"
#include "tty.h"
#include "module.h"
#include "klog.h"
#include "kprintf.h"
#include "kstring.h"
#include "fs.h"
#include "clock.h"
#include "uaccess.h"
#include "desktop.h"
#include <limits.h>
#include <stdint.h>

#define SYSCALL_NOINLINE __attribute__((noinline))

/* VGA functions from kernel.c */
extern void print_string(char *s);

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
static void kcwd_resolve(const char *cwd, const char *name,
                         char *out, int outsz)
{
    if (!name) { out[0] = '\0'; return; }

    if (k_strcmp(name, ".") == 0) {
        k_snprintf(out, (uint32_t)outsz, "%s", cwd);
    } else if (name[0] == '.' && name[1] == '/') {
        if (cwd[0] == '\0')
            k_snprintf(out, (uint32_t)outsz, "%s", name + 2);
        else
            k_snprintf(out, (uint32_t)outsz, "%s/%s", cwd, name + 2);
    } else if (name[0] == '/')
        k_snprintf(out, (uint32_t)outsz, "%s", name + 1);   /* strip leading '/' */
    else if (cwd[0] == '\0')
        k_snprintf(out, (uint32_t)outsz, "%s", name);        /* at root: use as-is */
    else
        k_snprintf(out, (uint32_t)outsz, "%s/%s", cwd, name); /* prepend cwd */
}
extern void print_bytes(const char *buf, int n);
extern void clear_screen(void);
extern void scroll_up(int n);
extern void scroll_down(int n);

/*
 * fd_alloc: find the lowest free fd slot in the process's table.
 * Returns the slot index (0–MAX_FDS-1) or -1 if the table is full.
 * Note: slots 0/1/2 are pre-populated by process_create(), so in the
 * normal case the first free slot will be 3.
 */
static int fd_alloc(process_t *proc)
{
    for (unsigned i = 0; i < MAX_FDS; i++) {
        if (proc->open_files[i].type == FD_TYPE_NONE)
            return (int)i;
    }
    return -1;
}

static tty_t *syscall_tty_from_fd(process_t *cur, uint32_t fd,
                                  uint32_t *tty_idx_out)
{
    file_handle_t *fh;
    tty_t *tty;

    if (!cur || fd >= MAX_FDS)
        return 0;

    fh = &cur->open_files[fd];
    if (fh->type != FD_TYPE_TTY)
        return 0;

    tty = tty_get((int)fh->u.tty.tty_idx);
    if (!tty)
        return 0;

    if (tty_idx_out)
        *tty_idx_out = fh->u.tty.tty_idx;
    return tty;
}

static int syscall_desktop_should_route_console_output(desktop_state_t *desktop,
                                                       process_t *cur)
{
    uint32_t shell_pid;
    tty_t *tty;

    if (!desktop || !cur)
        return 0;
    if (desktop_process_owns_shell(desktop, cur->pid, cur->pgid))
        return 1;

    shell_pid = desktop_shell_pid(desktop);
    if (shell_pid == 0)
        return 0;
    if (cur->parent_pid == shell_pid)
        return 1;

    tty = tty_get((int)cur->tty_id);
    if (tty && tty->fg_pgid != 0 && tty->fg_pgid == cur->pgid)
        return 1;

    return 0;
}

static int syscall_scroll_count(uint32_t count)
{
    if (count > (uint32_t)INT_MAX)
        return INT_MAX;
    return (int)count;
}

static int syscall_apply_sigmask(process_t *cur, uint32_t how,
                                 uint32_t newmask, int has_newmask)
{
    uint32_t old;

    if (!cur)
        return -1;

    old = cur->sig_blocked;
    if (has_newmask) {
        switch (how) {
        case 0: /* SIG_BLOCK   */
            cur->sig_blocked = old | newmask;
            break;
        case 1: /* SIG_UNBLOCK */
            cur->sig_blocked = old & ~newmask;
            break;
        case 2: /* SIG_SETMASK */
            cur->sig_blocked = newmask;
            break;
        default:
            return -1;
        }
        cur->sig_blocked &= ~((1u << SIGKILL) | (1u << SIGSTOP));
    }

    return 0;
}

static int syscall_copy_rt_sigset_to_user(process_t *cur, uint32_t user_dst,
                                          uint32_t sigset_size,
                                          uint32_t mask)
{
    uint8_t out[128];

    if (user_dst == 0)
        return 0;
    if (!cur || sigset_size < sizeof(uint32_t) || sigset_size > sizeof(out))
        return -1;

    k_memset(out, 0, sizeof(out));
    out[0] = (uint8_t)(mask & 0xFFu);
    out[1] = (uint8_t)((mask >> 8) & 0xFFu);
    out[2] = (uint8_t)((mask >> 16) & 0xFFu);
    out[3] = (uint8_t)((mask >> 24) & 0xFFu);
    return uaccess_copy_to_user(cur, user_dst, out, sigset_size);
}

static int syscall_write_console_bytes(process_t *cur,
                                       const char *buf,
                                       uint32_t len)
{
    desktop_state_t *desktop = desktop_is_active() ? desktop_global() : 0;

    if (desktop &&
        syscall_desktop_should_route_console_output(desktop, cur) &&
        desktop_write_console_output(desktop, buf, len) == (int)len) {
        return (int)len;
    }

    print_bytes(buf, (int)len);
    return (int)len;
}

static void syscall_invlpg(uint32_t virt)
{
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

static uint32_t syscall_read_blockdev(process_t *cur, file_handle_t *fh,
                                      uint32_t user_buf, uint32_t count)
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
        if (uaccess_copy_to_user(cur, user_buf + copied, sec + sec_off,
                                 chunk) != 0)
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

#define LINUX_S_IFIFO 0010000u
#define LINUX_S_IFCHR 0020000u
#define LINUX_S_IFBLK 0060000u
#define LINUX_S_IFDIR 0040000u
#define LINUX_S_IFREG 0100000u
#define LINUX_S_IFLNK 0120000u
#define LINUX_DT_FIFO 1u
#define LINUX_DT_CHR  2u
#define LINUX_DT_DIR  4u
#define LINUX_DT_REG  8u
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
#define LINUX_F_DUPFD 0u
#define LINUX_F_GETFD 1u
#define LINUX_F_SETFD 2u
#define LINUX_F_GETFL 3u
#define LINUX_F_SETFL 4u
#define LINUX_F_DUPFD_CLOEXEC 1030u
#define LINUX_O_WRONLY 01u
#define LINUX_O_RDWR 02u
#define LINUX_O_CREAT 0100u
#define LINUX_O_TRUNC 01000u
#define LINUX_O_APPEND 02000u
#define LINUX_POLLIN 0x0001u
#define LINUX_POLLOUT 0x0004u
#define USER_IO_CHUNK 512u
#define TTY_IO_CHUNK \
    ((TTY_CANON_BUF_SIZE > TTY_RAW_BUF_SIZE) ? TTY_CANON_BUF_SIZE : TTY_RAW_BUF_SIZE)

typedef struct {
    uint32_t mode;
    uint32_t nlink;
    uint32_t size;
    uint32_t mtime;
    uint32_t rdev_major;
    uint32_t rdev_minor;
    uint64_t ino;
} linux_fd_stat_t;

static int resolve_user_path(process_t *proc, uint32_t user_ptr,
                             char *out, uint32_t outsz);
static int resolve_user_path_at(process_t *proc, uint32_t dirfd,
                                uint32_t user_ptr,
                                char *out, uint32_t outsz);
static char *copy_user_string_alloc(process_t *proc, uint32_t user_ptr,
                                    uint32_t max_len);

static void linux_put_u32(uint8_t *buf, uint32_t off, uint32_t value)
{
    buf[off + 0] = (uint8_t)(value & 0xFFu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
    buf[off + 2] = (uint8_t)((value >> 16) & 0xFFu);
    buf[off + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

static void linux_put_u16(uint8_t *buf, uint32_t off, uint32_t value)
{
    buf[off + 0] = (uint8_t)(value & 0xFFu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint32_t linux_get_u32(const uint8_t *buf, uint32_t off)
{
    return (uint32_t)buf[off + 0] |
           ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) |
           ((uint32_t)buf[off + 3] << 24);
}

static uint32_t linux_get_u16(const uint8_t *buf, uint32_t off)
{
    return (uint32_t)buf[off + 0] | ((uint32_t)buf[off + 1] << 8);
}

static void linux_put_u64(uint8_t *buf, uint32_t off, uint64_t value)
{
    linux_put_u32(buf, off, (uint32_t)value);
    linux_put_u32(buf, off + 4u, (uint32_t)(value >> 32));
}

static uint64_t linux_encode_dev(uint32_t major, uint32_t minor)
{
    return ((uint64_t)major << 8) | (uint64_t)minor;
}

static void linux_copy_field(char *dst, uint32_t off, uint32_t len,
                             const char *src)
{
    uint32_t i = 0;

    if (!dst || len == 0)
        return;
    while (src && src[i] && i + 1u < len) {
        dst[off + i] = src[i];
        i++;
    }
    dst[off + i] = '\0';
}

static void linux_fill_stat64(uint8_t *st,
                              uint32_t mode,
                              uint32_t nlink,
                              uint32_t size,
                              uint32_t mtime,
                              uint32_t rdev_major,
                              uint32_t rdev_minor,
                              uint64_t ino)
{
    uint32_t blocks;

    k_memset(st, 0, 144u);
    blocks = (size + 511u) / 512u;

    linux_put_u64(st, 0u, 1u);          /* st_dev */
    linux_put_u32(st, 16u, mode);       /* st_mode */
    linux_put_u32(st, 20u, nlink);      /* st_nlink */
    linux_put_u32(st, 24u, 0u);         /* st_uid */
    linux_put_u32(st, 28u, 0u);         /* st_gid */
    linux_put_u64(st, 32u, linux_encode_dev(rdev_major, rdev_minor));
    linux_put_u64(st, 44u, size);       /* st_size */
    linux_put_u32(st, 52u, 4096u);      /* st_blksize */
    linux_put_u64(st, 56u, blocks);     /* st_blocks */
    linux_put_u64(st, 88u, ino);        /* st_ino */
    linux_put_u64(st, 96u, mtime);      /* st_atim.tv_sec */
    linux_put_u64(st, 112u, mtime);     /* st_mtim.tv_sec */
    linux_put_u64(st, 128u, mtime);     /* st_ctim.tv_sec */
}

static void linux_fill_statx_timestamp(uint8_t *stx, uint32_t off,
                                       uint32_t sec)
{
    linux_put_u64(stx, off, sec);       /* tv_sec */
    linux_put_u32(stx, off + 8u, 0u);   /* tv_nsec */
    linux_put_u32(stx, off + 12u, 0u);  /* __reserved */
}

static void linux_fill_statx(uint8_t *stx, const linux_fd_stat_t *meta)
{
    uint32_t blocks;

    k_memset(stx, 0, 256u);
    blocks = (meta->size + 511u) / 512u;

    linux_put_u32(stx, 0u, LINUX_STATX_BASIC_STATS); /* stx_mask */
    linux_put_u32(stx, 4u, 4096u);                   /* stx_blksize */
    linux_put_u32(stx, 16u, meta->nlink);            /* stx_nlink */
    linux_put_u32(stx, 20u, 0u);                     /* stx_uid */
    linux_put_u32(stx, 24u, 0u);                     /* stx_gid */
    linux_put_u32(stx, 28u, meta->mode);             /* stx_mode */
    linux_put_u64(stx, 32u, meta->ino);              /* stx_ino */
    linux_put_u64(stx, 40u, meta->size);             /* stx_size */
    linux_put_u64(stx, 48u, blocks);                 /* stx_blocks */
    linux_fill_statx_timestamp(stx, 64u, meta->mtime);
    linux_fill_statx_timestamp(stx, 96u, meta->mtime);
    linux_fill_statx_timestamp(stx, 112u, meta->mtime);
    linux_put_u32(stx, 128u, meta->rdev_major);      /* stx_rdev_major */
    linux_put_u32(stx, 132u, meta->rdev_minor);      /* stx_rdev_minor */
    linux_put_u32(stx, 136u, 1u);                    /* stx_dev_major */
    linux_put_u32(stx, 140u, 0u);                    /* stx_dev_minor */
}

static int linux_blockdev_identity(const char *name, uint64_t *ino,
                                   uint32_t *major, uint32_t *minor)
{
    blkdev_info_t info;
    int idx;

    if (!name)
        return -1;
    idx = blkdev_find_index(name);
    if (idx < 0)
        return -1;
    if (blkdev_info_at((uint32_t)idx, &info) != 0)
        return -1;
    if (ino)
        *ino = 0x80000000u + (uint32_t)idx;
    if (major)
        *major = info.major;
    if (minor)
        *minor = info.minor;
    return 0;
}

static int linux_fd_stat_metadata(process_t *cur, uint32_t fd,
                                  linux_fd_stat_t *meta)
{
    file_handle_t *fh;

    if (!cur || !meta || fd >= MAX_FDS)
        return -1;

    fh = &cur->open_files[fd];
    k_memset(meta, 0, sizeof(*meta));
    meta->nlink = 1;
    meta->size = 0;
    meta->mtime = clock_unix_time();
    meta->ino = fd + 1u;

    switch (fh->type) {
    case FD_TYPE_FILE:
        meta->mode = LINUX_S_IFREG | 0644u;
        meta->size = fh->u.file.size;
        meta->ino = fh->u.file.inode_num;
        break;
    case FD_TYPE_PROCFILE:
        meta->mode = LINUX_S_IFREG | 0444u;
        meta->size = fh->u.proc.size;
        meta->ino = 0x70000000ull + fh->u.proc.kind * 1024u +
                    fh->u.proc.pid * 16u + fh->u.proc.index;
        break;
    case FD_TYPE_DIR:
        meta->mode = LINUX_S_IFDIR | 0755u;
        meta->nlink = 2;
        meta->ino = 0x60000000ull + fd;
        break;
    case FD_TYPE_CHARDEV:
    case FD_TYPE_TTY:
    case FD_TYPE_STDOUT:
        meta->mode = LINUX_S_IFCHR | 0600u;
        break;
    case FD_TYPE_BLOCKDEV: {
        meta->mode = LINUX_S_IFBLK | 0444u;
        meta->size = fh->u.blockdev.size;
        (void)linux_blockdev_identity(fh->u.blockdev.name, &meta->ino,
                                      &meta->rdev_major, &meta->rdev_minor);
        break;
    }
    case FD_TYPE_PIPE_READ:
    case FD_TYPE_PIPE_WRITE:
        meta->mode = LINUX_S_IFIFO | 0600u;
        break;
    default:
        return -1;
    }

    return 0;
}

static void linux_metadata_from_vfs_stat(const vfs_stat_t *st,
                                         linux_fd_stat_t *meta)
{
    if (st->type == VFS_STAT_TYPE_DIR)
        meta->mode = LINUX_S_IFDIR | 0755u;
    else if (st->type == VFS_STAT_TYPE_SYMLINK)
        meta->mode = LINUX_S_IFLNK | 0777u;
    else if (st->type == VFS_STAT_TYPE_BLOCKDEV)
        meta->mode = LINUX_S_IFBLK | 0444u;
    else
        meta->mode = LINUX_S_IFREG | 0644u;
    meta->nlink = st->type == VFS_STAT_TYPE_DIR ? 2u : st->link_count;
    meta->size = st->size;
    meta->mtime = st->mtime;
    meta->ino = 1u;
}

static void linux_metadata_fix_blockdev_identity(const char *path,
                                                 const vfs_stat_t *st,
                                                 linux_fd_stat_t *meta)
{
    const char *base;

    if (!path || !st || !meta || st->type != VFS_STAT_TYPE_BLOCKDEV)
        return;

    base = k_strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base[0])
        return;

    (void)linux_blockdev_identity(base, &meta->ino, &meta->rdev_major,
                                  &meta->rdev_minor);
}

static int linux_path_stat_metadata(process_t *cur, uint32_t user_path,
                                    linux_fd_stat_t *meta)
{
    char *rpath;
    vfs_stat_t st;

    if (!cur || !meta || user_path == 0)
        return -1;

    k_memset(meta, 0, sizeof(*meta));
    rpath = (char *)kmalloc(4096);
    if (!rpath)
        return -1;
    if (resolve_user_path(cur, user_path, rpath, 4096) != 0) {
        kfree(rpath);
        return -1;
    }
    if (vfs_stat(rpath, &st) != 0) {
        kfree(rpath);
        return -1;
    }

    linux_metadata_from_vfs_stat(&st, meta);
    linux_metadata_fix_blockdev_identity(rpath, &st, meta);

    kfree(rpath);
    return 0;
}

static int linux_path_lstat_metadata(process_t *cur, uint32_t user_path,
                                     linux_fd_stat_t *meta)
{
    char *rpath;
    vfs_stat_t st;

    if (!cur || !meta || user_path == 0)
        return -1;

    k_memset(meta, 0, sizeof(*meta));
    rpath = (char *)kmalloc(4096);
    if (!rpath)
        return -1;
    if (resolve_user_path(cur, user_path, rpath, 4096) != 0) {
        kfree(rpath);
        return -1;
    }
    if (vfs_lstat(rpath, &st) != 0) {
        kfree(rpath);
        return -1;
    }

    linux_metadata_from_vfs_stat(&st, meta);
    linux_metadata_fix_blockdev_identity(rpath, &st, meta);

    kfree(rpath);
    return 0;
}

static int linux_path_stat_metadata_at_flags(process_t *cur, uint32_t dirfd,
                                             uint32_t user_path,
                                             linux_fd_stat_t *meta,
                                             uint32_t nofollow)
{
    char *raw;
    char *rpath;
    vfs_stat_t st;

    if (!cur || !meta || user_path == 0)
        return -1;

    k_memset(meta, 0, sizeof(*meta));
    raw = copy_user_string_alloc(cur, user_path, 4096);
    if (!raw)
        return -1;

    rpath = (char *)kmalloc(4096);
    if (!rpath) {
        kfree(raw);
        return -1;
    }

    if (raw[0] == '/' || dirfd == LINUX_AT_FDCWD) {
        kcwd_resolve(cur->cwd, raw, rpath, 4096);
    } else {
        file_handle_t *fh;

        if (dirfd >= MAX_FDS) {
            kfree(rpath);
            kfree(raw);
            return -1;
        }
        fh = &cur->open_files[dirfd];
        if (fh->type != FD_TYPE_DIR) {
            kfree(rpath);
            kfree(raw);
            return -1;
        }
        kcwd_resolve(fh->u.dir.path, raw, rpath, 4096);
    }

    if ((nofollow ? vfs_lstat(rpath, &st) : vfs_stat(rpath, &st)) != 0) {
        kfree(rpath);
        kfree(raw);
        return -1;
    }

    linux_metadata_from_vfs_stat(&st, meta);
    linux_metadata_fix_blockdev_identity(rpath, &st, meta);

    kfree(rpath);
    kfree(raw);
    return 0;
}

static int linux_path_stat_metadata_at(process_t *cur, uint32_t dirfd,
                                       uint32_t user_path,
                                       linux_fd_stat_t *meta)
{
    return linux_path_stat_metadata_at_flags(cur, dirfd, user_path, meta, 0);
}

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

static int linux_fill_getdents(process_t *cur, file_handle_t *fh,
                               uint32_t user_buf, uint32_t count)
{
    char *names;
    uint8_t *out;
    int names_len;
    uint32_t entry = 0;
    uint32_t written = 0;
    uint32_t pos = 0;

    if (!cur || !fh || fh->type != FD_TYPE_DIR || user_buf == 0 || count == 0)
        return -1;

    names = (char *)kmalloc(4096);
    out = (uint8_t *)kmalloc(count);
    if (!names || !out) {
        if (names) kfree(names);
        if (out) kfree(out);
        return -1;
    }

    names_len = vfs_getdents(fh->u.dir.path[0] ? fh->u.dir.path : 0,
                             names, 4096);
    if (names_len < 0) {
        kfree(out);
        kfree(names);
        return -1;
    }

    while (pos < (uint32_t)names_len) {
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

static int linux_fill_getdents64(process_t *cur, file_handle_t *fh,
                                 uint32_t user_buf, uint32_t count)
{
    char *names;
    uint8_t *out;
    int names_len;
    uint32_t entry = 0;
    uint32_t written = 0;
    uint32_t pos = 0;

    if (!cur || !fh || fh->type != FD_TYPE_DIR || user_buf == 0 || count == 0)
        return -1;

    names = (char *)kmalloc(4096);
    out = (uint8_t *)kmalloc(count);
    if (!names || !out) {
        if (names) kfree(names);
        if (out) kfree(out);
        return -1;
    }

    names_len = vfs_getdents(fh->u.dir.path[0] ? fh->u.dir.path : 0,
                             names, 4096);
    if (names_len < 0) {
        kfree(out);
        kfree(names);
        return -1;
    }

    while (pos < (uint32_t)names_len) {
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

static int linux_path_exists(process_t *cur, uint32_t user_path)
{
    char *rpath;
    vfs_stat_t st;
    int rc;

    if (!cur || user_path == 0)
        return -1;
    rpath = (char *)kmalloc(4096);
    if (!rpath)
        return -1;
    if (resolve_user_path(cur, user_path, rpath, 4096) != 0) {
        kfree(rpath);
        return -1;
    }
    rc = vfs_stat(rpath, &st);
    kfree(rpath);
    return rc == 0 ? 0 : -2;
}

static int linux_path_exists_at(process_t *cur, uint32_t dirfd,
                                uint32_t user_path)
{
    char *rpath;
    vfs_stat_t st;
    int rc;

    if (!cur || user_path == 0)
        return -1;
    rpath = (char *)kmalloc(4096);
    if (!rpath)
        return -1;
    if (resolve_user_path_at(cur, dirfd, user_path, rpath, 4096) != 0) {
        kfree(rpath);
        return -1;
    }
    rc = vfs_stat(rpath, &st);
    kfree(rpath);
    return rc == 0 ? 0 : -2;
}

static int linux_truncate_path(process_t *cur, uint32_t user_path,
                               uint64_t length)
{
    char *rpath;
    vfs_file_ref_t ref;
    uint32_t size = 0;
    int rc;

    if (!cur || user_path == 0)
        return -1;
    if (length > 0xFFFFFFFFull)
        return -22;

    rpath = (char *)kmalloc(4096);
    if (!rpath)
        return -1;
    if (resolve_user_path(cur, user_path, rpath, 4096) != 0) {
        kfree(rpath);
        return -1;
    }

    rc = vfs_open_file(rpath, &ref, &size);
    kfree(rpath);
    if (rc != 0)
        return -2;
    (void)size;
    return vfs_truncate(ref, (uint32_t)length) == 0 ? 0 : -1;
}

static int linux_truncate_fd(process_t *cur, uint32_t fd, uint64_t length)
{
    file_handle_t *fh;

    if (!cur || fd >= MAX_FDS)
        return -1;
    if (length > 0xFFFFFFFFull)
        return -22;

    fh = &cur->open_files[fd];
    if (fh->type != FD_TYPE_FILE || !fh->writable)
        return -1;
    if (vfs_truncate(fh->u.file.ref, (uint32_t)length) != 0)
        return -1;

    for (uint32_t i = 0; i < MAX_FDS; i++) {
        file_handle_t *other = &cur->open_files[i];
        if (other->type == FD_TYPE_FILE &&
            other->u.file.ref.mount_id == fh->u.file.ref.mount_id &&
            other->u.file.ref.inode_num == fh->u.file.ref.inode_num) {
            other->u.file.size = (uint32_t)length;
            if (other->u.file.offset > other->u.file.size)
                other->u.file.offset = other->u.file.size;
        }
    }

    return 0;
}

static int linux_copy_statfs64(process_t *cur, uint32_t user_buf,
                               uint32_t user_size)
{
    uint8_t st[84];
    uint32_t copy_size;

    if (!cur || user_buf == 0 || user_size < sizeof(st))
        return -22;

    k_memset(st, 0, sizeof(st));
    linux_put_u32(st, 0u, 0x4452554Eu);             /* f_type: "DRUN" */
    linux_put_u32(st, 4u, 4096u);                   /* f_bsize */
    linux_put_u64(st, 8u, 12800u);                  /* f_blocks */
    linux_put_u64(st, 16u, 6400u);                  /* f_bfree */
    linux_put_u64(st, 24u, 6400u);                  /* f_bavail */
    linux_put_u64(st, 32u, 4096u);                  /* f_files */
    linux_put_u64(st, 40u, 2048u);                  /* f_ffree */
    linux_put_u32(st, 56u, 255u);                   /* f_namelen */
    linux_put_u32(st, 60u, 4096u);                  /* f_frsize */

    copy_size = user_size < sizeof(st) ? user_size : sizeof(st);
    return uaccess_copy_to_user(cur, user_buf, st, copy_size) == 0 ? 0 : -1;
}

static uint32_t syscall_stat64_path_common(uint32_t user_path,
                                           uint32_t user_stat,
                                           uint32_t nofollow)
{
    process_t *cur = sched_current();
    uint8_t st64[144];
    linux_fd_stat_t meta;

    if (!cur || user_stat == 0 ||
        (nofollow ? linux_path_lstat_metadata(cur, user_path, &meta) :
                    linux_path_stat_metadata(cur, user_path, &meta)) != 0)
        return (uint32_t)-1;

        linux_fill_stat64(st64, meta.mode, meta.nlink, meta.size,
                          meta.mtime, meta.rdev_major, meta.rdev_minor,
                          meta.ino);
    if (uaccess_copy_to_user(cur, user_stat, st64, sizeof(st64)) != 0)
        return (uint32_t)-1;
    return 0;
}

static uint32_t syscall_stat64_path(uint32_t user_path, uint32_t user_stat)
{
    return syscall_stat64_path_common(user_path, user_stat, 0);
}

static uint32_t syscall_fstat64(uint32_t fd, uint32_t user_stat)
{
    process_t *cur = sched_current();
    uint8_t st64[144];
    linux_fd_stat_t meta;

    if (!cur || user_stat == 0 ||
        linux_fd_stat_metadata(cur, fd, &meta) != 0)
        return (uint32_t)-1;

        linux_fill_stat64(st64, meta.mode, meta.nlink, meta.size,
                          meta.mtime, meta.rdev_major, meta.rdev_minor,
                          meta.ino);
    if (uaccess_copy_to_user(cur, user_stat, st64, sizeof(st64)) != 0)
        return (uint32_t)-1;
    return 0;
}

static uint32_t syscall_statx(uint32_t dirfd, uint32_t user_path,
                              uint32_t flags, uint32_t mask,
                              uint32_t user_statx)
{
    process_t *cur = sched_current();
    uint8_t stx[256];
    linux_fd_stat_t meta;
    char first;

    (void)mask;
    if (!cur || user_path == 0 || user_statx == 0)
        return (uint32_t)-1;
    if ((flags & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_NO_AUTOMOUNT |
                   LINUX_AT_EMPTY_PATH |
                   LINUX_AT_STATX_SYNC_TYPE)) != 0)
        return (uint32_t)-1;
    if (uaccess_copy_from_user(cur, &first, user_path, sizeof(first)) != 0)
        return (uint32_t)-1;
    if (first == '\0' && (flags & LINUX_AT_EMPTY_PATH) != 0) {
        if (linux_fd_stat_metadata(cur, dirfd, &meta) != 0)
            return (uint32_t)-1;
    } else {
        if (linux_path_stat_metadata_at_flags(cur, dirfd, user_path, &meta,
                                              (flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0) != 0)
            return (uint32_t)-2; /* ENOENT */
    }

    linux_fill_statx(stx, &meta);
    if (uaccess_copy_to_user(cur, user_statx, stx, sizeof(stx)) != 0)
        return (uint32_t)-1;
    return 0;
}

static uint32_t syscall_uname(uint32_t user_uts)
{
    process_t *cur = sched_current();
    char uts[390];

    if (!cur || user_uts == 0)
        return (uint32_t)-1;
    k_memset(uts, 0, sizeof(uts));
    linux_copy_field(uts, 0u, 65u, "Drunix");
    linux_copy_field(uts, 65u, 65u, "drunix");
    linux_copy_field(uts, 130u, 65u, "0.1");
    linux_copy_field(uts, 195u, 65u, "Drunix Linux i386 ABI");
    linux_copy_field(uts, 260u, 65u, "i486");
    linux_copy_field(uts, 325u, 65u, "drunix.local");
    if (uaccess_copy_to_user(cur, user_uts, uts, sizeof(uts)) != 0)
        return (uint32_t)-1;
    return 0;
}

static uint32_t syscall_write_fd(uint32_t fd, uint32_t user_buf,
                                 uint32_t count)
{
    process_t *cur;
    file_handle_t *fh;

    if (fd >= MAX_FDS)
        return (uint32_t)-1;

    cur = sched_current();
    if (!cur)
        return (uint32_t)-1;

    fh = &cur->open_files[fd];

    if (fh->type == FD_TYPE_BLOCKDEV)
        return (uint32_t)-1;

    if (fh->type == FD_TYPE_STDOUT) {
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
            if (uaccess_copy_from_user(cur, kbuf, user_buf + written,
                                       chunk) != 0) {
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
            if (uaccess_copy_from_user(cur, kbuf, user_buf + copied,
                                       chunk) != 0)
                return copied ? copied : (uint32_t)-1;

            while (pb->count == PIPE_BUF_SIZE) {
                if (pb->read_open == 0 || cur->sig_pending)
                    return copied ? copied : (uint32_t)-1;
                sched_block(&pb->waiters);
            }

            for (uint32_t i = 0; i < chunk; i++) {
                while (pb->count == PIPE_BUF_SIZE) {
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

    if (fh->type == FD_TYPE_FILE) {
        uint8_t kbuf[USER_IO_CHUNK];
        uint32_t written = 0;

        if (!fh->writable)
            return (uint32_t)-1;
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
            if (uaccess_copy_from_user(cur, kbuf, user_buf + written,
                                       chunk) != 0)
                return written ? written : (uint32_t)-1;

            n = vfs_write(fh->u.file.ref, fh->u.file.offset + written,
                          kbuf, chunk);
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

static uint32_t syscall_read_fd(uint32_t fd, uint32_t user_buf,
                                uint32_t count)
{
    process_t *cur;
    file_handle_t *fh;

    if (fd >= MAX_FDS)
        return (uint32_t)-1;

    cur = sched_current();
    if (!cur)
        return (uint32_t)-1;

    fh = &cur->open_files[fd];

    if (fh->type == FD_TYPE_BLOCKDEV)
        return syscall_read_blockdev(cur, fh, user_buf, count);

    if (fh->type == FD_TYPE_TTY) {
        char kbuf[TTY_IO_CHUNK];
        uint32_t read_count = count ? count : 1;
        uint32_t chunk = read_count > TTY_IO_CHUNK ? TTY_IO_CHUNK : read_count;
        int n;

        if (uaccess_prepare(cur, user_buf, read_count, 1) != 0)
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
            __asm__ volatile ("pause");
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

    if (fh->type == FD_TYPE_FILE) {
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
            if (uaccess_copy_to_user(cur, user_buf + copied, kbuf,
                                     (uint32_t)n) != 0)
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

        if (procfs_file_size(fh->u.proc.kind, fh->u.proc.pid,
                             fh->u.proc.index, &size) != 0)
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
            n = procfs_read_file(fh->u.proc.kind, fh->u.proc.pid,
                                 fh->u.proc.index,
                                 fh->u.proc.offset + copied,
                                 (char *)kbuf, chunk);
            if (n < 0)
                return copied ? copied : (uint32_t)-1;
            if (n == 0)
                break;
            if (uaccess_copy_to_user(cur, user_buf + copied, kbuf,
                                     (uint32_t)n) != 0)
                return copied ? copied : (uint32_t)-1;
            copied += (uint32_t)n;
        }

        fh->u.proc.offset += copied;
        return copied;
    }

    return (uint32_t)-1;
}

static uint32_t syscall_sysinfo(uint32_t user_info)
{
    process_t *cur = sched_current();
    uint8_t info[64];
    uint32_t pids[MAX_PROCS];
    int n;

    if (!cur || user_info == 0)
        return (uint32_t)-1;
    n = sched_snapshot_pids(pids, MAX_PROCS, 1);
    k_memset(info, 0, sizeof(info));
    linux_put_u32(info, 0u, clock_unix_time());
    linux_put_u32(info, 16u, 16u * 1024u * 1024u);
    linux_put_u32(info, 20u, 8u * 1024u * 1024u);
    linux_put_u16(info, 40u, (uint32_t)(n < 0 ? 0 : n));
    linux_put_u32(info, 52u, 1u);
    return uaccess_copy_to_user(cur, user_info, info, sizeof(info)) == 0
        ? 0
        : (uint32_t)-1;
}

static int linux_wait_child(process_t *cur, uint32_t pid, int options,
                            uint32_t *pid_out)
{
    if (!cur || !pid_out)
        return -1;
    *pid_out = 0;

    if (pid != 0xFFFFFFFFu) {
        const process_t *target = sched_find_process(pid, 1);
        int target_ready;
        int status;

        if (!target)
            return -1;
        target_ready = target->state == PROC_ZOMBIE ||
                       ((options & WUNTRACED) &&
                        target->state == PROC_STOPPED);
        status = sched_waitpid(pid, options);
        if (status < 0)
            return status;
        if (status != 0 || target_ready || !(options & WNOHANG))
            *pid_out = pid;
        return status;
    }

    for (;;) {
        uint32_t pids[MAX_PROCS];
        int n = sched_snapshot_pids(pids, MAX_PROCS, 1);
        int found_child = 0;

        for (int i = 0; i < n; i++) {
            const process_t *child = sched_find_process(pids[i], 1);

            if (!child || child->parent_pid != cur->pid)
                continue;
            found_child = 1;
            if (child->state == PROC_ZOMBIE ||
                ((options & WUNTRACED) && child->state == PROC_STOPPED)) {
                uint32_t child_pid = child->pid;
                int status = sched_waitpid(child_pid, options);
                if (status >= 0)
                    *pid_out = child_pid;
                return status;
            }
        }

        if (!found_child)
            return -1;
        if (options & WNOHANG)
            return 0;
        schedule();
    }
}

static uint32_t syscall_wait_common(uint32_t pid, uint32_t user_status,
                                    uint32_t options, uint32_t user_rusage)
{
    process_t *cur = sched_current();
    uint32_t waited_pid = pid;
    int status = linux_wait_child(cur, pid, (int)options, &waited_pid);

    if (status < 0)
        return (uint32_t)-1;
    if (waited_pid == 0)
        return 0;
    if (user_status != 0 && (!cur ||
        uaccess_copy_to_user(cur, user_status, &status, sizeof(status)) != 0))
        return (uint32_t)-1;
    if (user_rusage != 0) {
        uint8_t zero[72];
        k_memset(zero, 0, sizeof(zero));
        if (!cur ||
            uaccess_copy_to_user(cur, user_rusage, zero, sizeof(zero)) != 0)
            return (uint32_t)-1;
    }
    return waited_pid;
}

static uint32_t syscall_nanosleep(uint32_t user_req, uint32_t user_rem)
{
    process_t *cur = sched_current();
    uint32_t req[2];
    uint32_t start;
    uint32_t sec_ticks;
    uint32_t tick_nsec;
    uint32_t nsec_ticks;
    uint32_t delta_ticks;
    uint32_t deadline;
    uint32_t now;

    if (!cur || user_req == 0)
        return (uint32_t)-1;
    if (uaccess_copy_from_user(cur, req, user_req, sizeof(req)) != 0)
        return (uint32_t)-1;
    if (req[1] >= 1000000000u)
        return (uint32_t)-1;
    if (req[0] == 0 && req[1] == 0)
        return 0;

    start = sched_ticks();
    sec_ticks = (req[0] > (0xFFFFFFFFu / SCHED_HZ)) ?
        0xFFFFFFFFu : req[0] * SCHED_HZ;
    tick_nsec = 1000000000u / SCHED_HZ;
    nsec_ticks = (req[1] == 0) ? 0 : (req[1] + tick_nsec - 1u) / tick_nsec;
    delta_ticks = (sec_ticks > 0xFFFFFFFFu - nsec_ticks) ?
        0xFFFFFFFFu : sec_ticks + nsec_ticks;
    deadline = start + delta_ticks;

    sched_block_until(deadline);

    now = sched_ticks();
    if ((int32_t)(deadline - now) > 0) {
        uint32_t remaining_ticks = deadline - now;
        if (user_rem != 0) {
            uint32_t rem[2];
            rem[0] = remaining_ticks / SCHED_HZ;
            rem[1] = (remaining_ticks % SCHED_HZ) * tick_nsec;
            if (uaccess_copy_to_user(cur, user_rem, rem, sizeof(rem)) != 0)
                return (uint32_t)-1;
        }
        return (uint32_t)-1;
    }
    return 0;
}

static uint32_t linux_poll_revents(process_t *cur, int32_t fd,
                                   uint32_t events)
{
    file_handle_t *fh;
    uint32_t rev = 0;

    if (!cur || fd < 0 || (uint32_t)fd >= MAX_FDS)
        return 0;
    fh = &cur->open_files[(uint32_t)fd];
    if (fh->type == FD_TYPE_NONE)
        return 0x0020u; /* POLLNVAL */

    if (events & LINUX_POLLOUT) {
        if (fh->type == FD_TYPE_STDOUT || fh->type == FD_TYPE_PIPE_WRITE ||
            fh->writable)
            rev |= LINUX_POLLOUT;
    }

    if (events & LINUX_POLLIN) {
        if (fh->type == FD_TYPE_FILE && fh->u.file.offset < fh->u.file.size)
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
        } else if (fh->type == FD_TYPE_TTY)
            rev |= LINUX_POLLIN;
    }

    return rev;
}

static uint32_t syscall_ioctl(uint32_t fd, uint32_t request, uint32_t argp)
{
    process_t *cur = sched_current();
    file_handle_t *fh;

    if (!cur || fd >= MAX_FDS)
        return (uint32_t)-1;
    fh = &cur->open_files[fd];
    if (fh->type == FD_TYPE_NONE)
        return (uint32_t)-1;

    switch (request) {
    case LINUX_TIOCGWINSZ: {
        uint16_t ws[4];

        if (argp == 0)
            return (uint32_t)-1;
        ws[0] = 48u;
        ws[1] = 128u;
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
        } else if (fh->type == FD_TYPE_FILE) {
            if (fh->u.file.offset < fh->u.file.size)
                available = fh->u.file.size - fh->u.file.offset;
        } else if (fh->type == FD_TYPE_PROCFILE) {
            uint32_t size = 0;
            if (procfs_file_size(fh->u.proc.kind, fh->u.proc.pid,
                                 fh->u.proc.index, &size) != 0)
                return (uint32_t)-1;
            if (fh->u.proc.offset < size)
                available = size - fh->u.proc.offset;
        }
        if (uaccess_copy_to_user(cur, argp, &available,
                                 sizeof(available)) != 0)
            return (uint32_t)-1;
        return 0;
    }
    case LINUX_TCGETS: {
        uint8_t termios[60];

        if (argp == 0)
            return (uint32_t)-1;
        k_memset(termios, 0, sizeof(termios));
        if (uaccess_copy_to_user(cur, argp, termios, sizeof(termios)) != 0)
            return (uint32_t)-1;
        return 0;
    }
    case LINUX_TCSETS:
    case LINUX_TCSETSW:
    case LINUX_TCSETSF:
        return 0;
    default:
        return (uint32_t)-1;
    }
}

static void fd_bump_pipe_ref(file_handle_t *fh)
{
    pipe_buf_t *pb;

    if (!fh)
        return;
    if (fh->type == FD_TYPE_PIPE_READ) {
        pb = pipe_get((int)fh->u.pipe.pipe_idx);
        if (pb) pb->read_open++;
    } else if (fh->type == FD_TYPE_PIPE_WRITE) {
        pb = pipe_get((int)fh->u.pipe.pipe_idx);
        if (pb) pb->write_open++;
    }
}

static int fd_duplicate_from(process_t *proc, uint32_t oldfd, uint32_t minfd)
{
    uint32_t fd;

    if (!proc || oldfd >= MAX_FDS || minfd >= MAX_FDS)
        return -1;
    if (proc->open_files[oldfd].type == FD_TYPE_NONE)
        return -1;

    for (fd = minfd; fd < MAX_FDS; fd++) {
        if (proc->open_files[fd].type == FD_TYPE_NONE) {
            proc->open_files[fd] = proc->open_files[oldfd];
            fd_bump_pipe_ref(&proc->open_files[fd]);
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
    if (fh->type == FD_TYPE_TTY && fh->writable)
        flags = LINUX_O_RDWR;
    else
        flags = fh->writable ? LINUX_O_WRONLY : 0u;
    if (fh->type == FD_TYPE_FILE && fh->append)
        flags |= LINUX_O_APPEND;
    return flags;
}

static int syscall_seek_handle(process_t *cur, uint32_t fd, int64_t offset,
                               uint32_t whence, uint64_t *new_offset_out)
{
    file_handle_t *fh;
    uint32_t size = 0;
    uint32_t current = 0;
    int64_t base;
    int64_t new_off;

    if (!cur || fd >= MAX_FDS)
        return -1;
    fh = &cur->open_files[fd];
    if (fh->type != FD_TYPE_FILE && fh->type != FD_TYPE_PROCFILE)
        return -1;

    if (fh->type == FD_TYPE_FILE) {
        size = fh->u.file.size;
        current = fh->u.file.offset;
    } else {
        if (procfs_file_size(fh->u.proc.kind, fh->u.proc.pid,
                             fh->u.proc.index, &size) != 0)
            return -1;
        fh->u.proc.size = size;
        current = fh->u.proc.offset;
    }

    switch (whence) {
    case 0: base = 0; break;                  /* SEEK_SET */
    case 1: base = (int64_t)current; break;   /* SEEK_CUR */
    case 2: base = (int64_t)size; break;      /* SEEK_END */
    default:
        return -1;
    }

    new_off = base + offset;
    if (new_off < 0 || new_off > UINT32_MAX)
        return -1;

    if (fh->type == FD_TYPE_FILE)
        fh->u.file.offset = (uint32_t)new_off;
    else
        fh->u.proc.offset = (uint32_t)new_off;
    if (new_offset_out)
        *new_offset_out = (uint64_t)new_off;
    return 0;
}

static uint32_t syscall_sendfile64(process_t *cur, uint32_t out_fd,
                                   uint32_t in_fd, uint32_t offset_ptr,
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
        return (uint32_t)-1;
    out = &cur->open_files[out_fd];
    in = &cur->open_files[in_fd];
    if (out->type != FD_TYPE_STDOUT)
        return (uint32_t)-1;
    if (in->type != FD_TYPE_FILE && in->type != FD_TYPE_PROCFILE)
        return (uint32_t)-1;

    if (offset_ptr) {
        if (uaccess_copy_from_user(cur, off_words, offset_ptr,
                                   sizeof(off_words)) != 0)
            return (uint32_t)-1;
        if (off_words[1] != 0)
            return (uint32_t)-1;
        read_off = off_words[0];
    } else if (in->type == FD_TYPE_FILE) {
        read_off = in->u.file.offset;
    } else {
        read_off = in->u.proc.offset;
    }

    if (in->type == FD_TYPE_FILE) {
        size = in->u.file.size;
    } else {
        if (procfs_file_size(in->u.proc.kind, in->u.proc.pid,
                             in->u.proc.index, &size) != 0)
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

        if (in->type == FD_TYPE_FILE) {
            n = vfs_read(in->u.file.ref, read_off, kbuf, chunk);
        } else {
            n = procfs_read_file(in->u.proc.kind, in->u.proc.pid,
                                 in->u.proc.index, read_off,
                                 (char *)kbuf, chunk);
        }
        if (n < 0)
            return copied ? copied : (uint32_t)-1;
        if (n == 0)
            break;
        if (syscall_write_console_bytes(cur, (const char *)kbuf,
                                        (uint32_t)n) != n)
            return copied ? copied : (uint32_t)-1;

        read_off += (uint32_t)n;
        copied += (uint32_t)n;
        if ((uint32_t)n < chunk)
            break;
    }

    if (offset_ptr) {
        off_words[0] = read_off;
        off_words[1] = 0;
        if (uaccess_copy_to_user(cur, offset_ptr, off_words,
                                 sizeof(off_words)) != 0)
            return copied ? copied : (uint32_t)-1;
    } else if (in->type == FD_TYPE_FILE) {
        in->u.file.offset = read_off;
    } else {
        in->u.proc.offset = read_off;
    }
    return copied;
}

static int prot_is_valid(uint32_t prot)
{
    return (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
}

static int prot_has_user_access(uint32_t prot)
{
    return (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) != 0;
}

static uint32_t prot_to_vma_flags(uint32_t prot)
{
    uint32_t flags = VMA_FLAG_ANON | VMA_FLAG_PRIVATE;

    if (prot & PROT_READ)
        flags |= VMA_FLAG_READ;
    if (prot & PROT_WRITE)
        flags |= VMA_FLAG_WRITE;
    if (prot & PROT_EXEC)
        flags |= VMA_FLAG_EXEC;
    return flags;
}

static void syscall_unmap_user_range(process_t *proc,
                                     uint32_t start, uint32_t end)
{
    uint32_t *pd;

    if (!proc || start >= end)
        return;

    pd = (uint32_t *)proc->pd_phys;
    for (uint32_t page = start; page < end; page += PAGE_SIZE) {
        uint32_t pdi = page >> 22;
        uint32_t pti = (page >> 12) & 0x3FFu;
        uint32_t *pt;

        if ((pd[pdi] & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER))
            continue;

        pt = (uint32_t *)paging_entry_addr(pd[pdi]);
        if ((pt[pti] & PG_PRESENT) == 0)
            continue;

        pmm_decref(paging_entry_addr(pt[pti]));
        pt[pti] = 0;
        syscall_invlpg(page);
    }
}

static void syscall_apply_mprotect(process_t *proc,
                                   uint32_t start, uint32_t end,
                                   uint32_t prot)
{
    if (!proc || start >= end)
        return;

    for (uint32_t page = start; page < end; page += PAGE_SIZE) {
        uint32_t *pte;
        uint32_t flags;
        uint32_t new_pte;

        if (paging_walk(proc->pd_phys, page, &pte) != 0)
            continue;

        /*
         * A PTE stores both the frame address and the low permission bits.
         * Rebuild the entry from its original address plus updated flags so
         * future permission changes cannot accidentally blend address bits.
         */
        flags = paging_entry_flags(*pte);
        if (prot_has_user_access(prot))
            flags |= PG_USER;
        else
            flags &= ~(uint32_t)PG_USER;

        if ((prot & PROT_WRITE) != 0 && (flags & PG_COW) == 0)
            flags |= PG_WRITABLE;
        else
            flags &= ~(uint32_t)PG_WRITABLE;

        new_pte = paging_entry_build(paging_entry_addr(*pte), flags);

        if (new_pte == *pte)
            continue;

        *pte = new_pte;
        syscall_invlpg(page);
    }
}

static int snapshot_user_string_vector(process_t *proc,
                                       uint32_t uvec,
                                       uint32_t max_count,
                                       uint32_t max_bytes,
                                       const char **out_vec,
                                       char *out_strs,
                                       int *out_count)
{
    uint32_t used = 0;

    *out_count = 0;
    if (uvec == 0) {
        out_vec[0] = 0;
        return 0;
    }

    for (uint32_t i = 0; i < max_count; i++) {
        uint32_t us = 0;
        uint32_t remaining;

        if (uaccess_copy_from_user(proc, &us, uvec + i * sizeof(uint32_t),
                                   sizeof(uint32_t)) != 0)
            return -1;
        if (us == 0) {
            out_vec[i] = 0;
            *out_count = (int)i;
            return 0;
        }

        if (used >= max_bytes)
            return -1;
        remaining = max_bytes - used;
        out_vec[i] = &out_strs[used];
        if (uaccess_copy_string_from_user(proc, &out_strs[used],
                                          remaining, us) != 0)
            return -1;
        used += k_strlen(&out_strs[used]) + 1;
    }

    return -1;
}

static int fd_install_vfs_node(process_t *proc, const vfs_node_t *node,
                               uint32_t writable, uint32_t append)
{
    int fd;

    if (!proc || !node)
        return -1;

    fd = fd_alloc(proc);
    if (fd < 0)
        return -1;

    proc->open_files[fd].writable = writable;
    proc->open_files[fd].append = 0;

    switch (node->type) {
    case VFS_NODE_FILE:
        proc->open_files[fd].type = FD_TYPE_FILE;
        proc->open_files[fd].append = append ? 1u : 0u;
        proc->open_files[fd].u.file.ref.mount_id = node->mount_id;
        proc->open_files[fd].u.file.ref.inode_num = node->inode_num;
        proc->open_files[fd].u.file.inode_num = node->inode_num;
        proc->open_files[fd].u.file.size = node->size;
        proc->open_files[fd].u.file.offset = 0;
        return fd;

    case VFS_NODE_DIR:
        proc->open_files[fd].type = FD_TYPE_DIR;
        proc->open_files[fd].u.dir.path[0] = '\0';
        proc->open_files[fd].u.dir.index = 0;
        return fd;

    case VFS_NODE_BLOCKDEV:
        if (writable) {
            proc->open_files[fd].type = FD_TYPE_NONE;
            proc->open_files[fd].writable = 0;
            return -1;
        }
        proc->open_files[fd].type = FD_TYPE_BLOCKDEV;
        k_strncpy(proc->open_files[fd].u.blockdev.name,
                  node->dev_name,
                  sizeof(proc->open_files[fd].u.blockdev.name) - 1);
        proc->open_files[fd].u.blockdev
            .name[sizeof(proc->open_files[fd].u.blockdev.name) - 1] = '\0';
        proc->open_files[fd].u.blockdev.offset = 0;
        proc->open_files[fd].u.blockdev.size = node->size;
        return fd;

    case VFS_NODE_TTY:
        proc->open_files[fd].type = FD_TYPE_TTY;
        proc->open_files[fd].u.tty.tty_idx = node->dev_id;
        return fd;

    case VFS_NODE_PROCFILE:
        proc->open_files[fd].type = FD_TYPE_PROCFILE;
        proc->open_files[fd].u.proc.kind = node->proc_kind;
        proc->open_files[fd].u.proc.pid = node->proc_pid;
        proc->open_files[fd].u.proc.index = node->proc_index;
        proc->open_files[fd].u.proc.size = node->size;
        proc->open_files[fd].u.proc.offset = 0;
        return fd;

    case VFS_NODE_CHARDEV:
        proc->open_files[fd].type = FD_TYPE_CHARDEV;
        k_strncpy(proc->open_files[fd].u.chardev.name,
                  node->dev_name,
                  sizeof(proc->open_files[fd].u.chardev.name) - 1);
        proc->open_files[fd].u.chardev
            .name[sizeof(proc->open_files[fd].u.chardev.name) - 1] = '\0';
        return fd;

    default:
        proc->open_files[fd].type = FD_TYPE_NONE;
        proc->open_files[fd].writable = 0;
        proc->open_files[fd].append = 0;
        return -1;
    }
}

static int syscall_open_resolved_path(process_t *cur, const char *rpath,
                                      uint32_t flags)
{
    uint32_t accmode;
    uint32_t writable;
    uint32_t append;
    vfs_node_t node;
    int rc;
    int fd;

    if (!cur || !rpath)
        return -1;

    accmode = flags & (LINUX_O_WRONLY | LINUX_O_RDWR);
    if (accmode == (LINUX_O_WRONLY | LINUX_O_RDWR))
        return -1;
    writable = accmode != 0;
    append = (flags & LINUX_O_APPEND) != 0;

    if ((flags & LINUX_O_TRUNC) && !writable)
        return -1;

    rc = vfs_resolve(rpath, &node);
    if (rc != 0) {
        if (rc < -1)
            return rc;
        if ((flags & LINUX_O_CREAT) == 0)
            return -2;
        if (vfs_create(rpath) < 0)
            return -1;
        if (vfs_resolve(rpath, &node) != 0)
            return -1;
    } else if (flags & LINUX_O_TRUNC) {
        vfs_file_ref_t ref;
        uint32_t size = 0;

        if (node.type != VFS_NODE_FILE)
            return -1;
        if (vfs_open_file(rpath, &ref, &size) != 0)
            return -1;
        if (vfs_truncate(ref, 0) != 0)
            return -1;
        node.size = 0;
    }

    if (node.type != VFS_NODE_FILE &&
        node.type != VFS_NODE_DIR &&
        node.type != VFS_NODE_BLOCKDEV &&
        node.type != VFS_NODE_PROCFILE &&
        node.type != VFS_NODE_TTY &&
        node.type != VFS_NODE_CHARDEV)
        return -1;

    fd = fd_install_vfs_node(cur, &node, writable, append);
    if (fd < 0)
        return -1;
    if (node.type == VFS_NODE_DIR) {
        k_strncpy(cur->open_files[fd].u.dir.path, rpath,
                  sizeof(cur->open_files[fd].u.dir.path) - 1);
        cur->open_files[fd].u.dir
            .path[sizeof(cur->open_files[fd].u.dir.path) - 1] = '\0';
    }
    return fd;
}

static char *copy_user_string_alloc(process_t *proc, uint32_t user_ptr,
                                    uint32_t max_len)
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

static int resolve_user_path(process_t *proc, uint32_t user_ptr,
                             char *resolved, uint32_t resolved_sz)
{
    char *raw;

    if (!proc || !resolved || resolved_sz == 0 || user_ptr == 0)
        return -1;

    raw = copy_user_string_alloc(proc, user_ptr, resolved_sz);
    if (!raw)
        return -1;

    kcwd_resolve(proc->cwd, raw, resolved, (int)resolved_sz);
    kfree(raw);
    return 0;
}

static int resolve_user_path_at(process_t *proc, uint32_t dirfd,
                                uint32_t user_ptr,
                                char *resolved, uint32_t resolved_sz)
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
        kcwd_resolve(proc->cwd, raw, resolved, (int)resolved_sz);
    } else {
        file_handle_t *fh;

        if (dirfd >= MAX_FDS) {
            kfree(raw);
            return -1;
        }
        fh = &proc->open_files[dirfd];
        if (fh->type != FD_TYPE_DIR) {
            kfree(raw);
            return -1;
        }
        kcwd_resolve(fh->u.dir.path, raw, resolved, (int)resolved_sz);
    }

    kfree(raw);
    return 0;
}

static uint32_t syscall_execve(uint32_t user_path, uint32_t user_argv,
                               uint32_t user_envp)
{
    const char *kargv[PROCESS_ARGV_MAX_COUNT + 1];
    char *kstrs = (char *)kmalloc(PROCESS_ARGV_MAX_BYTES);
    process_t *exec_cur = sched_current();
    int kargc = 0;
    const char *kenvp[PROCESS_ENV_MAX_COUNT + 1];
    char *kenvstrs;
    int kenvc = 0;
    char *exec_rpath;
    vfs_file_ref_t exec_ref;
    uint32_t sz;
    process_t *new_proc;

    if (!kstrs)
        return (uint32_t)-1;
    kenvstrs = (char *)kmalloc(PROCESS_ENV_MAX_BYTES);
    if (!kenvstrs) {
        kfree(kstrs);
        return (uint32_t)-1;
    }

    if (!exec_cur) {
        kfree(kenvstrs);
        kfree(kstrs);
        return (uint32_t)-1;
    }

    if (snapshot_user_string_vector(exec_cur, user_argv,
                                    PROCESS_ARGV_MAX_COUNT,
                                    PROCESS_ARGV_MAX_BYTES,
                                    kargv, kstrs, &kargc) != 0) {
        klog("EXEC", "bad argv");
        kfree(kenvstrs);
        kfree(kstrs);
        return (uint32_t)-1;
    }
    if (snapshot_user_string_vector(exec_cur, user_envp,
                                    PROCESS_ENV_MAX_COUNT,
                                    PROCESS_ENV_MAX_BYTES,
                                    kenvp, kenvstrs, &kenvc) != 0) {
        klog("EXEC", "bad envp");
        kfree(kenvstrs);
        kfree(kstrs);
        return (uint32_t)-1;
    }

    exec_rpath = (char *)kmalloc(4096);
    if (!exec_rpath) {
        kfree(kenvstrs);
        kfree(kstrs);
        return (uint32_t)-1;
    }
    if (resolve_user_path(exec_cur, user_path, exec_rpath, 4096) != 0) {
        kfree(exec_rpath);
        kfree(kenvstrs);
        kfree(kstrs);
        return (uint32_t)-1;
    }
    if (vfs_open_file(exec_rpath, &exec_ref, &sz) != 0) {
        klog("EXEC", "file not found");
        kfree(exec_rpath);
        kfree(kenvstrs);
        kfree(kstrs);
        return (uint32_t)-1;
    }
    kfree(exec_rpath);

    new_proc = (process_t *)kmalloc(sizeof(process_t));
    if (!new_proc) {
        kfree(kenvstrs);
        kfree(kstrs);
        return (uint32_t)-1;
    }

    if (process_create_file(new_proc, exec_ref, kargv, kargc, kenvp, kenvc, 0) != 0) {
        klog("EXEC", "process_create failed");
        kfree(new_proc);
        kfree(kenvstrs);
        kfree(kstrs);
        return (uint32_t)-1;
    }
    kfree(kenvstrs);
    kfree(kstrs);

    new_proc->pid = exec_cur->pid;
    new_proc->parent_pid = exec_cur->parent_pid;
    new_proc->pgid = exec_cur->pgid;
    new_proc->sid = exec_cur->sid;
    new_proc->tty_id = exec_cur->tty_id;
    new_proc->state = PROC_RUNNING;
    new_proc->wait_queue = 0;
    new_proc->wait_next = 0;
    new_proc->wait_deadline = 0;
    new_proc->wait_deadline_set = 0;
    new_proc->exit_status = 0;
    new_proc->state_waiters = exec_cur->state_waiters;
    k_memcpy(new_proc->cwd, exec_cur->cwd, sizeof(new_proc->cwd));
    for (unsigned i = 0; i < MAX_FDS; i++)
        new_proc->open_files[i] = exec_cur->open_files[i];

    new_proc->sig_pending = exec_cur->sig_pending;
    new_proc->sig_blocked = exec_cur->sig_blocked;
    for (int i = 0; i < NSIG; i++) {
        new_proc->sig_handlers[i] =
            (exec_cur->sig_handlers[i] == SIG_IGN) ? SIG_IGN : SIG_DFL;
    }
    new_proc->crash.valid = 0;
    new_proc->crash.signum = 0;
    new_proc->crash.cr2 = 0;

    klog_hex("EXEC", "new_proc brk", new_proc->brk);
    klog_hex("EXEC", "new_proc heap_start", new_proc->heap_start);
    process_build_exec_frame(new_proc, exec_cur->pd_phys,
                             exec_cur->kstack_bottom);
    sched_exec_current(new_proc);
    return 0;
}

/*
 * fd_close_one: close a single fd slot.
 *
 * - DUFS files: flush the inode if writable.
 * - Pipe ends: decrement the appropriate refcount; free the pipe buffer
 *   once both read_open and write_open reach zero.
 */
static void fd_close_one(process_t *proc, unsigned fd)
{
    file_handle_t *fh = &proc->open_files[fd];

    if (fh->type == FD_TYPE_FILE && fh->writable)
        vfs_flush(fh->u.file.ref);

    if (fh->type == FD_TYPE_PIPE_READ) {
        pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
        if (pb) {
            if (pb->read_open > 0) pb->read_open--;
            sched_wake_all(&pb->waiters);
            if (pb->read_open == 0 && pb->write_open == 0)
                pipe_free((int)fh->u.pipe.pipe_idx);
        }
    }

    if (fh->type == FD_TYPE_PIPE_WRITE) {
        pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
        if (pb) {
            if (pb->write_open > 0) pb->write_open--;
            sched_wake_all(&pb->waiters);
            if (pb->read_open == 0 && pb->write_open == 0)
                pipe_free((int)fh->u.pipe.pipe_idx);
        }
    }

    fh->type     = FD_TYPE_NONE;
    fh->writable = 0;
    fh->append   = 0;
}


static uint32_t SYSCALL_NOINLINE syscall_case_write(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = fd
         * ecx = pointer to byte buffer in user virtual space
         * edx = number of bytes to write
         *
         * Dispatches on fd type:
         *   FD_TYPE_STDOUT  → active desktop shell or legacy VGA console
         *   FD_TYPE_FILE    → fs_write() into the DUFS inode
         *
         * Returns the number of bytes written, or -1 on error.
         */
        return syscall_write_fd(ebx, ecx, edx);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_writev(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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
        if (cur->open_files[ebx].type == FD_TYPE_NONE)
            return (uint32_t)-1;

        for (uint32_t i = 0; i < edx; i++) {
            uint32_t iov[2];
            uint32_t n;

            if (uaccess_copy_from_user(cur, iov, ecx + i * sizeof(iov),
                                       sizeof(iov)) != 0)
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

static uint32_t SYSCALL_NOINLINE syscall_case_readv(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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
        if (cur->open_files[ebx].type == FD_TYPE_NONE)
            return (uint32_t)-1;

        for (uint32_t i = 0; i < edx; i++) {
            uint32_t iov[2];
            uint32_t n;

            if (uaccess_copy_from_user(cur, iov, ecx + i * sizeof(iov),
                                       sizeof(iov)) != 0)
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

static uint32_t SYSCALL_NOINLINE syscall_case_read(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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

static uint32_t SYSCALL_NOINLINE syscall_case_sendfile64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        /*
         * Linux i386 sendfile64(out_fd, in_fd, offset64 *, count).
         * The BusyBox fast path uses this to copy regular files to stdout.
         */
        return syscall_sendfile64(sched_current(), ebx, ecx, edx, esi);
}

static uint32_t SYSCALL_NOINLINE syscall_case_open(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 open(path, flags, mode).  Drunix only stores simple DUFS
         * metadata, but the access mode and create/truncate bits must follow
         * Linux enough for static BusyBox file utilities.
         */
        process_t *cur = sched_current();
        uint32_t flags = ecx;
        int fd;
        if (!cur)
            return (uint32_t)-1;

        char *rpath = (char *)kmalloc(4096);
        if (!rpath) return (uint32_t)-1;
        if (resolve_user_path(cur, ebx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }

        fd = syscall_open_resolved_path(cur, rpath, flags);
        kfree(rpath);
        return (uint32_t)fd;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_openat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        uint32_t flags = edx;
        char *rpath;
        int fd;

        (void)eax;
        (void)esi;
        (void)edi;
        (void)ebp;
        if (!cur)
            return (uint32_t)-1;

        rpath = (char *)kmalloc(4096);
        if (!rpath)
            return (uint32_t)-1;
        if (resolve_user_path_at(cur, ebx, ecx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }

        fd = syscall_open_resolved_path(cur, rpath, flags);
        kfree(rpath);
        return (uint32_t)fd;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_close(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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

        if (cur->open_files[ebx].type == FD_TYPE_NONE)
            return (uint32_t)-1;

        fd_close_one(cur, ebx);
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_access(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 access(path, mode).  DUFS does not carry Unix
         * permissions yet; existence is enough and uid 0 can access it.
         */
        process_t *cur = sched_current();
        int rc = linux_path_exists(cur, ebx);
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_faccessat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        int rc;

        (void)eax;
        (void)esi;
        (void)edi;
        (void)ebp;
        if ((edx & ~7u) != 0)
            return (uint32_t)-22;
        rc = linux_path_exists_at(cur, ebx, ecx);
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_fchmodat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        int rc;

        (void)eax;
        (void)edx;
        (void)esi;
        (void)edi;
        (void)ebp;
        rc = linux_path_exists_at(cur, ebx, ecx);
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_fchownat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        int rc;

        (void)eax;
        (void)edx;
        (void)esi;
        (void)ebp;
        if ((edi & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0)
            return (uint32_t)-22;
        if (ecx == 0) {
            if ((edi & LINUX_AT_EMPTY_PATH) == 0 ||
                !cur || ebx >= MAX_FDS ||
                cur->open_files[ebx].type == FD_TYPE_NONE)
                return (uint32_t)-1;
            return 0;
        }
        rc = linux_path_exists_at(cur, ebx, ecx);
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_futimesat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        int rc;

        (void)eax;
        (void)edx;
        (void)esi;
        (void)edi;
        (void)ebp;
        if (ecx == 0) {
            if (!cur || ebx >= MAX_FDS ||
                cur->open_files[ebx].type == FD_TYPE_NONE)
                return (uint32_t)-1;
            return 0;
        }
        rc = linux_path_exists_at(cur, ebx, ecx);
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_chmod(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 chmod(path, mode).  Mode persistence is not represented
         * in DUFS yet, so accept chmod on existing paths as a compatibility
         * no-op.
         */
        process_t *cur = sched_current();
        int rc = linux_path_exists(cur, ebx);
        (void)ecx;
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_lchown_chown32(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux chown/lchown compatibility.  Drunix currently runs as uid 0
         * with no per-inode uid/gid fields, so owner changes on existing paths
         * are accepted as no-ops.
         */
        process_t *cur = sched_current();
        int rc = linux_path_exists(cur, ebx);
        (void)ecx;
        (void)edx;
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_sync(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        return 0;
}

static uint32_t SYSCALL_NOINLINE syscall_case_umask(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        uint32_t old;

        if (!cur)
            return (uint32_t)-1;
        old = cur->umask;
        cur->umask = ebx & 0777u;
        return old;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_setsid(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        if (!cur)
            return (uint32_t)-1;
        cur->sid = cur->pid;
        cur->pgid = cur->pid;
        return cur->sid;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_getsid(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        process_t *target;
        uint32_t pid = ebx;

        if (!cur)
            return (uint32_t)-1;
        if (pid == 0 || pid == cur->pid)
            return cur->sid;
        target = sched_find_pid(pid);
        return target ? target->sid : (uint32_t)-3;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_readlink(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        char *rpath;
        char *target;
        int rc;

        (void)eax;
        (void)esi;
        (void)edi;
        (void)ebp;
        if (!cur || ecx == 0 || edx == 0)
            return (uint32_t)-22;
        rpath = (char *)kmalloc(4096);
        target = (char *)kmalloc(edx);
        if (!rpath || !target) {
            if (rpath) kfree(rpath);
            if (target) kfree(target);
            return (uint32_t)-1;
        }
        if (resolve_user_path(cur, ebx, rpath, 4096) != 0) {
            kfree(rpath);
            kfree(target);
            return (uint32_t)-1;
        }
        rc = vfs_readlink(rpath, target, edx);
        if (rc > 0 && uaccess_copy_to_user(cur, ecx, target, (uint32_t)rc) != 0)
            rc = -1;
        kfree(rpath);
        kfree(target);
        return (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_readlinkat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        char *rpath;
        char *target;
        int rc;

        (void)eax;
        (void)edi;
        (void)ebp;
        if (!cur || edx == 0 || esi == 0)
            return (uint32_t)-22;
        rpath = (char *)kmalloc(4096);
        target = (char *)kmalloc(esi);
        if (!rpath || !target) {
            if (rpath) kfree(rpath);
            if (target) kfree(target);
            return (uint32_t)-1;
        }
        if (resolve_user_path_at(cur, ebx, ecx, rpath, 4096) != 0) {
            kfree(rpath);
            kfree(target);
            return (uint32_t)-1;
        }
        rc = vfs_readlink(rpath, target, esi);
        if (rc > 0 && uaccess_copy_to_user(cur, edx, target, (uint32_t)rc) != 0)
            rc = -1;
        kfree(rpath);
        kfree(target);
        return (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_getpriority(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        (void)ebx;
        (void)ecx;
        return 0;
}

static uint32_t SYSCALL_NOINLINE syscall_case_setpriority(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        (void)ebx;
        (void)ecx;
        (void)edx;
        return 0;
}

static uint32_t SYSCALL_NOINLINE syscall_case_sysinfo(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        return syscall_sysinfo(ebx);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_truncate64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        uint64_t length = (uint64_t)ecx | ((uint64_t)edx << 32);
        int rc = linux_truncate_path(cur, ebx, length);
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_ftruncate64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        uint64_t length = (uint64_t)ecx | ((uint64_t)edx << 32);
        int rc = linux_truncate_fd(cur, ebx, length);
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_sched_getaffinity(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        uint8_t mask[4];

        if (!cur || edx == 0 || ecx < sizeof(mask))
            return (uint32_t)-22;
        (void)ebx;
        k_memset(mask, 0, sizeof(mask));
        mask[0] = 1u;
        if (uaccess_copy_to_user(cur, edx, mask, sizeof(mask)) != 0)
            return (uint32_t)-1;
        return sizeof(mask);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_utimensat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        linux_fd_stat_t meta;
        int rc;

        if (!cur)
            return (uint32_t)-1;
        (void)edx;
        if ((esi & ~LINUX_AT_SYMLINK_NOFOLLOW) != 0)
            return (uint32_t)-22;
        if (ecx == 0) {
            if (ebx >= MAX_FDS || cur->open_files[ebx].type == FD_TYPE_NONE)
                return (uint32_t)-1;
            return 0;
        }
        rc = linux_path_stat_metadata_at(cur, ebx, ecx, &meta);
        return rc == 0 ? 0 : (uint32_t)-2;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_poll(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 poll(struct pollfd *fds, nfds_t nfds, int timeout).
         * Return immediately with readiness for the simple fd types Drunix
         * supports.  This is enough for BusyBox terminal/stdout probes.
         */
        process_t *cur = sched_current();
        uint32_t ready = 0;

        if (!cur || ebx == 0 || ecx > 1024u)
            return (uint32_t)-1;

        for (uint32_t i = 0; i < ecx; i++) {
            uint8_t pfd[8];
            int32_t fd;
            uint32_t events;
            uint32_t revents;

            if (uaccess_copy_from_user(cur, pfd, ebx + i * sizeof(pfd),
                                       sizeof(pfd)) != 0)
                return (uint32_t)-1;
            fd = (int32_t)linux_get_u32(pfd, 0u);
            events = linux_get_u16(pfd, 4u);
            revents = linux_poll_revents(cur, fd, events);
            linux_put_u16(pfd, 6u, revents);
            if (uaccess_copy_to_user(cur, ebx + i * sizeof(pfd), pfd,
                                     sizeof(pfd)) != 0)
                return (uint32_t)-1;
            if (revents)
                ready++;
        }
        (void)edx;
        return ready;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_ioctl(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 ioctl(fd, request, argp).  Implement the terminal probes
         * used by static musl/BusyBox and fail unsupported requests cleanly.
         */
        return syscall_ioctl(ebx, ecx, edx);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_fcntl64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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
        if (cur->open_files[ebx].type == FD_TYPE_NONE)
            return (uint32_t)-1;

        switch (ecx) {
        case LINUX_F_DUPFD:
        case LINUX_F_DUPFD_CLOEXEC:
            dup_fd = fd_duplicate_from(cur, ebx, edx);
            return dup_fd < 0 ? (uint32_t)-1 : (uint32_t)dup_fd;
        case LINUX_F_GETFD:
            return 0;
        case LINUX_F_SETFD:
            return 0;
        case LINUX_F_GETFL:
            return linux_fd_status_flags(&cur->open_files[ebx]);
        case LINUX_F_SETFL:
            cur->open_files[ebx].append =
                (cur->open_files[ebx].type == FD_TYPE_FILE &&
                 (edx & LINUX_O_APPEND)) ? 1u : 0u;
            return 0;
        default:
            return (uint32_t)-1;
        }
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_execve(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pointer to null-terminated filename in user space
         * ecx = pointer to a user-space char *[] array (or 0 for no argv)
         * edx = pointer to a user-space envp[] array (or 0 for no env)
         *
         * Replace the calling process in-place.  PID, parent linkage,
         * process-group/session membership, cwd, and the open-fd table are
         * preserved; the user address space, user stack, heap, entry point,
         * and process metadata derived from argv are rebuilt from the new ELF.
         *
         * On success this syscall does not return to the old image.
         */

        return syscall_execve(ebx, ecx, edx);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_creat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pointer to null-terminated filename in user space.
         *
         * Creates a new file (or truncates an existing one) in DUFS, finds
         * a free slot in the fd table, and installs a writable FD_TYPE_FILE
         * handle.  Returns the fd on success, or -1 on error.
         */
        process_t *cur = sched_current();
        if (!cur)
            return (uint32_t)-1;

        char *rpath = (char *)kmalloc(4096);
        if (!rpath) return (uint32_t)-1;
        if (resolve_user_path(cur, ebx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }

        int ino_c = vfs_create(rpath);
        vfs_file_ref_t ref_c;
        uint32_t size_c = 0;
        if (ino_c < 0) {
            kfree(rpath);
            klog("CREATE", "vfs_create failed");
            return (uint32_t)-1;
        }
        if (vfs_open_file(rpath, &ref_c, &size_c) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        kfree(rpath);

        int fd = fd_alloc(cur);
        if (fd < 0) {
            klog("CREATE", "fd table full");
            return (uint32_t)-1;
        }
        cur->open_files[fd].type             = FD_TYPE_FILE;
        cur->open_files[fd].writable         = 1;
        cur->open_files[fd].append           = 0;
        cur->open_files[fd].u.file.ref        = ref_c;
        cur->open_files[fd].u.file.inode_num = (uint32_t)ino_c;
        cur->open_files[fd].u.file.size      = size_c;
        cur->open_files[fd].u.file.offset    = 0;
        return (uint32_t)fd;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_unlink(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pointer to null-terminated filename in user space.
         *
         * Deletes the named file: frees its bitmap sectors, clears its
         * directory entry, and flushes both to disk.
         * Returns 0 on success, -1 on error (file not found or I/O error).
         */
        process_t *cur = sched_current();
        char *rpath = (char *)kmalloc(4096);
        if (!rpath) return (uint32_t)-1;
        if (!cur || resolve_user_path(cur, ebx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        uint32_t ret = (uint32_t)vfs_unlink(rpath);
        kfree(rpath);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_unlinkat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        char *rpath;
        uint32_t ret;

        (void)eax;
        (void)esi;
        (void)edi;
        (void)ebp;
        if ((edx & ~LINUX_AT_REMOVEDIR) != 0)
            return (uint32_t)-22;
        rpath = (char *)kmalloc(4096);
        if (!rpath)
            return (uint32_t)-1;
        if (!cur || resolve_user_path_at(cur, ebx, ecx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        ret = (edx & LINUX_AT_REMOVEDIR) ?
            (uint32_t)vfs_rmdir(rpath) : (uint32_t)vfs_unlink(rpath);
        kfree(rpath);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_fork_vfork(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * fork() takes no arguments.  vfork() is implemented as fork() for
         * compatibility; copy-on-write keeps the common fork+exec path cheap.
         *
         * Creates a child process that is an exact copy of the caller's
         * address space, registers, and open-file table.  The child's
         * fork() returns 0; the parent's fork() returns the child's PID.
         * Both processes resume execution at the instruction after INT 0x80.
         *
         * Implemented via copy-on-write user mappings, with the live user
         * stack eagerly copied so pre-exec child activity cannot mutate the
         * parent's active stack pages. See process_fork() in process.c and
         * paging_clone_user_space() in paging.c.
         *
         * Returns child PID in parent, 0 in child, (uint32_t)-1 on error.
         */
        process_t *parent = sched_current();
        if (!parent) return (uint32_t)-1;

        /* Allocate child descriptor on the heap: process_t is ~5 KB. */
        process_t *child = (process_t *)kmalloc(sizeof(process_t));
        if (!child) {
            klog_uint("FORK", "heap free bytes", kheap_free_bytes());
            return (uint32_t)-1;
        }

        if (process_fork(child, parent) != 0) {
            klog("FORK", "process_fork failed");
            kfree(child);
            return (uint32_t)-1;
        }

        int cpid = sched_add(child);
        kfree(child);  /* sched_add copies by value into proc_table[] */
        if (cpid < 0) {
            klog("FORK", "process table full");
            return (uint32_t)-1;
        }
        return (uint32_t)cpid;  /* parent gets child PID; child frame already has EAX=0 */
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_drunix_clear(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        if (desktop_is_active() && desktop_clear_console(desktop_global()))
            return 0;
        clear_screen();
        return 0;
}

static uint32_t SYSCALL_NOINLINE syscall_case_drunix_scroll_up(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        if (desktop_is_active() &&
            desktop_scroll_console(desktop_global(),
                                   syscall_scroll_count(ebx)))
            return 0;
        scroll_up(syscall_scroll_count(ebx));
        return 0;
}

static uint32_t SYSCALL_NOINLINE syscall_case_drunix_scroll_down(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        if (desktop_is_active() &&
            desktop_scroll_console(desktop_global(),
                                   -syscall_scroll_count(ebx)))
            return 0;
        scroll_down(syscall_scroll_count(ebx));
        return 0;
}

static uint32_t SYSCALL_NOINLINE syscall_case_yield(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        /* Voluntarily give up the rest of the current timeslice. */
        schedule();
        return 0;
}

static uint32_t SYSCALL_NOINLINE syscall_case_nanosleep(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = const struct timespec *req, ecx = struct timespec *rem.
         *
         * Blocks the caller until the deadline expires or a signal wakes it.
         * Returns 0 on full sleep, or -1 after copying remaining time when
         * interrupted by a signal.
         */
        return syscall_nanosleep(ebx, ecx);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_newselect(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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

        (void)eax;
        (void)edi;
        (void)ebp;

        if (!cur || ebx > 32u)
            return (uint32_t)-1;
        if (ecx && uaccess_copy_from_user(cur, &in_read, ecx,
                                          sizeof(in_read)) != 0)
            return (uint32_t)-1;
        if (edx && uaccess_copy_from_user(cur, &in_write, edx,
                                          sizeof(in_write)) != 0)
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

        if (ecx && uaccess_copy_to_user(cur, ecx, &out_read,
                                        sizeof(out_read)) != 0)
            return (uint32_t)-1;
        if (edx && uaccess_copy_to_user(cur, edx, &out_write,
                                        sizeof(out_write)) != 0)
            return (uint32_t)-1;
        if (esi) {
            uint32_t zero = 0;
            if (uaccess_copy_to_user(cur, esi, &zero, sizeof(zero)) != 0)
                return (uint32_t)-1;
        }
        return ready;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_mkdir(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pointer to null-terminated directory name in user space.
         * Creates a directory. Path is resolved relative to process cwd.
         * Returns 0 on success, -1 on error.
         */
        process_t *cur = sched_current();
        char *rpath = (char *)kmalloc(4096);
        if (!rpath) return (uint32_t)-1;
        if (!cur || resolve_user_path(cur, ebx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        uint32_t ret = (uint32_t)vfs_mkdir(rpath);
        kfree(rpath);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_mkdirat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        char *rpath = (char *)kmalloc(4096);
        uint32_t ret;

        (void)eax;
        (void)edx;
        (void)esi;
        (void)edi;
        (void)ebp;
        if (!rpath)
            return (uint32_t)-1;
        if (!cur || resolve_user_path_at(cur, ebx, ecx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        ret = (uint32_t)vfs_mkdir(rpath);
        kfree(rpath);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_rmdir(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pointer to null-terminated directory name in user space.
         * Removes an empty directory. Path is resolved relative to process cwd.
         * Returns 0 on success, -1 if not found, not empty, or on error.
         */
        process_t *cur = sched_current();
        char *rpath = (char *)kmalloc(4096);
        if (!rpath) return (uint32_t)-1;
        if (!cur || resolve_user_path(cur, ebx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        uint32_t ret = (uint32_t)vfs_rmdir(rpath);
        kfree(rpath);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_chdir(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pointer to null-terminated path in user space.
         *
         * Changes the calling process's current working directory.
         * Special forms handled before VFS validation:
         *   NULL / "" / "/"  → move to root (cwd = "").
         *   ".."             → strip the last component from cwd.
         * All other paths are resolved relative to the current cwd, then
         * validated as an existing directory via vfs_stat before being stored.
         * Returns 0 on success, -1 if the path is not a valid directory.
         */
        process_t *cur = sched_current();
        if (!cur) return (uint32_t)-1;

        char *path = 0;

        if (ebx != 0) {
            path = copy_user_string_alloc(cur, ebx, 4096);
            if (!path)
                return (uint32_t)-1;
        }

        /* Go to root. */
        if (!path || path[0] == '\0' ||
            (path[0] == '/' && path[1] == '\0')) {
            cur->cwd[0] = '\0';
            kfree(path);
            return 0;
        }

        /* Go up one level. */
        if (path[0] == '.' && path[1] == '.' && path[2] == '\0') {
            if (cur->cwd[0] == '\0') { kfree(path); return 0; } /* already at root */
            int i = 0;
            while (cur->cwd[i]) i++;
            while (i > 0 && cur->cwd[i - 1] != '/') i--;
            if (i > 0) i--; /* trim trailing slash */
            cur->cwd[i] = '\0';
            kfree(path);
            return 0;
        }

        /* Resolve path relative to cwd. */
        char *resolved = (char *)kmalloc(4096);
        if (!resolved) {
            kfree(path);
            return (uint32_t)-1;
        }
        kcwd_resolve(cur->cwd, path, resolved, 4096);
        kfree(path);

        /* Trim any trailing slash. */
        int rlen = 0;
        while (resolved[rlen]) rlen++;
        if (rlen > 0 && resolved[rlen - 1] == '/') resolved[--rlen] = '\0';

        /* Empty after trimming → root. */
        if (resolved[0] == '\0') { cur->cwd[0] = '\0'; kfree(resolved); return 0; }

        /* Validate: must exist and be a directory (type == 2). */
        vfs_stat_t st;
        if (vfs_stat(resolved, &st) != 0 || st.type != 2) {
            klog("CHDIR", "not a directory");
            kfree(resolved);
            return (uint32_t)-1;
        }

        /* Commit the new cwd. */
        k_strncpy(cur->cwd, resolved, 4095);
        cur->cwd[4095] = '\0';
        kfree(resolved);
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_getcwd(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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
        path = (char *)kmalloc(4096);
        if (!path)
            return (uint32_t)-1;
        if (cur->cwd[0] == '\0')
            k_strncpy(path, "/", 4095u);
        else
            k_snprintf(path, 4096u, "/%s", cur->cwd);
        path[4095] = '\0';

        len = k_strnlen(path, 4095u);
        if (len + 1u > ecx) {
            kfree(path);
            return (uint32_t)-1;
        }
        if (uaccess_copy_to_user(cur, ebx, path, len + 1u) != 0) {
            kfree(path);
            return (uint32_t)-1;
        }
        kfree(path);
        return len + 1u;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_rename(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pointer to null-terminated old path in user space.
         * ecx = pointer to null-terminated new path in user space.
         *
         * Both paths are resolved relative to the process cwd.
         * Renames or moves a file or directory by updating its directory-
         * entry name and parent fields in place.  No file data is copied.
         * If newpath names an existing file it is atomically replaced.
         * Returns 0 on success, -1 on error.
         */
        process_t *cur = sched_current();
        char *rold = (char *)kmalloc(4096);
        if (!rold) return (uint32_t)-1;
        char *rnew = (char *)kmalloc(4096);
        if (!rnew) { kfree(rold); return (uint32_t)-1; }
        if (!cur ||
            resolve_user_path(cur, ebx, rold, 4096) != 0 ||
            resolve_user_path(cur, ecx, rnew, 4096) != 0) {
            kfree(rold);
            kfree(rnew);
            return (uint32_t)-1;
        }
        uint32_t ret = (uint32_t)vfs_rename(rold, rnew);
        kfree(rold);
        kfree(rnew);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_renameat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        char *rold = (char *)kmalloc(4096);
        char *rnew;
        uint32_t ret;

        (void)eax;
        (void)edi;
        (void)ebp;
        if (!rold)
            return (uint32_t)-1;
        rnew = (char *)kmalloc(4096);
        if (!rnew) {
            kfree(rold);
            return (uint32_t)-1;
        }
        if (!cur ||
            resolve_user_path_at(cur, ebx, ecx, rold, 4096) != 0 ||
            resolve_user_path_at(cur, edx, esi, rnew, 4096) != 0) {
            kfree(rold);
            kfree(rnew);
            return (uint32_t)-1;
        }
        ret = (uint32_t)vfs_rename(rold, rnew);
        kfree(rold);
        kfree(rnew);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_linkat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        char *oldpath;
        char *newpath;
        uint32_t ret;

        (void)eax;
        (void)ebp;
        if ((edi & ~LINUX_AT_SYMLINK_FOLLOW) != 0)
            return (uint32_t)-22;
        oldpath = (char *)kmalloc(4096);
        newpath = (char *)kmalloc(4096);
        if (!oldpath || !newpath) {
            if (oldpath) kfree(oldpath);
            if (newpath) kfree(newpath);
            return (uint32_t)-1;
        }
        if (!cur ||
            resolve_user_path_at(cur, ebx, ecx, oldpath, 4096) != 0 ||
            resolve_user_path_at(cur, edx, esi, newpath, 4096) != 0) {
            kfree(oldpath);
            kfree(newpath);
            return (uint32_t)-1;
        }
        ret = (uint32_t)vfs_link(oldpath, newpath,
                                 (edi & LINUX_AT_SYMLINK_FOLLOW) != 0);
        kfree(oldpath);
        kfree(newpath);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_symlinkat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        char *target;
        char *newpath;
        uint32_t ret;

        (void)eax;
        (void)esi;
        (void)edi;
        (void)ebp;
        if (!cur)
            return (uint32_t)-1;
        target = copy_user_string_alloc(cur, ebx, 4096);
        newpath = (char *)kmalloc(4096);
        if (!target || !newpath) {
            if (target) kfree(target);
            if (newpath) kfree(newpath);
            return (uint32_t)-1;
        }
        if (target[0] == '\0' ||
            resolve_user_path_at(cur, ecx, edx, newpath, 4096) != 0) {
            kfree(target);
            kfree(newpath);
            return (uint32_t)-1;
        }
        ret = (uint32_t)vfs_symlink(target, newpath);
        kfree(target);
        kfree(newpath);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_getdents(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 getdents(fd, dirp, count).  The older Drunix path-based
         * directory listing ABI is kept as SYS_DRUNIX_GETDENTS_PATH.
         */
        process_t *cur = sched_current();

        if (!cur || ebx >= MAX_FDS)
            return (uint32_t)-1;
        return (uint32_t)linux_fill_getdents(cur, &cur->open_files[ebx],
                                             ecx, edx);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_drunix_getdents_path(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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
        char *rpath = (char *)kmalloc(4096);
        char *kbuf;
        if (!rpath) return (uint32_t)-1;
        if (!cur) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        if (ebx == 0) {
            /* NULL → list the process cwd (empty string lists root). */
            k_strncpy(rpath, cur->cwd, 4095);
            rpath[4095] = '\0';
            upath = rpath;
        } else {
            upath = copy_user_string_alloc(cur, ebx, 4096);
            if (!upath) {
                kfree(rpath);
                return (uint32_t)-1;
            }
            kcwd_resolve(cur->cwd, upath, rpath, 4096);
            kfree(upath);
            upath = rpath;
        }
        kbuf = (char *)kmalloc(edx ? edx : 1);
        if (!kbuf) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        uint32_t ret = (uint32_t)vfs_getdents(*upath ? upath : (const char *)0,
                                              kbuf, edx);
        if ((int32_t)ret >= 0 &&
            uaccess_copy_to_user(cur, ecx, kbuf, ret) != 0)
            ret = (uint32_t)-1;
        kfree(kbuf);
        kfree(rpath);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_getdents64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 getdents64(fd, dirp, count).  This is used by static
         * BusyBox/musl for directory iteration.
         */
        process_t *cur = sched_current();

        if (!cur || ebx >= MAX_FDS)
            return (uint32_t)-1;
        return (uint32_t)linux_fill_getdents64(cur, &cur->open_files[ebx],
                                               ecx, edx);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_stat(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pointer to null-terminated path in user space.
         * ecx = pointer to vfs_stat_t in user space (kernel writes result here).
         *
         * Path is resolved relative to the process cwd.
         * Works for both regular files and directories.
         * Returns 0 on success, (uint32_t)-1 if the path is not found.
         */
        process_t *cur = sched_current();
        char *rpath = (char *)kmalloc(4096);
        if (!rpath) return (uint32_t)-1;
        if (!cur || resolve_user_path(cur, ebx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        vfs_stat_t st;
        uint32_t ret = (uint32_t)vfs_stat(rpath, &st);
        if ((int32_t)ret >= 0 &&
            uaccess_copy_to_user(cur, ecx, &st, sizeof(st)) != 0)
            ret = (uint32_t)-1;
        kfree(rpath);
        return ret;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_stat64_lstat64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        /*
         * Linux i386 stat64/lstat64(path, struct stat64 *).
         * stat64 follows symlinks; lstat64 reports the link inode itself.
         */
        return syscall_stat64_path_common(ebx, ecx, eax == SYS_LSTAT64);
}

static uint32_t SYSCALL_NOINLINE syscall_case_fstatat64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        uint8_t st64[144];
        linux_fd_stat_t meta;
        char first;

        (void)eax;
        (void)edi;
        (void)ebp;
        if (!cur || ecx == 0 || edx == 0)
            return (uint32_t)-1;
        if ((esi & ~(LINUX_AT_SYMLINK_NOFOLLOW |
                     LINUX_AT_NO_AUTOMOUNT |
                     LINUX_AT_EMPTY_PATH)) != 0)
            return (uint32_t)-1;
        if (uaccess_copy_from_user(cur, &first, ecx, sizeof(first)) != 0)
            return (uint32_t)-1;
        if (first == '\0') {
            if ((esi & LINUX_AT_EMPTY_PATH) == 0)
                return (uint32_t)-2;
            if (linux_fd_stat_metadata(cur, ebx, &meta) != 0)
                return (uint32_t)-1;
        } else {
            if (linux_path_stat_metadata_at_flags(cur, ebx, ecx, &meta,
                                                  (esi & LINUX_AT_SYMLINK_NOFOLLOW) != 0) != 0)
                return (uint32_t)-2;
        }

    linux_fill_stat64(st64, meta.mode, meta.nlink, meta.size,
                      meta.mtime, meta.rdev_major, meta.rdev_minor,
                      meta.ino);
        if (uaccess_copy_to_user(cur, edx, st64, sizeof(st64)) != 0)
            return (uint32_t)-1;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_fstat64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        /*
         * ebx = fd, ecx = struct stat64 *.
         *
         * Linux i386 musl uses fstat64 for stdio and file metadata.  The
         * layout here matches musl's i386 struct stat (144 bytes).
         */
        return syscall_fstat64(ebx, ecx);
}

static uint32_t SYSCALL_NOINLINE syscall_case_statx(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        /*
         * Linux i386 statx:
         *   ebx = dirfd, ecx = path, edx = flags, esi = mask, edi = statx *.
         *
         * musl implements fstat(fd, &st) with statx(fd, "", AT_EMPTY_PATH,
         * ...).  BusyBox ls also uses path-based statx with AT_FDCWD.
         */
        return syscall_statx(ebx, ecx, edx, esi, edi);
}

static uint32_t SYSCALL_NOINLINE syscall_case_statfs64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        int rc = linux_path_exists(cur, ebx);

        if (rc != 0)
            return (uint32_t)rc;
        rc = linux_copy_statfs64(cur, edx, ecx);
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_fstatfs64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        int rc;

        if (!cur || ebx >= MAX_FDS || cur->open_files[ebx].type == FD_TYPE_NONE)
            return (uint32_t)-1;
        rc = linux_copy_statfs64(cur, edx, ecx);
        return rc == 0 ? 0 : (uint32_t)rc;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_clock_gettime(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = clock id (0 = CLOCK_REALTIME, 1 = CLOCK_MONOTONIC).
         * ecx = pointer to struct timespec in user space:
         *       long tv_sec; long tv_nsec;
         *
         * Returns 0 on success or -1 for an unsupported clock / bad pointer.
         */
        process_t *cur = sched_current();
        uint32_t ts[2];
        uint32_t ticks;

        if (!cur || ecx == 0)
            return (uint32_t)-1;
        if (ebx == 0) {
            ts[0] = clock_unix_time();
            ts[1] = 0;
        } else if (ebx == 1) {
            ticks = clock_uptime_ticks();
            ts[0] = ticks / SCHED_HZ;
            ts[1] = (ticks % SCHED_HZ) * (1000000000u / SCHED_HZ);
        } else {
            return (uint32_t)-1;
        }
        if (uaccess_copy_to_user(cur, ecx, ts, sizeof(ts)) != 0)
            return (uint32_t)-1;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_clock_gettime64(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = clock id, ecx = struct __kernel_timespec64 *.
         * Linux time64 ABI uses 64-bit seconds and nanoseconds on i386.
         */
        process_t *cur = sched_current();
        uint32_t ts64[4];
        uint32_t ticks;

        if (!cur || ecx == 0)
            return (uint32_t)-1;
        if (ebx == 0) {
            ts64[0] = clock_unix_time();
            ts64[1] = 0;
            ts64[2] = 0;
            ts64[3] = 0;
        } else if (ebx == 1) {
            ticks = clock_uptime_ticks();
            ts64[0] = ticks / SCHED_HZ;
            ts64[1] = 0;
            ts64[2] = (ticks % SCHED_HZ) * (1000000000u / SCHED_HZ);
            ts64[3] = 0;
        } else {
            return (uint32_t)-1;
        }
        if (uaccess_copy_to_user(cur, ecx, ts64, sizeof(ts64)) != 0)
            return (uint32_t)-1;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_gettimeofday(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = struct timeval32 *, ecx = struct timezone *.
         * The timezone argument is obsolete; Linux still zeros it when given.
         */
        process_t *cur = sched_current();
        uint32_t tv[2];
        uint32_t tz[2] = { 0, 0 };

        if (!cur)
            return (uint32_t)-1;
        if (ebx != 0) {
            tv[0] = clock_unix_time();
            tv[1] = 0;
            if (uaccess_copy_to_user(cur, ebx, tv, sizeof(tv)) != 0)
                return (uint32_t)-1;
        }
        if (ecx != 0 &&
            uaccess_copy_to_user(cur, ecx, tz, sizeof(tz)) != 0)
            return (uint32_t)-1;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_uname(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        /*
         * ebx = struct utsname *.
         * Linux i386 old_utsname fields are 65-byte NUL-terminated strings.
         */
        return syscall_uname(ebx);
}

static uint32_t SYSCALL_NOINLINE syscall_case_set_thread_area(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = struct user_desc *.
         *
         * Static i386 musl uses set_thread_area during startup to install a
         * TLS descriptor and then loads %gs with the returned entry number.
         * Drunix exposes one user TLS slot in the GDT for this compatibility
         * path.
         */
        process_t *cur = sched_current();
        linux_user_desc_t desc;
        uint32_t contents;
        int limit_in_pages;

        if (!cur || ebx == 0)
            return (uint32_t)-1;
        if (uaccess_copy_from_user(cur, &desc, ebx, sizeof(desc)) != 0)
            return (uint32_t)-1;

        if (desc.entry_number != 0xFFFFFFFFu &&
            desc.entry_number != GDT_USER_TLS_ENTRY)
            return (uint32_t)-1;

        contents = (desc.flags >> 1) & 0x3u;
        if (contents != 0)
            return (uint32_t)-1;
        if ((desc.flags & (1u << 3)) != 0)
            return (uint32_t)-1;

        desc.entry_number = GDT_USER_TLS_ENTRY;
        limit_in_pages = (desc.flags & (1u << 4)) != 0;
        if ((desc.flags & (1u << 5)) != 0) {
            cur->user_tls_base = 0;
            cur->user_tls_limit = 0;
            cur->user_tls_limit_in_pages = 0;
            cur->user_tls_present = 0;
        } else {
            cur->user_tls_base = desc.base_addr;
            cur->user_tls_limit = desc.limit;
            cur->user_tls_limit_in_pages = (uint32_t)limit_in_pages;
            cur->user_tls_present = 1;
        }
        process_restore_user_tls(cur);

        if (uaccess_copy_to_user(cur, ebx, &desc, sizeof(desc)) != 0)
            return (uint32_t)-1;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_set_tid_address(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = int *tidptr.
         * A single-threaded runtime only needs the Linux return contract:
         * return the caller's thread id, which is the process id in Drunix.
         */
        process_t *cur = sched_current();

        if (!cur)
            return (uint32_t)-1;
        return cur->pid;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_drunix_modload(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pointer to null-terminated module filename in user space.
         *
         * Looks up the file via the VFS to get a mount-qualified file ref and
         * size, then calls module_load_file() to read the ELF relocatable object
         * from disk, resolve symbols against kernel_exports[], apply
         * relocations, and call the module's module_init() function.
         *
         * Returns 0 on success, or a negative error code from module_load_file():
         *   -1  invalid ELF
         *   -2  relocation error (undefined symbol or unsupported reloc type)
         *   -3  out of kernel heap memory
         *   -4  module_init() returned non-zero
         *   -5  module too large (> MODULE_MAX_SIZE)
         */
        process_t *cur = sched_current();
        char *rpath = (char *)kmalloc(4096);
        if (!rpath) return (uint32_t)-1;
        if (!cur || resolve_user_path(cur, ebx, rpath, 4096) != 0) {
            kfree(rpath);
            return (uint32_t)-1;
        }
        vfs_file_ref_t mod_ref;
        uint32_t sz;
        if (vfs_open_file(rpath, &mod_ref, &sz) != 0) {
            klog("MODLOAD", "file not found");
            kfree(rpath);
            return (uint32_t)-1;
        }
        {
            uint32_t ret = (uint32_t)module_load_file(rpath, mod_ref, sz);
            kfree(rpath);
            return ret;
        }
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_exit_exit_group(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        /*
         * ebx = exit status code.
         * Store the exit code, mark the process as a zombie, and switch
         * away.  schedule() never returns here — the zombie's kernel stack
         * is abandoned and will be freed when the slot is reused.
         */
        sched_set_exit_status(ebx);
        sched_mark_exit();
        schedule();
        __builtin_unreachable();
}

static uint32_t SYSCALL_NOINLINE syscall_case_brk(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = requested new program break, or 0 to query the current brk.
         *
         * Behaviour (matches Linux i386 brk(2) semantics):
         *   - ebx == 0: return the current brk without changing anything.
         *   - ebx < heap_start: refuse (cannot move break below the heap base).
         *   - ebx > USER_HEAP_MAX: refuse (would collide with the user stack).
         *   - Otherwise: update brk immediately. Heap pages are committed on
         *     first touch by the page-fault handler.
         *   - On shrink, any currently present heap pages in the truncated
         *     range are unmapped and their frames are decref'd immediately.
         *
         * The caller must compare the return value to its request to detect
         * failure — this is the standard Linux brk() contract.
         */
        process_t *cur = sched_current();
        vm_area_t *heap_vma;
        if (!cur)
            return (uint32_t)-1;
        heap_vma = vma_find_kind(cur, VMA_KIND_HEAP);
        if (!heap_vma)
            return (uint32_t)-1;

        uint32_t new_brk = ebx;

        /* Query: return current brk without changing anything. */
        if (new_brk == 0) {
            klog_hex("BRK", "query brk", cur->brk);
            klog_uint("BRK", "query pid", cur->pid);
            return cur->brk;
        }

        /* Guard: must be above the heap base and below the stack region. */
        if (new_brk < heap_vma->start || new_brk > USER_HEAP_MAX)
            return cur->brk;

        if (new_brk < cur->brk) {
            uint32_t unmap_start = (new_brk + 0xFFFu) & ~0xFFFu;
            uint32_t old_end = (cur->brk + 0xFFFu) & ~0xFFFu;
            uint32_t *pd = (uint32_t *)cur->pd_phys;

            for (uint32_t vpage = unmap_start; vpage < old_end; vpage += 0x1000u) {
                uint32_t pdi = vpage >> 22;
                uint32_t pti = (vpage >> 12) & 0x3FFu;

                if (!(pd[pdi] & PG_PRESENT))
                    continue;

                uint32_t *pt = (uint32_t *)paging_entry_addr(pd[pdi]);
                if (!(pt[pti] & PG_PRESENT))
                    continue;

                pmm_decref(paging_entry_addr(pt[pti]));
                pt[pti] = 0;
                syscall_invlpg(vpage);
            }
        }

        cur->brk = new_brk;
        heap_vma->end = new_brk;
        return new_brk;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_mmap(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        old_mmap_args_t args;
        process_t *cur = sched_current();
        uint32_t map_addr = 0;
        uint32_t required = MAP_PRIVATE | MAP_ANONYMOUS;

        if (!cur || ebx == 0)
            return (uint32_t)-1;
        if (uaccess_copy_from_user(cur, &args, ebx, sizeof(args)) != 0)
            return (uint32_t)-1;
        if (args.length == 0 || !prot_is_valid(args.prot))
            return (uint32_t)-1;
        if ((args.flags & required) != required ||
            (args.flags & ~required) != 0)
            return (uint32_t)-1;
        if (args.fd != (uint32_t)-1 || args.offset != 0)
            return (uint32_t)-1;
        if (vma_map_anonymous(cur, args.addr, args.length,
                              prot_to_vma_flags(args.prot),
                              &map_addr) != 0)
            return (uint32_t)-1;
        return map_addr;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_mmap2(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 mmap2:
         *   ebx=addr, ecx=len, edx=prot, esi=flags, edi=fd, ebp=pgoffset.
         *
         * For now Drunix supports the static-runtime case: private anonymous
         * mappings without MAP_FIXED.
         */
        process_t *cur = sched_current();
        uint32_t map_addr = 0;
        uint32_t required = MAP_PRIVATE | MAP_ANONYMOUS;

        if (!cur || ecx == 0 || !prot_is_valid(edx))
            return (uint32_t)-1;
        if ((esi & required) != required ||
            (esi & ~(MAP_PRIVATE | MAP_ANONYMOUS)) != 0)
            return (uint32_t)-1;
        if (edi != (uint32_t)-1 || ebp != 0)
            return (uint32_t)-1;
        if (vma_map_anonymous(cur, ebx, ecx, prot_to_vma_flags(edx),
                              &map_addr) != 0)
            return (uint32_t)-1;
        return map_addr;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_munmap(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        uint32_t length;
        uint32_t end;

        if (!cur || ebx == 0 || (ebx & (PAGE_SIZE - 1u)) != 0 || ecx == 0)
            return (uint32_t)-1;

        length = (ecx + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
        end = ebx + length;
        if (length == 0 || end <= ebx || end > USER_STACK_TOP)
            return (uint32_t)-1;
        if (vma_unmap_range(cur, ebx, end) != 0)
            return (uint32_t)-1;

        syscall_unmap_user_range(cur, ebx, end);
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_mprotect(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        process_t *cur = sched_current();
        uint32_t length;
        uint32_t end;

        if (!cur || ebx == 0 || (ebx & (PAGE_SIZE - 1u)) != 0 ||
            ecx == 0 || !prot_is_valid(edx))
            return (uint32_t)-1;

        length = (ecx + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
        end = ebx + length;
        if (length == 0 || end <= ebx || end > USER_STACK_TOP)
            return (uint32_t)-1;
        if (vma_protect_range(cur, ebx, end, prot_to_vma_flags(edx)) != 0)
            return (uint32_t)-1;

        syscall_apply_mprotect(cur, ebx, end, edx);
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_pipe(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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
        if (!cur) return (uint32_t)-1;

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
        cur->open_files[rfd].type           = FD_TYPE_PIPE_READ;
        cur->open_files[rfd].writable       = 0;
        cur->open_files[rfd].append         = 0;
        cur->open_files[rfd].u.pipe.pipe_idx = (uint32_t)pipe_idx;

        int wfd = fd_alloc(cur);
        if (wfd < 0) {
            fd_close_one(cur, (unsigned)rfd);
            klog("PIPE", "fd table full (write end)");
            return (uint32_t)-1;
        }
        cur->open_files[wfd].type           = FD_TYPE_PIPE_WRITE;
        cur->open_files[wfd].writable       = 1;
        cur->open_files[wfd].append         = 0;
        cur->open_files[wfd].u.pipe.pipe_idx = (uint32_t)pipe_idx;

        {
            int user_fds[2];
            user_fds[0] = rfd;
            user_fds[1] = wfd;
            if (uaccess_copy_to_user(cur, ebx, user_fds, sizeof(user_fds)) != 0) {
                fd_close_one(cur, (unsigned)wfd);
                fd_close_one(cur, (unsigned)rfd);
                return (uint32_t)-1;
            }
        }
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_dup(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 dup(oldfd).  Return the lowest available descriptor that
         * refers to the same open file description.
         */
        process_t *cur = sched_current();
        int fd;

        (void)eax;
        (void)ecx;
        (void)edx;
        (void)esi;
        (void)edi;
        (void)ebp;

        fd = fd_duplicate_from(cur, ebx, 0);
        return fd < 0 ? (uint32_t)-1 : (uint32_t)fd;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_dup2(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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
        if (ebx >= MAX_FDS || ecx >= MAX_FDS) return (uint32_t)-1;

        process_t *cur = sched_current();
        if (!cur) return (uint32_t)-1;

        if (cur->open_files[ebx].type == FD_TYPE_NONE) return (uint32_t)-1;

        if (ebx == ecx) return ecx;   /* dup2(fd, fd) is a documented no-op */

        /* Close the destination if it is already open. */
        if (cur->open_files[ecx].type != FD_TYPE_NONE)
            fd_close_one(cur, ecx);

        /* Copy the handle. */
        cur->open_files[ecx] = cur->open_files[ebx];

        /* Bump the pipe refcount for the duplicated end. */
        if (cur->open_files[ecx].type == FD_TYPE_PIPE_READ) {
            pipe_buf_t *pb = pipe_get((int)cur->open_files[ecx].u.pipe.pipe_idx);
            if (pb) pb->read_open++;
        } else if (cur->open_files[ecx].type == FD_TYPE_PIPE_WRITE) {
            pipe_buf_t *pb = pipe_get((int)cur->open_files[ecx].u.pipe.pipe_idx);
            if (pb) pb->write_open++;
        }

        return ecx;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_kill(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = target pid (> 0) or process group id (< 0)
         * ecx = signal number
         *
         * Positive `ebx` targets a single process.  Negative `ebx` targets
         * every process in that process group, mirroring kill(2).
         *
         * Signal 0 is the Linux-compatible existence probe: it does not
         * deliver anything, but it still validates that the target exists.
         *
         * Returns 0 on success, -ESRCH if the target does not exist, and -1
         * if signum is out of range.
         */
        int sig = (int)ecx;
        int32_t target = (int32_t)ebx;
        if (sig < 0 || sig >= NSIG)
            return (uint32_t)-1;
        if (target > 0) {
            if (!sched_find_pid((uint32_t)target))
                return (uint32_t)-3;
            if (sig == 0)
                return 0;
            sched_send_signal((uint32_t)target, sig);
            return 0;
        }
        if (target < 0) {
            process_t *cur = sched_current();
            uint32_t pgid = (uint32_t)(-target);
            if (!cur || !sched_session_has_pgid(cur->sid, pgid))
                return (uint32_t)-3;
            if (sig == 0)
                return 0;
            sched_send_signal_to_pgid(pgid, sig);
            return 0;
        }
        {
            process_t *cur = sched_current();
            if (!cur)
                return (uint32_t)-1;
            if (sig == 0)
                return 0;
            sched_send_signal_to_pgid(cur->pgid, sig);
            return 0;
        }
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_sigaction(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = signal number
         * ecx = new handler: SIG_DFL (0), SIG_IGN (1), or a user VA
         * edx = pointer to uint32_t to receive the old handler, or 0
         *
         * Installs a new signal disposition for signal `ebx`.  SIGKILL (9)
         * and SIGSTOP (19) cannot be caught or ignored — returns -1.
         *
         * Returns 0 on success, -1 on error.
         */
        process_t *cur = sched_current();
        if (!cur) return (uint32_t)-1;

        int sig = (int)ebx;
        if (sig < 1 || sig >= NSIG)
            return (uint32_t)-1;
        if (sig == SIGKILL || sig == SIGSTOP)
            return (uint32_t)-1;
        if (ecx > SIG_IGN && ecx >= USER_STACK_TOP)
            return (uint32_t)-1;

        if (edx &&
            uaccess_copy_to_user(cur, edx, &cur->sig_handlers[sig],
                                 sizeof(cur->sig_handlers[sig])) != 0)
            return (uint32_t)-1;

        cur->sig_handlers[sig] = ecx;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_rt_sigaction(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 rt_sigaction(signum, act, oldact, sigsetsize).  Drunix
         * stores the handler disposition and currently ignores flags,
         * restorer, and mask fields.
         */
        process_t *cur = sched_current();
        uint8_t kact[32];
        uint8_t kold[32];
        uint32_t handler = 0;
        int sig = (int)ebx;

        if (!cur || sig < 1 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
            return (uint32_t)-1;
        if (esi < sizeof(uint32_t) || esi > 128u)
            return (uint32_t)-1;
        if (edx != 0) {
            k_memset(kold, 0, sizeof(kold));
            linux_put_u32(kold, 0u, cur->sig_handlers[sig]);
            if (uaccess_copy_to_user(cur, edx, kold, sizeof(kold)) != 0)
                return (uint32_t)-1;
        }
        if (ecx != 0) {
            if (uaccess_copy_from_user(cur, kact, ecx, sizeof(kact)) != 0)
                return (uint32_t)-1;
            handler = linux_get_u32(kact, 0u);
            if (handler > SIG_IGN && handler >= USER_STACK_TOP)
                return (uint32_t)-1;
            cur->sig_handlers[sig] = handler;
        }
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_sigreturn(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Restores the process context saved on the user stack before the
         * signal handler was called.  Called exclusively by the trampoline
         * code embedded in the signal frame — not by user programs directly.
         *
         * Signal frame layout at the time of this syscall:
         *   The handler has returned (ret), popping `ret_addr` and jumping
         *   to the trampoline.  ESP is now pointing at the `signum` slot,
         *   i.e. sig_frame + 4.
         *
         *   sig_frame + 0  : ret_addr  (already consumed by ret)
         *   sig_frame + 4  : signum    ← user ESP here
         *   sig_frame + 8  : saved_eip
         *   sig_frame + 12 : saved_eflags
         *   sig_frame + 16 : saved_eax
         *   sig_frame + 20 : saved_esp
         *
         * The ISR frame is always at kstack_top - 76 for the current process
         * (INT 0x80 pushes from TSS.ESP0, plus the fixed trampoline pushes).
         * Kernel frame offsets (uint32_t * from gs slot):
         *   [11] = EAX, [14] = EIP_user, [16] = EFLAGS, [17] = ESP_user
         */
        process_t *cur = sched_current();
        uint32_t sf[6];

        if (!cur)
            return (uint32_t)-1;
        uint32_t *kframe = (uint32_t *)(cur->kstack_top - 76);
        uint32_t user_esp = kframe[17];   /* ESP_user: pointing at signum */
        if (uaccess_copy_from_user(cur, sf, user_esp - 4, sizeof(sf)) != 0) {
            sched_mark_signaled(SIGSEGV, 0);
            schedule();
            __builtin_unreachable();
        }

        kframe[14] = sf[2];   /* restore EIP    */
        kframe[16] = sf[3];   /* restore EFLAGS */
        kframe[11] = sf[4];   /* restore EAX    */
        kframe[17] = sf[5];   /* restore ESP    */
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_sigprocmask(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = how:  0 = SIG_BLOCK, 1 = SIG_UNBLOCK, 2 = SIG_SETMASK
         * ecx = pointer to new mask (uint32_t bitmask), or 0 to query only
         * edx = pointer to receive old mask (uint32_t), or 0
         *
         * Updates the signal mask of the calling process.  SIGKILL (bit 9)
         * is always cleared from the blocked set — it cannot be blocked.
         *
         * Returns 0 on success, -1 on error.
         */
        process_t *cur = sched_current();
        if (!cur) return (uint32_t)-1;

        uint32_t old = cur->sig_blocked;
        uint32_t newmask = 0;

        if (edx && uaccess_copy_to_user(cur, edx, &old, sizeof(old)) != 0)
            return (uint32_t)-1;

        if (ecx) {
            if (uaccess_copy_from_user(cur, &newmask, ecx, sizeof(newmask)) != 0)
                return (uint32_t)-1;
        }
        if (syscall_apply_sigmask(cur, ebx, newmask, ecx != 0) != 0)
            return (uint32_t)-1;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_rt_sigprocmask(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = how, ecx = sigset_t *new, edx = sigset_t *old,
         * esi = sigset size.  Drunix stores a 32-bit signal mask; Linux i386
         * libc may pass a wider sigset_t, so copy the low word and zero-fill
         * the rest on output.
         */
        process_t *cur = sched_current();
        uint32_t old;
        uint32_t newmask = 0;

        if (!cur || esi < sizeof(uint32_t) || esi > 128u)
            return (uint32_t)-1;

        old = cur->sig_blocked;
        if (syscall_copy_rt_sigset_to_user(cur, edx, esi, old) != 0)
            return (uint32_t)-1;
        if (ecx &&
            uaccess_copy_from_user(cur, &newmask, ecx, sizeof(newmask)) != 0)
            return (uint32_t)-1;
        if (syscall_apply_sigmask(cur, ebx, newmask, ecx != 0) != 0)
            return (uint32_t)-1;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_drunix_tcgetattr(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /* ebx = fd, ecx = termios_t* (user pointer) */
        process_t *cur = sched_current();
        tty_t *tty = syscall_tty_from_fd(cur, ebx, 0);
        if (!tty) return (uint32_t)-1;
        if (uaccess_copy_to_user(cur, ecx, &tty->termios, sizeof(tty->termios)) != 0)
            return (uint32_t)-1;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_drunix_tcsetattr(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /* ebx = fd, ecx = action (TCSANOW=0 / TCSAFLUSH=2), edx = termios_t* */
        process_t *cur = sched_current();
        tty_t *tty = syscall_tty_from_fd(cur, ebx, 0);
        termios_t new_termios;
        if (!tty) return (uint32_t)-1;
        if (uaccess_copy_from_user(cur, &new_termios, edx, sizeof(new_termios)) != 0)
            return (uint32_t)-1;
        if (ecx == TCSAFLUSH) {
            /* Discard unread input */
            tty->raw_head = tty->raw_tail = 0;
            tty->canon_len   = 0;
            tty->canon_ready = 0;
        }
        tty->termios = new_termios;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_setpgid(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /* ebx = pid (0 = self), ecx = pgid (0 = use pid) */
        process_t *cur = sched_current();
        process_t *target;
        if (!cur) return (uint32_t)-1;
        uint32_t target_pid = ebx ? ebx : cur->pid;
        uint32_t new_pgid   = ecx ? ecx : target_pid;

        if (target_pid == cur->pid) {
            target = cur;
        } else {
            target = sched_find_pid(target_pid);
            if (!target || target->parent_pid != cur->pid)
                return (uint32_t)-1;
        }

        if (target->sid != cur->sid)
            return (uint32_t)-1;
        if (target->pid == target->sid && new_pgid != target->pgid)
            return (uint32_t)-1;
        if (new_pgid != target_pid &&
            !sched_session_has_pgid(target->sid, new_pgid))
            return (uint32_t)-1;

        target->pgid = new_pgid;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_getpgid(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /* ebx = pid (0 = self) → returns pgid */
        process_t *cur = sched_current();
        if (!cur) return (uint32_t)-1;
        if (ebx == 0 || ebx == cur->pid) return cur->pgid;
        process_t *target = sched_find_pid(ebx);
        if (!target) return (uint32_t)-1;
        return target->pgid;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_lseek(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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

        if (syscall_seek_handle(cur, ebx, (int32_t)ecx, edx, &new_off) != 0)
            return (uint32_t)-1;
        return (uint32_t)new_off;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_llseek(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
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

        if (!cur || esi == 0)
            return (uint32_t)-1;
        if (syscall_seek_handle(cur, ebx, signed_off, edi, &new_off) != 0)
            return (uint32_t)-1;
        result[0] = (uint32_t)new_off;
        result[1] = (uint32_t)(new_off >> 32);
        if (uaccess_copy_to_user(cur, esi, result, sizeof(result)) != 0)
            return (uint32_t)-1;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_getpid(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        return sched_current_pid();
}

static uint32_t SYSCALL_NOINLINE syscall_case_gettid(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        return sched_current_pid();
}

static uint32_t SYSCALL_NOINLINE syscall_case_getppid(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        return sched_current_ppid();
}

static uint32_t SYSCALL_NOINLINE syscall_case_getuid32_getgid32_geteuid32_getegid32(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        return 0;
}

static uint32_t SYSCALL_NOINLINE syscall_case_setuid32_setgid32(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        return ebx == 0 ? 0 : (uint32_t)-1;
}

static uint32_t SYSCALL_NOINLINE syscall_case_waitpid(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * ebx = pid, ecx = int *status, edx = options.
         * Writes Linux-style encoded status:
         *   Exited:  (exit_code << 8)          — low 7 bits == 0
         *   Stopped: (stop_signal << 8) | 0x7F — low 7 bits == 0x7F
         * Returns pid, 0 with WNOHANG, or -1 for no such process / bad status.
        */
        return syscall_wait_common(ebx, ecx, edx, 0);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_wait4(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /*
         * Linux i386 wait4(pid, status, options, rusage).  Resource usage is
         * not tracked yet; the wait/reap semantics match waitpid.
        */
        return syscall_wait_common(ebx, ecx, edx, esi);
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_drunix_tcsetpgrp(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /* ebx = fd, ecx = pgid → 0 or -1 */
        process_t *cur = sched_current();
        uint32_t tty_idx;
        tty_t *tty = syscall_tty_from_fd(cur, ebx, &tty_idx);
        if (!tty) return (uint32_t)-1;
        if (ecx == 0) return (uint32_t)-1;
        if (cur->tty_id != tty_idx) return (uint32_t)-1;
        if (tty->ctrl_sid == 0)
            tty->ctrl_sid = cur->sid;
        if (tty->ctrl_sid != cur->sid) return (uint32_t)-1;
        if (!sched_session_has_pgid(tty->ctrl_sid, ecx)) return (uint32_t)-1;
        tty->fg_pgid = ecx;
        return 0;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_drunix_tcgetpgrp(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
    {
        /* ebx = fd → fg_pgid or -1 */
        process_t *cur = sched_current();
        tty_t *tty = syscall_tty_from_fd(cur, ebx, 0);
        if (!tty) return (uint32_t)-1;
        return tty->fg_pgid;
    }
}

static uint32_t SYSCALL_NOINLINE syscall_case_unknown(uint32_t eax, uint32_t ebx,
                              uint32_t ecx,
                              uint32_t edx, uint32_t esi,
                              uint32_t edi, uint32_t ebp)
{
        klog_uint("KERN", "unknown syscall", eax);
        return (uint32_t)-1;
}

/*
 * syscall_handler: dispatches INT 0x80 calls from user space.
 *
 * When this function runs, the CPU is in ring 0 but still using the
 * process's page directory (CR3 was not changed by the interrupt).
 * This means kernel code can walk the caller's user mappings, but it still
 * must validate every user pointer explicitly.  All user buffers and strings
 * are copied through uaccess helpers so bad pointers fail cleanly and kernel
 * writes into copy-on-write user pages allocate a private frame first.
 *
 * The INT 0x80 gate is a trap gate (type_attr=0xEF), so IF is NOT cleared
 * on entry — hardware interrupts (including keyboard IRQ1 and timer IRQ0)
 * remain active.  This allows SYS_READ to spin-wait for keyboard input and
 * allows the timer to fire (and sched_tick() to set need_switch) while a
 * process is blocked.  The context switch itself happens in syscall_common
 * (isr.asm) after this function returns, when sched_needs_switch() is true.
 *
 * Return value: written back to the saved EAX slot in isr.asm so the user
 * sees it in EAX after iret.
 */
uint32_t syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx,
                         uint32_t edx, uint32_t esi, uint32_t edi,
                         uint32_t ebp)
{
    switch (eax) {
    case SYS_WRITE:
        return syscall_case_write(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_WRITEV:
        return syscall_case_writev(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_READV:
        return syscall_case_readv(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_READ:
        return syscall_case_read(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SENDFILE64:
        return syscall_case_sendfile64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_OPEN:
        return syscall_case_open(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_OPENAT:
        return syscall_case_openat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_CLOSE:
        return syscall_case_close(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_ACCESS:
        return syscall_case_access(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FACCESSAT:
        return syscall_case_faccessat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FCHMODAT:
        return syscall_case_fchmodat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FCHOWNAT:
        return syscall_case_fchownat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FUTIMESAT:
        return syscall_case_futimesat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_CHMOD:
        return syscall_case_chmod(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_LCHOWN:
    case SYS_CHOWN32:
        return syscall_case_lchown_chown32(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SYNC:
        return syscall_case_sync(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_UMASK:
        return syscall_case_umask(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SETSID:
        return syscall_case_setsid(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETSID:
        return syscall_case_getsid(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_READLINK:
        return syscall_case_readlink(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_READLINKAT:
        return syscall_case_readlinkat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETPRIORITY:
        return syscall_case_getpriority(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SETPRIORITY:
        return syscall_case_setpriority(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SYSINFO:
        return syscall_case_sysinfo(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_TRUNCATE64:
        return syscall_case_truncate64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FTRUNCATE64:
        return syscall_case_ftruncate64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SCHED_GETAFFINITY:
        return syscall_case_sched_getaffinity(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_UTIMENSAT:
        return syscall_case_utimensat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_POLL:
        return syscall_case_poll(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_IOCTL:
        return syscall_case_ioctl(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FCNTL64:
        return syscall_case_fcntl64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_EXECVE:
        return syscall_case_execve(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_CREAT:
        return syscall_case_creat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_UNLINK:
        return syscall_case_unlink(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_UNLINKAT:
        return syscall_case_unlinkat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FORK:
    case SYS_VFORK:
        return syscall_case_fork_vfork(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DRUNIX_CLEAR:
        return syscall_case_drunix_clear(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DRUNIX_SCROLL_UP:
        return syscall_case_drunix_scroll_up(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DRUNIX_SCROLL_DOWN:
        return syscall_case_drunix_scroll_down(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_YIELD:
        return syscall_case_yield(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_NANOSLEEP:
        return syscall_case_nanosleep(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_MKDIR:
        return syscall_case_mkdir(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_MKDIRAT:
        return syscall_case_mkdirat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_RMDIR:
        return syscall_case_rmdir(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DUP:
        return syscall_case_dup(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_CHDIR:
        return syscall_case_chdir(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETCWD:
        return syscall_case_getcwd(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_RENAME:
        return syscall_case_rename(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_RENAMEAT:
        return syscall_case_renameat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_LINKAT:
        return syscall_case_linkat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SYMLINKAT:
        return syscall_case_symlinkat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETDENTS:
        return syscall_case_getdents(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS__NEWSELECT:
        return syscall_case_newselect(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DRUNIX_GETDENTS_PATH:
        return syscall_case_drunix_getdents_path(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETDENTS64:
        return syscall_case_getdents64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_STAT:
        return syscall_case_stat(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_STAT64:
    case SYS_LSTAT64:
        return syscall_case_stat64_lstat64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FSTATAT64:
        return syscall_case_fstatat64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FSTAT64:
        return syscall_case_fstat64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_STATX:
        return syscall_case_statx(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_STATFS64:
        return syscall_case_statfs64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_FSTATFS64:
        return syscall_case_fstatfs64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_CLOCK_GETTIME:
        return syscall_case_clock_gettime(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_CLOCK_GETTIME64:
        return syscall_case_clock_gettime64(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETTIMEOFDAY:
        return syscall_case_gettimeofday(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_UNAME:
        return syscall_case_uname(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SET_THREAD_AREA:
        return syscall_case_set_thread_area(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SET_TID_ADDRESS:
        return syscall_case_set_tid_address(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DRUNIX_MODLOAD:
        return syscall_case_drunix_modload(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        return syscall_case_exit_exit_group(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_BRK:
        return syscall_case_brk(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_MMAP:
        return syscall_case_mmap(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_MMAP2:
        return syscall_case_mmap2(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_MUNMAP:
        return syscall_case_munmap(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_MPROTECT:
        return syscall_case_mprotect(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_PIPE:
        return syscall_case_pipe(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DUP2:
        return syscall_case_dup2(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_KILL:
        return syscall_case_kill(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SIGACTION:
        return syscall_case_sigaction(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_RT_SIGACTION:
        return syscall_case_rt_sigaction(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SIGRETURN:
        return syscall_case_sigreturn(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SIGPROCMASK:
        return syscall_case_sigprocmask(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_RT_SIGPROCMASK:
        return syscall_case_rt_sigprocmask(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DRUNIX_TCGETATTR:
        return syscall_case_drunix_tcgetattr(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DRUNIX_TCSETATTR:
        return syscall_case_drunix_tcsetattr(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SETPGID:
        return syscall_case_setpgid(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETPGID:
        return syscall_case_getpgid(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_LSEEK:
        return syscall_case_lseek(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS__LLSEEK:
        return syscall_case_llseek(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETPID:
        return syscall_case_getpid(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETTID:
        return syscall_case_gettid(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETPPID:
        return syscall_case_getppid(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_GETUID32:
    case SYS_GETGID32:
    case SYS_GETEUID32:
    case SYS_GETEGID32:
        return syscall_case_getuid32_getgid32_geteuid32_getegid32(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_SETUID32:
    case SYS_SETGID32:
        return syscall_case_setuid32_setgid32(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_WAITPID:
        return syscall_case_waitpid(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_WAIT4:
        return syscall_case_wait4(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DRUNIX_TCSETPGRP:
        return syscall_case_drunix_tcsetpgrp(eax, ebx, ecx, edx, esi, edi, ebp);

    case SYS_DRUNIX_TCGETPGRP:
        return syscall_case_drunix_tcgetpgrp(eax, ebx, ecx, edx, esi, edi, ebp);

    default:
        return syscall_case_unknown(eax, ebx, ecx, edx, esi, edi, ebp);
    }
}

#ifdef KTEST_ENABLED
int syscall_console_write_for_test(process_t *proc, const char *buf,
                                   uint32_t len)
{
    return syscall_write_console_bytes(proc, buf, len);
}
#endif
