#!/usr/bin/env python3
"""
Focused regression guard for the remaining process/trap/context arch boundary.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_PATTERNS = {
    ROOT / "kernel/proc/process.h": [
        r"\btrap_frame_t\b",
        r"\bsaved_esp\b",
        r"\buser_tls_base\b",
        r"\buser_tls_limit\b",
        r"\buser_tls_limit_in_pages\b",
        r"\buser_tls_present\b",
        r"\bfpu_state\b",
    ],
    ROOT / "kernel/proc/process.c": [
        r'extern void process_enter_usermode',
        r'extern void process_initial_launch',
        r'extern void process_exec_launch',
        r'\bpaging_guard_page\b',
        r'\bpaging_unguard_page\b',
        r'#include "gdt\.h"',
        r'#include "sse\.h"',
    ],
    ROOT / "kernel/proc/sched.c": [
        r'extern uint32_t g_irq_frame_esp',
        r'\bswitch_context\s*\(',
        r'#include "gdt\.h"',
        r'#include "paging\.h"',
        r'sti; hlt; cli',
    ],
}

REQUIRED_PATTERNS = {
    ROOT / "kernel/proc/process.h": [r"\barch_process_state_t\s+arch_state\b"],
    ROOT / "kernel/proc/process.c": [
        r"\barch_process_build_initial_frame\b",
        r"\barch_process_build_exec_frame\b",
        r"\barch_process_launch\b",
        r"\barch_kstack_guard\b",
    ],
    ROOT / "kernel/proc/sched.c": [
        r"\barch_context_prepare\b",
        r"\barch_context_switch\b",
        r"\barch_idle_wait\b",
        r"\barch_current_irq_frame\b",
        r"\barch_irq_frame_is_user\b",
    ],
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
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

    print("phase4/5 boundary guard passed")


if __name__ == "__main__":
    main()
