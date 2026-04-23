#!/usr/bin/env python3
"""
Focused regression guard for the Phase 3 architecture/MM boundary.

Shared MM, process, and framebuffer code must stop depending on x86 paging
headers, raw CR3 manipulation, and inline invlpg operations.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_INCLUDES = {
    ROOT / "kernel/mm/fault.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/uaccess.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/syscall/mem.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/process.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/elf.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/mem_forensics.c": ['#include "paging.h"'],
    ROOT / "kernel/gui/desktop.c": ['#include "paging.h"'],
}

FORBIDDEN_PATTERNS = {
    ROOT / "kernel/mm/fault.c": [r"\binvlpg\b", r"\bpaging_entry_"],
    ROOT / "kernel/proc/uaccess.c": [r"\binvlpg\b", r"\bpaging_entry_"],
    ROOT / "kernel/proc/syscall/mem.c": [r"\binvlpg\b", r"\bpaging_entry_"],
    ROOT / "kernel/gui/desktop.c": [r"\bcr3\b", r"\bmov %0, %%cr3\b"],
}

REQUIRED_PATTERNS = {
    ROOT / "kernel/mm/fault.c": [r'#include "arch\.h"', r"\barch_mm_query\b"],
    ROOT / "kernel/proc/uaccess.c": [r'#include "arch\.h"', r"\barch_mm_query\b"],
    ROOT / "kernel/proc/process.c": [r'#include "arch\.h"', r"\barch_aspace_"],
    ROOT / "kernel/gui/desktop.c": [r'#include "arch\.h"', r"\barch_mm_present_begin\b"],
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    for path, needles in FORBIDDEN_INCLUDES.items():
        text = path.read_text()
        for needle in needles:
            if needle in text:
                fail(f"{path.relative_to(ROOT)} still contains {needle}")

    for path, patterns in FORBIDDEN_PATTERNS.items():
        text = path.read_text()
        for pattern in patterns:
            if re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} still matches {pattern}")

    for path, patterns in REQUIRED_PATTERNS.items():
        text = path.read_text()
        for pattern in patterns:
            if not re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} is missing {pattern}")

    print("phase3 boundary guard passed")


if __name__ == "__main__":
    main()
