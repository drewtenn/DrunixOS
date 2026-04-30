#!/usr/bin/env python3
"""Boot the ARM64 kernel with KTEST=1 and require the in-kernel test summary."""

from __future__ import annotations

import argparse

import arm64_qemu_harness as harness


SUMMARY = "SUMMARY pass="
SUCCESS = "fail=0"
TIMEOUT = 30.0


def ktest_summary_lines(text: str) -> list[str]:
    return [
        line for line in text.splitlines() if "KTEST" in line and SUMMARY in line
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--platform",
        choices=("raspi3b", "virt"),
        default="raspi3b",
        help="QEMU machine to boot (default: raspi3b)",
    )
    args = parser.parse_args()

    # ROOT_FS=dufs embeds the rootfs blob and registers sda+sdb from it.
    # Both platforms use this in KTEST so the fs test suite (which expects
    # sdb1 when the live root is on sda1) and the dev-namespace tests
    # (which expect sdb) have the disks they need.
    build_args = ["KTEST=1", "ROOT_FS=dufs", f"PLATFORM={args.platform}"]
    harness.build(build_args)

    serial_log = harness.ROOT / "logs" / f"serial-arm64-ktest-{args.platform}.log"
    stderr_log = harness.ROOT / "logs" / f"qemu-arm64-ktest-{args.platform}.stderr"
    result = harness.boot_and_wait(
        serial_log,
        stderr_log,
        done=lambda text: bool(ktest_summary_lines(text)),
        timeout=TIMEOUT,
        platform=args.platform,
    )

    summary = ktest_summary_lines(result.text)
    if not summary:
        print(f"missing ARM64 KTEST summary ({args.platform})")
        if result.stderr:
            print(result.stderr)
        if result.text:
            print(result.text[-2000:], end="")
        return 1

    if not any(SUCCESS in line for line in summary):
        print(f"ARM64 KTEST summary did not report fail=0 ({args.platform})")
        print("\n".join(summary))
        return 1

    print(f"arm64 kernel unit tests passed ({args.platform})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
