#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
QEMU = os.environ.get("QEMU_ARM", "qemu-system-aarch64")
MACHINE = os.environ.get("QEMU_ARM_MACHINE", "raspi3b")
LOG = ROOT / "logs" / "serial-arm64-mmu.log"
ERR = ROOT / "logs" / "qemu-arm64-mmu.stderr"
BOOT_TIMEOUT = 20
REQUIRED = (
    "ARM64 MMU enabled",
    "Drunix ARM64 console",
)


def refresh_arm64_boot_image() -> None:
    for path in (
        ROOT / "kernel" / "arch" / "arm64" / "start_kernel.o",
        ROOT / "kernel-arm64.elf",
        ROOT / "kernel8.img",
    ):
        path.unlink(missing_ok=True)


def read_log() -> str:
    if not LOG.exists():
        return ""
    return LOG.read_text(errors="ignore")


refresh_arm64_boot_image()
build = subprocess.run(
    ["make", "ARCH=arm64", "build"],
    cwd=ROOT,
    capture_output=True,
    text=True,
)
if build.returncode != 0:
    raise SystemExit(
        "arm64 build failed:\n"
        + (build.stderr.strip() or build.stdout.strip() or "no build output")
    )

LOG.parent.mkdir(exist_ok=True)
LOG.unlink(missing_ok=True)
ERR.unlink(missing_ok=True)

with ERR.open("w") as stderr:
    try:
        proc = subprocess.Popen(
            [
                QEMU,
                "-display",
                "none",
                "-M",
                MACHINE,
                "-kernel",
                "kernel-arm64.elf",
                "-serial",
                "null",
                "-serial",
                f"file:{LOG}",
                "-monitor",
                "none",
                "-no-reboot",
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
    except FileNotFoundError as exc:
        raise SystemExit(f"failed to start QEMU command {QEMU!r}: {exc}") from exc

    try:
        deadline = time.time() + BOOT_TIMEOUT
        while time.time() < deadline:
            text = read_log()
            if all(marker in text for marker in REQUIRED):
                break
            if proc.poll() is not None:
                break
            time.sleep(1)
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()

text = read_log()
stderr_text = ERR.read_text(errors="ignore").strip()
missing = [marker for marker in REQUIRED if marker not in text]
if missing:
    raise SystemExit(
        "missing ARM64 MMU markers: "
        + ", ".join(missing)
        + (f"\nstderr:\n{stderr_text}" if stderr_text else "\nstderr: <empty>")
    )

print("arm64 mmu enabled check passed")
