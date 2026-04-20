/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ext3_internal.h - private ext2/ext3 backend structures and helpers.
 *
 * This header is shared only by the ext3 backend implementation files. It
 * contains on-disk layout structs, JBD/ext3 constants, module-global state,
 * and low-level helper contracts used across ext3 backend files.
 */

#ifndef EXT3_INTERNAL_H
#define EXT3_INTERNAL_H

#include "blkdev.h"
#include "vfs.h"
#include <stdint.h>

#define EXT3_SUPER_OFFSET 1024u
#define EXT3_SUPER_MAGIC 0xEF53u
#define EXT3_ROOT_INO 2u
#define EXT3_NAME_MAX 255u
#define EXT3_N_BLOCKS 15u
#define EXT3_NDIR_BLOCKS 12u
#define EXT3_FT_REG_FILE 1u
#define EXT3_FT_DIR 2u
#define EXT3_FT_SYMLINK 7u
#define EXT3_S_IFMT 0xF000u
#define EXT3_S_IFREG 0x8000u
#define EXT3_S_IFDIR 0x4000u
#define EXT3_S_IFLNK 0xA000u
#define EXT3_FEATURE_COMPAT_HAS_JOURNAL 0x0004u
#define EXT3_FEATURE_INCOMPAT_FILETYPE 0x0002u
#define EXT3_FEATURE_INCOMPAT_RECOVER 0x0004u
#define EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001u
#define EXT3_FEATURE_RO_COMPAT_LARGE_FILE 0x0002u
#define JBD_MAGIC 0xC03B3998u
#define JBD_DESCRIPTOR_BLOCK 1u
#define JBD_COMMIT_BLOCK 2u
#define JBD_SUPERBLOCK_V1 3u
#define JBD_SUPERBLOCK_V2 4u
#define JBD_REVOKE_BLOCK 5u
#define JBD_FLAG_ESCAPE 1u
#define JBD_FLAG_SAME_UUID 2u
#define JBD_FLAG_LAST_TAG 8u
#define JBD_FEATURE_INCOMPAT_REVOKE 0x00000001u
#define EXT3_REPLAY_MAX_BLOCKS 32u
#define EXT3_REPLAY_MAX_TAGS 64u
#define EXT3_REPLAY_MAX_REVOKES 128u
#define EXT3_TX_MAX_BLOCKS 128u

typedef struct {
	uint32_t inodes_count;
	uint32_t blocks_count;
	uint32_t r_blocks_count;
	uint32_t free_blocks_count;
	uint32_t free_inodes_count;
	uint32_t first_data_block;
	uint32_t log_block_size;
	uint32_t log_frag_size;
	uint32_t blocks_per_group;
	uint32_t frags_per_group;
	uint32_t inodes_per_group;
	uint32_t mtime;
	uint32_t wtime;
	uint16_t mnt_count;
	uint16_t max_mnt_count;
	uint16_t magic;
	uint16_t state;
	uint16_t errors;
	uint16_t minor_rev_level;
	uint32_t lastcheck;
	uint32_t checkinterval;
	uint32_t creator_os;
	uint32_t rev_level;
	uint16_t def_resuid;
	uint16_t def_resgid;
	uint32_t first_ino;
	uint16_t inode_size;
	uint16_t block_group_nr;
	uint32_t feature_compat;
	uint32_t feature_incompat;
	uint32_t feature_ro_compat;
	uint8_t uuid[16];
	char volume_name[16];
	char last_mounted[64];
	uint32_t algorithm_usage_bitmap;
	uint8_t prealloc_blocks;
	uint8_t prealloc_dir_blocks;
	uint16_t reserved_gdt_blocks;
	uint8_t journal_uuid[16];
	uint32_t journal_inum;
	uint32_t journal_dev;
	uint32_t last_orphan;
} __attribute__((packed)) ext3_super_t;

typedef struct {
	uint32_t block_bitmap;
	uint32_t inode_bitmap;
	uint32_t inode_table;
	uint16_t free_blocks_count;
	uint16_t free_inodes_count;
	uint16_t used_dirs_count;
	uint16_t pad;
	uint8_t reserved[12];
} __attribute__((packed)) ext3_group_desc_t;

typedef struct {
	uint16_t mode;
	uint16_t uid;
	uint32_t size;
	uint32_t atime;
	uint32_t ctime;
	uint32_t mtime;
	uint32_t dtime;
	uint16_t gid;
	uint16_t links_count;
	uint32_t blocks;
	uint32_t flags;
	uint32_t osd1;
	uint32_t block[EXT3_N_BLOCKS];
	uint32_t generation;
	uint32_t file_acl;
	uint32_t dir_acl;
	uint32_t faddr;
	uint8_t osd2[12];
} __attribute__((packed)) ext3_inode_t;

typedef struct {
	uint32_t inode;
	uint16_t rec_len;
	uint8_t name_len;
	uint8_t file_type;
} __attribute__((packed)) ext3_dirent_t;

