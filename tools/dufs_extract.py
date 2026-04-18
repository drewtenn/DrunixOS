#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
dufs_extract.py — extract one file from an DUFS v3 disk image.

Usage:
    python3 tools/dufs_extract.py <disk.img> <path-in-image> [output]

Examples:
    python3 tools/dufs_extract.py disk.img hello.txt
    python3 tools/dufs_extract.py disk.img core.2 /tmp/core.2
"""

from __future__ import annotations

import pathlib
import struct
import sys


DUFS_MAGIC = 0x44554603
DUFS_BLOCK_SIZE = 4096
DUFS_SECTOR_SIZE = 512
DUFS_INODE_SIZE = 128
DUFS_INODES_PER_SECTOR = 4
DUFS_DIRECT_BLOCKS = 12
DUFS_INDIR_ENTRIES = 1024
DUFS_TYPE_FILE = 1
DUFS_TYPE_DIR = 2
DUFS_MAX_NAME = 256
DUFS_DIRENT_SIZE = 260


class DUFSError(RuntimeError):
    pass


def mbr_partition_offset(image_path: pathlib.Path) -> int:
    with image_path.open("rb") as f:
        mbr = f.read(DUFS_SECTOR_SIZE)
    if len(mbr) != DUFS_SECTOR_SIZE or mbr[510:512] != b"\x55\xaa":
        return 0
    entry = mbr[446:462]
    if len(entry) != 16 or entry[4] == 0:
        return 0
    start = int.from_bytes(entry[8:12], "little")
    sectors = int.from_bytes(entry[12:16], "little")
    if start == 0 or sectors == 0:
        return 0
    return start * DUFS_SECTOR_SIZE


class DUFSImage:
    def __init__(self, image_path: pathlib.Path) -> None:
        self._fp = image_path.open("rb")
        self._base = mbr_partition_offset(image_path)
        self.super = self._read_super()

    def close(self) -> None:
        self._fp.close()

    def _read_super(self) -> dict[str, int]:
        self._fp.seek(self._base + DUFS_SECTOR_SIZE)
        raw = self._fp.read(DUFS_SECTOR_SIZE)
        if len(raw) != DUFS_SECTOR_SIZE:
            raise DUFSError("short read on superblock")

        magic, total_sectors, inode_count, inode_bitmap_lba, block_bitmap_lba, inode_table_lba, data_lba = struct.unpack_from(
            "<7I", raw, 0
        )
        if magic != DUFS_MAGIC:
            raise DUFSError(f"bad DUFS magic: 0x{magic:08x}")

        return {
            "total_sectors": total_sectors,
            "inode_count": inode_count,
            "inode_bitmap_lba": inode_bitmap_lba,
            "block_bitmap_lba": block_bitmap_lba,
            "inode_table_lba": inode_table_lba,
            "data_lba": data_lba,
        }

    def _read_sector(self, lba: int) -> bytes:
        self._fp.seek(self._base + lba * DUFS_SECTOR_SIZE)
        raw = self._fp.read(DUFS_SECTOR_SIZE)
        if len(raw) != DUFS_SECTOR_SIZE:
            raise DUFSError(f"short read on sector {lba}")
        return raw

    def _read_block(self, lba: int) -> bytes:
        self._fp.seek(self._base + lba * DUFS_SECTOR_SIZE)
        raw = self._fp.read(DUFS_BLOCK_SIZE)
        if len(raw) != DUFS_BLOCK_SIZE:
            raise DUFSError(f"short read on block starting at LBA {lba}")
        return raw

    def read_inode(self, ino: int) -> dict[str, object]:
        sector = self.super["inode_table_lba"] + ino // DUFS_INODES_PER_SECTOR
        slot = ino % DUFS_INODES_PER_SECTOR
        raw = self._read_sector(sector)
        off = slot * DUFS_INODE_SIZE

        fields = struct.unpack_from("<HHIIII14I", raw, off)
        inode_type = fields[0]
        link_count = fields[1]
        size = fields[2]
        block_count = fields[3]
        mtime = fields[4]
        atime = fields[5]
        direct = list(fields[6:18])
        indirect = fields[18]
        double_indirect = fields[19]

        return {
            "type": inode_type,
            "link_count": link_count,
            "size": size,
            "block_count": block_count,
            "mtime": mtime,
            "atime": atime,
            "direct": direct,
            "indirect": indirect,
            "double_indirect": double_indirect,
        }

    def _block_map(self, inode: dict[str, object], block_idx: int) -> int:
        direct = inode["direct"]
        assert isinstance(direct, list)

        if block_idx < DUFS_DIRECT_BLOCKS:
            return int(direct[block_idx])
        block_idx -= DUFS_DIRECT_BLOCKS

        if block_idx < DUFS_INDIR_ENTRIES:
            indirect = int(inode["indirect"])
            if indirect == 0:
                return 0
            table = self._read_block(indirect)
            return struct.unpack_from("<I", table, block_idx * 4)[0]

        block_idx -= DUFS_INDIR_ENTRIES
        if block_idx < DUFS_INDIR_ENTRIES * DUFS_INDIR_ENTRIES:
            double_indirect = int(inode["double_indirect"])
            if double_indirect == 0:
                return 0

            l0 = self._read_block(double_indirect)
            l1_idx = block_idx // DUFS_INDIR_ENTRIES
            l2_idx = block_idx % DUFS_INDIR_ENTRIES
            l1_lba = struct.unpack_from("<I", l0, l1_idx * 4)[0]
            if l1_lba == 0:
                return 0
            l1 = self._read_block(l1_lba)
            return struct.unpack_from("<I", l1, l2_idx * 4)[0]

        return 0

    def _read_dir_entries(self, dir_ino: int) -> list[tuple[str, int]]:
        inode = self.read_inode(dir_ino)
        if inode["type"] != DUFS_TYPE_DIR:
            raise DUFSError(f"inode {dir_ino} is not a directory")

        entries: list[tuple[str, int]] = []
        for blk in range(int(inode["block_count"])):
            lba = self._block_map(inode, blk)
            if lba == 0:
                continue
            block = self._read_block(lba)
            for off in range(0, (DUFS_BLOCK_SIZE // DUFS_DIRENT_SIZE) * DUFS_DIRENT_SIZE, DUFS_DIRENT_SIZE):
                raw_name = block[off : off + DUFS_MAX_NAME]
                child_ino = struct.unpack_from("<I", block, off + DUFS_MAX_NAME)[0]
                if child_ino == 0:
                    continue
                name = raw_name.split(b"\0", 1)[0].decode("utf-8")
                entries.append((name, child_ino))
        return entries

    def resolve_path(self, path: str) -> int:
        path = path.lstrip("/")
        if path == "":
            return 1

        cur = 1
        for part in path.split("/"):
            found = 0
            for name, ino in self._read_dir_entries(cur):
                if name == part:
                    found = ino
                    break
            if found == 0:
                raise DUFSError(f"path not found: {path}")
            cur = found
        return cur

    def read_file(self, path: str) -> bytes:
        ino = self.resolve_path(path)
        inode = self.read_inode(ino)
        if inode["type"] != DUFS_TYPE_FILE:
            raise DUFSError(f"path is not a file: {path}")

        size = int(inode["size"])
        data = bytearray()
        block_count = (size + DUFS_BLOCK_SIZE - 1) // DUFS_BLOCK_SIZE

        for blk in range(block_count):
            lba = self._block_map(inode, blk)
            if lba == 0:
                break
            data.extend(self._read_block(lba))

        return bytes(data[:size])


def main(argv: list[str]) -> int:
    if len(argv) not in (3, 4):
        print(
            "usage: dufs_extract.py <disk.img> <path-in-image> [output]",
            file=sys.stderr,
        )
        return 2

    image_path = pathlib.Path(argv[1])
    image_file_path = argv[2]
    output_path = pathlib.Path(argv[3]) if len(argv) == 4 else pathlib.Path(pathlib.PurePosixPath(image_file_path).name)

    fs = DUFSImage(image_path)
    try:
        data = fs.read_file(image_file_path)
    finally:
        fs.close()

    output_path.write_bytes(data)
    print(f"[extract] {image_file_path} -> {output_path} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv))
    except DUFSError as exc:
        print(f"dufs_extract.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
