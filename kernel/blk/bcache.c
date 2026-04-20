/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * bcache.c — shared 4096-byte block cache for the kernel's filesystems.
 *
 * A fixed pool of BCACHE_SLOTS entries, each holding one 4096-byte block
 * keyed by (device ops-table pointer, block number). Ordered by a doubly
 * linked list so the head is the most recently used entry and the tail is
 * the eviction target.
 *
 * The cache is write-back: bcache_write() populates the slot and marks it
 * dirty, deferring the 8-sector write until eviction or an explicit sync.
 * Journal code that needs disk-first durability calls bcache_write_through,
 * which performs the sector writes and keeps any existing cached copy in
 * sync (but not dirty).
 */

#include "bcache.h"
#include "blkdev.h"
#include "klog.h"
#include "kstring.h"
#include <stdint.h>

#define BCACHE_SLOTS 64u

typedef struct bcache_slot {
	const blkdev_ops_t *dev;
	uint32_t lba;
	uint32_t valid;
	uint32_t dirty;
	struct bcache_slot *prev;
	struct bcache_slot *next;
	uint8_t data[BCACHE_BLOCK_SIZE];
} bcache_slot_t;

static bcache_slot_t g_slots[BCACHE_SLOTS];
static bcache_slot_t *g_lru_head; /* most recently used */
static bcache_slot_t *g_lru_tail; /* eviction target */
static bcache_stats_t g_stats;

/* ── LRU list plumbing ──────────────────────────────────────────────────── */

static void lru_detach(bcache_slot_t *s)
{
	if (s->prev)
		s->prev->next = s->next;
	else
		g_lru_head = s->next;
	if (s->next)
		s->next->prev = s->prev;
	else
		g_lru_tail = s->prev;
	s->prev = s->next = 0;
}

static void lru_push_front(bcache_slot_t *s)
{
	s->prev = 0;
	s->next = g_lru_head;
	if (g_lru_head)
		g_lru_head->prev = s;
	g_lru_head = s;
	if (!g_lru_tail)
		g_lru_tail = s;
}

static void lru_push_back(bcache_slot_t *s)
{
	s->prev = g_lru_tail;
	s->next = 0;
	if (g_lru_tail)
		g_lru_tail->next = s;
	g_lru_tail = s;
	if (!g_lru_head)
		g_lru_head = s;
}

static void lru_touch(bcache_slot_t *s)
{
	if (g_lru_head == s)
		return;
	lru_detach(s);
	lru_push_front(s);
}

static void lru_demote(bcache_slot_t *s)
{
	if (g_lru_tail == s)
		return;
	lru_detach(s);
	lru_push_back(s);
}

/* ── Slot lookup and disk I/O helpers ───────────────────────────────────── */

static bcache_slot_t *find_slot(const blkdev_ops_t *dev, uint32_t lba)
{
	for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
		bcache_slot_t *s = &g_slots[i];
		if (s->valid && s->dev == dev && s->lba == lba)
			return s;
	}
	return 0;
}

static int disk_read_block(const blkdev_ops_t *dev, uint32_t lba, uint8_t *buf)
{
	for (uint32_t i = 0; i < BCACHE_SECS_PER_BLK; i++) {
		if (dev->read_sector(lba + i, buf + i * BLKDEV_SECTOR_SIZE) != 0)
			return -1;
	}
	return 0;
}

static int
disk_write_block(const blkdev_ops_t *dev, uint32_t lba, const uint8_t *buf)
{
	if (!dev->write_sector)
		return -1;
	for (uint32_t i = 0; i < BCACHE_SECS_PER_BLK; i++) {
		if (dev->write_sector(lba + i, buf + i * BLKDEV_SECTOR_SIZE) != 0)
			return -1;
	}
	return 0;
}

/* Write a dirty slot to disk. The slot stays valid and becomes clean. */
static int slot_writeback(bcache_slot_t *s)
{
	if (!s->valid || !s->dirty)
		return 0;
	if (disk_write_block(s->dev, s->lba, s->data) != 0)
		return -1;
	s->dirty = 0;
	g_stats.writebacks++;
	return 0;
}

/*
 * Acquire a slot for (dev, block). If the block is already cached, return
 * that slot. Otherwise evict the LRU entry (writing it back if dirty) and
 * return a fresh empty slot. The returned slot is moved to the MRU end and
 * its dev/block fields are set for the caller.
 */
