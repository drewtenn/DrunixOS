#!/usr/bin/env python3
"""Force the ARM64 smoke fallback and verify the smoke pass markers."""

from __future__ import annotations

import arm64_qemu_harness as harness


REQUIRED_MARKERS = (
    "ARM64 user smoke: entered",
    "ARM64 user smoke: syscall ok",
    "ARM64 user smoke: pass",
)


def main() -> int:
    harness.refresh_boot_image()
    harness.build(["INIT_PROGRAM=bin/missing-arm64", "ARM64_SMOKE_FALLBACK=1"])

    serial_log = harness.ROOT / "logs" / "serial-arm-userspace.log"
    stderr_log = harness.ROOT / "logs" / "qemu-arm-userspace.stderr"
    result = harness.boot_and_wait(
        serial_log,
        stderr_log,
        done=lambda text: "ARM64 user smoke: pass" in text,
        timeout=20.0,
    )

    if "ARM64 user smoke: pass" not in result.text:
        if not result.text:
            details = result.stderr or "<empty>"
            raise SystemExit(
                f"QEMU did not produce serial output: {serial_log}\nstderr:\n{details}"
            )
        missing = [m for m in REQUIRED_MARKERS if m not in result.text]
        if missing:
            details = (
                f"\nstderr:\n{result.stderr}" if result.stderr else "\nstderr: <empty>"
            )
            raise SystemExit(
                "ARM64 userspace smoke missing markers: "
                + ", ".join(missing)
                + details
            )

    missing = [m for m in REQUIRED_MARKERS if m not in result.text]
    if missing:
        raise SystemExit(
            "saw pass marker without the full smoke sequence: " + ", ".join(missing)
        )

    print("arm64 userspace smoke check passed")

    # Restore the default boot image for downstream targets.
    harness.refresh_boot_image()
    harness.build()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
