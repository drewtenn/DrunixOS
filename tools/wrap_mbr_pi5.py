#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""wrap_mbr_pi5.py - write a two-partition MBR header onto an existing image.

The existing tools/wrap_mbr.py wraps a single-partition payload in a fresh
output file. This sibling writes the MBR partition table in-place onto an
image that already has both partitions dd'd into position. Used by
tools/build-pi5-sd-image.sh.

usage:
  wrap_mbr_pi5.py <image> <p1_start> <p1_sectors> <p1_type> \\
                          <p2_start> <p2_sectors> <p2_type>

Partition types use the standard MBR byte values (0x0c FAT32 LBA, 0x83
Linux native, etc.). CHS fields are written zero — every modern BIOS,
UEFI, and the Pi firmware bootloader use the LBA fields and ignore CHS.
"""

import struct
import sys

SECTOR = 512
PARTITION_TABLE_OFFSET = 446
MBR_SIGNATURE_OFFSET = 510


def parse_int(text, field):
    try:
        return int(text, 0)
    except ValueError as exc:
        raise SystemExit(f"wrap_mbr_pi5.py: invalid {field}: {text!r}") from exc


def encode_entry(start_sector, sectors, ptype):
    return (
        b"\x00"
        + b"\x00\x00\x00"  # CHS first (zero — LBA-only consumers ignore)
        + bytes([ptype & 0xFF])
        + b"\x00\x00\x00"  # CHS last
        + start_sector.to_bytes(4, "little")
        + sectors.to_bytes(4, "little")
    )


def main():
    if len(sys.argv) != 8:
        print(
            "usage: wrap_mbr_pi5.py <image> "
            "<p1_start> <p1_sectors> <p1_type> "
            "<p2_start> <p2_sectors> <p2_type>",
            file=sys.stderr,
        )
        return 1

    image_path = sys.argv[1]
    p1_start = parse_int(sys.argv[2], "p1_start")
    p1_sectors = parse_int(sys.argv[3], "p1_sectors")
    p1_type = parse_int(sys.argv[4], "p1_type")
    p2_start = parse_int(sys.argv[5], "p2_start")
    p2_sectors = parse_int(sys.argv[6], "p2_sectors")
    p2_type = parse_int(sys.argv[7], "p2_type")

    for name, value in (
        ("p1_start", p1_start),
        ("p1_sectors", p1_sectors),
        ("p2_start", p2_start),
        ("p2_sectors", p2_sectors),
    ):
        if value < 0 or value >= (1 << 32):
            raise SystemExit(f"wrap_mbr_pi5.py: {name}={value} out of MBR range")
    for name, value in (("p1_type", p1_type), ("p2_type", p2_type)):
        if not (0 <= value <= 0xFF):
            raise SystemExit(f"wrap_mbr_pi5.py: {name}={value:#x} not a byte")

    if p1_start + p1_sectors > p2_start:
        raise SystemExit("wrap_mbr_pi5.py: partition 1 overlaps partition 2")

    with open(image_path, "r+b") as f:
        f.seek(PARTITION_TABLE_OFFSET)
        f.write(encode_entry(p1_start, p1_sectors, p1_type))
        f.write(encode_entry(p2_start, p2_sectors, p2_type))
        f.write(encode_entry(0, 0, 0))
        f.write(encode_entry(0, 0, 0))
        f.seek(MBR_SIGNATURE_OFFSET)
        f.write(b"\x55\xaa")

    return 0


if __name__ == "__main__":
    sys.exit(main())
