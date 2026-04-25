#!/usr/bin/env python3
"""Boot the ARM64 kernel with KTEST=1 and require the in-kernel test summary."""

from __future__ import annotations

from pathlib import Path

import arm64_qemu_harness as harness


SUMMARY = "SUMMARY pass="
SUCCESS = "fail=0"
TIMEOUT = 30.0


def ktest_summary_lines(text: str) -> list[str]:
    return [
        line for line in text.splitlines() if "KTEST" in line and SUMMARY in line
    ]


def main() -> int:
    harness.build(["KTEST=1"])

    serial_log = harness.ROOT / "logs" / "serial-arm64-ktest.log"
    stderr_log = harness.ROOT / "logs" / "qemu-arm64-ktest.stderr"
    result = harness.boot_and_wait(
        serial_log,
        stderr_log,
        done=lambda text: bool(ktest_summary_lines(text)),
        timeout=TIMEOUT,
    )

    summary = ktest_summary_lines(result.text)
    if not summary:
        print("missing ARM64 KTEST summary")
        if result.stderr:
            print(result.stderr)
        if result.text:
            print(result.text[-2000:], end="")
        return 1

    if not any(SUCCESS in line for line in summary):
        print("ARM64 KTEST summary did not report fail=0")
        print("\n".join(summary))
        return 1

    print("arm64 kernel unit tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
