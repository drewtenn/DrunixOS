/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * journal.c - ext3 JBD transaction and replay support.
 *
 * This file owns the small JBD subset Drunix needs for ext3 recovery and
 * metadata transactions: journal block I/O, descriptor/revoke parsing, replay
 * overlays, checkpointing, and commit/rollback bookkeeping.
 */

#include "ext3_internal.h"
#include "bcache.h"
#include "kheap.h"
#include "klog.h"
#include "kstring.h"
#include <stdint.h>

typedef struct {
	uint32_t fs_block;
	uint32_t journal_block;
	uint32_t flags;
} jbd_tag_t;

static uint32_t jbd_next(uint32_t cur, uint32_t first, uint32_t maxlen)
{
	cur++;
	if (cur >= maxlen)
		cur = first;
	return cur;
}

static int ext3_journal_read_block(const ext3_inode_t *journal,
                                   uint32_t logical,
                                   uint8_t *buf)
{
	uint32_t phys = ext3_block_index(journal, logical);

	if (phys == 0)
		return -1;
	return ext3_read_disk_block(phys, buf);
}

static int ext3_journal_write_block(const ext3_inode_t *journal,
                                    uint32_t logical,
                                    const uint8_t *buf)
{
	uint32_t phys = ext3_block_index(journal, logical);

	if (phys == 0 || !g_dev || !g_dev->write_sector)
		return -1;
	/* Journal blocks bypass the cache's write-back queue: the whole point of
     * the JBD protocol is that descriptor/data/commit blocks are on the
     * platter before we act on them. bcache_write_through writes the sectors
     * and keeps any existing cached copy coherent with the new contents. */
	return bcache_write_through(g_dev, phys * g_sectors_per_block, buf);
}

static void jbd_write_header(uint8_t *blk, uint32_t type, uint32_t seq)
{
	put_be32(blk, JBD_MAGIC);
	put_be32(blk + 4u, type);
	put_be32(blk + 8u, seq);
}

static int
jbd_update_super(const ext3_inode_t *journal, uint32_t start, uint32_t seq)
{
	uint8_t *blk = (uint8_t *)kmalloc(g_block_size);
	int rc;

	if (!blk)
		return -1;
	if (ext3_journal_read_block(journal, 0, blk) != 0) {
		kfree(blk);
		return -1;
	}
	if (seq != 0)
		put_be32(blk + 24u, seq);
	put_be32(blk + 28u, start);
	rc = ext3_journal_write_block(journal, 0, blk);
	kfree(blk);
	return rc;
}

static int jbd_write_transaction(const ext3_inode_t *journal,
                                 uint32_t seq,
                                 uint32_t maxlen)
{
	uint8_t *desc;
	uint8_t *data;
	uint32_t off = 12u;

	if (g_tx_count == 0)
		return 0;
	/* Bound against the JBD superblock's s_maxlen — the inode size can round
     * up past the journal's logical length on some mke2fs images, and writing
     * beyond maxlen stomps whatever follows the journal on disk. */
	if (maxlen < 3u || g_tx_count + 2u >= maxlen)
		return -1;
	if (12u + g_tx_count * 8u > g_block_size)
		return -1;

	desc = (uint8_t *)kmalloc(g_block_size);
	data = (uint8_t *)kmalloc(g_block_size);
	if (!desc || !data) {
		if (desc)
			kfree(desc);
		if (data)
			kfree(data);
		return -1;
	}

	k_memset(desc, 0, g_block_size);
	jbd_write_header(desc, JBD_DESCRIPTOR_BLOCK, seq);
	for (uint32_t i = 0; i < g_tx_count; i++) {
		uint32_t flags = JBD_FLAG_SAME_UUID;
		if (i + 1u == g_tx_count)
			flags |= JBD_FLAG_LAST_TAG;
		if (be32(g_tx[i].data) == JBD_MAGIC)
			flags |= JBD_FLAG_ESCAPE;
		put_be32(desc + off, g_tx[i].fs_block);
		put_be32(desc + off + 4u, flags);
		off += 8u;
	}
	if (ext3_journal_write_block(journal, 1u, desc) != 0) {
		kfree(desc);
		kfree(data);
		return -1;
	}

	for (uint32_t i = 0; i < g_tx_count; i++) {
		k_memcpy(data, g_tx[i].data, g_block_size);
		if (be32(data) == JBD_MAGIC)
			k_memset(data, 0, 4u);
		if (ext3_journal_write_block(journal, 2u + i, data) != 0) {
			kfree(desc);
			kfree(data);
			return -1;
		}
	}

	k_memset(desc, 0, g_block_size);
	jbd_write_header(desc, JBD_COMMIT_BLOCK, seq);
	if (ext3_journal_write_block(journal, 2u + g_tx_count, desc) != 0) {
		kfree(desc);
		kfree(data);
		return -1;
	}

	kfree(desc);
	kfree(data);
	return 0;
}

