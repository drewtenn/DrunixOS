/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * syscall.c — INT 0x80 syscall dispatcher and kernel-side syscall implementations.
 */

#include "syscall.h"
#include "sched.h"
#include "process.h"
#include "pipe.h"
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
#include <stdint.h>

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

    if (name[0] == '/')
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

typedef struct {
    uint32_t addr;
    uint32_t length;
    uint32_t prot;
    uint32_t flags;
    uint32_t fd;
    uint32_t offset;
} old_mmap_args_t;

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

static int fd_install_vfs_node(process_t *proc, const vfs_node_t *node,
                               uint32_t writable)
{
    int fd;

    if (!proc || !node)
        return -1;

    fd = fd_alloc(proc);
    if (fd < 0)
        return -1;

    proc->open_files[fd].writable = writable;

    switch (node->type) {
    case VFS_NODE_FILE:
        proc->open_files[fd].type = FD_TYPE_FILE;
        proc->open_files[fd].u.file.inode_num = node->inode_num;
        proc->open_files[fd].u.file.size = node->size;
        proc->open_files[fd].u.file.offset = 0;
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
        return -1;
    }
}

#define USER_IO_CHUNK 128u
#define TTY_IO_CHUNK \
    ((TTY_CANON_BUF_SIZE > TTY_RAW_BUF_SIZE) ? TTY_CANON_BUF_SIZE : TTY_RAW_BUF_SIZE)

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
        fs_flush_inode(fh->u.file.inode_num);

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
                         uint32_t edx, uint32_t esi, uint32_t edi)
{
    switch (eax) {

    case SYS_FWRITE: {
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
        if (ebx >= MAX_FDS)
            return (uint32_t)-1;

        process_t *cur = sched_current();
        if (!cur)
            return (uint32_t)-1;

        file_handle_t *fh = &cur->open_files[ebx];

        if (fh->type == FD_TYPE_STDOUT) {
            uint8_t kbuf[USER_IO_CHUNK];
            uint32_t written = 0;

            if (uaccess_prepare(cur, ecx, edx, 0) != 0)
                return (uint32_t)-1;

            while (written < edx) {
                uint32_t chunk = edx - written;
                if (chunk > USER_IO_CHUNK)
                    chunk = USER_IO_CHUNK;
                if (uaccess_copy_from_user(cur, kbuf, ecx + written, chunk) != 0)
                    return written ? written : (uint32_t)-1;
                syscall_write_console_bytes(cur, (const char *)kbuf, chunk);
                written += chunk;
            }
            return written;
        }

        if (fh->type == FD_TYPE_PIPE_WRITE) {
            pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
            uint8_t kbuf[USER_IO_CHUNK];
            uint32_t copied = 0;

            if (!pb || pb->read_open == 0)
                return (uint32_t)-1;   /* broken pipe */

            if (uaccess_prepare(cur, ecx, edx, 0) != 0)
                return (uint32_t)-1;

            while (copied < edx) {
                uint32_t chunk = edx - copied;
                if (chunk > USER_IO_CHUNK)
                    chunk = USER_IO_CHUNK;
                if (uaccess_copy_from_user(cur, kbuf, ecx + copied, chunk) != 0)
                    return copied ? copied : (uint32_t)-1;

                /* Block while the pipe buffer is full. */
                while (pb->count == PIPE_BUF_SIZE) {
                    if (pb->read_open == 0 || cur->sig_pending)
                        return copied ? copied : (uint32_t)-1;
                    sched_block(&pb->waiters);
                    /* Resumed after another process drains data; re-check. */
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
            /* Wake readers and writers sleeping on this pipe. */
            sched_wake_all(&pb->waiters);
            return copied;
        }

        if (fh->type == FD_TYPE_FILE) {
            uint8_t kbuf[USER_IO_CHUNK];
            uint32_t written = 0;

            if (!fh->writable)
                return (uint32_t)-1;
            if (edx == 0)
                return 0;

            if (uaccess_prepare(cur, ecx, edx, 0) != 0)
                return (uint32_t)-1;

            while (written < edx) {
                uint32_t chunk = edx - written;
                if (chunk > USER_IO_CHUNK)
                    chunk = USER_IO_CHUNK;
                if (uaccess_copy_from_user(cur, kbuf, ecx + written, chunk) != 0)
                    return written ? written : (uint32_t)-1;

                int n = fs_write(fh->u.file.inode_num,
                                 fh->u.file.offset + written,
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

    case SYS_WRITE:
        /*
         * ebx = pointer to a byte buffer in user virtual space
         * ecx = number of bytes to write
         *
         * The buffer is NOT required to be null-terminated — SYS_WRITE emits
         * exactly `count` bytes to the console.  This lets user code pipe
         * binary data (for example, the bytes of an ELF image) through the
         * write path without an embedded 0x00 truncating the output.
         *
         * Returns the number of bytes written.
         */
        {
            process_t *cur = sched_current();
            uint8_t kbuf[USER_IO_CHUNK];
            uint32_t written = 0;

            if (!cur)
                return (uint32_t)-1;
            if (uaccess_prepare(cur, ebx, ecx, 0) != 0)
                return (uint32_t)-1;

            while (written < ecx) {
                uint32_t chunk = ecx - written;
                if (chunk > USER_IO_CHUNK)
                    chunk = USER_IO_CHUNK;
                if (uaccess_copy_from_user(cur, kbuf, ebx + written, chunk) != 0)
                    return written ? written : (uint32_t)-1;
                syscall_write_console_bytes(cur, (const char *)kbuf, chunk);
                written += chunk;
            }
            return written;
        }

    case SYS_READ: {
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
        if (ebx >= MAX_FDS)
            return (uint32_t)-1;

        process_t *cur = sched_current();
        if (!cur)
            return (uint32_t)-1;

        file_handle_t *fh = &cur->open_files[ebx];

        if (fh->type == FD_TYPE_TTY) {
            char kbuf[TTY_IO_CHUNK];
            uint32_t count = edx ? edx : 1;
            uint32_t chunk = count > TTY_IO_CHUNK ? TTY_IO_CHUNK : count;

            if (uaccess_prepare(cur, ecx, count, 1) != 0)
                return (uint32_t)-1;
            int n = tty_read((int)fh->u.tty.tty_idx, kbuf, chunk);
            if (n > 0 && uaccess_copy_to_user(cur, ecx, kbuf, (uint32_t)n) != 0)
                return (uint32_t)-1;
            return (uint32_t)n;
        }

        if (fh->type == FD_TYPE_CHARDEV) {
            /*
             * Legacy spin-wait path for any remaining FD_TYPE_CHARDEV fds.
             * New code uses FD_TYPE_TTY; this branch is kept for compatibility.
             */
            const chardev_ops_t *dev = chardev_get(fh->u.chardev.name);
            if (!dev)
                return (uint32_t)-1;
            if (uaccess_prepare(cur, ecx, 1, 1) != 0)
                return (uint32_t)-1;
            char c = 0;
            while ((c = dev->read_char()) == 0)
                __asm__ volatile ("pause");
            if (uaccess_copy_to_user(cur, ecx, &c, 1) != 0)
                return (uint32_t)-1;
            return 1;
        }

        if (fh->type == FD_TYPE_PIPE_READ) {
            pipe_buf_t *pb = pipe_get((int)fh->u.pipe.pipe_idx);
            if (!pb) return (uint32_t)-1;

            /*
             * Block while the pipe is empty and at least one write end is
             * still open.  schedule() returns when we are rescheduled;
             * pipe state changes wake the shared pipe wait queue.
             */
            while (pb->count == 0) {
                if (pb->write_open == 0)
                    return 0;   /* EOF: all write ends closed */
                if (cur->sig_pending)
                    return (uint32_t)-1;  /* interrupted by signal */
                sched_block(&pb->waiters);
                /* After resuming, pb->count may still be 0 if we were
                 * woken speculatively — re-check the loop condition. */
            }

            uint32_t to_read = (edx < pb->count) ? edx : pb->count;
            uint8_t kbuf[USER_IO_CHUNK];
            uint32_t copied = 0;

            if (uaccess_prepare(cur, ecx, to_read, 1) != 0)
                return (uint32_t)-1;

            while (copied < to_read) {
                uint32_t chunk = to_read - copied;
                if (chunk > USER_IO_CHUNK)
                    chunk = USER_IO_CHUNK;
                for (uint32_t i = 0; i < chunk; i++) {
                    kbuf[i] = pb->buf[pb->read_idx];
                    pb->read_idx = (pb->read_idx + 1) % PIPE_BUF_SIZE;
                }
                if (uaccess_copy_to_user(cur, ecx + copied, kbuf, chunk) != 0)
                    return copied ? copied : (uint32_t)-1;
                copied += chunk;
            }
            pb->count -= to_read;
            sched_wake_all(&pb->waiters);
            return to_read;
        }

        if (fh->type == FD_TYPE_FILE) {
            /* EOF — tell user space to stop reading. */
            if (fh->u.file.offset >= fh->u.file.size)
                return 0;

            uint32_t remaining = fh->u.file.size - fh->u.file.offset;
            uint32_t to_read   = (edx < remaining) ? edx : remaining;
            uint32_t copied = 0;
            uint32_t file_off = fh->u.file.offset;
            uint8_t kbuf[USER_IO_CHUNK];

            if (to_read == 0)
                return 0;

            if (uaccess_prepare(cur, ecx, to_read, 1) != 0)
                return (uint32_t)-1;

            while (copied < to_read) {
                uint32_t chunk = to_read - copied;
                if (chunk > USER_IO_CHUNK)
                    chunk = USER_IO_CHUNK;

                int n = fs_read(fh->u.file.inode_num, file_off + copied,
                                kbuf, chunk);
                if (n < 0) {
                    klog("READ", "fs_read failed");
                    return copied ? copied : (uint32_t)-1;
                }
                if (n == 0)
                    break;
                if (uaccess_copy_to_user(cur, ecx + copied, kbuf, (uint32_t)n) != 0)
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

            if (procfs_file_size(fh->u.proc.kind, fh->u.proc.pid,
                                 fh->u.proc.index, &size) != 0)
                return (uint32_t)-1;

            fh->u.proc.size = size;
            if (fh->u.proc.offset >= size)
                return 0;

            uint32_t to_read = size - fh->u.proc.offset;
            if (edx < to_read)
                to_read = edx;

            if (to_read == 0)
                return 0;
            if (uaccess_prepare(cur, ecx, to_read, 1) != 0)
                return (uint32_t)-1;

            while (copied < to_read) {
                uint32_t chunk = to_read - copied;
                if (chunk > USER_IO_CHUNK)
                    chunk = USER_IO_CHUNK;

                int n = procfs_read_file(fh->u.proc.kind, fh->u.proc.pid,
                                         fh->u.proc.index,
                                         fh->u.proc.offset + copied,
                                         (char *)kbuf, chunk);
                if (n < 0)
                    return copied ? copied : (uint32_t)-1;
                if (n == 0)
                    break;
                if (uaccess_copy_to_user(cur, ecx + copied, kbuf, (uint32_t)n) != 0)
                    return copied ? copied : (uint32_t)-1;
                copied += (uint32_t)n;
            }

            fh->u.proc.offset += copied;
            return copied;
        }

        return (uint32_t)-1;
    }

    case SYS_OPEN: {
        /*
         * ebx = pointer to null-terminated filename in user space.
         *
         * Resolve the pathname through the VFS mount tree, install the
         * corresponding read-only fd, and return it.
         * fd_alloc() scans from 0; since slots 0/1/2 are pre-populated,
         * the first returned fd will normally be 3.
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

        vfs_node_t node;
        if (vfs_resolve(rpath, &node) != 0 ||
            (node.type != VFS_NODE_FILE &&
             node.type != VFS_NODE_PROCFILE &&
             node.type != VFS_NODE_TTY &&
             node.type != VFS_NODE_CHARDEV)) {
            klog("OPEN", "path not openable");
            kfree(rpath);
            return (uint32_t)-1;
        }
        kfree(rpath);

        int fd = fd_install_vfs_node(cur, &node, 0);
        if (fd < 0) {
            klog("OPEN", "fd table full");
            return (uint32_t)-1;
        }
        return (uint32_t)fd;
    }

    case SYS_CLOSE: {
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

    case SYS_EXEC: {
        /*
         * ebx = pointer to null-terminated filename in user space
         * ecx = pointer to a user-space char *[] array (or 0 for no argv)
         * edx = argc (number of non-NULL entries in argv; 0 if ecx is 0)
         * esi = pointer to a user-space envp[] array (or 0 for no env)
         * edi = envc (number of non-NULL entries in envp; 0 if esi is 0)
         *
         * Replace the calling process in-place.  PID, parent linkage,
         * process-group/session membership, cwd, and the open-fd table are
         * preserved; the user address space, user stack, heap, entry point,
         * and process metadata derived from argv are rebuilt from the new ELF.
         *
         * On success this syscall does not return to the old image.
         */

        /*
         * Snapshot argv into kernel memory.  Pointer argument arrives in
         * the caller's virtual address space, which is still active here,
         * so we can dereference it directly — but process_create will
         * write the new process's stack through physical addresses behind
         * a different page directory, so every string has to live in
         * identity-mapped kernel space first.
         */
        /* kargv points into kstrs; both are heap-allocated to keep this
         * frame small (process_t alone is ~5 KB on the stack). */
        const char *kargv[PROCESS_ARGV_MAX_COUNT + 1];
        char       *kstrs = (char *)kmalloc(PROCESS_ARGV_MAX_BYTES);
        process_t  *exec_cur = sched_current();
        if (!kstrs) return (uint32_t)-1;
        int kargc = 0;
        const char *kenvp[PROCESS_ENV_MAX_COUNT + 1];
        char       *kenvstrs = (char *)kmalloc(PROCESS_ENV_MAX_BYTES);
        if (!kenvstrs) { kfree(kstrs); return (uint32_t)-1; }
        int kenvc = 0;

        /*
         * argv and argc must agree: either both zero (no arguments) or
         * both non-zero (argc entries at ecx).  A NULL argv with argc > 0
         * — or a non-NULL argv with argc == 0 — is a caller bug, and we
         * refuse to silently drop arguments or fabricate an empty frame.
         */
        if ((ecx == 0) != (edx == 0)) {
            klog("EXEC", "argv/argc mismatch");
            kfree(kenvstrs);
            kfree(kstrs);
            return (uint32_t)-1;
        }
        if ((esi == 0) != (edi == 0)) {
            klog("EXEC", "envp/envc mismatch");
            kfree(kenvstrs);
            kfree(kstrs);
            return (uint32_t)-1;
        }

        if (ecx != 0) {
            if (edx > PROCESS_ARGV_MAX_COUNT) {
                klog("EXEC", "argc over limit");
                kfree(kenvstrs);
                kfree(kstrs);
                return (uint32_t)-1;
            }
            uint32_t used = 0;
            for (uint32_t i = 0; i < edx; i++) {
                uint32_t us = 0;
                uint32_t remaining = PROCESS_ARGV_MAX_BYTES - used;

                if (uaccess_copy_from_user(exec_cur,
                                           &us, ecx + i * sizeof(uint32_t),
                                           sizeof(uint32_t)) != 0 || us == 0) {
                    klog("EXEC", "null argv entry");
                    kfree(kenvstrs);
                    kfree(kstrs);
                    return (uint32_t)-1;
                }
                kargv[i] = &kstrs[used];
                if (uaccess_copy_string_from_user(exec_cur,
                                                  &kstrs[used], remaining,
                                                  us) != 0) {
                    klog("EXEC", "argv bytes over limit");
                    kfree(kenvstrs);
                    kfree(kstrs);
                    return (uint32_t)-1;
                }
                used += k_strlen(&kstrs[used]) + 1;
            }
            kargv[edx] = 0;
            kargc = (int)edx;
        }

        if (esi != 0) {
            if (edi > PROCESS_ENV_MAX_COUNT) {
                klog("EXEC", "envc over limit");
                kfree(kenvstrs);
                kfree(kstrs);
                return (uint32_t)-1;
            }
            uint32_t used = 0;
            for (uint32_t i = 0; i < edi; i++) {
                uint32_t us = 0;
                uint32_t remaining = PROCESS_ENV_MAX_BYTES - used;

                if (uaccess_copy_from_user(exec_cur,
                                           &us, esi + i * sizeof(uint32_t),
                                           sizeof(uint32_t)) != 0 || us == 0) {
                    klog("EXEC", "null envp entry");
                    kfree(kenvstrs);
                    kfree(kstrs);
                    return (uint32_t)-1;
                }
                kenvp[i] = &kenvstrs[used];
                if (uaccess_copy_string_from_user(exec_cur,
                                                  &kenvstrs[used], remaining,
                                                  us) != 0) {
                    klog("EXEC", "env bytes over limit");
                    kfree(kenvstrs);
                    kfree(kstrs);
                    return (uint32_t)-1;
                }
                used += k_strlen(&kenvstrs[used]) + 1;
            }
            kenvp[edi] = 0;
            kenvc = (int)edi;
        }

        char *exec_rpath = (char *)kmalloc(4096);
        if (!exec_rpath) { kfree(kenvstrs); kfree(kstrs); return (uint32_t)-1; }
        if (!exec_cur || resolve_user_path(exec_cur, ebx, exec_rpath, 4096) != 0) {
            kfree(exec_rpath);
            kfree(kenvstrs);
            kfree(kstrs);
            return (uint32_t)-1;
        }
        uint32_t ino, sz;
        if (vfs_open(exec_rpath, &ino, &sz) != 0) {
            klog("EXEC", "file not found");
            kfree(exec_rpath);
            kfree(kenvstrs);
            kfree(kstrs);
            return (uint32_t)-1;
        }
        kfree(exec_rpath);

        /* Allocate process_t on the heap: the struct is ~5 KB and would
         * overflow the 1 KB stack-frame budget if placed as a local. */
        process_t *new_proc = (process_t *)kmalloc(sizeof(process_t));
        if (!new_proc) { kfree(kenvstrs); kfree(kstrs); return (uint32_t)-1; }

        if (process_create(new_proc, ino, kargv, kargc, kenvp, kenvc, 0) != 0) {
            klog("EXEC", "process_create failed");
            kfree(new_proc);
            kfree(kenvstrs);
            kfree(kstrs);
            return (uint32_t)-1;
        }
        kfree(kenvstrs);
        kfree(kstrs);

        if (!exec_cur) {
            process_release_user_space(new_proc);
            process_release_kstack(new_proc);
            kfree(new_proc);
            return (uint32_t)-1;
        }

        new_proc->pid        = exec_cur->pid;
        new_proc->parent_pid = exec_cur->parent_pid;
        new_proc->pgid       = exec_cur->pgid;
        new_proc->sid        = exec_cur->sid;
        new_proc->tty_id     = exec_cur->tty_id;
        new_proc->state      = PROC_RUNNING;
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
        new_proc->crash.valid  = 0;
        new_proc->crash.signum = 0;
        new_proc->crash.cr2    = 0;

        klog_hex("EXEC", "new_proc brk", new_proc->brk);
        klog_hex("EXEC", "new_proc heap_start", new_proc->heap_start);
        process_build_exec_frame(new_proc, exec_cur->pd_phys,
                                 exec_cur->kstack_bottom);
        sched_exec_current(new_proc);
        return 0;
    }

    case SYS_WAIT:
        /*
         * ebx = pid to wait for.
         * Blocks via schedule() until the target process exits.
         * Returns the target's encoded exit status, or -1 if not found.
         * Status is Linux-encoded: (exit_code << 8), low 7 bits == 0.
         */
        return (uint32_t)sched_waitpid(ebx, 0);

    case SYS_CREATE: {
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
        kfree(rpath);
        if (ino_c < 0) {
            klog("CREATE", "vfs_create failed");
            return (uint32_t)-1;
        }

        int fd = fd_alloc(cur);
        if (fd < 0) {
            klog("CREATE", "fd table full");
            return (uint32_t)-1;
        }
        cur->open_files[fd].type             = FD_TYPE_FILE;
        cur->open_files[fd].writable         = 1;
        cur->open_files[fd].u.file.inode_num = (uint32_t)ino_c;
        cur->open_files[fd].u.file.size      = 0;
        cur->open_files[fd].u.file.offset    = 0;
        return (uint32_t)fd;
    }

    case SYS_UNLINK: {
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

    case SYS_FORK: {
        /*
         * No arguments.
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

    case SYS_CLEAR:
        if (desktop_is_active() && desktop_clear_console(desktop_global()))
            return 0;
        clear_screen();
        return 0;

    case SYS_SCROLL_UP:
        if (desktop_is_active())
            return 0;
        scroll_up((int)ebx);
        return 0;

    case SYS_SCROLL_DOWN:
        if (desktop_is_active())
            return 0;
        scroll_down((int)ebx);
        return 0;

    case SYS_YIELD:
        /* Voluntarily give up the rest of the current timeslice. */
        schedule();
        return 0;

    case SYS_SLEEP: {
        /*
         * ebx = whole seconds to sleep.
         *
         * Blocks the caller until the deadline expires or a signal wakes it.
         * Returns 0 on full sleep, or the remaining whole seconds if
         * interrupted by a signal.
         */
        process_t *cur = sched_current();
        if (!cur) return (uint32_t)-1;
        if (ebx == 0) return 0;

        uint32_t start = sched_ticks();
        uint32_t delta_ticks =
            (ebx > (0xFFFFFFFFu / SCHED_HZ)) ? 0xFFFFFFFFu : ebx * SCHED_HZ;
        uint32_t deadline = start + delta_ticks;

        sched_block_until(deadline);

        uint32_t now = sched_ticks();
        if ((int32_t)(deadline - now) > 0) {
            uint32_t remaining_ticks = deadline - now;
            return (remaining_ticks + SCHED_HZ - 1u) / SCHED_HZ;
        }
        return 0;
    }

    case SYS_MKDIR: {
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

    case SYS_RMDIR: {
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

    case SYS_CHDIR: {
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

    case SYS_GETCWD: {
        /*
         * ebx = pointer to output buffer in user space.
         * ecx = size of the buffer.
         *
         * Copies the process cwd (without leading slash) into the user buffer
         * as a NUL-terminated string.  An empty string means the process is
         * at the filesystem root.  Returns the number of characters written
         * (not counting the NUL), or (uint32_t)-1 on error.
         */
        process_t *cur = sched_current();
        if (!cur || ecx == 0) return (uint32_t)-1;
        uint32_t len = k_strnlen(cur->cwd, ecx - 1);
        if (uaccess_copy_to_user(cur, ebx, cur->cwd, len) != 0)
            return (uint32_t)-1;
        {
            char zero = '\0';
            if (uaccess_copy_to_user(cur, ebx + len, &zero, 1) != 0)
                return (uint32_t)-1;
        }
        return len;
    }

    case SYS_RENAME: {
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

    case SYS_GETDENTS: {
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

    case SYS_STAT: {
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

    case SYS_CLOCK_GETTIME: {
        /*
         * ebx = clock id (0 = CLOCK_REALTIME).
         * ecx = pointer to struct timespec in user space:
         *       long tv_sec; long tv_nsec;
         *
         * Returns 0 on success or -1 for an unsupported clock / bad pointer.
         */
        process_t *cur = sched_current();
        uint32_t ts[2];

        if (!cur || ebx != 0 || ecx == 0)
            return (uint32_t)-1;
        ts[0] = clock_unix_time();
        ts[1] = 0;
        if (uaccess_copy_to_user(cur, ecx, ts, sizeof(ts)) != 0)
            return (uint32_t)-1;
        return 0;
    }

    case SYS_MODLOAD: {
        /*
         * ebx = pointer to null-terminated module filename in user space.
         *
         * Looks up the file via the VFS to get its starting LBA and byte
         * size, then calls module_load() to read the ELF relocatable object
         * from disk, resolve symbols against kernel_exports[], apply
         * relocations, and call the module's module_init() function.
         *
         * Returns 0 on success, or a negative error code from module_load():
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
        uint32_t ino, sz;
        if (vfs_open(rpath, &ino, &sz) != 0) {
            klog("MODLOAD", "file not found");
            kfree(rpath);
            return (uint32_t)-1;
        }
        {
            uint32_t ret = (uint32_t)module_load(rpath, ino, sz);
            kfree(rpath);
            return ret;
        }
    }

    case SYS_EXIT:
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

    case SYS_BRK: {
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

    case SYS_MMAP: {
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

    case SYS_MUNMAP: {
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

    case SYS_MPROTECT: {
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

    case SYS_PIPE: {
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
        cur->open_files[rfd].u.pipe.pipe_idx = (uint32_t)pipe_idx;

        int wfd = fd_alloc(cur);
        if (wfd < 0) {
            fd_close_one(cur, (unsigned)rfd);
            klog("PIPE", "fd table full (write end)");
            return (uint32_t)-1;
        }
        cur->open_files[wfd].type           = FD_TYPE_PIPE_WRITE;
        cur->open_files[wfd].writable       = 1;
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

    case SYS_DUP2: {
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

    case SYS_KILL: {
        /*
         * ebx = target pid (> 0) or process group id (< 0)
         * ecx = signal number
         *
         * Positive `ebx` targets a single process.  Negative `ebx` targets
         * every process in that process group, mirroring kill(2).
         *
         * Returns 0 on success, -1 if signum is out of range.
         */
        int sig = (int)ecx;
        int32_t target = (int32_t)ebx;
        if (sig < 1 || sig >= NSIG)
            return (uint32_t)-1;
        if (target > 0) {
            sched_send_signal((uint32_t)target, sig);
            return 0;
        }
        if (target < 0) {
            sched_send_signal_to_pgid((uint32_t)(-target), sig);
            return 0;
        }
        return (uint32_t)-1;
    }

    case SYS_SIGACTION: {
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

    case SYS_SIGRETURN: {
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

    case SYS_SIGPROCMASK: {
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

        if (edx && uaccess_copy_to_user(cur, edx, &old, sizeof(old)) != 0)
            return (uint32_t)-1;

        if (ecx) {
            uint32_t newmask;
            if (uaccess_copy_from_user(cur, &newmask, ecx, sizeof(newmask)) != 0)
                return (uint32_t)-1;
            switch (ebx) {
            case 0: /* SIG_BLOCK   */ cur->sig_blocked = old | newmask;  break;
            case 1: /* SIG_UNBLOCK */ cur->sig_blocked = old & ~newmask; break;
            case 2: /* SIG_SETMASK */ cur->sig_blocked = newmask;        break;
            default: return (uint32_t)-1;
            }
            /* SIGKILL and SIGSTOP can never be blocked. */
            cur->sig_blocked &= ~((1u << SIGKILL) | (1u << SIGSTOP));
        }
        return 0;
    }

    case SYS_TCGETATTR: {
        /* ebx = fd, ecx = termios_t* (user pointer) */
        process_t *cur = sched_current();
        tty_t *tty = syscall_tty_from_fd(cur, ebx, 0);
        if (!tty) return (uint32_t)-1;
        if (uaccess_copy_to_user(cur, ecx, &tty->termios, sizeof(tty->termios)) != 0)
            return (uint32_t)-1;
        return 0;
    }

    case SYS_TCSETATTR: {
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

    case SYS_SETPGID: {
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

    case SYS_GETPGID: {
        /* ebx = pid (0 = self) → returns pgid */
        process_t *cur = sched_current();
        if (!cur) return (uint32_t)-1;
        if (ebx == 0 || ebx == cur->pid) return cur->pgid;
        process_t *target = sched_find_pid(ebx);
        if (!target) return (uint32_t)-1;
        return target->pgid;
    }

    case SYS_LSEEK: {
        /*
         * ebx = fd, ecx = offset (signed), edx = whence.
         * Repositions the file offset of an open fd.
         *   SEEK_SET (0) — set offset to ecx
         *   SEEK_CUR (1) — set offset to current + ecx
         *   SEEK_END (2) — set offset to file_size + ecx
         * Returns the new offset, or (uint32_t)-1 on error.
         */
        process_t *cur = sched_current();
        if (!cur || ebx >= MAX_FDS) return (uint32_t)-1;
        file_handle_t *fh = &cur->open_files[ebx];
        if (fh->type != FD_TYPE_FILE && fh->type != FD_TYPE_PROCFILE)
            return (uint32_t)-1;

        int32_t signed_off = (int32_t)ecx;
        int32_t new_off;
        uint32_t size = 0;

        if (fh->type == FD_TYPE_FILE)
            size = fh->u.file.size;
        else {
            if (procfs_file_size(fh->u.proc.kind, fh->u.proc.pid,
                                 fh->u.proc.index, &size) != 0)
                return (uint32_t)-1;
            fh->u.proc.size = size;
        }

        switch (edx) {
        case 0: /* SEEK_SET */
            new_off = signed_off;
            break;
        case 1: /* SEEK_CUR */
            new_off = (int32_t)((fh->type == FD_TYPE_FILE)
                                ? fh->u.file.offset
                                : fh->u.proc.offset) + signed_off;
            break;
        case 2: /* SEEK_END */
            new_off = (int32_t)size + signed_off;
            break;
        default:
            return (uint32_t)-1;
        }
        if (new_off < 0) return (uint32_t)-1;
        if (fh->type == FD_TYPE_FILE)
            fh->u.file.offset = (uint32_t)new_off;
        else
            fh->u.proc.offset = (uint32_t)new_off;
        return (uint32_t)new_off;
    }

    case SYS_GETPID:
        return sched_current_pid();

    case SYS_GETPPID:
        return sched_current_ppid();

    case SYS_WAITPID:
        /*
         * ebx = pid to wait for, ecx = option flags (WNOHANG | WUNTRACED).
         * Returns Linux-style encoded status:
         *   Exited:  (exit_code << 8)          — low 7 bits == 0
         *   Stopped: (stop_signal << 8) | 0x7F — low 7 bits == 0x7F
         *   0 with WNOHANG: child exists but has not changed state
         *  -1: no such process
         */
        return (uint32_t)sched_waitpid(ebx, (int)ecx);

    case SYS_TCSETPGRP: {
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

    case SYS_TCGETPGRP: {
        /* ebx = fd → fg_pgid or -1 */
        process_t *cur = sched_current();
        tty_t *tty = syscall_tty_from_fd(cur, ebx, 0);
        if (!tty) return (uint32_t)-1;
        return tty->fg_pgid;
    }

    default:
        klog_uint("KERN", "unknown syscall", eax);
        return (uint32_t)-1;
    }
}

#ifdef KTEST_ENABLED
int syscall_console_write_for_test(process_t *proc, const char *buf,
                                   uint32_t len)
{
    return syscall_write_console_bytes(proc, buf, len);
}
#endif
