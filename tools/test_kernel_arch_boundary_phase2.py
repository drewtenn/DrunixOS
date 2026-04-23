#!/usr/bin/env python3
"""
Focused regression guard for the Phase 2 architecture boundary.

This intentionally fails on the current tree: shared startup and the PC
keyboard/mouse registration code still reach directly into x86 IRQ/PIT
interfaces instead of the planned arch layer.
"""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_PATTERNS = {
    ROOT / "kernel/kernel.c": [
        '#include "irq.h"',
        '#include "pit.h"',
        "irq_dispatch_init();",
        "pit_init();",
    ],
    ROOT / "kernel/platform/pc/keyboard.c": [
        '#include "irq.h"',
        "irq_register(1, keyboard_handler);",
    ],
    ROOT / "kernel/platform/pc/mouse.c": [
        '#include "irq.h"',
        "irq_register(12, mouse_handler);",
        "irq_unmask(2);",
        "irq_unmask(12);",
    ],
}

REQUIRED_PATTERNS = {
    ROOT / "kernel/kernel.c": [
        '#include "arch.h"',
        'klog("BOOT", "bringing up interrupt, timer, and clock subsystems");',
        "arch_irq_init();",
        "arch_timer_set_periodic_handler(sched_tick);",
        "arch_timer_start(SCHED_HZ);",
        "arch_interrupts_enable();",
    ],
    ROOT / "kernel/platform/pc/keyboard.c": [
        '#include "arch.h"',
        "arch_irq_register(1, keyboard_handler);",
    ],
    ROOT / "kernel/platform/pc/mouse.c": [
        '#include "arch.h"',
        "arch_irq_register(12, mouse_handler);",
        "arch_irq_unmask(2);",
        "arch_irq_unmask(12);",
    ],
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    for path, patterns in FORBIDDEN_PATTERNS.items():
        text = path.read_text()
        for pattern in patterns:
            if pattern in text:
                fail(f"{path.relative_to(ROOT)} still contains {pattern}")

    for path, patterns in REQUIRED_PATTERNS.items():
        text = path.read_text()
        for pattern in patterns:
            if pattern not in text:
                fail(f"{path.relative_to(ROOT)} is missing {pattern}")

    print("phase2 boundary guard passed")


if __name__ == "__main__":
    main()
