#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
mkfs.py — build an DUFS v3 disk image (inode-based, 4096-byte blocks).

Usage:
    python3 tools/mkfs.py <output> <total_sectors> \
        <src1> <name1> [<src2> <name2> ...]

Destination names may contain '/' to place files in nested subdirectories
(e.g. "usr/bin/hello").  All intermediate directories are created
automatically.  Arbitrary depth is supported.

DUFS v3 on-disk layout (LBAs = 512-byte sectors; blocks = 4096 bytes = 8 sectors):
    LBA 0          unused (always — LBA 0 is the "not allocated" sentinel)
    LBA 1          superblock  (28 bytes used; rest zeroed)
    LBA 2-9        inode bitmap (1 block = 4096 bytes → 32,768 inode bits)
    LBA 10-17      block bitmap (1 block = 4096 bytes → 32,768 block bits)
    LBA 18-273     inode table  (1024 inodes x 128 bytes = 256 sectors)
    LBA 274+       data blocks  (block N at LBA 274 + Nx8)

Inode numbering:
    0  reserved (never allocated)
    1  root directory
    2+ subdirectories (breadth-first by path depth), then files
"""

import os
import struct
import sys
import time

SECTOR            = 512
BLOCK_SIZE        = 4096
SECS_PER_BLOCK    = 8          # BLOCK_SIZE // SECTOR
MAGIC             = 0x44554603   # "DUF\x03" — DUFS v3
INODE_BITMAP_LBA  = 2
BLOCK_BITMAP_LBA  = 10
INODE_TABLE_LBA   = 18
DATA_LBA          = 274
INODE_COUNT       = 1024
INODE_SIZE        = 128
INODES_PER_SECTOR = 4            # SECTOR / INODE_SIZE
DIRENT_SIZE       = 260          # name[256] + inode[4]
DIRENTS_PER_BLOCK = 15           # BLOCK_SIZE // DIRENT_SIZE (76 bytes unused per block)
DIRECT_BLOCKS     = 12
INDIR_ENTRIES     = 1024         # BLOCK_SIZE // 4

DUFS_TYPE_FILE = 1
DUFS_TYPE_DIR  = 2


def ceil_div(n, d):
    return (n + d - 1) // d


def pack_inode(itype, link_count, size, block_count, direct_lbas, indirect_lba=0,
               double_indirect_lba=0, mtime=0, atime=0):
    """Return a 128-byte packed inode."""
    d = list(direct_lbas) + [0] * DIRECT_BLOCKS
    d = d[:DIRECT_BLOCKS]
    return struct.pack(
        "<HHIIII" + "I" * DIRECT_BLOCKS + "II52s",
        itype, link_count, size, block_count,
        mtime, atime,
        *d,
        indirect_lba,
        double_indirect_lba,
        b'\x00' * 52,
    )


def pack_dirent(name, inode_num):
    """Return a 260-byte packed directory entry (name[256] + inode[4])."""
    nb = name.encode('ascii')[:255]
    nb = nb + b'\x00' * (256 - len(nb))
    return struct.pack("<256sI", nb, inode_num)


def main():
    args = sys.argv[1:]
    if len(args) < 2 or (len(args) - 2) % 2 != 0:
        print("usage: mkfs.py <output> <total_sectors> [<src> <name> ...]",
              file=sys.stderr)
        sys.exit(1)

    output        = args[0]
    total_sectors = int(args[1])
    file_pairs    = [(args[i], args[i + 1]) for i in range(2, len(args), 2)]
    build_time    = int(time.time())

    # ── Build the directory tree ──────────────────────────────────────────────
    # dirs: path → {'children': [(name, ino, type)], 'ino': N}
    # Root is keyed by '' (empty string).
    dirs = {'': {'children': [], 'ino': 1}}

    file_objs = []
    for src, dest_name in file_pairs:
        with open(src, 'rb') as f:
            data = f.read()
        src_mtime = int(os.path.getmtime(src))

        parts = dest_name.split('/')
        leaf  = parts[-1]

        # Ensure every intermediate directory exists in the tree.
        for depth in range(1, len(parts)):
            dir_path = '/'.join(parts[:depth])
            if dir_path not in dirs:
                dirs[dir_path] = {'children': [], 'ino': None}

        parent_path = '/'.join(parts[:-1])
        file_objs.append({
            'src':         src,
            'dest':        dest_name,
            'leaf':        leaf,
            'parent_path': parent_path,
            'data':        data,
            'mtime':       src_mtime,
            'ino':         None,
        })

    # ── Assign inode numbers ──────────────────────────────────────────────────
    # Directories sorted by depth (root first, then shallowest, then deeper).
    def dir_depth(path):
        return 0 if path == '' else path.count('/') + 1

    sorted_dir_paths = sorted(dirs.keys(), key=dir_depth)

    # Root is always inode 1; remaining dirs assigned from 2 upward.
    next_ino = 2
    for path in sorted_dir_paths:
        if path == '':
            dirs[path]['ino'] = 1
        else:
            dirs[path]['ino'] = next_ino
            next_ino += 1

    # Files get the next available inodes.
    for obj in file_objs:
        obj['ino'] = next_ino
        next_ino += 1

    total_inodes = next_ino
    if total_inodes > INODE_COUNT:
        print(f"error: too many inodes ({total_inodes}), max {INODE_COUNT}",
              file=sys.stderr)
        sys.exit(1)

    # ── Register children in parent directories ───────────────────────────────
    # Each non-root directory registers itself as a child of its parent.
    for path in sorted_dir_paths:
        if path == '':
            continue
        parts = path.split('/')
        name  = parts[-1]
        parent_path = '/'.join(parts[:-1])
        dirs[parent_path]['children'].append((name, dirs[path]['ino'], DUFS_TYPE_DIR))

    # Each file registers itself in its parent directory.
    for obj in file_objs:
        dirs[obj['parent_path']]['children'].append(
            (obj['leaf'], obj['ino'], DUFS_TYPE_FILE))

    # ── Allocate data blocks sequentially from DATA_LBA ───────────────────────
    next_lba     = DATA_LBA
    block_bitmap = bytearray(BLOCK_SIZE)    # 4096 bytes = 32768 bits

    def alloc_block():
        """Allocate one 4096-byte data block; return its start LBA."""
        nonlocal next_lba
        bit = (next_lba - DATA_LBA) // SECS_PER_BLOCK
        if bit >= BLOCK_SIZE * 8:
            print("error: block bitmap exhausted", file=sys.stderr)
            sys.exit(1)
        block_bitmap[bit >> 3] |= 1 << (bit & 7)
        lba = next_lba
        next_lba += SECS_PER_BLOCK
        return lba

    # Allocate directory data blocks.
    for path, info in dirs.items():
        n_blocks = ceil_div(len(info['children']), DIRENTS_PER_BLOCK) if info['children'] else 0
        info['lbas'] = [alloc_block() for _ in range(n_blocks)]

    # Allocate file data blocks.
    for obj in file_objs:
        data    = obj['data']
        n_data  = ceil_div(len(data), BLOCK_SIZE) if data else 0
        n_direct     = min(n_data, DIRECT_BLOCKS)
        n_indir_data = max(0, n_data - DIRECT_BLOCKS)
        # Double indirect not needed at mkfs time (files are < 4 MB in practice).
        n_indir_data_capped = min(n_indir_data, INDIR_ENTRIES)

        obj['direct_lbas']     = [alloc_block() for _ in range(n_direct)]
        obj['indir_lba']       = alloc_block() if n_indir_data_capped > 0 else 0
        obj['indir_data_lbas'] = [alloc_block() for _ in range(n_indir_data_capped)]
        obj['block_count']     = (n_direct +
                                  (1 if n_indir_data_capped > 0 else 0) +
                                  n_indir_data_capped)

    # ── Build inode bitmap (stored in first BLOCK_SIZE bytes) ─────────────────
    inode_bitmap = bytearray(BLOCK_SIZE)    # 4096 bytes = 32768 bits
    for ino in range(total_inodes):
        inode_bitmap[ino >> 3] |= 1 << (ino & 7)

    # ── Build inode table (256 sectors = 32 BLOCK_SIZE blocks) ───────────────
    INODE_TABLE_SECS = (INODE_COUNT * INODE_SIZE) // SECTOR   # = 256
    inode_table = bytearray(INODE_TABLE_SECS * SECTOR)

    def write_inode(ino, packed):
        sec  = ino // INODES_PER_SECTOR
        slot = ino % INODES_PER_SECTOR
        off  = sec * SECTOR + slot * INODE_SIZE
        inode_table[off : off + INODE_SIZE] = packed

    # Inode 0: reserved — all zeros.

    # Directory inodes.
    for path, info in dirs.items():
        lbas = info['lbas']
        write_inode(info['ino'], pack_inode(
            DUFS_TYPE_DIR, 1, 0,
            len(lbas),
            lbas,
            mtime=build_time,
            atime=build_time,
        ))

    # File inodes.
    for obj in file_objs:
        write_inode(obj['ino'], pack_inode(
            DUFS_TYPE_FILE, 1, len(obj['data']),
            obj['block_count'],
            obj['direct_lbas'],
            obj['indir_lba'],
            0,
            obj['mtime'],
            obj['mtime'],
        ))

    # ── Build superblock ──────────────────────────────────────────────────────
    sb = struct.pack("<IIIIIII",
                     MAGIC,
                     total_sectors,
                     INODE_COUNT,
                     INODE_BITMAP_LBA,
                     BLOCK_BITMAP_LBA,
                     INODE_TABLE_LBA,
                     DATA_LBA)
    sb += b'\x00' * (SECTOR - len(sb))

    # ── Assemble disk image ───────────────────────────────────────────────────
    image = bytearray(total_sectors * SECTOR)

    # Superblock at LBA 1.
    image[1 * SECTOR : 2 * SECTOR] = sb

    # Inode bitmap: LBAs 2–9 (one 4096-byte block).
    image[INODE_BITMAP_LBA * SECTOR : INODE_BITMAP_LBA * SECTOR + BLOCK_SIZE] = inode_bitmap

    # Block bitmap: LBAs 10–17 (one 4096-byte block).
    image[BLOCK_BITMAP_LBA * SECTOR : BLOCK_BITMAP_LBA * SECTOR + BLOCK_SIZE] = block_bitmap

    # Inode table: LBAs 18–273 (256 sectors).
    image[INODE_TABLE_LBA * SECTOR : INODE_TABLE_LBA * SECTOR + len(inode_table)] = inode_table

    # Write directory data blocks.
    def write_dir_blocks(lbas, children):
        for i, lba in enumerate(lbas):
            block = bytearray(BLOCK_SIZE)
            batch = children[i * DIRENTS_PER_BLOCK : (i + 1) * DIRENTS_PER_BLOCK]
            for j, (name, ino, _) in enumerate(batch):
                block[j * DIRENT_SIZE : (j + 1) * DIRENT_SIZE] = pack_dirent(name, ino)
            image[lba * SECTOR : lba * SECTOR + BLOCK_SIZE] = block

    for path, info in dirs.items():
        write_dir_blocks(info['lbas'], info['children'])

    # Write file data blocks.
    for obj in file_objs:
        data = obj['data']
        for i, lba in enumerate(obj['direct_lbas']):
            chunk = data[i * BLOCK_SIZE : (i + 1) * BLOCK_SIZE]
            image[lba * SECTOR : lba * SECTOR + len(chunk)] = chunk

        if obj['indir_lba']:
            # Write the indirect block (array of LBAs).
            indir_block = bytearray(BLOCK_SIZE)
            for i, lba in enumerate(obj['indir_data_lbas']):
                struct.pack_into("<I", indir_block, i * 4, lba)
            il = obj['indir_lba']
            image[il * SECTOR : il * SECTOR + BLOCK_SIZE] = indir_block

            # Write data blocks in the indirect region.
            for i, lba in enumerate(obj['indir_data_lbas']):
                chunk_idx = DIRECT_BLOCKS + i
                chunk = data[chunk_idx * BLOCK_SIZE : (chunk_idx + 1) * BLOCK_SIZE]
                image[lba * SECTOR : lba * SECTOR + len(chunk)] = chunk

    # ── Write output file ─────────────────────────────────────────────────────
    with open(output, 'wb') as f:
        f.write(image)

    # ── Print summary ─────────────────────────────────────────────────────────
    blocks_used = (next_lba - DATA_LBA) // SECS_PER_BLOCK
    print(f"[mkfs] DUFS v3 → {output}  ({total_sectors} sectors, "
          f"{total_sectors * SECTOR // 1024 // 1024} MiB)")
    print(f"       inode bitmap : LBA {INODE_BITMAP_LBA}–{INODE_BITMAP_LBA + SECS_PER_BLOCK - 1}")
    print(f"       block bitmap : LBA {BLOCK_BITMAP_LBA}–{BLOCK_BITMAP_LBA + SECS_PER_BLOCK - 1}")
    print(f"       inode table  : LBA {INODE_TABLE_LBA}–{INODE_TABLE_LBA + INODE_TABLE_SECS - 1}")
    print(f"       data region  : LBA {DATA_LBA}+ ({blocks_used} block(s) used)")

    for path in sorted_dir_paths:
        info = dirs[path]
        label = '/' if path == '' else f"/{path}/"
        n_files = sum(1 for (_, _, t) in info['children'] if t == DUFS_TYPE_FILE)
        n_dirs  = sum(1 for (_, _, t) in info['children'] if t == DUFS_TYPE_DIR)
        print(f"       {label:<35} inode {info['ino']}, "
              f"{n_dirs} dir(s), {n_files} file(s)")

    for obj in file_objs:
        first_lba = obj['direct_lbas'][0] if obj['direct_lbas'] else '-'
        print(f"       /{obj['dest']:<34} inode {obj['ino']}, "
              f"{len(obj['data'])} bytes, LBA {first_lba}")


if __name__ == '__main__':
    main()