static int ext3_checkpoint_tx(void)
{
	for (uint32_t i = 0; i < g_tx_count; i++) {
		if (ext3_write_disk_block(g_tx[i].fs_block, g_tx[i].data) != 0)
			return -1;
	}
	return 0;
}

static int ext3_commit_tx(void)
{
	ext3_inode_t journal;
	ext3_super_t recover_super;
	uint8_t *jsb;
	uint32_t seq;
	uint32_t jmaxlen;

	if (g_tx_failed)
		return -1;
	/* Flush the deferred BG counter update into the tx buffer. The super is
     * flushed below anyway (for the RECOVER flag), so it already picks up
     * the updated free counts from g_super. */
	if (g_tx_counts_dirty) {
		if (ext3_flush_bg() != 0)
			return -1;
		g_tx_counts_dirty = 0;
	}
	if (g_tx_count == 0)
		return 0;
	if (g_super.journal_dev != 0 || g_super.journal_inum == 0)
		return -1;
	if (ext3_read_inode(g_super.journal_inum, &journal) != 0)
		return -1;

	jsb = (uint8_t *)kmalloc(g_block_size);
	if (!jsb)
		return -1;
	if (ext3_journal_read_block(&journal, 0, jsb) != 0) {
		kfree(jsb);
		return -1;
	}
	seq = be32(jsb + 24u);
	if (seq == 0)
		seq = 1u;
	jmaxlen = be32(jsb + 16u);
	kfree(jsb);
	if (jmaxlen == 0)
		return -1;

	recover_super = g_tx_has_snapshot ? g_tx_super_before : g_super;
	recover_super.feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
	if (ext3_flush_super_image_raw(&recover_super) != 0)
		return -1;

	g_super.feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
	if (ext3_flush_super() != 0)
		return -1;
	if (jbd_update_super(&journal, 1u, seq) != 0)
		return -1;
	if (jbd_write_transaction(&journal, seq, jmaxlen) != 0)
		return -1;
	if (ext3_checkpoint_tx() != 0)
		return -1;
	/* Checkpoint writes land in the cache as dirty entries. They must be on
     * the platter before we clear the journal's start field — otherwise a
     * crash here would leave the journal saying "no recovery needed" while
     * the home blocks still hold their pre-transaction contents. */
	if (bcache_sync(g_dev) != 0)
		return -1;
	if (jbd_update_super(&journal, 0u, seq + 1u) != 0)
		return -1;
	g_super.feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
	return ext3_flush_super_raw();
}

int ext3_tx_begin(void)
{
	if (!g_writable || g_overlay_count != 0)
		return -1;
	if (g_tx_depth == 0) {
		ext3_tx_reset();
		g_tx_super_before = g_super;
		g_tx_has_snapshot = 1;
	}
	g_tx_depth++;
	return 0;
}

int ext3_tx_end(int rc)
{
	int commit_rc = 0;

	if (g_tx_depth == 0)
		return rc;
	if (g_tx_depth > 1) {
		g_tx_depth--;
		return rc;
	}

	if (rc == 0 || rc > 0)
		commit_rc = ext3_commit_tx();
	if (commit_rc != 0) {
		/* Commit may have stopped at any step: before the descriptor, between
         * descriptor and commit record, mid-checkpoint, or after checkpoint
         * but before the final RECOVER-clear. Disk state is handled by replay
         * on the next mount, but in-memory g_super and g_bg have already
         * absorbed the aborted tx's allocations/frees. Restore them from the
         * snapshot (and reload g_bg from disk once the tx buffer is cleared)
         * and force the FS read-only so subsequent calls surface the failure
         * rather than proceeding from drifted free counts. */
		if (g_tx_has_snapshot)
			g_super = g_tx_super_before;
		rc = -1;
	}
	ext3_tx_reset();
	if (commit_rc != 0) {
		(void)ext3_load_bg();
		g_writable = 0;
		klog("EXT3", "commit failed, filesystem forced read-only");
	}
	return rc;
}

int ext3_load_bg(void)
{
	uint8_t *blk = (uint8_t *)kmalloc(g_block_size);

	if (!blk)
		return -1;
	if (ext3_read_block(g_bgdt_block, blk) != 0) {
		kfree(blk);
		return -1;
	}
	k_memcpy(&g_bg, blk, sizeof(g_bg));
	kfree(blk);
	return 0;
}

