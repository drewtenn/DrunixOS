#!/usr/bin/env python3
"""Verify the unattended BusyBox compatibility harness is wired in."""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "user" / "bbcompat.c"
MAKEFILE = ROOT / "Makefile"
USER_MAKEFILE = ROOT / "user" / "Makefile"


def fail(message: str) -> None:
    print(f"check_busybox_compat_harness: {message}", file=sys.stderr)
    sys.exit(1)


def require(pattern: str, text: str, message: str) -> None:
    if not re.search(pattern, text, flags=re.S):
        fail(message)


def main() -> int:
    if not RUNNER.exists():
        fail("missing user/bbcompat.c runner")

    runner = RUNNER.read_text()
    makefile = MAKEFILE.read_text()
    user_makefile = USER_MAKEFILE.read_text()

    cases = re.findall(r"^\s*BB_CASE\(", runner, flags=re.M)
    if len(cases) < 50:
        fail(f"user/bbcompat.c must define at least 50 BB_CASE entries, got {len(cases)}")

    require(r"\bbbcompat\b", user_makefile, "user/Makefile must build bbcompat")
    require(r"\bbbcompat\b", makefile, "top-level Makefile must include bbcompat in disk image")
    require(r"INIT_PROGRAM\s*\?=", makefile, "Makefile must expose INIT_PROGRAM")
    require(r"\.init-program-flag", makefile, "kernel.o must rebuild when INIT_PROGRAM changes")
    require(r"test-busybox-compat\s*:", makefile, "Makefile must provide test-busybox-compat target")
    require(r"dufs_extract\.py\s+disk-bbcompat\.img\s+bbcompat\.log",
            makefile,
            "test-busybox-compat must extract bbcompat.log from the test disk")
    total_match = re.search(r"#define\s+BBCOMPAT_TOTAL\s+([0-9]+)", runner)
    if not total_match:
        fail("runner must define BBCOMPAT_TOTAL")
    total = int(total_match.group(1))
    if total != len(cases):
        fail(f"BBCOMPAT_TOTAL must match BB_CASE count: {total} != {len(cases)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
