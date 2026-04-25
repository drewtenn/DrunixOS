#!/usr/bin/env python3
"""Boot the ARM64 kernel and require the MMU-enabled banner."""

from __future__ import annotations

import arm64_qemu_harness as harness


REQUIRED = (
    "ARM64 MMU enabled",
    "Drunix ARM64 console",
)


def main() -> int:
    return harness.run_arm64_test(
        serial_log_name="serial-arm64-mmu.log",
        required=REQUIRED,
        success_message="arm64 mmu enabled check passed",
        refresh=True,
    )


if __name__ == "__main__":
    raise SystemExit(main())
