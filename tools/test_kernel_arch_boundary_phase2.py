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
        r'#include "irq\.h"',
        r'#include "pit\.h"',
        r"\birq_dispatch_init\s*\(",
        r"\bpit_init\s*\(",
        r"\binterrupts_enable\s*\(",
    ],
    ROOT / "kernel/platform/pc/keyboard.c": [
        r'#include "irq\.h"',
        r"\birq_register\s*\(\s*1\s*,\s*keyboard_handler\s*\)",
    ],
    ROOT / "kernel/platform/pc/mouse.c": [
        r'#include "irq\.h"',
        r"\birq_register\s*\(\s*12\s*,\s*mouse_handler\s*\)",
        r"\birq_unmask\s*\(\s*2\s*\)",
        r"\birq_unmask\s*\(\s*12\s*\)",
    ],
}

REQUIRED_PATTERNS = {
    ROOT / "kernel/kernel.c": [
        r'#include "arch\.h"',
        r"\barch_irq_init\s*\(",
        r"\barch_timer_set_periodic_handler\s*\(\s*sched_tick\s*\)",
        r"\barch_timer_start\s*\(\s*SCHED_HZ\s*\)",
        r"\barch_interrupts_enable\s*\(",
    ],
    ROOT / "kernel/platform/pc/keyboard.c": [
        r'#include "arch\.h"',
        r"\barch_irq_register\s*\(\s*1\s*,\s*keyboard_handler\s*\)",
    ],
    ROOT / "kernel/platform/pc/mouse.c": [
        r'#include "arch\.h"',
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


def main() -> None:
    for path, patterns in FORBIDDEN_PATTERNS.items():
        text = path.read_text()
        for pattern in patterns:
            if re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} still contains {pattern}")

    for path, patterns in REQUIRED_PATTERNS.items():
        text = path.read_text()
        for pattern in patterns:
            if not re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} is missing {pattern}")

    print("phase2 boundary guard passed")


if __name__ == "__main__":
    main()
