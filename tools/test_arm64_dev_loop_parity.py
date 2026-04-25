#!/usr/bin/env python3
"""Check that arm64 Make dev-loop targets are real arm64 targets."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def make_dry_run(target: str, *args: str) -> str:
    result = subprocess.run(
        ["make", "-B", "-n", "ARCH=arm64", target, *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise SystemExit(result.returncode)
    return output


def reject_x86_delegate(target: str, output: str, failures: list[str]) -> None:
    if "ARCH=x86" in output:
        failures.append(f"{target} delegates to ARCH=x86")
    if "not implemented yet" in output:
        failures.append(f"{target} is still a not-implemented stub")


def main() -> int:
    failures: list[str] = []

    scanner_targets = (
        "compile-commands",
        "format-check",
        "cppcheck",
        "sparse-check",
        "clang-tidy-include-check",
        "scan",
    )
    for target in scanner_targets:
        output = make_dry_run(target)
        reject_x86_delegate(target, output, failures)

    compile_output = make_dry_run("compile-commands")
    if "kernel/platform/raspi3b/uart.o" not in compile_output:
        failures.append("compile-commands does not include arm64 platform objects")
    if "--kernel-cc=\"aarch64" not in compile_output:
        failures.append("compile-commands does not use the arm64 kernel compiler")

    debug_output = make_dry_run("debug")
    reject_x86_delegate("debug", debug_output, failures)
    if "-s -S" not in debug_output:
        failures.append("debug does not start QEMU with the GDB stub paused")
    if "kernel-arm64.elf" not in debug_output:
        failures.append("debug does not load the arm64 kernel image")

    debug_user_output = make_dry_run("debug-user", "APP=shell")
    reject_x86_delegate("debug-user", debug_user_output, failures)
    if "add-symbol-file build/arm64-user/shell 0x02000000" not in debug_user_output:
        failures.append("debug-user does not load arm64 user symbols")

    for target in ("debug-fresh", "test-halt", "test-threadtest"):
        reject_x86_delegate(target, make_dry_run(target), failures)

    if failures:
        print("arm64 dev-loop parity check failed:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("arm64 dev-loop parity check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
