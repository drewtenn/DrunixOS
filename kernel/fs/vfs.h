/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>

/*
 * Virtual Filesystem Switch (VFS).
 *
 * Filesystems register an ops-table once during init.  The namespace is a
 * mount tree rooted at "/": each mount point shadows the directory entry at
 * its path and path resolution follows the deepest mounted prefix.
 */

#define VFS_FS_NAME_MAX  8   /* max filesystem name length including NUL */
#define VFS_MAX_FS       4   /* max registered filesystems */
#define VFS_MAX_MOUNTS   8   /* max mount points in the namespace */
#define VFS_DEV_NAME_MAX 12  /* max synthetic device name incl. NUL */

/*
 * File/directory metadata returned by vfs_stat().
 * type: 1 = regular file or file-like node, 2 = directory, 3 = symlink.
 * mtime is a Unix timestamp in UTC seconds.
 */
typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t link_count;
    uint32_t mtime;
} vfs_stat_t;

typedef enum {
    VFS_NODE_NONE    = 0,
    VFS_NODE_FILE    = 1,
    VFS_NODE_DIR     = 2,
    VFS_NODE_CHARDEV = 3,
    VFS_NODE_TTY     = 4,
    VFS_NODE_PROCFILE = 5,
    VFS_NODE_SYMLINK = 6,
    VFS_NODE_BLOCKDEV = 7,
} vfs_node_type_t;

/*
 * Result of resolving a path through the VFS mount tree.
 *
 * For VFS_NODE_FILE, inode_num and size are valid.
 * For VFS_NODE_CHARDEV and VFS_NODE_BLOCKDEV, dev_name names the registered
 * kernel device entry (for example, "stdin" or "sda1").
 * For VFS_NODE_TTY, dev_id is the tty_table[] index.
 */
typedef struct {
    uint32_t type;
    uint32_t inode_num;
    uint32_t mount_id;
    uint32_t size;
    uint32_t dev_id;
    char     dev_name[VFS_DEV_NAME_MAX];
    uint32_t proc_kind;
    uint32_t proc_pid;
    uint32_t proc_index;
} vfs_node_t;

typedef struct {
    uint32_t mount_id;
    uint32_t inode_num;
} vfs_file_ref_t;

typedef struct {
    void *ctx;

    /*
     * init: initialise the filesystem implementation before it is mounted.
     * Returns 0 on success, negative on error.
     */
    int (*init)(void *ctx);

    /*
     * open: look up a file by name.
     * On success writes the file's inode number to *inode_out and its size
     * in bytes to *size_out, then returns 0.  Returns -1 if not found.
     */
    int (*open)(void *ctx, const char *name,
                uint32_t *inode_out, uint32_t *size_out);

    /*
     * getdents: enumerate null-terminated names into buf.
     * path == NULL lists root-level entries; directory names get a '/' suffix.
     * path != NULL lists the contents of the named subdirectory.
     * Returns the number of bytes written (including NUL terminators).
     * Stops early if the buffer would overflow.
     */
    int (*getdents)(void *ctx, const char *path, char *buf, uint32_t bufsz);

    /*
     * create: create a new file or truncate an existing one by path.
     * Returns the file's inode number (>= 0) on success, or -1 on error.
     */
    int (*create)(void *ctx, const char *path);

    /*
     * unlink: delete a file by path.
     * Returns 0 on success, -1 if not found or on I/O error.
     */
    int (*unlink)(void *ctx, const char *path);

    /*
     * mkdir: create a directory at the root level.
     * Returns 0 on success, -1 on error (duplicate, invalid name, or full).
     * May be NULL if the filesystem does not support directories.
     */
    int (*mkdir)(void *ctx, const char *name);

    /*
     * rmdir: remove an empty directory at the root level.
     * Returns 0 on success, -1 if not found, not empty, or on I/O error.
     * May be NULL if the filesystem does not support directory removal.
     */
    int (*rmdir)(void *ctx, const char *name);

    /*
     * rename: atomically rename or move a file or directory.
     * oldpath and newpath use the same "leaf" or "dir/leaf" format as open.
     * If newpath names an existing file it is replaced; if it names an
     * existing directory the call fails.
     * Returns 0 on success, -1 on error.
     * May be NULL if the filesystem does not support rename.
     */
    int (*rename)(void *ctx, const char *oldpath, const char *newpath);

    /*
     * link: create a hardlink from newpath to oldpath.
     * symlink: create a symbolic link at linkpath containing target.
     * readlink: copy the symbolic-link target to buf and return byte count.
     */
    int (*link)(void *ctx, const char *oldpath, const char *newpath,
                uint32_t follow);
    int (*symlink)(void *ctx, const char *target, const char *linkpath);
    int (*readlink)(void *ctx, const char *path, char *buf, uint32_t bufsz);

    /*
     * stat: retrieve metadata for the file or directory at path.
     * Fills *st and returns 0 on success, -1 if not found.
     * May be NULL if the filesystem does not support stat.
     */
    int (*stat)(void *ctx, const char *path, vfs_stat_t *st);
    int (*lstat)(void *ctx, const char *path, vfs_stat_t *st);

    int (*read)(void *ctx, uint32_t inode_num, uint32_t offset,
                uint8_t *buf, uint32_t count);
    int (*write)(void *ctx, uint32_t inode_num, uint32_t offset,
                 const uint8_t *buf, uint32_t count);
    int (*truncate)(void *ctx, uint32_t inode_num, uint32_t size);
    int (*flush)(void *ctx, uint32_t inode_num);
} fs_ops_t;

