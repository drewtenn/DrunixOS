#!/usr/bin/env python3
"""Boot ARM64 virt under QEMU WITHOUT `-device virtio-gpu-device` and
require ramfb to publish /dev/fb0 (the M3.1 fallback path).

The default arm64_qemu_harness device list includes both `ramfb` and
`virtio-gpu-device`. When both are present, virtio-gpu wins (M3.1
Commit 2 reordered init so virtio-gpu runs first; ramfb's
chardev_get("fb0") skip check fires after virtio-gpu has registered).
This script drops `virtio-gpu-device` from the device list so the
init pathway falls back to ramfb, and asserts the boot log shows
ramfb owning /dev/fb0 with no virtio-gpu protocol activity.
"""

from __future__ import annotations

import argparse

import arm64_qemu_harness as harness


SUMMARY = "SUMMARY pass="
SUCCESS = "fail=0"
TIMEOUT = 30.0

# Device list for the fallback path: same as the default minus
# virtio-gpu-device. ramfb stays so /dev/fb0 has a provider.
RAMFB_DEVICES = [
    "virtio-blk-device,drive=hd0",
    "ramfb",
    "virtio-keyboard-device",
    "virtio-mouse-device",
]


def ktest_summary_lines(text: str) -> list[str]:
    return [
        line for line in text.splitlines() if "KTEST" in line and SUMMARY in line
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()

    build_args = [
        "KTEST=1",
        "ROOT_FS=dufs",
        "PLATFORM=virt",
        "DRUNIX_ARM64_VIRT_HW_CURSOR=1",
    ]
    harness.build(build_args)

    serial_log = (
        harness.ROOT / "logs" / "serial-arm64-ktest-virt-ramfb-fallback.log"
    )
    stderr_log = (
        harness.ROOT / "logs" / "qemu-arm64-ktest-virt-ramfb-fallback.stderr"
    )
    result = harness.boot_and_wait(
        serial_log,
        stderr_log,
        done=lambda text: bool(ktest_summary_lines(text)),
        timeout=TIMEOUT,
        platform="virt",
        devices=RAMFB_DEVICES,
    )

    summary = ktest_summary_lines(result.text)
    if not summary:
        print("missing ARM64 KTEST summary (virt ramfb-fallback)")
        if result.stderr:
            print(result.stderr)
        if result.text:
            print(result.text[-2000:], end="")
        return 1

    if not any(SUCCESS in line for line in summary):
        print(
            "ARM64 KTEST summary did not report fail=0 (virt ramfb-fallback)"
        )
        print("\n".join(summary))
        return 1

    # Functional assertions for the fallback path:
    # 1. ramfb must publish /dev/fb0 (its M2.5a log line).
    # 2. virtio-gpu must NOT log "M3.0 sequence complete" (the
    #    six-command path) because the device was not advertised, so
    #    discovery returned -1 and the driver skipped.
    if "ramfb: 1024x768x32" not in result.text:
        print(
            "ramfb did not publish /dev/fb0 on the fallback path; aborting"
        )
        print(result.text[-2000:], end="")
        return 1

    if "virtio-gpu: M3.0 sequence complete" in result.text:
        print(
            "virtio-gpu reached the M3.0 sequence on the fallback path; "
            "the device list refactor regressed"
        )
        print(result.text[-2000:], end="")
        return 1

    print("arm64 kernel unit tests passed (virt ramfb fallback)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
