#!/usr/bin/env python3
"""Check the arm64 W1 ext3-root integration points."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def make_dry_run(target: str, *args: str) -> str:
    result = subprocess.run(
        ["make", "-B", "-n", "ARCH=arm64", target, *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise SystemExit(result.returncode)
    return output


def main() -> int:
    failures: list[str] = []

    run_output = make_dry_run("run")
    build_output = make_dry_run("build")
    disk_output = make_dry_run("disk")
    debug_user_output = make_dry_run("debug-user", "APP=shell")

    if "ROOT_FS=dufs" in run_output + build_output + disk_output:
        failures.append("arm64 dry-runs still force ROOT_FS=dufs")
    if "tools/mkext3.py" not in disk_output:
        failures.append("make ARCH=arm64 disk does not build an ext3 root image")
    if "tools/mkfs.py" in disk_output:
        failures.append("make ARCH=arm64 disk still builds a DUFS root image")
    if "if=sd" not in run_output or "img/disk.img" not in run_output:
        failures.append("make ARCH=arm64 run does not attach img/disk.img as SD media")
    if "kernel/platform/raspi3b/emmc.o" not in build_output:
        failures.append("arm64 kernel build does not include the raspi3b EMMC driver")
    if "rootfs_blob" in build_output:
        failures.append("default arm64 ext3 build still embeds the DUFS rootfs blob")
    if "build/arm64-root.fs" in debug_user_output:
        failures.append("debug-user still depends on the embedded DUFS rootfs image")

    start_kernel = (ROOT / "kernel/arch/arm64/start_kernel.c").read_text()
    if "platform_block_register()" not in start_kernel:
        failures.append("arm64 boot does not call platform_block_register()")
    if 'vfs_mount_with_source("/", DRUNIX_ROOT_FS, "/dev/sda1")' not in start_kernel:
        failures.append("arm64 boot does not mount DRUNIX_ROOT_FS from /dev/sda1")
    if "ext3_register()" not in start_kernel:
        failures.append("arm64 boot does not register ext3")

    emmc = ROOT / "kernel/platform/raspi3b/emmc.c"
    if not emmc.exists():
        failures.append("kernel/platform/raspi3b/emmc.c is missing")

    if failures:
        print("arm64 ext3-root parity check failed:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("arm64 ext3-root parity check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
