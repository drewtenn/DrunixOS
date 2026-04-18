#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
check_ext3_linux_compat.py - structural checks for Drunix ext3 images.

This is not a replacement for e2fsck.  It verifies the image properties that
make the deterministic Drunix builder look like a conventional Linux ext3
filesystem: dynamic-revision superblock, internal journal inode 8, clean JBD
superblock, sane directory link counts, and padded allocation bitmaps.
"""

import struct
import sys

BLOCK = 4096
INODE_SIZE = 128
ROOT_INO = 2
JOURNAL_INO = 8
EXT3_MAGIC = 0xEF53
EXT3_FEATURE_COMPAT_HAS_JOURNAL = 0x0004
EXT3_FEATURE_INCOMPAT_FILETYPE = 0x0002
EXT3_FEATURE_RO_COMPAT_LARGE_FILE = 0x0002
EXT3_VALID_FS = 1
S_IFMT = 0xF000
S_IFREG = 0x8000
S_IFDIR = 0x4000
JBD_MAGIC = 0xC03B3998
JBD_SUPERBLOCK_V2 = 4


def fail(msg):
    raise SystemExit(f"ext3 compat check failed: {msg}")


def bit_is_set(buf, bit):
    return bool(buf[bit // 8] & (1 << (bit % 8)))


def mbr_partition_offset(path):
    with open(path, "rb") as f:
        mbr = f.read(512)
    if len(mbr) != 512 or mbr[510:512] != b"\x55\xaa":
        return 0
    entry = mbr[446:462]
    if len(entry) != 16 or entry[4] == 0:
        return 0
    start = int.from_bytes(entry[8:12], "little")
    sectors = int.from_bytes(entry[12:16], "little")
    if start == 0 or sectors == 0:
        return 0
    return start * 512


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.img = f.read()
        self.base = mbr_partition_offset(path)
        self.sb = self.img[self.base + 1024:self.base + 2048]
        if len(self.sb) != 1024:
            fail("missing superblock")
        if self.u16(self.sb, 56) != EXT3_MAGIC:
            fail("bad superblock magic")
        self.blocks_count = self.u32(self.sb, 4)
        self.first_data_block = self.u32(self.sb, 20)
        self.log_block_size = self.u32(self.sb, 24)
        self.block_size = 1024 << self.log_block_size
        self.blocks_per_group = self.u32(self.sb, 32)
        self.inodes_count = self.u32(self.sb, 0)
        self.inodes_per_group = self.u32(self.sb, 40)
        self.rev_level = self.u32(self.sb, 76)
        self.first_ino = self.u32(self.sb, 84)
        self.inode_size = self.u16(self.sb, 88)
        self.feature_compat = self.u32(self.sb, 92)
        self.feature_incompat = self.u32(self.sb, 96)
        self.feature_ro_compat = self.u32(self.sb, 100)
        self.journal_inum = self.u32(self.sb, 224)
        if self.block_size != BLOCK:
            fail(f"unexpected block size {self.block_size}")
        self.bg = self.block(1)
        self.block_bitmap = self.u32(self.bg, 0)
        self.inode_bitmap = self.u32(self.bg, 4)
        self.inode_table = self.u32(self.bg, 8)

    @staticmethod
    def u16(buf, off):
        return struct.unpack_from("<H", buf, off)[0]

    @staticmethod
    def u32(buf, off):
        return struct.unpack_from("<I", buf, off)[0]

    @staticmethod
    def be32(buf, off):
        return struct.unpack_from(">I", buf, off)[0]

    def block(self, num):
        off = self.base + num * self.block_size
        return self.img[off:off + self.block_size]

    def inode(self, ino):
        off = self.base + self.inode_table * self.block_size + (ino - 1) * self.inode_size
        raw = self.img[off:off + self.inode_size]
        if len(raw) != self.inode_size:
            fail(f"inode {ino} outside image")
        mode = self.u16(raw, 0)
        size = self.u32(raw, 4)
        links = self.u16(raw, 26)
        blocks = self.u32(raw, 28)
        flags = self.u32(raw, 32)
        ptrs = list(struct.unpack_from("<15I", raw, 40))
        return {
            "mode": mode,
            "size": size,
            "links": links,
            "blocks": blocks,
            "flags": flags,
            "ptrs": ptrs,
        }

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

    def dir_entries(self, ino):
        inode = self.inode(ino)
        if inode["mode"] & S_IFMT != S_IFDIR:
            fail(f"inode {ino} is not a directory")
        entries = []
        pos = 0
        size = inode["size"]
        while pos < size:
            logical = pos // self.block_size
            block = inode["ptrs"][logical]
            if block == 0:
                fail(f"directory inode {ino} has a sparse block")
            data = self.block(block)
            off = pos % self.block_size
            while off + 8 <= self.block_size and pos < size:
                de_ino, rec_len, name_len, ftype = struct.unpack_from(
                    "<IHBB", data, off
                )
                if rec_len < 8 or off + rec_len > self.block_size:
                    fail(f"corrupt directory entry in inode {ino}")
                name = data[off + 8:off + 8 + name_len].decode("ascii")
                if de_ino:
                    entries.append((name, de_ino, ftype))
                off += rec_len
                pos = (pos // self.block_size) * self.block_size + off
                if off >= self.block_size:
                    break
            pos = ((pos + self.block_size - 1) // self.block_size) * self.block_size
        return entries


def check_super(img):
    if img.rev_level != 1:
        fail("superblock is not dynamic revision")
    if img.u16(img.sb, 58) != EXT3_VALID_FS:
        fail("filesystem state is not clean")
    if img.feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL == 0:
        fail("has_journal feature is not set")
    if img.feature_incompat != EXT3_FEATURE_INCOMPAT_FILETYPE:
        fail(f"unexpected incompat features 0x{img.feature_incompat:x}")
    if img.feature_ro_compat & EXT3_FEATURE_RO_COMPAT_LARGE_FILE == 0:
        fail("large_file ro-compatible feature is not set")
    if img.journal_inum != JOURNAL_INO:
        fail(f"journal inode is {img.journal_inum}, expected {JOURNAL_INO}")


def check_journal(img):
    inode = img.inode(JOURNAL_INO)
    if inode["mode"] & S_IFMT != S_IFREG:
        fail("journal inode is not a regular file")
    if inode["links"] != 1:
        fail("journal inode link count is not 1")
    if inode["size"] < 1024 * BLOCK:
        fail("journal is smaller than 1024 filesystem blocks")
    expected_sectors = inode["size"] // 512
    if inode["ptrs"][12] != 0:
        expected_sectors += BLOCK // 512
    if inode["blocks"] != expected_sectors:
        fail("journal inode i_blocks does not match allocated blocks")
    jsb = img.journal_superblock()
    if img.be32(jsb, 0) != JBD_MAGIC:
        fail("journal superblock magic is missing")
    if img.be32(jsb, 4) != JBD_SUPERBLOCK_V2:
        fail("journal is not a JBD v2 superblock")
    if img.be32(jsb, 12) != BLOCK:
        fail("journal block size does not match filesystem block size")
    maxlen = img.be32(jsb, 16)
    if maxlen != inode["size"] // BLOCK:
        fail("journal maxlen does not match inode size")
    if img.be32(jsb, 20) != 1:
        fail("journal first transaction block is not 1")
    if img.be32(jsb, 28) != 0:
        fail("journal is not clean")


def check_bitmap_padding(img):
    block_map = img.block(img.block_bitmap)
    inode_map = img.block(img.inode_bitmap)
    for bit in range(img.blocks_count, len(block_map) * 8):
        if not bit_is_set(block_map, bit):
            fail("block bitmap padding bits are not set")
    for bit in range(img.inodes_count, len(inode_map) * 8):
        if not bit_is_set(inode_map, bit):
            fail("inode bitmap padding bits are not set")


def check_dir_links(img):
    dirs = {ROOT_INO}
    children = {}
    queue = [ROOT_INO]
    while queue:
        ino = queue.pop(0)
        entries = img.dir_entries(ino)
        subdirs = []
        for name, child, ftype in entries:
            if name in (".", ".."):
                continue
            child_inode = img.inode(child)
            if child_inode["mode"] & S_IFMT == S_IFDIR:
                subdirs.append(child)
                if child not in dirs:
                    dirs.add(child)
                    queue.append(child)
        children[ino] = subdirs
    for ino in dirs:
        expected = 2 + len(children.get(ino, []))
        actual = img.inode(ino)["links"]
        if actual != expected:
            fail(f"directory inode {ino} links {actual}, expected {expected}")


def main():
    if len(sys.argv) != 2:
        print("usage: check_ext3_linux_compat.py <disk.img>", file=sys.stderr)
        return 2
    img = Image(sys.argv[1])
    check_super(img)
    check_journal(img)
    check_bitmap_padding(img)
    check_dir_links(img)
    print("ext3 linux compatibility structure ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
