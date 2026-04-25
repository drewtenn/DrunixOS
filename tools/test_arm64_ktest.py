#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
QEMU = os.environ.get("QEMU_ARM", "qemu-system-aarch64")
MACHINE = os.environ.get("QEMU_ARM_MACHINE", "raspi3b")
LOG = ROOT / "logs" / "serial-arm64-ktest.log"
ERR = ROOT / "logs" / "qemu-arm64-ktest.stderr"
SUMMARY = "SUMMARY pass="
SUCCESS = "fail=0"
TIMEOUT = 30


def main() -> int:
    build = subprocess.run(
        ["make", "ARCH=arm64", "KTEST=1", "kernel"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if build.returncode != 0:
        print(build.stdout, end="")
        print(build.stderr, end="")
        return build.returncode

    LOG.parent.mkdir(exist_ok=True)
    LOG.unlink(missing_ok=True)
    ERR.unlink(missing_ok=True)

    with ERR.open("w") as stderr:
        try:
            proc = subprocess.Popen(
                [
                    QEMU,
                    "-display",
                    "none",
                    "-M",
                    MACHINE,
                    "-kernel",
                    "kernel-arm64.elf",
                    "-serial",
                    "null",
                    "-serial",
                    f"file:{LOG}",
                    "-monitor",
                    "none",
                    "-no-reboot",
                ],
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=stderr,
            )
        except FileNotFoundError as exc:
            print(f"failed to start QEMU command {QEMU!r}: {exc}")
            return 1

        try:
            deadline = time.time() + TIMEOUT
            while time.time() < deadline:
                if LOG.exists():
                    text = LOG.read_text(errors="ignore")
                    if ktest_summary_lines(text):
                        break
                if proc.poll() is not None:
                    break
                time.sleep(1)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()

    text = LOG.read_text(errors="ignore") if LOG.exists() else ""
    summary_lines = ktest_summary_lines(text)
    if not summary_lines:
        print("missing ARM64 KTEST summary")
        print(ERR.read_text(errors="ignore"), end="")
        if text:
            print(text[-2000:], end="")
        return 1

    if not any(SUCCESS in line for line in summary_lines):
        print("ARM64 KTEST summary did not report fail=0")
        print("\n".join(summary_lines))
        return 1

    print("arm64 kernel unit tests passed")
    return 0


def ktest_summary_lines(text: str) -> list[str]:
    return [
        line
        for line in text.splitlines()
        if "KTEST" in line and SUMMARY in line
    ]


if __name__ == "__main__":
    raise SystemExit(main())
