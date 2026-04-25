#!/usr/bin/env python3
"""Boot the ARM64 kernel with arm64init as PID 1 and verify syscall parity."""

from __future__ import annotations

import arm64_qemu_harness as harness


REQUIRED = (
    "ARM64 init: entered",
    "ARM64 syscall: identity ok",
    "ARM64 syscall: getcwd ok",
    "ARM64 syscall: open/read/close ok",
    "ARM64 syscall: metadata ok",
    "ARM64 syscall: dirents ok",
    "ARM64 syscall: time/info ok",
    "ARM64 syscall: memory ok",
    "ARM64 syscall: mutation ok",
    "ARM64 syscall: fd/path ok",
    "ARM64 syscall: signal ok",
    "ARM64 syscall: process ok",
    "ARM64 syscall: clone/wait ok",
    "ARM64 syscall: utility ok",
    "ARM64 syscall: errno ok",
    "ARM64 init: pass",
    "ARM64 init exited with status 0",
    "drunix> ",
)

FORBIDDEN = (
    "unknown syscall",
    "Unhandled syscall",
    "ARM64 syscall: fail",
)


def main() -> int:
    return harness.run_arm64_test(
        serial_log_name="serial-arm64-syscall-parity.log",
        required=REQUIRED,
        forbidden=FORBIDDEN,
        success_message="arm64 syscall parity check passed",
        build_args=["INIT_PROGRAM=bin/arm64init", "INIT_ARG0=arm64init"],
        timeout=25.0,
    )


if __name__ == "__main__":
    raise SystemExit(main())