static bcache_slot_t *acquire_slot(const blkdev_ops_t *dev, uint32_t lba)
{
	bcache_slot_t *s = find_slot(dev, lba);
	if (s) {
		lru_touch(s);
		return s;
	}

	/* Walk from the LRU tail toward the head looking for a usable victim.
     * Invalid and clean slots reuse for free; dirty slots need a successful
     * writeback. If writeback fails we'd otherwise leave a dirty slot pinned
     * at the tail and repeat the same failing write on every subsequent
     * miss — move it toward the head so the next candidate gets a turn. A
     * later bcache_sync, invalidate, or remount can still retry it. */
	bcache_slot_t *victim = 0;
	bcache_slot_t *c = g_lru_tail;
	while (c) {
		bcache_slot_t *prev = c->prev;
		if (!c->valid || !c->dirty) {
			victim = c;
			break;
		}
		if (slot_writeback(c) == 0) {
			victim = c;
			break;
		}
		lru_touch(c);
		c = prev;
	}
	if (!victim)
		return 0;

	if (victim->valid)
		g_stats.evictions++;

	victim->dev = dev;
	victim->lba = lba;
	victim->valid = 0; /* caller must populate and set valid=1 */
	victim->dirty = 0;
	lru_touch(victim);
	return victim;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int bcache_init(void)
{
	g_lru_head = g_lru_tail = 0;
	k_memset(&g_stats, 0, sizeof(g_stats));
	for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
		bcache_slot_t *s = &g_slots[i];
		s->dev = 0;
		s->lba = 0;
		s->valid = 0;
		s->dirty = 0;
		s->prev = s->next = 0;
		lru_push_front(s);
	}
	klog("BCACHE", "initialised");
	return 0;
}

int bcache_read(const blkdev_ops_t *dev, uint32_t lba, uint8_t *buf)
{
	if (!dev || !buf)
		return -1;

	bcache_slot_t *s = find_slot(dev, lba);
	if (s) {
		k_memcpy(buf, s->data, BCACHE_BLOCK_SIZE);
		lru_touch(s);
		g_stats.hits++;
		return 0;
	}

	s = acquire_slot(dev, lba);
	if (!s)
		return -1;
	if (disk_read_block(dev, lba, s->data) != 0) {
		s->dev = 0;
		s->valid = 0;
		s->dirty = 0;
		/* acquire_slot moved this to the head. A failed read leaves it empty
         * but taking a prime cache slot — demote so the next miss reuses it
         * before evicting live data. */
		lru_demote(s);
		return -1;
	}
	s->valid = 1;
	s->dirty = 0;
	k_memcpy(buf, s->data, BCACHE_BLOCK_SIZE);
	g_stats.misses++;
	return 0;
}

int bcache_write(const blkdev_ops_t *dev, uint32_t lba, const uint8_t *buf)
{
	if (!dev || !buf)
		return -1;

	bcache_slot_t *s = acquire_slot(dev, lba);
	if (!s)
		return -1;
	k_memcpy(s->data, buf, BCACHE_BLOCK_SIZE);
	s->valid = 1;
	s->dirty = 1;
	return 0;
}

int bcache_write_through(const blkdev_ops_t *dev,
                         uint32_t lba,
                         const uint8_t *buf)
{
	if (!dev || !buf)
		return -1;

	if (disk_write_block(dev, lba, buf) != 0)
		return -1;
	g_stats.write_through++;

	/* Keep any cached copy coherent with what just hit the platter. */
	bcache_slot_t *s = find_slot(dev, lba);
	if (s) {
		k_memcpy(s->data, buf, BCACHE_BLOCK_SIZE);
		s->dirty = 0;
		lru_touch(s);
	}
	return 0;
}

int bcache_sync(const blkdev_ops_t *dev)
{
	int rc = 0;
	for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
		bcache_slot_t *s = &g_slots[i];
		if (!s->valid || !s->dirty)
			continue;
		if (dev && s->dev != dev)
			continue;
		if (slot_writeback(s) != 0)
			rc = -1;
	}
	return rc;
}

void bcache_invalidate(const blkdev_ops_t *dev, uint32_t lba)
{
	bcache_slot_t *s = find_slot(dev, lba);
	if (!s)
		return;
	s->valid = 0;
	s->dirty = 0;
	s->dev = 0;
	/* Move to the eviction end so a future miss reuses it first. */
	lru_demote(s);
}

void bcache_get_stats(bcache_stats_t *out)
{
	if (out)
		*out = g_stats;
}

void bcache_reset_stats(void)
{
	k_memset(&g_stats, 0, sizeof(g_stats));
}
