/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>
#include "blkdev.h"

/*
 * Shared 4096-byte block cache sitting between filesystems and the block
 * device registry. Each cache entry represents the 4096 bytes starting at
 * a specific LBA on a specific device — the key is (device ops-table
 * pointer, starting LBA). Callers are expected to use LBAs that are the
 * start of a 4096-byte range they care about: ext3 passes fs_block * 8,
 * DUFS passes the 8-sector LBAs it already tracks (which are not all 8-
 * aligned from LBA 0 — its inode bitmap lives at LBA 2, for example).
 *
 * Policy: true LRU, fixed 64-slot pool, write-back for regular writes.
 * Journal code that must bypass the cache's dirty queue uses the explicit
 * write-through entry point.
 */

#define BCACHE_BLOCK_SIZE   4096u
#define BCACHE_SECS_PER_BLK 8u

int bcache_init(void);

/* Fill buf with BCACHE_BLOCK_SIZE bytes from (dev, lba). Returns 0 on
 * success or a non-zero error. */
int bcache_read(const blkdev_ops_t *dev, uint32_t lba, uint8_t *buf);

/* Write-back: the block is copied into the cache and marked dirty. The
 * actual disk write happens on eviction or an explicit sync. */
int bcache_write(const blkdev_ops_t *dev, uint32_t lba, const uint8_t *buf);

/* Write-through: write to disk immediately. Any cached copy of the same
 * range is updated to match (and its dirty flag cleared). Used for journal
 * blocks whose durability is required before a later commit. */
int bcache_write_through(const blkdev_ops_t *dev, uint32_t lba,
                         const uint8_t *buf);

/* Flush every dirty block belonging to dev to disk. Pass NULL to flush
 * every dirty block across all devices. */
int bcache_sync(const blkdev_ops_t *dev);

/* Drop a cached entry without writing it back. Used when a block's on-disk
 * contents have been rewritten by a path that bypassed the cache. */
void bcache_invalidate(const blkdev_ops_t *dev, uint32_t lba);

/* Telemetry, exposed for tests and for the chapter's measurement story. */
typedef struct {
    uint32_t hits;
    uint32_t misses;
    uint32_t writebacks;
    uint32_t write_through;
    uint32_t evictions;
} bcache_stats_t;

void bcache_get_stats(bcache_stats_t *out);
void bcache_reset_stats(void);

#endif
