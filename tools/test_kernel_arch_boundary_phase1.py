#!/usr/bin/env python3
"""
Focused regression guard for the Phase 1 architecture boundary.

Shared kernel code must stop reaching into x86-specific time, console, and
debug-port interfaces directly. This check intentionally targets the concrete
call sites migrated in Phase 1 rather than trying to infer architecture
ownership from the whole tree.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_INCLUDES = {
    "clock.h": [
        ROOT / "kernel/lib/klog.c",
        ROOT / "kernel/fs/fs.c",
        ROOT / "kernel/fs/ext3/main.c",
        ROOT / "kernel/fs/ext3/mutation.c",
        ROOT / "kernel/proc/syscall/info.c",
        ROOT / "kernel/proc/syscall/time.c",
        ROOT / "kernel/proc/syscall/vfs/path.c",
        ROOT / "kernel/proc/syscall/vfs/stat.c",
    ],
    "io.h": [
        ROOT / "kernel/lib/klog.c",
    ],
}

FORBIDDEN_PATTERNS = {
    r"\bprint_string\s*\(": [
        ROOT / "kernel/lib/klog.c",
        ROOT / "kernel/drivers/tty.c",
    ],
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    for header, paths in FORBIDDEN_INCLUDES.items():
        needle = f'#include "{header}"'
        for path in paths:
            if needle in path.read_text():
                fail(f"{path.relative_to(ROOT)} still includes {header}")

    for pattern, paths in FORBIDDEN_PATTERNS.items():
        regex = re.compile(pattern)
        for path in paths:
            if regex.search(path.read_text()):
                fail(f"{path.relative_to(ROOT)} still matches {pattern}")

    print("phase1 boundary guard passed")


if __name__ == "__main__":
    main()
