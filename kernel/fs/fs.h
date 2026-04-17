/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef FS_H
#define FS_H

#include <stdint.h>

/*
 * DUFS v3 — an inode-based filesystem with Linux-compatible limits.
 *
 * Disk layout (all I/O via 512-byte ATA sectors; data blocks are 4096 bytes = 8 sectors):
 *
 *   LBA 0          unused (always — LBA 0 is the "not allocated" sentinel)
 *   LBA 1          superblock (first 28 bytes used; rest zeroed)
 *   LBA 2–9        inode bitmap  (1 block = 4096 bytes → 32,768 inode bits)
 *   LBA 10–17      block bitmap  (1 block = 4096 bytes → 32,768 block bits → 128 MB)
 *   LBA 18–273     inode table   (1024 inodes × 128 bytes = 256 sectors)
 *   LBA 274+       data blocks   (block N starts at LBA 274 + N×8)
 *
 * Inode 0 is reserved (never allocated).  Inode 1 is always the root directory.
 * A block pointer value of 0 means "not allocated" — LBA 0 is always unused.
 *
 * Directory data is stored in ordinary data blocks as a flat array of
 * dufs_dirent_t records (260 bytes each, 15 per 4096-byte block).  A directory
 * entry with inode == 0 is an empty / deleted slot.
 *
 * File size limits (4096-byte blocks):
 *   direct[0..11]    12 blocks × 4096 bytes  =    49,152 bytes
 *   indirect         1024 blocks × 4096 bytes = 4,194,304 bytes
 *   double_indirect  1024 × 1024 × 4096 bytes ≈ 4 GB
 *   Total (practical limit): the 50 MB disk
 */

#define DUFS_MAGIC             0x44554603u  /* "DUF\x03" — v3                   */
#define DUFS_BLOCK_SIZE        4096         /* bytes per data block              */
#define DUFS_SECS_PER_BLOCK    8            /* DUFS_BLOCK_SIZE / 512             */
#define DUFS_MAX_NAME          256          /* name field size incl. NUL (255-char max) */
#define DUFS_INODE_COUNT       1024         /* total inode slots                 */
#define DUFS_INODE_SIZE        128          /* bytes per inode                   */
#define DUFS_INODES_PER_SECTOR 4            /* 512 / DUFS_INODE_SIZE             */
#define DUFS_DIRENT_SIZE       260          /* sizeof(dufs_dirent_t)             */
#define DUFS_DIRENTS_PER_BLOCK 15           /* DUFS_BLOCK_SIZE / DUFS_DIRENT_SIZE */
#define DUFS_DIRECT_BLOCKS     12           /* direct block pointers per inode   */
#define DUFS_INDIR_ENTRIES     1024         /* LBA entries per indirect block (4096/4) */

#define DUFS_INODE_BITMAP_LBA  2            /* start LBA of inode bitmap block   */
#define DUFS_BLOCK_BITMAP_LBA  10           /* start LBA of block bitmap block   */
#define DUFS_INODE_TABLE_LBA   18           /* start LBA of inode table          */
#define DUFS_DATA_LBA          274          /* start LBA of first data block     */

#define DUFS_TYPE_FREE  0
#define DUFS_TYPE_FILE  1
#define DUFS_TYPE_DIR   2
#define DUFS_TYPE_SYMLINK 3

/* ── On-disk structures ─────────────────────────────────────────────────── */

/* Superblock — occupies the first 28 bytes of LBA 1; rest zeroed. */
typedef struct {
    uint32_t magic;             /* DUFS_MAGIC                                    */
    uint32_t total_sectors;     /* total sectors in the disk image               */
    uint32_t inode_count;       /* number of inode slots (DUFS_INODE_COUNT)      */
    uint32_t inode_bitmap_lba;  /* start LBA of the inode bitmap block           */
    uint32_t block_bitmap_lba;  /* start LBA of the block bitmap block           */
    uint32_t inode_table_lba;   /* start LBA of the inode table                  */
    uint32_t data_lba;          /* start LBA of the first data block             */
    uint8_t  pad[512 - 28];     /* zeroed padding to fill the sector             */
} __attribute__((packed)) dufs_super_t;

/*
 * On-disk inode — 128 bytes, 4 per sector.
 *
 *   direct[0..11]   12 × 4096 bytes =     48 KB direct
 *   indirect        1024 × 4096 bytes =    4 MB single-indirect
 *   double_indirect 1024² × 4096 bytes ≈   4 GB double-indirect
 */
typedef struct {
    uint16_t type;              /* DUFS_TYPE_FILE, DUFS_TYPE_DIR, or DUFS_TYPE_FREE */
    uint16_t link_count;        /* number of directory entries referencing this inode */
    uint32_t size;              /* file size in bytes (0 for directories)            */
    uint32_t block_count;       /* number of data blocks allocated                   */
    uint32_t mtime;             /* last-modified Unix timestamp (UTC seconds)        */
    uint32_t atime;             /* last-accessed Unix timestamp (UTC seconds)        */
    uint32_t direct[12];        /* direct block LBAs (0 = unallocated)               */
    uint32_t indirect;          /* single-indirect block LBA (0 = none)              */
    uint32_t double_indirect;   /* double-indirect block LBA (0 = none)              */
    uint8_t  pad[52];           /* zeroed padding to reach 128 bytes                 */
} __attribute__((packed)) dufs_inode_t;

/* On-disk directory entry — 260 bytes, 15 per 4096-byte block. */
typedef struct {
    char     name[256];         /* null-terminated filename; max 255 chars           */
    uint32_t inode;             /* inode number (0 = empty slot)                     */
} __attribute__((packed)) dufs_dirent_t;

/* ── Public API ─────────────────────────────────────────────────────────── */

void dufs_register(void);
int  dufs_use_device(const char *blkdev_name);
int  fs_init(void);
int  fs_open(const char *path, uint32_t *inode_out, uint32_t *size_out);
int  fs_read(uint32_t inode_num, uint32_t offset, uint8_t *buf, uint32_t count);
int  fs_write(uint32_t inode_num, uint32_t offset, const uint8_t *buf, uint32_t count);
int  fs_truncate(uint32_t inode_num, uint32_t size);
int  fs_create(const char *path);
int  fs_flush_inode(uint32_t inode_num);
int  fs_unlink(const char *path);
int  fs_mkdir(const char *path);
int  fs_rmdir(const char *path);
int  fs_rename(const char *oldpath, const char *newpath);
int  fs_link(const char *oldpath, const char *newpath, uint32_t follow);
int  fs_symlink(const char *target, const char *linkpath);
int  fs_readlink(const char *path, char *buf, uint32_t bufsz);
int  fs_list(const char *path, char *buf, uint32_t bufsz);
/* fs_stat is not exported directly — accessed via the VFS ops table only. */

#endif
