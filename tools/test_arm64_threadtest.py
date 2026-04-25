#!/usr/bin/env python3
"""Boot the ARM64 kernel into the shared threadtest user program."""

from __future__ import annotations

import arm64_qemu_harness as harness


REQUIRED = ("THREADTEST PASS",)
FORBIDDEN = (
    "THREADTEST FAIL",
    "unknown syscall",
    "Unhandled syscall",
)


def main() -> int:
    harness.refresh_boot_image()
    harness.build(["INIT_PROGRAM=bin/threadtest", "INIT_ARG0=threadtest"])

    serial_log = harness.ROOT / "logs" / "serial-arm64-threadtest.log"
    stderr_log = harness.ROOT / "logs" / "qemu-arm64-threadtest.stderr"
    result = harness.boot_and_wait(
        serial_log,
        stderr_log,
        done=harness.all_markers_seen(REQUIRED),
        timeout=30.0,
    )
    harness.assert_markers(result, REQUIRED, FORBIDDEN)
    print("arm64 threadtest passed")

    harness.refresh_boot_image()
    harness.build()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
