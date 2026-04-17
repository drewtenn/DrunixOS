# Ext3 Journalling Design

Date: 2026-04-17

## Context

Drunix already boots from a deterministic Linux-compatible ext3 image. The image has an internal Journal Block Device (JBD) journal inode, and the kernel can parse committed host-created journal records into an in-memory replay overlay. The current write path does not create journal records. It writes changed ext3 blocks directly to their home locations, and it disables mutation after journal recovery because recovered blocks live only in RAM.

The goal is to make Drunix ext3 writes journalled while preserving the existing single-block-group ext3 image format, VFS API, userland smoke tests, and host Linux compatibility checks.

## Goals

- Write Drunix ext3 mutations through the internal JBD journal inode.
- Keep the generated image compatible with Linux tools: `check_ext3_linux_compat.py`, `e2fsck -fn`, and `dumpe2fs` should continue to recognize the internal journal.
- Recover committed journal transactions on the next Drunix mount, checkpoint them to home blocks when the disk is writable, clear the recovery flag, and re-enable writes.
- Keep the first implementation small enough to audit: one mounted ext3 filesystem, one internal journal, one transaction at a time, bounded in-memory staging, no background checkpoint thread.

## Non-Goals

- External journal devices.
- Multiple block groups beyond the existing writable constraint.
- Asynchronous commit, batching across system calls, checksummed JBD2 records, revoke generation for Drunix-created transactions, or mount options such as ordered/writeback/data-journal modes.
- Full Linux ext3 feature parity.

## Considered Approaches

### Replay-only

Keep the current replay parser and continue writing Drunix mutations directly to home blocks.

This is not enough. It handles dirty journals created elsewhere but does not make Drunix writes journalled.

### Metadata-only journalling

Classify each write as metadata or file data. Journal only metadata, and ensure file data reaches disk before the metadata commit.

This matches common ext3 `data=ordered` behavior, but it requires carefully classifying directory blocks, bitmaps, inode-table blocks, indirect blocks, superblock updates, and plain file data across the existing helper functions. That is a larger first step and easier to get subtly wrong.

### Full-block synchronous journalling

Stage every filesystem block written by one public ext3 mutation, write those staged blocks to the JBD journal as one transaction, commit the transaction, checkpoint the blocks to their home locations, then clear the journal when checkpointing succeeds.

This is the selected approach. It is heavier than metadata-only journalling, but the current ext3 image is small and the correctness model is straightforward: nothing reaches its home location until a committed transaction exists. The same replay parser can recover committed transactions after an interrupted checkpoint.

## Architecture

Add a small transaction layer inside `kernel/fs/ext3.c`.

The transaction layer owns:

- a fixed-size array of staged filesystem blocks,
- transaction depth for nested public operations such as truncate growth calling write,
- the next JBD sequence number,
- helper functions to begin, stage, commit, abort, and checkpoint a transaction.

Existing raw disk I/O remains available for mount, journal reads, and journal writes. Filesystem mutation helpers switch from direct block writes to a transaction-aware block writer. Reads used by mutation helpers use the transaction overlay first, then the replay overlay, then disk, so later steps in the same operation see their earlier staged changes.

## Transaction Flow

Each public mutating ext3 operation opens a transaction:

- `create`
- `unlink`
- `mkdir`
- `rmdir`
- `write`
- `truncate`

The existing `rename`, `link`, and `symlink` paths remain read-only stubs.

During the operation, each block write stages a full filesystem block. Rewriting the same block updates the staged copy instead of adding a duplicate.

At the outermost transaction commit:

1. Read the journal superblock from journal logical block 0.
2. Mark the ext3 superblock as needing recovery by setting `EXT3_FEATURE_INCOMPAT_RECOVER`.
3. Write the journal superblock with a non-zero `s_start` and the current sequence.
4. Write one descriptor block with big-endian JBD tags.
5. Write staged block images into following journal blocks.
6. Write one commit block with the same sequence.
7. Checkpoint staged blocks to their home filesystem blocks.
8. Clear `s_start` in the journal superblock.
9. Clear `EXT3_FEATURE_INCOMPAT_RECOVER` in the ext3 superblock.
10. Free staged buffers and allow later mutations.

If the system stops before the commit block exists, replay ignores the incomplete transaction. If it stops after the commit block but before the checkpoint is complete, the next mount replays the committed transaction and checkpoints the final block images.

## JBD Encoding

The writer uses the JBD v1/v2 block format already parsed by Drunix:

- big-endian common block headers,
- descriptor blocks with 32-bit block numbers and flags,
- `JBD_FLAG_SAME_UUID` tags to omit repeated UUID bytes,
- `JBD_FLAG_LAST_TAG` on the final tag,
- `JBD_FLAG_ESCAPE` when a data block begins with the JBD magic word,
- one commit block per transaction.

The first transaction block remains logical journal block 1. The initial implementation writes one transaction at a time and checkpoints it before starting another, so journal wraparound is not needed for clean operation. Replay still handles wrapped host-created journals through the existing scan logic.

## Recovery

Mount recovery changes from read-only overlay replay to durable checkpointing:

- Parse committed JBD transactions as today.
- Store the latest image for each affected filesystem block in the replay overlay.
- If the disk is writable and the filesystem shape is supported, write replay overlay blocks to their home locations.
- Clear the journal start field and the ext3 recovery incompatibility bit.
- Reload the group descriptor and continue with writes enabled.

If checkpointing fails, keep writes disabled. Reads can still use replay overlay data for the mounted session.

## Error Handling

Failed transaction begin or staging returns the same style of negative result as the current ext3 helpers.

If commit fails before any home block is written, abort the staged transaction and leave home blocks unchanged.

If commit succeeds but checkpointing fails, leave the recovery bit and journal start intact when possible, disable future writes, and rely on the next mount or host tools to replay the committed journal.

If a transaction would exceed the bounded staging capacity or available journal blocks, fail the operation without checkpointing partial changes. The first implementation will size the staging table for the current smoke tests and normal shell usage, not for arbitrarily large writes.

## Testing

Update or add tests around these behaviors:

- generated ext3 images remain Linux-compatible and have a clean journal,
- Drunix writable ext3 smoke test still passes,
- the mutated image passes host `check_ext3_linux_compat.py` and `e2fsck -fn`,
- a synthetic committed journal can be replayed and checkpointed by Drunix,
- a synthetic uncommitted journal is ignored,
- repeated create/write/unlink operations leave a clean journal.

The main verification target is `make test-ext3-linux-compat`, with focused host-side journal structure tests added where QEMU crash injection would be too slow or brittle.

## Scope Guardrails

The implementation should stay inside `kernel/fs/ext3.c` and the ext3 image/test tooling unless a small public test hook is required. It should not change VFS contracts or userland syscall behavior. Any documentation outside this spec should be limited to keeping existing ext3 comments and chapter text accurate.
