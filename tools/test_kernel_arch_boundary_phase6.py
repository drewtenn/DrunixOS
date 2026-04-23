#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN = {
    ROOT / "kernel/proc/elf.c": [r"\behdr\.e_machine\s*!=\s*EM_386\b"],
}

REQUIRED = {
    ROOT / "kernel/arch/arch.h": [
        r"\barch_syscall_number\b",
        r"\barch_syscall_arg0\b",
        r"\barch_syscall_arg1\b",
        r"\barch_syscall_arg2\b",
        r"\barch_syscall_arg3\b",
        r"\barch_syscall_arg4\b",
        r"\barch_syscall_arg5\b",
        r"\barch_syscall_set_result\b",
        r"\barch_trap_frame_is_syscall\b",
        r"\barch_trap_frame_fault_addr\b",
    ],
    ROOT / "kernel/proc/elf.c": [
        r"\belf_load_file\b[\s\S]*?\barch_elf_load_user_image\s*\(",
    ],
    ROOT / "kernel/proc/syscall.c": [
        r"\bsyscall_dispatch_from_frame\b[\s\S]*?\barch_syscall_[A-Za-z0-9_]*\s*\(",
        r"\bsyscall_dispatch_from_frame\b[\s\S]*?\barch_syscall_set_result\b",
    ],
}

_COMMENT_RE = re.compile(r"/\*.*?\*/|//.*?$", re.DOTALL | re.MULTILINE)


def strip_comments(text):
    return _COMMENT_RE.sub("", text)


def check(path_map, predicate, label):
    for path, patterns in path_map.items():
        text = strip_comments(path.read_text())
        for pattern in patterns:
            if predicate(re.search(pattern, text)):
                print(f"{label}: {path.relative_to(ROOT)} {pattern}", file=sys.stderr)
                raise SystemExit(1)

check(REQUIRED, lambda m: not m, "missing")
check(FORBIDDEN, bool, "forbidden")
print("phase6 boundary guard passed")
