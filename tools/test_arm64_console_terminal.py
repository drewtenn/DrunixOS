#!/usr/bin/env python3
"""Boot the ARM64 kernel and require the console terminal banner/prompt."""

from __future__ import annotations

import arm64_qemu_harness as harness


REQUIRED = (
    "Drunix ARM64 console",
    "drunix> ",
)


def main() -> int:
    return harness.run_arm64_test(
        serial_log_name="serial-arm.log",
        required=REQUIRED,
        success_message="arm64 console terminal check passed",
        timeout=10.0,
    )


if __name__ == "__main__":
    raise SystemExit(main())
