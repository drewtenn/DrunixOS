#!/usr/bin/env python3
"""
Focused regression guard for the Phase 2 architecture boundary.

This intentionally fails on the current tree: shared startup and the PC
keyboard/mouse registration code still reach directly into x86 IRQ/PIT
interfaces instead of the planned arch layer.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_PATTERNS = {
    ROOT / "kernel/kernel.c": [
        r"\birq_dispatch_init\s*\(",
        r"\bpit_init\s*\(",
        r"\binterrupts_enable\s*\(",
    ],
    ROOT / "kernel/platform/pc/keyboard.c": [
        r"\birq_register\s*\(\s*1\s*,\s*keyboard_handler\s*\)",
    ],
    ROOT / "kernel/platform/pc/mouse.c": [
        r"\birq_register\s*\(\s*12\s*,\s*mouse_handler\s*\)",
        r"\birq_unmask\s*\(\s*2\s*\)",
        r"\birq_unmask\s*\(\s*12\s*\)",
    ],
}

REQUIRED_PATTERNS = {
    # Target the concrete Phase 2 call sites from the approved plan rather
    # than inferring architecture ownership from the rest of the tree.
    ROOT / "kernel/kernel.c": [
        r"\barch_irq_init\s*\(",
        r"\barch_timer_set_periodic_handler\s*\(\s*sched_tick\s*\)",
        r"\barch_timer_start\s*\(\s*SCHED_HZ\s*\)",
        r"\barch_interrupts_enable\s*\(",
    ],
    ROOT / "kernel/platform/pc/keyboard.c": [
        r"\barch_irq_register\s*\(\s*1\s*,\s*keyboard_handler\s*\)",
    ],
    ROOT / "kernel/platform/pc/mouse.c": [
        r"\barch_irq_register\s*\(\s*12\s*,\s*mouse_handler\s*\)",
        r"\barch_irq_unmask\s*\(\s*2\s*\)",
        r"\barch_irq_unmask\s*\(\s*12\s*\)",
    ],
    ROOT / "kernel/arch/x86/pit.c": [
        r"\bpit_set_periodic_handler\s*\(",
        r"\bpit_start\s*\(",
        r"\bclock_tick\s*\(",
    ],
    ROOT / "kernel/arch/arm64/start_kernel.c": [
        r"\barch_irq_init\s*\(",
        r"\barch_timer_set_periodic_handler\s*\(",
        r"\barch_timer_start\s*\(\s*10u\s*\)",
        r"\barch_interrupts_enable\s*\(",
    ],
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def normalize_c_source(text: str) -> str:
    out = []
    i = 0
    n = len(text)
    in_block_comment = False
    in_line_comment = False
    in_string = False
    in_char = False

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 2
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
                out.append(ch)
            else:
                out.append(" ")
            i += 1
            continue

        if in_string:
            if ch == "\\" and nxt:
                out.extend("  ")
                i += 2
                continue
            if ch == '"':
                in_string = False
            out.append(" " if ch != "\n" else "\n")
            i += 1
            continue

        if in_char:
            if ch == "\\" and nxt:
                out.extend("  ")
                i += 2
                continue
            if ch == "'":
                in_char = False
            out.append(" " if ch != "\n" else "\n")
            i += 1
            continue

        if ch == "/" and nxt == "*":
            in_block_comment = True
            out.extend("  ")
            i += 2
            continue
        if ch == "/" and nxt == "/":
            in_line_comment = True
            out.extend("  ")
            i += 2
            continue
        if ch == '"':
            in_string = True
            out.append(" ")
            i += 1
            continue
        if ch == "'":
            in_char = True
            out.append(" ")
            i += 1
            continue

        out.append(ch)
        i += 1

    return "".join(out)


def main() -> None:
    include_forbidden = {
        ROOT / "kernel/kernel.c": [
            r'#include "irq\.h"',
            r'#include "pit\.h"',
        ],
        ROOT / "kernel/platform/pc/keyboard.c": [
            r'#include "irq\.h"',
        ],
        ROOT / "kernel/platform/pc/mouse.c": [
            r'#include "irq\.h"',
        ],
    }

    include_required = {
        ROOT / "kernel/kernel.c": [
            r'#include "arch\.h"',
        ],
        ROOT / "kernel/platform/pc/keyboard.c": [
            r'#include "arch\.h"',
        ],
        ROOT / "kernel/platform/pc/mouse.c": [
            r'#include "arch\.h"',
        ],
    }

    for path, patterns in include_forbidden.items():
        text = path.read_text()
        for pattern in patterns:
            if re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} still contains {pattern}")

    for path, patterns in include_required.items():
        text = path.read_text()
        for pattern in patterns:
            if not re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} is missing {pattern}")

    for path, patterns in FORBIDDEN_PATTERNS.items():
        text = normalize_c_source(path.read_text())
        for pattern in patterns:
            if re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} still contains {pattern}")

    for path, patterns in REQUIRED_PATTERNS.items():
        text = normalize_c_source(path.read_text())
        for pattern in patterns:
            if not re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} is missing {pattern}")

    print("phase2 boundary guard passed")


if __name__ == "__main__":
    main()
