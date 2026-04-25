#!/usr/bin/env python3
"""
Check that generic kernel code does not grow avoidable ARM64 forks.

The remaining allowed cases are architectural ABI boundaries. Hardware, trap
frame, MMU, device, and per-architecture process entry code belongs under
kernel/arch/.
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

ARCH_TOKENS = ("__aarch64__", "DRUNIX_ARM64", "ARM64_")

ALLOWED_GENERIC_FILES = {
    Path("kernel/proc/core.c"): "ELF core format is still x86-only",
    Path("kernel/proc/syscall.c"): "Linux syscall ABI number map differs by arch",
}


def main() -> int:
    offenders: list[str] = []
    for path in sorted((ROOT / "kernel").rglob("*")):
        if path.suffix not in {".c", ".h"}:
            continue
        rel = path.relative_to(ROOT)
        if rel.parts[:2] in {("kernel", "arch"), ("kernel", "platform")}:
            continue
        if rel in ALLOWED_GENERIC_FILES:
            continue

        text = path.read_text(encoding="utf-8", errors="ignore")
        for lineno, line in enumerate(text.splitlines(), 1):
            if any(token in line for token in ARCH_TOKENS):
                offenders.append(f"{rel}:{lineno}: {line.strip()}")

    if offenders:
        print("generic ARM64 boundary leaks found:")
        for offender in offenders:
            print(offender)
        return 1

    print("arch boundary reuse check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
