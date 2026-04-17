# Ext3 Journalling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Drunix ext3 mutations write real internal JBD journal transactions and recover committed transactions durably on mount.

**Architecture:** Add a bounded synchronous transaction layer inside `kernel/fs/ext3.c`. Public mutating ext3 operations stage full block images, commit them through the existing internal journal inode, checkpoint them to home blocks, and clear the clean journal state before returning.

**Tech Stack:** Freestanding C kernel, Drunix VFS/ext3 backend, Python ext3 image check tools, GNU Make, QEMU, e2fsprogs.

---

## File Structure

- Modify `kernel/fs/ext3.c`: transaction staging, JBD writer, recovery checkpointing, and transaction wrappers around public mutating operations.
- Modify `tools/check_ext3_linux_compat.py`: expose journal sequence/start readers used by checks and keep existing compatibility checks intact.
- Create `tools/check_ext3_journal_activity.py`: host-side assertion that a mutated ext3 image has a clean journal and a sequence number greater than the generated baseline.
- Modify `Makefile`: run journal activity check after `ext3wtest` mutates the image.
- No VFS API or syscall changes.

## Task 1: Add A Failing Journal-Activity Check

**Files:**
- Modify: `tools/check_ext3_linux_compat.py`
- Create: `tools/check_ext3_journal_activity.py`
- Modify: `Makefile`

- [ ] **Step 1: Add journal state helpers to `tools/check_ext3_linux_compat.py`**

Add these methods to `Image`, immediately after `inode()`:

```python
    def journal_superblock(self):
        inode = self.inode(JOURNAL_INO)
        first = inode["ptrs"][0]
        if first == 0:
            fail("journal inode has no first block")
        return self.block(first)

    def journal_sequence(self):
        return self.be32(self.journal_superblock(), 24)

    def journal_start(self):
        return self.be32(self.journal_superblock(), 28)
```

Then update `check_journal()` to use the helper:

```python
    jsb = img.journal_superblock()
```

