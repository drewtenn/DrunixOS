#!/usr/bin/env python3
"""
Boot the ARM64 kernel in QEMU and require the console terminal banner/prompt.
"""

from __future__ import annotations

from pathlib import Path
import os
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"
SERIAL_LOG = LOG_DIR / "serial-arm.log"
REQUIRED_MARKERS = (
    "Drunix ARM64 console",
    "drunix> ",
)


def build_arm64() -> None:
    subprocess.run(
        ["make", "ARCH=arm64", "build"],
        cwd=ROOT,
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def read_log() -> str:
    try:
        return SERIAL_LOG.read_text()
    except FileNotFoundError:
        return ""


def main() -> int:
    build_arm64()
    LOG_DIR.mkdir(exist_ok=True)
    SERIAL_LOG.unlink(missing_ok=True)

    proc = subprocess.Popen(
        [
            os.environ.get("QEMU_ARM", "qemu-system-aarch64"),
            "-display",
            "none",
            "-M",
            os.environ.get("QEMU_ARM_MACHINE", "raspi3b"),
            "-kernel",
            "kernel-arm64.elf",
            "-serial",
            "null",
            "-serial",
            f"file:{SERIAL_LOG}",
            "-monitor",
            "none",
            "-no-reboot",
        ],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        deadline = time.time() + 10.0
        while time.time() < deadline:
            log = read_log()
            if all(marker in log for marker in REQUIRED_MARKERS):
                print("arm64 console terminal check passed")
                return 0
            time.sleep(0.2)
        missing = [marker for marker in REQUIRED_MARKERS if marker not in read_log()]
        print("missing console markers:", ", ".join(missing))
        return 1
    finally:
        proc.kill()
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=2.0)


if __name__ == "__main__":
    raise SystemExit(main())
