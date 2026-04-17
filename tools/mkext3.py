#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
mkext3.py — build a deterministic Linux-compatible ext3 root image.

The image uses a single block group, 4096-byte blocks, 128-byte inodes, classic
block maps, ext2 directory entries with file types, and an internal JBD journal
inode. It is intentionally small and deterministic so Drunix can boot a native
ext3 reader without relying on host e2fsprogs.
"""

import struct
import sys

SECTOR = 512
BLOCK = 4096
INODES = 1024
INODE_SIZE = 128
FIRST_NON_RESERVED_INO = 11
ROOT_INO = 2
JOURNAL_INO = 8
JOURNAL_BLOCKS = 1024
EXT3_MAGIC = 0xEF53
EXT3_VALID_FS = 1
EXT3_FEATURE_COMPAT_HAS_JOURNAL = 0x0004
EXT3_FEATURE_INCOMPAT_FILETYPE = 0x0002
EXT3_FEATURE_RO_COMPAT_LARGE_FILE = 0x0002
JBD_MAGIC = 0xC03B3998
JBD_SUPERBLOCK_V2 = 4
FS_UUID = bytes.fromhex("4452554e4958455833524f4f54303031")

S_IFDIR = 0x4000
S_IFREG = 0x8000
FT_REG = 1
FT_DIR = 2

BLOCK_BITMAP = 2
INODE_BITMAP = 3
INODE_TABLE = 4
INODE_TABLE_BLOCKS = (INODES * INODE_SIZE + BLOCK - 1) // BLOCK
DATA_START = INODE_TABLE + INODE_TABLE_BLOCKS


def align4(n):
    return (n + 3) & ~3


def set_bit(buf, bit):
    buf[bit // 8] |= 1 << (bit % 8)


def set_padding_bits(buf, valid_bits):
    for bit in range(valid_bits, len(buf) * 8):
        set_bit(buf, bit)


def pack_inode(mode, size, blocks, links, indirect=0, mtime=0):
    direct = list(blocks[:12])
    extra = list(blocks[12:])
    block_count = len(blocks) + (1 if extra else 0)
    ptrs = direct + [0] * (12 - len(direct)) + [indirect, 0, 0]
    sectors = block_count * (BLOCK // SECTOR)
    return struct.pack(
        "<HHIIIIIHHIII15I4I12s",
        mode, 0, size, mtime, mtime, mtime, 0, 0, links,
        sectors, 0, 0, *ptrs, 0, 0, 0, 0, b"\0" * 12,
    )


def dir_entry(name, ino, ftype, rec_len=None):
    nb = name.encode("ascii")
    need = align4(8 + len(nb))
    if rec_len is None:
        rec_len = need
    return struct.pack("<IHBB", ino, rec_len, len(nb), ftype) + nb + b"\0" * (rec_len - 8 - len(nb))


def build_dir(entries):
    raw = bytearray()
    for i, (name, ino, ftype) in enumerate(entries):
        need = align4(8 + len(name.encode("ascii")))
        rec_len = need if i + 1 < len(entries) else BLOCK - len(raw)
        raw += dir_entry(name, ino, ftype, rec_len)
    if not raw:
        raw += dir_entry(".", ROOT_INO, FT_DIR, BLOCK)
    return bytes(raw)


def build_journal_superblock():
    raw = bytearray(BLOCK)

    struct.pack_into(">III", raw, 0, JBD_MAGIC, JBD_SUPERBLOCK_V2, 0)
    struct.pack_into(">I", raw, 12, BLOCK)
    struct.pack_into(">I", raw, 16, JOURNAL_BLOCKS)
    struct.pack_into(">I", raw, 20, 1)
    struct.pack_into(">I", raw, 24, 1)
    struct.pack_into(">I", raw, 28, 0)
    raw[48:64] = FS_UUID
    return bytes(raw)


def main():
    args = sys.argv[1:]
    if len(args) < 2 or (len(args) - 2) % 2:
        print("usage: mkext3.py <output> <total_sectors> [<src> <dest> ...]", file=sys.stderr)
        sys.exit(1)

    out = args[0]
    sectors = int(args[1])
    total_blocks = sectors // (BLOCK // SECTOR)
    pairs = [(args[i], args[i + 1]) for i in range(2, len(args), 2)]
    if total_blocks < DATA_START + 16:
        raise SystemExit("image too small")

    dirs = {
        "": {"ino": ROOT_INO, "children": []},
        "dufs": {"ino": None, "children": []},
        "lost+found": {"ino": None, "children": []},
    }
    files = []
    for src, dest in pairs:
        with open(src, "rb") as f:
            data = f.read()
        parts = [p for p in dest.split("/") if p]
        if not parts:
            raise SystemExit(f"invalid destination {dest!r}")
        for depth in range(1, len(parts)):
            dpath = "/".join(parts[:depth])
            dirs.setdefault(dpath, {"ino": None, "children": []})
        files.append({
            "src": src,
            "dest": dest,
            "parent": "/".join(parts[:-1]),
            "name": parts[-1],
            "data": data,
            "ino": None,
        })

    next_ino = FIRST_NON_RESERVED_INO
    for path in sorted([p for p in dirs if p], key=lambda p: (p.count("/"), p)):
        dirs[path]["ino"] = next_ino
        next_ino += 1
    for f in files:
        f["ino"] = next_ino
        next_ino += 1
    if next_ino > INODES:
        raise SystemExit("too many inodes")

    for path, info in dirs.items():
        if not path:
            continue
        parent = "/".join(path.split("/")[:-1])
        name = path.split("/")[-1]
        dirs[parent]["children"].append((name, info["ino"], FT_DIR))
    for f in files:
        dirs[f["parent"]]["children"].append((f["name"], f["ino"], FT_REG))

    image = bytearray(total_blocks * BLOCK)
    block_bitmap = bytearray(BLOCK)
    inode_bitmap = bytearray(BLOCK)
    inode_table = bytearray(INODE_TABLE_BLOCKS * BLOCK)
    next_block = DATA_START

    def alloc_block(payload=b""):
        nonlocal next_block
        if next_block >= total_blocks:
            raise SystemExit("out of blocks")
        b = next_block
        next_block += 1
        image[b * BLOCK:b * BLOCK + len(payload)] = payload
        return b

    for b in range(DATA_START):
        set_bit(block_bitmap, b)
    for ino in range(1, FIRST_NON_RESERVED_INO):
        set_bit(inode_bitmap, ino - 1)

    def write_inode(ino, packed):
        off = (ino - 1) * INODE_SIZE
        inode_table[off:off + INODE_SIZE] = packed
        set_bit(inode_bitmap, ino - 1)

    journal_blocks = []
    for i in range(JOURNAL_BLOCKS):
        payload = build_journal_superblock() if i == 0 else b""
        journal_blocks.append(alloc_block(payload))
    journal_indirect = 0
    if len(journal_blocks) > 12:
        payload = bytearray(BLOCK)
        for i, block in enumerate(journal_blocks[12:]):
            struct.pack_into("<I", payload, i * 4, block)
        journal_indirect = alloc_block(payload)
    write_inode(JOURNAL_INO,
                pack_inode(S_IFREG | 0o600, JOURNAL_BLOCKS * BLOCK,
                           journal_blocks, 1, journal_indirect))

    dir_blocks = {}
    for path, info in dirs.items():
        parent_ino = ROOT_INO if not path or "/" not in path else dirs["/".join(path.split("/")[:-1])]["ino"]
        entries = [(".", info["ino"], FT_DIR), ("..", parent_ino, FT_DIR)] + sorted(info["children"])
        b = alloc_block(build_dir(entries))
        dir_blocks[path] = b
        subdir_count = sum(1 for _name, _ino, ftype in info["children"] if ftype == FT_DIR)
        write_inode(info["ino"],
                    pack_inode(S_IFDIR | 0o755, BLOCK, [b],
                               2 + subdir_count))

    for f in files:
        blocks = []
        data = f["data"]
        for off in range(0, len(data), BLOCK):
            blocks.append(alloc_block(data[off:off + BLOCK]))
        indirect = 0
        if len(blocks) > 12:
            if len(blocks) > 12 + BLOCK // 4:
                raise SystemExit(f"{f['dest']} is too large for mkext3 single-indirect writer")
            payload = bytearray(BLOCK)
            for i, block in enumerate(blocks[12:]):
                struct.pack_into("<I", payload, i * 4, block)
            indirect = alloc_block(payload)
        if not blocks and len(data) == 0:
            blocks = []
        write_inode(f["ino"], pack_inode(S_IFREG | 0o755, len(data), blocks, 1,
                                          indirect))

    for b in range(DATA_START, next_block):
        set_bit(block_bitmap, b)
    set_padding_bits(block_bitmap, total_blocks)
    set_padding_bits(inode_bitmap, INODES)

    free_blocks = total_blocks - next_block
    used_inodes = next_ino - 1
    free_inodes = INODES - used_inodes
    superblock = bytearray(1024)
    def u16(off, value):
        struct.pack_into("<H", superblock, off, value)
    def u32(off, value):
        struct.pack_into("<I", superblock, off, value)

    u32(0, INODES)
    u32(4, total_blocks)
    u32(8, 0)
    u32(12, free_blocks)
    u32(16, free_inodes)
    u32(20, 0)
    u32(24, 2)
    u32(28, 2)
    u32(32, total_blocks)
    u32(36, total_blocks)
    u32(40, INODES)
    u32(44, 0)
    u32(48, 0)
    u16(52, 0)
    u16(54, 0xFFFF)
    u16(56, EXT3_MAGIC)
    u16(58, EXT3_VALID_FS)
    u16(60, 1)
    u16(62, 0)
    u32(64, 0)
    u32(68, 0)
    u32(72, 0)
    u32(76, 1)
    u16(80, 0)
    u16(82, 0)
    u32(84, FIRST_NON_RESERVED_INO)
    u16(88, INODE_SIZE)
    u16(90, 0)
    u32(92, EXT3_FEATURE_COMPAT_HAS_JOURNAL)
    u32(96, EXT3_FEATURE_INCOMPAT_FILETYPE)
    u32(100, EXT3_FEATURE_RO_COMPAT_LARGE_FILE)
    superblock[104:120] = FS_UUID
    superblock[120:136] = b"DrunixExt3\0" + b"\0" * 5
    u32(200, 0)
    superblock[204] = 0
    superblock[205] = 0
    u16(206, 0)
    u32(224, JOURNAL_INO)
    u32(228, 0)
    u32(232, 0)
    image[1024:2048] = superblock

    bg = struct.pack("<IIIHHHH12s", BLOCK_BITMAP, INODE_BITMAP, INODE_TABLE,
                     free_blocks, free_inodes, len(dirs), 0, b"\0" * 12)
    image[BLOCK:BLOCK + len(bg)] = bg
    image[BLOCK_BITMAP * BLOCK:(BLOCK_BITMAP + 1) * BLOCK] = block_bitmap
    image[INODE_BITMAP * BLOCK:(INODE_BITMAP + 1) * BLOCK] = inode_bitmap
    image[INODE_TABLE * BLOCK:INODE_TABLE * BLOCK + len(inode_table)] = inode_table

    with open(out, "wb") as f:
        f.write(image[:sectors * SECTOR])


if __name__ == "__main__":
    main()
