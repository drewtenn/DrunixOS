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
