#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN = {
    ROOT / "kernel/proc/elf.c": [r"\behdr\.e_machine\s*!=\s*EM_386\b"],
    ROOT / "kernel/arch/arm64/exceptions.c": [
        r"\barm64_sync_handler\b[\s\S]*?mrs\s+%0,\s+esr_el1",
        r"\barm64_sync_handler\b[\s\S]*?mrs\s+%0,\s+far_el1",
    ],
    ROOT / "kernel/proc/syscall/syscall_internal.h": [
        r'#include\s+"desktop\.h"',
    ],
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
    ROOT / "kernel/arch/arm64/proc/arch_proc.c": [
        r"\barch_syscall_number\b[\s\S]*?frame->x\s*\[\s*8\s*\]",
        r"\barch_syscall_arg0\b[\s\S]*?frame->x\s*\[\s*0\s*\]",
        r"\barch_syscall_arg1\b[\s\S]*?frame->x\s*\[\s*1\s*\]",
        r"\barch_syscall_arg2\b[\s\S]*?frame->x\s*\[\s*2\s*\]",
        r"\barch_syscall_arg3\b[\s\S]*?frame->x\s*\[\s*3\s*\]",
        r"\barch_syscall_arg4\b[\s\S]*?frame->x\s*\[\s*4\s*\]",
        r"\barch_syscall_arg5\b[\s\S]*?frame->x\s*\[\s*5\s*\]",
        r"\barch_syscall_set_result\b[\s\S]*?frame->x\s*\[\s*0\s*\]",
        r"\barch_trap_frame_is_syscall\b[\s\S]*?\besr_el1\b",
        r"\barch_trap_frame_fault_addr\b[\s\S]*?\bfar_el1\b",
    ],
    ROOT / "kernel/arch/arm64/arch.mk": [
        r"\bARM_INC\s*:=",
        r"\bkernel/proc/process\.arm64\.o\b",
        r"\bkernel/proc/sched\.arm64\.o\b",
        r"\bkernel/proc/syscall\.arm64\.o\b",
        r"\bkernel/proc/syscall/task\.arm64\.o\b",
        # The per-subdir compile rule used to be repeated literally for
        # every kernel subtree.  It now lives in ARM_C_SUBDIR_RULE,
        # iterated over ARM_C_SUBDIRS — guard the expanded inputs.
        r"\bdefine\s+ARM_C_SUBDIR_RULE\b",
        r"\bARM_C_SUBDIRS\b[\s\S]*?\bproc\b",
        r"\bARM_C_SUBDIRS\b[\s\S]*?\bproc/syscall\b",
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