/*
 * vfs_register: register a filesystem ops-table under a short name.
 * Returns 0 on success, -1 if the registry is full.
 */
int vfs_register(const char *name, const fs_ops_t *ops);

/*
 * vfs_mount: attach a registered filesystem at mount_path.
 * mount_path may be "/", "", or NULL for the root mount.
 * Returns 0 on success, -1 on namespace / registry errors, or the init()
 * return value on init failure.
 */
int vfs_mount(const char *mount_path, const char *fs_name);

/*
 * vfs_reset: clear the VFS registry and mount tree.
 * Intended for early boot and test isolation.
 */
void vfs_reset(void);

/*
 * vfs_resolve: walk the mount tree and describe the resulting node.
 * Returns 0 on success, -1 if the path does not exist.
 */
int vfs_resolve(const char *path, vfs_node_t *node_out);

/*
 * vfs_open_file / vfs_getdents / vfs_create / vfs_unlink / vfs_mkdir:
 * operate on the mounted namespace.  Return -1 if the path is invalid, does
 * not exist, or the selected backend does not support the requested operation.
 *
 * vfs_open_file succeeds only for regular files backed by a real filesystem
 * and writes a mount-qualified file reference to *ref_out.
 * vfs_create returns the new inode number on success, or -1 on error.
 */
int vfs_open_file(const char *path, vfs_file_ref_t *ref_out,
                  uint32_t *size_out);
int vfs_read(vfs_file_ref_t ref, uint32_t offset,
             uint8_t *buf, uint32_t count);
int vfs_write(vfs_file_ref_t ref, uint32_t offset,
              const uint8_t *buf, uint32_t count);
int vfs_truncate(vfs_file_ref_t ref, uint32_t size);
int vfs_flush(vfs_file_ref_t ref);
int vfs_getdents(const char *path, char *buf, uint32_t bufsz);
int vfs_create(const char *path);
int vfs_unlink(const char *path);
int vfs_mkdir(const char *name);
int vfs_rmdir(const char *name);
int vfs_rename(const char *oldpath, const char *newpath);
int vfs_link(const char *oldpath, const char *newpath, uint32_t follow);
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, uint32_t bufsz);
int vfs_stat(const char *path, vfs_stat_t *st);
int vfs_lstat(const char *path, vfs_stat_t *st);

#endif
