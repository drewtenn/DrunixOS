#!/usr/bin/env python3
"""Check that ARM64 target platform code stays under kernel/arch/arm64."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

REQUIRED_FILES = [
    Path("kernel/arch/arm64/platform/platform.h"),
    Path("kernel/arch/arm64/platform/raspi3b/platform.h"),
    Path("kernel/arch/arm64/platform/raspi3b/uart.c"),
    Path("kernel/arch/arm64/platform/raspi3b/irq.c"),
    Path("kernel/arch/arm64/platform/raspi3b/video.c"),
    Path("kernel/arch/arm64/platform/raspi3b/usb_hci.c"),
]

FORBIDDEN_ARCH_FILES = [
    Path("kernel/arch/arm64/uart.c"),
    Path("kernel/arch/arm64/uart.h"),
    Path("kernel/arch/arm64/irq.c"),
    Path("kernel/arch/arm64/irq.h"),
    Path("kernel/arch/arm64/video.c"),
    Path("kernel/arch/arm64/video.h"),
    Path("kernel/arch/arm64/usb_keyboard.c"),
    Path("kernel/arch/arm64/usb_keyboard.h"),
]

FORBIDDEN_NON_ARCH_FILES = [
    Path("kernel/platform/platform.h"),
    Path("kernel/platform/raspi3b/platform.h"),
    Path("kernel/platform/raspi3b/uart.c"),
    Path("kernel/platform/raspi3b/irq.c"),
    Path("kernel/platform/raspi3b/video.c"),
    Path("kernel/platform/raspi3b/usb_hci.c"),
]

FORBIDDEN_ARCH_TOKENS = [
    "0x3F000000",
    "0x3F00B880",
    "0x3F980000",
    "0x40000040",
    "0x40000060",
    "BCM283",
    "DWC2",
    "mini-UART",
]


def main() -> int:
    errors: list[str] = []

    for rel in REQUIRED_FILES:
        if not (ROOT / rel).is_file():
            errors.append(f"missing required arch-local platform file: {rel}")

    for rel in FORBIDDEN_ARCH_FILES:
        if (ROOT / rel).exists():
            errors.append(f"Pi-specific file still flattened under arch/arm64: {rel}")

    for rel in FORBIDDEN_NON_ARCH_FILES:
        if (ROOT / rel).exists():
            errors.append(f"Pi-specific file remains outside arch tree: {rel}")

    arch_root = ROOT / "kernel/arch/arm64"
    for path in sorted(arch_root.rglob("*")):
        if path.suffix not in {".c", ".h", ".S"}:
            continue
        rel = path.relative_to(ROOT)
        if rel.parts[:4] == ("kernel", "arch", "arm64", "platform"):
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        for token in FORBIDDEN_ARCH_TOKENS:
            if token in text:
                errors.append(f"{rel}: contains platform token {token}")

    if errors:
        print("arm64 platform split check failed:")
        for error in errors:
            print(error)
        return 1

    print("arm64 platform split check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
