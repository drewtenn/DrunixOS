#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"
SUMMARY_RE = re.compile(r"BBCOMPAT SUMMARY passed ([0-9]+)/([0-9]+)")


def run(cmd: list[str], *, env: dict[str, str] | None = None) -> None:
    result = subprocess.run(
        cmd,
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"command failed: {' '.join(cmd)}\n"
            + (result.stderr.strip() or result.stdout.strip() or "no output")
        )


def build_busybox(arch: str) -> Path:
    target = (
        "build/busybox/x86/busybox"
        if arch == "x86"
        else "build/busybox/arm64/busybox"
    )
    run(["make", f"ARCH={arch}", target])
    return Path(target)


def wait_for_summary(log: Path, err: Path, timeout: float) -> str:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if log.exists():
            text = log.read_text(errors="ignore")
            if "BBCOMPAT SUMMARY" in text:
                return text
        time.sleep(1)

    tail = log.read_text(errors="ignore")[-4000:] if log.exists() else ""
    stderr = err.read_text(errors="ignore")[-2000:] if err.exists() else ""
    raise SystemExit(
        "timed out waiting for BBCOMPAT SUMMARY"
        + (f"\nserial tail:\n{tail}" if tail else "\nserial log missing")
        + (f"\nqemu stderr tail:\n{stderr}" if stderr else "")
    )


def verify_summary(text: str) -> None:
    match = SUMMARY_RE.search(text)
    if not match:
        fail_lines = "\n".join(
            line for line in text.splitlines() if "BBCOMPAT FAIL" in line
        )
        raise SystemExit(
            "BusyBox compatibility did not pass"
            + (f"\n{fail_lines}" if fail_lines else "\nmissing passed summary")
        )

    passed = int(match.group(1))
    total = int(match.group(2))
    if passed != total:
        fail_lines = "\n".join(
            line for line in text.splitlines() if "BBCOMPAT FAIL" in line
        )
        raise SystemExit(
            f"BusyBox compatibility incomplete: {passed}/{total}"
            + (f"\n{fail_lines}" if fail_lines else "")
        )


def run_x86(busybox: Path, timeout: float) -> None:
    for path in (ROOT / "disk.fs", ROOT / "img" / "disk.img"):
        path.unlink(missing_ok=True)

    run(
        [
            "make",
            "ARCH=x86",
            "X86_SERIAL_CONSOLE=1",
            "NO_DESKTOP=1",
            "INIT_PROGRAM=bin/bbcompat",
            "INIT_ARG0=bbcompat",
            "INCLUDE_BUSYBOX=1",
            "kernel",
            "disk",
        ]
    )

    log = LOG_DIR / "serial-busybox-x86.log"
    err = LOG_DIR / "qemu-busybox-x86.stderr"
    LOG_DIR.mkdir(exist_ok=True)
    log.unlink(missing_ok=True)
    err.unlink(missing_ok=True)

    with err.open("w") as stderr:
        proc = subprocess.Popen(
            [
                os.environ.get("QEMU", "qemu-system-i386"),
                "-display",
                "none",
                "-drive",
                "format=raw,file=img/disk.img,if=ide,index=0",
                "-drive",
                "format=raw,file=img/dufs.img,if=ide,index=1",
                "-cdrom",
                "os.iso",
                "-boot",
                "d",
                "-no-reboot",
                "-no-shutdown",
                "-serial",
                f"file:{log}",
                "-monitor",
                "none",
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
        try:
            text = wait_for_summary(log, err, timeout)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    verify_summary(text)


def refresh_arm64_outputs() -> None:
    for path in (
        ROOT / "build" / "arm64-root.fs",
        ROOT / "kernel" / "arch" / "arm64" / "rootfs_blob.o",
        ROOT / "kernel" / "arch" / "arm64" / "rootfs_blob.d",
        ROOT / "kernel-arm64.elf",
        ROOT / "kernel8.img",
        ROOT / "disk.fs",
        ROOT / "img" / "disk.img",
    ):
        path.unlink(missing_ok=True)


def run_arm64(busybox: Path, timeout: float, mode: str) -> None:
    refresh_arm64_outputs()
    run(
        [
            "make",
            "ARCH=arm64",
            "INIT_PROGRAM=bin/bbcompat",
            "INIT_ARG0=bbcompat",
            (
                f"INIT_ENV0=BBCOMPAT_MODE={mode}"
                if mode != "full"
                else "INIT_ENV0=PATH=/bin"
            ),
            "INCLUDE_BUSYBOX=1",
            "ARM_EXTRA_ROOTFS_FILES=build/arm64-rootfs-empty dufs/.keep",
            "build",
        ]
    )

    log = LOG_DIR / "serial-busybox-arm64.log"
    err = LOG_DIR / "qemu-busybox-arm64.stderr"
    LOG_DIR.mkdir(exist_ok=True)
    log.unlink(missing_ok=True)
    err.unlink(missing_ok=True)

    with err.open("w") as stderr:
        proc = subprocess.Popen(
            [
                os.environ.get("QEMU_ARM", "qemu-system-aarch64"),
                "-display",
                "none",
                "-M",
                os.environ.get("QEMU_ARM_MACHINE", "raspi3b"),
                "-kernel",
                "kernel-arm64.elf",
                "-drive",
                "if=sd,format=raw,file=img/disk.img",
                "-serial",
                "null",
                "-serial",
                f"file:{log}",
                "-monitor",
                "none",
                "-no-reboot",
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
        try:
            text = wait_for_summary(log, err, timeout)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    verify_summary(text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=("x86", "arm64"), required=True)
    parser.add_argument(
        "--mode",
        choices=("auto", "full", "arm64-smoke"),
        default="auto",
        help="BusyBox case set to require; auto uses full on x86 and arm64-smoke on arm64.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=float(os.environ.get("BUSYBOX_COMPAT_TIMEOUT", "120")),
    )
    args = parser.parse_args()
    mode = args.mode
    if mode == "auto":
        mode = "arm64-smoke" if args.arch == "arm64" else "full"

    busybox = build_busybox(args.arch)
    try:
        if args.arch == "x86":
            run_x86(busybox, args.timeout)
        else:
            run_arm64(busybox, args.timeout, mode)
    finally:
        if args.arch == "arm64":
            refresh_arm64_outputs()
            run(["make", "ARCH=arm64", "build"])

    print(f"{args.arch} BusyBox compatibility passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
