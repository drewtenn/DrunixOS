#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
dump_drunix_notes.py — print the DRUNIX text notes from a DrunixOS core file.

Usage:
    python3 tools/dump_drunix_notes.py <core-file>

The three DRUNIX notes mirror the live /proc/<pid>/ memory-forensics views:
    DXVM (0x4458564d) — /proc/<pid>/vmstat
    DXFT (0x44584654) — /proc/<pid>/fault
    DXMP (0x44584d50) — /proc/<pid>/maps

Diff the output against `cat /proc/<pid>/{vmstat,fault,maps}` captured live
before the crash to confirm the post-mortem notes match the live views.
"""

from __future__ import annotations

import struct
import sys

NOTE_TYPES = {
    0x4458564D: "vmstat",
    0x44584654: "fault",
    0x44584D50: "maps",
}


def iter_notes(data: bytes):
    if data[:4] != b"\x7fELF":
        raise SystemExit("not an ELF file")
    if data[4] != 1:
        raise SystemExit("expected 32-bit ELF core")
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, = struct.unpack_from("<H", data, 42)
    phnum, = struct.unpack_from("<H", data, 44)

    for i in range(phnum):
        off = phoff + i * phentsize
        ptype, poffset, _, _, pfilesz = struct.unpack_from("<IIIII", data, off)
        if ptype != 4:  # PT_NOTE
            continue
        pos = poffset
        end = poffset + pfilesz
        while pos + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from("<III", data, pos)
            pos += 12
            name = data[pos:pos + namesz].rstrip(b"\x00")
            pos += (namesz + 3) & ~3
            desc = data[pos:pos + descsz]
            pos += (descsz + 3) & ~3
            yield name, ntype, desc


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    with open(argv[1], "rb") as f:
        data = f.read()

    found = False
    for name, ntype, desc in iter_notes(data):
        if name != b"DRUNIX":
            continue
        label = NOTE_TYPES.get(ntype, f"unknown(0x{ntype:08x})")
        text = desc.rstrip(b"\x00").decode("utf-8", errors="replace")
        print(f"=== DRUNIX {label} ({len(desc)} bytes) ===")
        print(text, end="" if text.endswith("\n") else "\n")
        found = True

    if not found:
        print("no DRUNIX notes found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
