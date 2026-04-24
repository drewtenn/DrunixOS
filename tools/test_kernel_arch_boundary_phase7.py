#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN = {
    ROOT / "kernel/kernel.c": [
        r"\bvfs_open_file\s*\(\s*DRUNIX_INIT_PROGRAM",
        r"\bprocess_create_file\s*\(",
    ],
    ROOT / "kernel/proc/init_launch.c": [
        r"\bstatic\s+process_t\s+init_proc\b",
        r"\bstatic\s+const\s+char\s+\*argv\s*\[",
        r"\bstatic\s+const\s+char\s+\*envp\s*\[",
    ],
    ROOT / "kernel/arch/arm64/start_kernel.c": [
        r"\barm64_user_smoke_boot\s*\(\s*\)",
    ],
    ROOT / "kernel/proc/process.c": [
        r"\bbuild_user_stack_frame\s*\(",
    ],
}

REQUIRED = {
    ROOT / "kernel/kernel.c": [
        r"\bboot_launch_init_process\s*\(",
    ],
    ROOT / "kernel/proc/init_launch.h": [
        r"\bboot_launch_init_process\s*\(\s*const char \*[^,]+,\s*const char \*[^,]+,\s*const char \*[^,]+,\s*int\s+\w+\s*\)",
    ],
    ROOT / "kernel/proc/init_launch.c": [
        r"\bboot_launch_init_process\s*\(\s*const char \*[^,]+,\s*const char \*[^,]+,\s*const char \*[^,]+,\s*int\s+\w+\s*\)",
        r"\bvfs_open_file\s*\(",
        r"\bprocess_create_file\s*\(",
    ],
    ROOT / "kernel/arch/arch.h": [
        r"\barch_process_build_user_stack\b",
    ],
    ROOT / "kernel/arch/arm64/rootfs.c": [
        r"\barm64_rootfs_register\b",
    ],
    ROOT / "kernel/arch/arm64/arch.mk": [
        r"\brootfs_blob\.o\b",
        r"\barm64init\.elf\b",
        r"\barm64-root\.fs\b",
    ],
}

_COMMENT_RE = re.compile(r"/\*.*?\*/|//.*?$", re.DOTALL | re.MULTILINE)


def strip_comments(text):
    return _COMMENT_RE.sub("", text)


def read_source(path):
    try:
        return strip_comments(path.read_text())
    except FileNotFoundError:
        print(f"missing: {path.relative_to(ROOT)}", file=sys.stderr)
        raise SystemExit(1)


def check(table, predicate, label):
    for path, patterns in table.items():
        text = read_source(path)
        for pattern in patterns:
            if predicate(re.search(pattern, text)):
                print(f"{label}: {path.relative_to(ROOT)} {pattern}", file=sys.stderr)
                raise SystemExit(1)


check(REQUIRED, lambda m: not m, "missing")
check(FORBIDDEN, bool, "forbidden")
print("phase7 boundary guard passed")
