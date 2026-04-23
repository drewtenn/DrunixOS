#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN = {
    ROOT / "kernel/proc/elf.h": [r"\bEM_386\b"],
    ROOT / "kernel/proc/syscall.c": [r"INT 0x80", r"\beax,\s*ebx,\s*ecx"],
    ROOT / "kernel/arch/arm64/exceptions.c": [r"uart_puts\\(\"sync exception"],
}

REQUIRED = {
    ROOT / "kernel/proc/elf.c": [r"\barch_elf", r"\barch_process_build_initial_frame\b"],
    ROOT / "kernel/proc/syscall.c": [r"\barch_syscall", r"\barch_syscall_dispatch\b"],
    ROOT / "kernel/arch/arm64/exceptions.c": [r"\barch_current_irq_frame\b", r"\bsched_record_user_fault\b"],
}

def check(path_map, predicate, label):
    for path, patterns in path_map.items():
        text = path.read_text()
        for pattern in patterns:
            if predicate(re.search(pattern, text)):
                print(f"{label}: {path.relative_to(ROOT)} {pattern}", file=sys.stderr)
                raise SystemExit(1)

check(FORBIDDEN, bool, "forbidden")
check(REQUIRED, lambda m: not m, "missing")
print("phase6 boundary guard passed")
