#!/usr/bin/env python3
from pathlib import Path
import subprocess
import time
import os

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT / "logs" / "serial-arm-userspace.log"
QEMU = os.environ.get("QEMU_ARM", "qemu-system-aarch64")
QEMU_MACHINE = os.environ.get("QEMU_ARM_MACHINE", "raspi3b")
BOOT_TIMEOUT = 20

subprocess.run(["make", "ARCH=arm64", "build"], cwd=ROOT, check=True)
LOG.parent.mkdir(exist_ok=True)
LOG.unlink(missing_ok=True)
proc = subprocess.Popen(
    [
        QEMU,
        "-display", "none",
        "-M", QEMU_MACHINE,
        "-kernel", "kernel-arm64.elf",
        "-serial", "null",
        "-serial", f"file:{LOG}",
        "-monitor", "none",
        "-no-reboot",
    ],
    cwd=ROOT,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
try:
    deadline = time.time() + BOOT_TIMEOUT
    while time.time() < deadline:
        if LOG.exists():
            text = LOG.read_text(errors="ignore")
            if "ARM64 user smoke: pass" in text:
                break
        time.sleep(1)
finally:
    proc.kill()
    proc.wait()

if not LOG.exists():
    raise SystemExit(f"timed out waiting for serial log: {LOG}")

text = LOG.read_text(errors="ignore")
missing = [
    marker
    for marker in (
        "ARM64 user smoke: entered",
        "ARM64 user smoke: syscall ok",
        "ARM64 user smoke: pass",
    )
    if marker not in text
]
if missing:
    raise SystemExit(
        "timed out waiting for ARM64 userspace smoke markers: "
        + ", ".join(missing)
    )

print("arm64 userspace smoke check passed")