int ext3_checkpoint_replay(void)
{
	ext3_inode_t journal;

	if (g_overlay_count == 0)
		return 0;
	if (!g_dev->write_sector)
		return -1;
	for (uint32_t i = 0; i < g_overlay_count; i++) {
		if (ext3_write_disk_block(g_overlay[i].fs_block, g_overlay[i].data) !=
		    0)
			return -1;
	}
	/* Replay writes go through the cache like any other home-location
     * write. Flush them before advancing the journal superblock so that a
     * crash immediately after replay does not leave committed-but-not-home
     * blocks lost in dirty cache entries. */
	if (bcache_sync(g_dev) != 0)
		return -1;
	if (g_super.journal_inum == 0 ||
	    ext3_read_inode(g_super.journal_inum, &journal) != 0)
		return -1;
	if (jbd_update_super(&journal, 0u, 0u) != 0)
		return -1;
	g_super.feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
	if (ext3_flush_super_raw() != 0)
		return -1;
	ext3_overlay_clear();
	g_needs_recovery = 0;
	return ext3_load_bg();
}

static int
jbd_revoked(uint32_t block, const uint32_t *revokes, uint32_t revoke_count)
{
	for (uint32_t i = 0; i < revoke_count; i++) {
		if (revokes[i] == block)
			return 1;
	}
	return 0;
}

static int jbd_apply_transaction(const ext3_inode_t *journal,
                                 const jbd_tag_t *tags,
                                 uint32_t tag_count,
                                 const uint32_t *revokes,
                                 uint32_t revoke_count)
{
	uint8_t *data = (uint8_t *)kmalloc(g_block_size);

	if (!data)
		return -1;
	for (uint32_t i = 0; i < tag_count; i++) {
		if (jbd_revoked(tags[i].fs_block, revokes, revoke_count))
			continue;
		if (ext3_journal_read_block(journal, tags[i].journal_block, data) !=
		    0) {
			kfree(data);
			return -1;
		}
		if (tags[i].flags & JBD_FLAG_ESCAPE) {
			data[0] = 0xC0;
			data[1] = 0x3B;
			data[2] = 0x39;
			data[3] = 0x98;
		}
		if (ext3_overlay_put(tags[i].fs_block, data) != 0) {
			kfree(data);
			return -1;
		}
	}
	kfree(data);
	return 0;
}

static int jbd_parse_descriptor(const uint8_t *blk,
                                uint32_t pos,
                                uint32_t first,
                                uint32_t maxlen,
                                jbd_tag_t *tags,
                                uint32_t *tag_count_out,
                                uint32_t *after_data_out)
{
	uint32_t off = 12u;
	uint32_t data_pos = jbd_next(pos, first, maxlen);
	uint32_t count = 0;

	while (off + 8u <= g_block_size) {
		uint32_t fs_block = be32(blk + off);
		uint32_t flags = be32(blk + off + 4u);
		uint32_t tag_size = (flags & JBD_FLAG_SAME_UUID) ? 8u : 24u;

		if (fs_block == 0 || count >= EXT3_REPLAY_MAX_TAGS ||
		    off + tag_size > g_block_size) {
			klog("EXT3", "corrupt journal descriptor");
			return -1;
		}
		tags[count].fs_block = fs_block;
		tags[count].journal_block = data_pos;
		tags[count].flags = flags;
		count++;
		data_pos = jbd_next(data_pos, first, maxlen);
		off += tag_size;
		if (flags & JBD_FLAG_LAST_TAG)
			break;
	}

	*tag_count_out = count;
	*after_data_out = data_pos;
	return 0;
}

static int jbd_parse_revoke(const uint8_t *blk,
                            uint32_t seq,
                            uint32_t cur_seq,
                            uint32_t *revokes,
                            uint32_t *revoke_count)
{
	uint32_t bytes = be32(blk + 12u);
	uint32_t off = 16u;

	if (seq != cur_seq)
		return 0;
	if (bytes > g_block_size)
		return -1;
	while (off + 4u <= bytes) {
		if (*revoke_count >= EXT3_REPLAY_MAX_REVOKES) {
			klog("EXT3", "journal revoke table exhausted");
			return -1;
		}
		revokes[*revoke_count] = be32(blk + off);
		(*revoke_count)++;
		off += 4u;
	}
	return 0;
}

