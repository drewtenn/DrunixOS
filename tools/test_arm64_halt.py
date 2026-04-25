#!/usr/bin/env python3
"""Boot the ARM64 kernel's halt-inducing exception smoke test."""

from __future__ import annotations

import arm64_qemu_harness as harness


REQUIRED = (
    "ARM64 halt test: triggering sync exception",
    "sync exception",
)


def main() -> int:
    harness.refresh_boot_image()
    harness.build(["ARM64_HALT_TEST=1"])

    serial_log = harness.ROOT / "logs" / "serial-arm64-halt.log"
    stderr_log = harness.ROOT / "logs" / "qemu-arm64-halt.stderr"
    result = harness.boot_and_wait(
        serial_log,
        stderr_log,
        done=harness.all_markers_seen(REQUIRED),
        timeout=10.0,
    )
    harness.assert_markers(result, REQUIRED)
    print("arm64 halt test passed")

    harness.refresh_boot_image()
    harness.build()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