extern const blkdev_ops_t *g_dev;
extern ext3_super_t g_super;
extern ext3_group_desc_t g_bg;
extern uint32_t g_block_size;
extern uint32_t g_sectors_per_block;
extern uint32_t g_bgdt_block;
extern uint32_t g_writable;
extern uint32_t g_needs_recovery;
extern uint32_t g_block_alloc_cursor;
extern uint32_t g_inode_alloc_cursor;
extern uint32_t g_tx_counts_dirty;

typedef struct {
	uint32_t fs_block;
	uint8_t *data;
} ext3_overlay_block_t;

typedef struct {
	uint32_t fs_block;
	uint8_t *data;
} ext3_tx_block_t;

extern ext3_overlay_block_t g_overlay[EXT3_REPLAY_MAX_BLOCKS];
extern uint32_t g_overlay_count;
extern ext3_tx_block_t g_tx[EXT3_TX_MAX_BLOCKS];
extern uint32_t g_tx_count;
extern uint32_t g_tx_depth;
extern uint32_t g_tx_failed;
extern ext3_super_t g_tx_super_before;
extern uint32_t g_tx_has_snapshot;

uint16_t be16(const uint8_t *p);
uint32_t be32(const uint8_t *p);
void put_be32(uint8_t *p, uint32_t v);

int ext3_read_bytes(uint32_t byte_off, uint8_t *buf, uint32_t count);
int ext3_write_bytes(uint32_t byte_off, const uint8_t *buf, uint32_t count);
int ext3_read_disk_block(uint32_t block, uint8_t *buf);
int ext3_write_disk_block(uint32_t block, const uint8_t *buf);
int ext3_read_block(uint32_t block, uint8_t *buf);
int ext3_write_block(uint32_t block, const uint8_t *buf);
int ext3_can_mutate(void);
void ext3_tx_reset(void);
int ext3_tx_begin(void);
int ext3_tx_end(int rc);
int ext3_load_bg(void);
int ext3_flush_super_image_raw(const ext3_super_t *super);
int ext3_flush_super_raw(void);
int ext3_flush_super(void);
int ext3_flush_bg(void);
int ext3_overlay_put(uint32_t fs_block, const uint8_t *data);
void ext3_overlay_clear(void);
int ext3_read_inode(uint32_t ino, ext3_inode_t *out);
int ext3_write_inode(uint32_t ino, const ext3_inode_t *in);
void ext3_inode_blocks_add(ext3_inode_t *in, uint32_t fs_blocks);
void ext3_inode_blocks_sub(ext3_inode_t *in, uint32_t fs_blocks);
uint32_t ext3_alloc_block(void);
int ext3_free_block(uint32_t block);
uint32_t ext3_alloc_inode(uint32_t is_dir);
int ext3_free_inode(uint32_t ino, uint32_t was_dir);
uint32_t ext3_inode_size(const ext3_inode_t *in);
uint32_t ext3_block_index(const ext3_inode_t *in, uint32_t logical);
uint32_t ext3_ensure_block(ext3_inode_t *in, uint32_t logical);
int ext3_free_inode_payload(ext3_inode_t *in);
int ext3_free_inode_blocks(ext3_inode_t *in);
int ext3_truncate_blocks(ext3_inode_t *in, uint32_t keep_blocks);
uint32_t ext3_dir_rec_len(uint32_t name_len);
int ext3_dir_lookup(uint32_t dir_ino,
                    const char *name,
                    uint32_t *ino_out,
                    uint8_t *type_out);
int ext3_resolve(const char *path, uint32_t follow_final, uint32_t *ino_out);
int ext3_open(void *ctx,
              const char *path,
              uint32_t *inode_out,
              uint32_t *size_out);
int ext3_getdents(void *ctx, const char *path, char *buf, uint32_t bufsz);
int ext3_readlink(void *ctx, const char *path, char *buf, uint32_t bufsz);
int ext3_stat(void *ctx, const char *path, vfs_stat_t *st);
int ext3_lstat(void *ctx, const char *path, vfs_stat_t *st);
int ext3_read(void *ctx,
              uint32_t inode_num,
              uint32_t offset,
              uint8_t *buf,
              uint32_t size);
int ext3_write(void *ctx,
               uint32_t inode_num,
               uint32_t offset,
               const uint8_t *buf,
               uint32_t size);
int ext3_truncate(void *ctx, uint32_t inode_num, uint32_t size);
int ext3_create(void *ctx, const char *path);
int ext3_unlink(void *ctx, const char *path);
int ext3_mkdir(void *ctx, const char *path);
int ext3_rmdir(void *ctx, const char *path);
int ext3_rename(void *ctx, const char *oldpath, const char *newpath);
int ext3_link(void *ctx,
              const char *oldpath,
              const char *newpath,
              uint32_t follow);
int ext3_symlink(void *ctx, const char *target, const char *linkpath);
int ext3_replay_journal(void);
int ext3_checkpoint_replay(void);

#endif
