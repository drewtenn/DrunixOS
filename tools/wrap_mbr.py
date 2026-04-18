#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
wrap_mbr.py - wrap a raw filesystem payload in a single-partition MBR disk.
"""

import struct
import sys

SECTOR = 512
MBR_SIGNATURE = b"\x55\xaa"
PARTITION_ENTRY = 446


def usage():
    print(
        "usage: wrap_mbr.py <payload> <output> <start-sector> <total-sectors> <type>",
        file=sys.stderr,
    )
    return 1


def parse_int(text, field):
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise SystemExit(f"wrap_mbr.py: invalid {field}: {text!r}") from exc
    return value


def main():
    if len(sys.argv) != 6:
        return usage()

    payload_path, output_path = sys.argv[1], sys.argv[2]
    start_sector = parse_int(sys.argv[3], "start sector")
    total_sectors = parse_int(sys.argv[4], "total sectors")
    part_type = parse_int(sys.argv[5], "partition type")

    if start_sector <= 0 or total_sectors <= start_sector:
        return usage()
    if not (0 <= part_type <= 0xFF):
        return usage()

    with open(payload_path, "rb") as f:
        payload = f.read()

    payload_sectors = (len(payload) + SECTOR - 1) // SECTOR
    if start_sector + payload_sectors > total_sectors:
        raise SystemExit("payload does not fit in partition")

    image = bytearray(total_sectors * SECTOR)
    image[start_sector * SECTOR:start_sector * SECTOR + len(payload)] = payload

    struct.pack_into("<B", image, PARTITION_ENTRY + 4, part_type)
    struct.pack_into("<I", image, PARTITION_ENTRY + 8, start_sector)
    struct.pack_into("<I", image, PARTITION_ENTRY + 12, payload_sectors)
    image[510:512] = MBR_SIGNATURE

    with open(output_path, "wb") as f:
        f.write(image)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
