#!/usr/bin/env python3
"""Boot the ARM64 kernel under several INIT_PROGRAM/SMOKE_FALLBACK matrices."""

from __future__ import annotations

from typing import Sequence

import arm64_qemu_harness as harness


SERIAL_LOG = harness.ROOT / "logs" / "serial-arm64-fs-init.log"
STDERR_LOG = harness.ROOT / "logs" / "qemu-arm64-fs-init.stderr"
EXT3_ROOT_MARKERS = (
    "ARM64 EMMC disk registered",
    "ARM64 root mounted: ext3",
)


def run_case(make_args: Sequence[str],
             required: Sequence[str],
             forbidden: Sequence[str] = ()) -> None:
    harness.refresh_boot_image()
    harness.build(make_args)
    result = harness.boot_and_wait(
        SERIAL_LOG,
        STDERR_LOG,
        done=harness.all_markers_seen((*EXT3_ROOT_MARKERS, *required)),
        timeout=20.0,
    )
    harness.assert_markers(result, (*EXT3_ROOT_MARKERS, *required), forbidden)


run_case(
    [],
    [
        "Drunix ARM64 console",
        "drunix> ",
        "drunix shell -- type 'help' for commands",
        "drunix:",
    ],
    [
        "ARM64 init launch failed",
        "ARM64 init: entered",
        "ARM64 user smoke: entered",
    ],
)

run_case(
    ["INIT_PROGRAM=bin/arm64init", "INIT_ARG0=arm64init"],
    [
        "ARM64 init: entered",
        "ARM64 init: pass",
        "ARM64 init exited with status 0",
        "drunix> ",
    ],
    [
        "ARM64 user smoke: entered",
    ],
)

run_case(
    ["INIT_PROGRAM=bin/missing-arm64"],
    [
        "ARM64 init launch failed: bin/missing-arm64",
    ],
    [
        "ARM64 user smoke: entered",
    ],
)

run_case(
    ["INIT_PROGRAM=bin/missing-arm64", "ARM64_SMOKE_FALLBACK=1"],
    [
        "ARM64 init launch failed: bin/missing-arm64",
        "ARM64 user smoke: entered",
        "ARM64 user smoke: pass",
    ],
)

print("arm64 filesystem init check passed")