- [ ] **Step 2: Add `tools/check_ext3_journal_activity.py`**

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
check_ext3_journal_activity.py - verify Drunix advanced the internal JBD journal.
"""

import sys

from check_ext3_linux_compat import Image, fail


def main():
    if len(sys.argv) != 3:
        print(
            "usage: check_ext3_journal_activity.py <disk.img> <min-sequence>",
            file=sys.stderr,
        )
        return 2

    img = Image(sys.argv[1])
    min_sequence = int(sys.argv[2], 0)
    sequence = img.journal_sequence()
    start = img.journal_start()

    if start != 0:
        fail(f"journal is not clean: start={start}")
    if sequence <= min_sequence:
        fail(
            f"journal sequence {sequence} did not advance past {min_sequence}"
        )

    print(f"ext3 journal activity ok: sequence {sequence}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 3: Wire the new check into `test-ext3-linux-compat`**

In `Makefile`, after this line:

```make
	$(PYTHON) tools/check_ext3_linux_compat.py disk-ext3w.img
```

add:

```make
	$(PYTHON) tools/check_ext3_journal_activity.py disk-ext3w.img 1
```

Update the `validate-ext3-linux` prerequisite line:

```make
validate-ext3-linux: disk.img tools/check_ext3_linux_compat.py
```

to:

```make
validate-ext3-linux: disk.img tools/check_ext3_linux_compat.py tools/check_ext3_journal_activity.py
```

- [ ] **Step 4: Run the focused failing check**

Run:

```bash
make disk
python3 tools/check_ext3_journal_activity.py disk.img 1
```

Expected: the second command fails with `journal sequence 1 did not advance past 1`.

- [ ] **Step 5: Commit the failing test**

```bash
git add Makefile tools/check_ext3_linux_compat.py tools/check_ext3_journal_activity.py
git commit -m "test: check ext3 journal activity"
```

## Task 2: Add Transaction Staging Primitives

**Files:**
- Modify: `kernel/fs/ext3.c`

- [ ] **Step 1: Add constants and transaction state near the existing replay overlay globals**

```c
#define EXT3_TX_MAX_BLOCKS      128u

typedef struct {
    uint32_t fs_block;
    uint8_t *data;
} ext3_tx_block_t;

static ext3_tx_block_t g_tx[EXT3_TX_MAX_BLOCKS];
static uint32_t g_tx_count;
static uint32_t g_tx_depth;
static uint32_t g_tx_failed;
```

- [ ] **Step 2: Add transaction lookup and cleanup helpers after `ext3_overlay_put()`**

```c
static int ext3_tx_find(uint32_t fs_block)
{
    for (uint32_t i = 0; i < g_tx_count; i++) {
        if (g_tx[i].fs_block == fs_block)
            return (int)i;
    }
    return -1;
}

static void ext3_tx_reset(void)
{
    for (uint32_t i = 0; i < g_tx_count; i++) {
        if (g_tx[i].data)
            kfree(g_tx[i].data);
        g_tx[i].fs_block = 0;
        g_tx[i].data = 0;
    }
    g_tx_count = 0;
    g_tx_depth = 0;
    g_tx_failed = 0;
}
```

- [ ] **Step 3: Make reads see staged transaction blocks**

In `ext3_read_block()`, before the replay overlay loop, add:

```c
    int tx_idx = ext3_tx_find(block);
    if (tx_idx >= 0) {
        k_memcpy(buf, g_tx[tx_idx].data, g_block_size);
        return 0;
    }
```

- [ ] **Step 4: Add transaction-aware full-block staging**

Add after `ext3_read_block()`:

```c
static int ext3_write_block(uint32_t block, const uint8_t *buf)
{
    uint8_t *copy;
    int idx;

    if (!buf || block == 0)
        return -1;
    if (g_tx_depth == 0)
        return ext3_write_disk_block(block, buf);

    idx = ext3_tx_find(block);
    if (idx >= 0) {
        k_memcpy(g_tx[idx].data, buf, g_block_size);
        return 0;
    }

    if (g_tx_count >= EXT3_TX_MAX_BLOCKS) {
        klog("EXT3", "journal transaction block limit hit");
        g_tx_failed = 1;
        return -1;
    }

    copy = (uint8_t *)kmalloc(g_block_size);
    if (!copy) {
        g_tx_failed = 1;
        return -1;
    }
    k_memcpy(copy, buf, g_block_size);
    g_tx[g_tx_count].fs_block = block;
    g_tx[g_tx_count].data = copy;
    g_tx_count++;
    return 0;
}
```

- [ ] **Step 5: Convert metadata helper writes to transaction-aware writes**

Replace these direct write calls with `ext3_write_block()`:

```c
ext3_write_disk_block(block, z)
ext3_write_disk_block(g_bgdt_block, blk)
ext3_write_disk_block(block, blk)
ext3_write_disk_block(g_bg.block_bitmap, map)
ext3_write_disk_block(g_bg.inode_bitmap, map)
ext3_write_disk_block(in->block[12], blk)
ext3_write_disk_block(phys, blk)
```

Do not replace journal-specific writes introduced in later tasks.

- [ ] **Step 6: Run a compile check**

Run:

```bash
make kernel
```

Expected: kernel links successfully.

- [ ] **Step 7: Commit staging primitives**

```bash
git add kernel/fs/ext3.c
git commit -m "feat: stage ext3 transaction blocks"
```

## Task 3: Add Transaction-Aware Superblock And Group Writes

**Files:**
- Modify: `kernel/fs/ext3.c`

- [ ] **Step 1: Split raw and transaction-aware superblock flushing**

Replace `ext3_flush_super()` with:

```c
static int ext3_flush_super_raw(void)
{
    return ext3_write_bytes(EXT3_SUPER_OFFSET, (const uint8_t *)&g_super,
                            sizeof(g_super));
}

static int ext3_flush_super(void)
{
    uint8_t *blk;
    uint32_t block = EXT3_SUPER_OFFSET / g_block_size;
    uint32_t off = EXT3_SUPER_OFFSET % g_block_size;
    int rc;

    blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk)
        return -1;
    if (ext3_read_block(block, blk) != 0) {
        kfree(blk);
        return -1;
    }
    k_memcpy(blk + off, &g_super, sizeof(g_super));
    rc = ext3_write_block(block, blk);
    kfree(blk);
    return rc;
}
```

- [ ] **Step 2: Update `ext3_flush_bg()` to read through staged state**

Change:

```c
    if (ext3_read_disk_block(g_bgdt_block, blk) != 0) {
```

to:

```c
    if (ext3_read_block(g_bgdt_block, blk) != 0) {
```

Confirm the final write in that function uses `ext3_write_block(g_bgdt_block, blk)`.

- [ ] **Step 3: Run a compile check**

Run:

```bash
make kernel
```

Expected: kernel links successfully.

- [ ] **Step 4: Commit transaction-aware metadata flushing**

```bash
git add kernel/fs/ext3.c
git commit -m "feat: stage ext3 metadata flushes"
```

## Task 4: Write Clean JBD Transactions

**Files:**
- Modify: `kernel/fs/ext3.c`

- [ ] **Step 1: Add big-endian writers near `be32()`**

```c
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
```

- [ ] **Step 2: Add JBD raw journal write helpers before `ext3_replay_journal()`**

```c
static int ext3_journal_write_block(const ext3_inode_t *journal,
                                    uint32_t logical, const uint8_t *buf)
{
    uint32_t phys = ext3_block_index(journal, logical);

    if (phys == 0)
        return -1;
    return ext3_write_disk_block(phys, buf);
}

static void jbd_write_header(uint8_t *blk, uint32_t type, uint32_t seq)
{
    put_be32(blk, JBD_MAGIC);
    put_be32(blk + 4u, type);
    put_be32(blk + 8u, seq);
}
```

- [ ] **Step 3: Add journal superblock field update helper**

```c
static int jbd_update_super(const ext3_inode_t *journal, uint32_t start,
                            uint32_t seq)
{
    uint8_t *blk = (uint8_t *)kmalloc(g_block_size);
    int rc;

    if (!blk)
        return -1;
    if (ext3_journal_read_block(journal, 0, blk) != 0) {
        kfree(blk);
        return -1;
    }
    put_be32(blk + 24u, seq);
    put_be32(blk + 28u, start);
    rc = ext3_journal_write_block(journal, 0, blk);
    kfree(blk);
    return rc;
}
```

- [ ] **Step 4: Add descriptor/data/commit writer**

```c
static int jbd_write_transaction(const ext3_inode_t *journal, uint32_t seq)
{
    uint8_t *desc;
    uint8_t *data;
    uint32_t off = 12u;

    if (g_tx_count == 0)
        return 0;
    if (g_tx_count + 2u >= (uint32_t)(journal->size / g_block_size))
        return -1;

    desc = (uint8_t *)kmalloc(g_block_size);
    data = (uint8_t *)kmalloc(g_block_size);
    if (!desc || !data) {
        if (desc) kfree(desc);
        if (data) kfree(data);
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
```

- [ ] **Step 5: Add checkpoint and commit helpers**

```c
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
    uint32_t seq;

    if (g_tx_failed)
        return -1;
    if (g_tx_count == 0)
        return 0;
    if (g_super.journal_dev != 0 || g_super.journal_inum == 0)
        return -1;
    if (ext3_read_inode(g_super.journal_inum, &journal) != 0)
        return -1;

    seq = 2u;
    {
        uint8_t *jsb = (uint8_t *)kmalloc(g_block_size);
        if (!jsb)
            return -1;
        if (ext3_journal_read_block(&journal, 0, jsb) != 0) {
            kfree(jsb);
            return -1;
        }
        seq = be32(jsb + 24u);
        if (seq == 0)
            seq = 1u;
        kfree(jsb);
    }

    g_super.feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
    if (ext3_flush_super() != 0)
        return -1;
    if (ext3_flush_super_raw() != 0)
        return -1;
    if (jbd_update_super(&journal, 1u, seq) != 0)
        return -1;
    if (jbd_write_transaction(&journal, seq) != 0)
        return -1;
    if (ext3_checkpoint_tx() != 0)
        return -1;
    if (jbd_update_super(&journal, 0u, seq + 1u) != 0)
        return -1;
    g_super.feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
    return ext3_flush_super_raw();
}
```

- [ ] **Step 6: Run a compile check**

Run:

```bash
make kernel
```

Expected: kernel links successfully.

- [ ] **Step 7: Commit the JBD writer**

```bash
git add kernel/fs/ext3.c
git commit -m "feat: write ext3 journal transactions"
```

## Task 5: Wrap Mutating Ext3 Operations In Transactions

**Files:**
- Modify: `kernel/fs/ext3.c`

- [ ] **Step 1: Add begin/end wrappers after `ext3_commit_tx()`**

```c
static int ext3_tx_begin(void)
{
    if (!g_writable || g_overlay_count != 0)
        return -1;
    if (g_tx_depth == 0)
        ext3_tx_reset();
    g_tx_depth++;
    return 0;
}

static int ext3_tx_end(int rc)
{
    int commit_rc = 0;

    if (g_tx_depth == 0)
        return rc;
    g_tx_depth--;
    if (g_tx_depth != 0)
        return rc;

    if (rc == 0 || rc > 0)
        commit_rc = ext3_commit_tx();
    if (commit_rc != 0)
        rc = -1;
    ext3_tx_reset();
    return rc;
}
```

- [ ] **Step 2: Update `ext3_can_mutate()`**

Replace:

```c
    return g_writable && g_overlay_count == 0;
```

with:

```c
    return g_writable && g_overlay_count == 0 && g_tx_failed == 0;
```

- [ ] **Step 3: Wrap each public mutating operation**

For `ext3_write`, `ext3_truncate`, `ext3_create`, `ext3_unlink`, `ext3_mkdir`, and `ext3_rmdir`, add:

```c
    if (ext3_tx_begin() != 0)
        return -1;
```

after `(void)ctx;`, then return through `ext3_tx_end(rc)` using a local `int rc`.

For example, `ext3_create()` should become structurally:

```c
static int ext3_create(void *ctx, const char *path)
{
    int rc = -1;
    uint32_t dir_ino;
    uint32_t existing;
    char leaf[EXT3_NAME_MAX + 1];
    ext3_inode_t in;
    uint32_t ino;
    uint32_t now;

    (void)ctx;
    if (ext3_tx_begin() != 0)
        return -1;

    /* Existing body sets rc instead of returning directly. */

    return ext3_tx_end(rc);
}
```

Preserve existing return values: successful `create()` returns the inode number, successful `write()` returns bytes written, and successful directory operations return `0`.

- [ ] **Step 4: Run a compile check**

Run:

```bash
make kernel
```

Expected: kernel links successfully.

- [ ] **Step 5: Commit transaction wrappers**

```bash
git add kernel/fs/ext3.c
git commit -m "feat: journal ext3 mutations"
```

## Task 6: Checkpoint Replayed Journals On Mount

**Files:**
- Modify: `kernel/fs/ext3.c`

- [ ] **Step 1: Add replay overlay cleanup and checkpoint helpers after `ext3_overlay_put()`**

```c
static void ext3_overlay_clear(void)
{
    for (uint32_t i = 0; i < g_overlay_count; i++) {
        if (g_overlay[i].data)
            kfree(g_overlay[i].data);
        g_overlay[i].fs_block = 0;
        g_overlay[i].data = 0;
    }
    g_overlay_count = 0;
}

static int ext3_checkpoint_replay(void)
{
    ext3_inode_t journal;

    if (g_overlay_count == 0)
        return 0;
    if (!g_dev->write_sector)
        return -1;
    for (uint32_t i = 0; i < g_overlay_count; i++) {
        if (ext3_write_disk_block(g_overlay[i].fs_block,
                                  g_overlay[i].data) != 0)
            return -1;
    }
    if (g_super.journal_inum == 0 ||
        ext3_read_inode(g_super.journal_inum, &journal) != 0)
        return -1;
    if (jbd_update_super(&journal, 0u, 0u) != 0)
        return -1;
    g_super.feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
    if (ext3_flush_super_raw() != 0)
        return -1;
    ext3_overlay_clear();
    return 0;
}
```

- [ ] **Step 2: Call checkpointing from `ext3_init()`**

After:

```c
    if (ext3_replay_journal() != 0)
        return -1;
```

add:

```c
    if (g_overlay_count != 0 && ext3_checkpoint_replay() != 0)
        klog("EXT3", "journal checkpoint failed; keeping writes disabled");
```

- [ ] **Step 3: Re-read the group descriptor after successful checkpoint**

After checkpointing, re-read `g_bgdt_block` using the existing block buffer pattern so `g_bg` reflects replayed metadata before enabling writes.

- [ ] **Step 4: Run a compile check**

Run:

```bash
make kernel
```

Expected: kernel links successfully.

- [ ] **Step 5: Commit replay checkpointing**

```bash
git add kernel/fs/ext3.c
git commit -m "feat: checkpoint ext3 journal replay"
```

## Task 7: Full Verification

**Files:**
- No required file changes unless verification exposes a defect.

- [ ] **Step 1: Run the focused compatibility target**

Run:

```bash
make test-ext3-linux-compat
```

Expected:

```text
EXT3WTEST PASS
ext3 linux compatibility structure ok
ext3 journal activity ok: sequence <number greater than 1>
```

`e2fsck -fn disk-ext3w.img` must exit successfully.

- [ ] **Step 2: Run host write interop**

Run:

```bash
make test-ext3-host-write-interop
```

Expected: command exits `0`; `ext3-host-readback.txt` contains `linux-host`.

- [ ] **Step 3: Run the kernel build and in-kernel tests**

Run:

```bash
make test-fresh
```

Expected: QEMU boots with KTEST enabled and `debugcon.log` contains no failed test cases.

- [ ] **Step 4: Inspect git state**

Run:

```bash
git status --short
```

Expected: only intentional source changes and generated runtime artifacts are present. Do not commit `disk*.img`, QEMU logs, or extracted test logs.

- [ ] **Step 5: Final commit if verification required fixes**

If Task 7 required source fixes, commit them:

```bash
git add kernel/fs/ext3.c Makefile tools/check_ext3_linux_compat.py tools/check_ext3_journal_activity.py
git commit -m "fix: stabilize ext3 journalling"
```
