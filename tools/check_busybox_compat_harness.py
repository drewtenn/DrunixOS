#!/usr/bin/env python3
"""Verify the unattended BusyBox compatibility harness is wired in."""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "user" / "bbcompat.c"
MAKEFILE = ROOT / "Makefile"
USER_MAKEFILE = ROOT / "user" / "Makefile"
PROGRAMS_MK = ROOT / "user" / "programs.mk"
TEST_SCRIPT = ROOT / "tools" / "test_busybox_compat.py"
ARM_ARCH_MK = ROOT / "kernel" / "arch" / "arm64" / "arch.mk"


def fail(message: str) -> None:
    print(f"check_busybox_compat_harness: {message}", file=sys.stderr)
    sys.exit(1)


def require(pattern: str, text: str, message: str) -> None:
    if not re.search(pattern, text, flags=re.S):
        fail(message)


def main() -> int:
    if not RUNNER.exists():
        fail("missing user/bbcompat.c runner")

    runner = RUNNER.read_text()
    makefile = MAKEFILE.read_text()
    user_makefile = USER_MAKEFILE.read_text()
    programs_mk = PROGRAMS_MK.read_text()
    test_script = TEST_SCRIPT.read_text() if TEST_SCRIPT.exists() else ""
    arm_arch_mk = ARM_ARCH_MK.read_text()

    cases = re.findall(r"^\s*BB_CASE\(", runner, flags=re.M)
    if len(cases) < 50:
        fail(f"user/bbcompat.c must define at least 50 BB_CASE entries, got {len(cases)}")

    require(
        r"include\s+programs\.mk",
        user_makefile,
        "user/Makefile must include user/programs.mk",
    )
    require(r"\bbbcompat\b", programs_mk, "user/programs.mk must build bbcompat")
    require(r"include\s+user/programs\.mk", makefile, "top-level Makefile must include user/programs.mk")
    require(r"USER_PROGS\s*:=", makefile, "top-level Makefile must derive disk programs from PROGS")
    require(r"INCLUDE_BUSYBOX\s*\?=\s*0", makefile, "Makefile must expose opt-in BusyBox image payloads")
    require(r"BUSYBOX_DISK_FILES", makefile, "x86 disk image must support adding /bin/busybox")
    require(r"ARM_BUSYBOX_ROOTFS_FILES", arm_arch_mk, "arm64 rootfs image must support adding /bin/busybox")
    require(
        r"DISK_FILES\s*:=[^\n]*\$\(foreach\s+prog,\$\(USER_PROGS\)",
        makefile,
        "top-level Makefile must include user programs in the disk image",
    )
    require(r"INIT_PROGRAM\s*\?=", makefile, "Makefile must expose INIT_PROGRAM")
    require(r"\.init-program-flag", makefile, "kernel.o must rebuild when INIT_PROGRAM changes")
    require(r"test-busybox-compat\s*:", makefile, "Makefile must provide test-busybox-compat target")
    require(
        r"test_busybox_compat\.py\s+--arch\s+x86",
        makefile,
        "x86 test-busybox-compat must run the BusyBox runtime harness",
    )
    require(
        r"test_busybox_compat\.py\s+--arch\s+arm64",
        makefile,
        "arm64 test-busybox-compat must run the BusyBox runtime harness",
    )
    require(
        r"BBCOMPAT SUMMARY passed",
        test_script,
        "BusyBox runtime harness must require a passed BBCOMPAT summary",
    )
    require(
        r"INCLUDE_BUSYBOX=1",
        test_script,
        "BusyBox runtime tests must exercise the shared image payload switch",
    )
    require(
        r"BBCOMPAT_MODE",
        test_script,
        "arm64 BusyBox runtime harness must run the supported ARM smoke subset",
    )
    require(
        r"arm64-smoke",
        runner,
        "bbcompat runner must support the ARM BusyBox smoke subset",
    )
    require(
        r"sys_stat\(\"/bin/busybox\"",
        runner,
        "runner must preflight /bin/busybox before running cases",
    )
    require(
        r"BBCOMPAT ERROR missing /bin/busybox",
        runner,
        "runner must explain missing /bin/busybox instead of cascading failures",
    )
    require(r"sys_create\(\"/dufs/bbcompat\.log\"\)",
            runner,
            "runner must write bbcompat.log to the mounted DUFS side disk")
    total_match = re.search(r"#define\s+BBCOMPAT_TOTAL\s+([0-9]+)", runner)
    if not total_match:
        fail("runner must define BBCOMPAT_TOTAL")
    total = int(total_match.group(1))
    if total != len(cases):
        fail(f"BBCOMPAT_TOTAL must match BB_CASE count: {total} != {len(cases)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