int ext3_replay_journal(void)
{
	ext3_inode_t journal;
	uint8_t *blk;
	jbd_tag_t *tags;
	uint32_t *revokes;
	uint32_t tag_count = 0;
	uint32_t revoke_count = 0;
	uint32_t cur_seq = 0;
	uint32_t active = 0;
	uint32_t blocksize;
	uint32_t maxlen;
	uint32_t first;
	uint32_t start;
	uint32_t pos;
	uint32_t scanned = 0;
	uint32_t incompat;

	if ((g_super.feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) == 0)
		return 0;
	if ((g_super.feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) == 0)
		return 0;
	if (g_super.journal_dev != 0 || g_super.journal_inum == 0) {
		klog("EXT3", "external or missing journal unsupported");
		return -1;
	}
	if (ext3_read_inode(g_super.journal_inum, &journal) != 0 ||
	    (journal.mode & EXT3_S_IFMT) != EXT3_S_IFREG) {
		klog("EXT3", "journal inode invalid");
		return -1;
	}

	tags = (jbd_tag_t *)kmalloc(sizeof(jbd_tag_t) * EXT3_REPLAY_MAX_TAGS);
	revokes = (uint32_t *)kmalloc(sizeof(uint32_t) * EXT3_REPLAY_MAX_REVOKES);
	blk = (uint8_t *)kmalloc(g_block_size);
	if (!tags || !revokes || !blk) {
		if (tags)
			kfree(tags);
		if (revokes)
			kfree(revokes);
		if (blk)
			kfree(blk);
		return -1;
	}
	if (ext3_journal_read_block(&journal, 0, blk) != 0) {
		kfree(tags);
		kfree(revokes);
		kfree(blk);
		return -1;
	}
	if (be32(blk) != JBD_MAGIC || (be32(blk + 4u) != JBD_SUPERBLOCK_V1 &&
	                               be32(blk + 4u) != JBD_SUPERBLOCK_V2)) {
		klog("EXT3", "journal superblock invalid");
		kfree(tags);
		kfree(revokes);
		kfree(blk);
		return -1;
	}

	blocksize = be32(blk + 12u);
	maxlen = be32(blk + 16u);
	first = be32(blk + 20u);
	start = be32(blk + 28u);
	incompat = be32(blk + 40u);
	if (blocksize != g_block_size || first == 0 || first >= maxlen ||
	    maxlen == 0 || start >= maxlen) {
		klog("EXT3", "journal layout unsupported");
		kfree(tags);
		kfree(revokes);
		kfree(blk);
		return -1;
	}
	if ((incompat & ~JBD_FEATURE_INCOMPAT_REVOKE) != 0 ||
	    be32(blk + 36u) != 0 || be32(blk + 44u) != 0) {
		klog("EXT3", "journal features unsupported");
		kfree(tags);
		kfree(revokes);
		kfree(blk);
		return -1;
	}
	if (start == 0) {
		kfree(tags);
		kfree(revokes);
		kfree(blk);
		return 0;
	}

	pos = start;
	while (scanned < maxlen - first) {
		uint32_t magic;
		uint32_t type;
		uint32_t seq;

		if (ext3_journal_read_block(&journal, pos, blk) != 0) {
			kfree(tags);
			kfree(revokes);
			kfree(blk);
			return -1;
		}
		magic = be32(blk);
		type = be32(blk + 4u);
		seq = be32(blk + 8u);
		if (magic != JBD_MAGIC)
			break;

		if (type == JBD_DESCRIPTOR_BLOCK) {
			uint32_t after_data;
			if (jbd_parse_descriptor(
			        blk, pos, first, maxlen, tags, &tag_count, &after_data) !=
			    0) {
				kfree(blk);
				kfree(tags);
				kfree(revokes);
				return -1;
			}
			revoke_count = 0;
			cur_seq = seq;
			active = 1;
			while (pos != after_data) {
				pos = jbd_next(pos, first, maxlen);
				scanned++;
			}
			continue;
		}

		if (type == JBD_REVOKE_BLOCK) {
			if (active && jbd_parse_revoke(
			                  blk, seq, cur_seq, revokes, &revoke_count) != 0) {
				kfree(blk);
				kfree(tags);
				kfree(revokes);
				return -1;
			}
		} else if (type == JBD_COMMIT_BLOCK) {
			if (active && seq == cur_seq) {
				if (jbd_apply_transaction(
				        &journal, tags, tag_count, revokes, revoke_count) !=
				    0) {
					kfree(blk);
					kfree(tags);
					kfree(revokes);
					return -1;
				}
				active = 0;
				tag_count = 0;
				revoke_count = 0;
			}
		} else if (type != JBD_SUPERBLOCK_V1 && type != JBD_SUPERBLOCK_V2) {
			break;
		}

		pos = jbd_next(pos, first, maxlen);
		scanned++;
	}

	klog_uint("EXT3", "journal replay blocks", g_overlay_count);
	kfree(tags);
	kfree(revokes);
	kfree(blk);
	return 0;
}
