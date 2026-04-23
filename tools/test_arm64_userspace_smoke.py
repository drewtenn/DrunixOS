#!/usr/bin/env python3
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT / "logs" / "serial-arm-userspace.log"

subprocess.run(["make", "ARCH=arm64", "build"], cwd=ROOT, check=True)
LOG.unlink(missing_ok=True)
proc = subprocess.Popen(
    [
        "qemu-system-aarch64",
        "-display", "none",
        "-M", "raspi3b",
        "-kernel", "kernel-arm64.elf",
        "-serial", "null",
        "-serial", f"file:{LOG}",
        "-monitor", "none",
        "-no-reboot",
    ],
    cwd=ROOT,
)
try:
    for _ in range(20):
        if LOG.exists():
            text = LOG.read_text(errors="ignore")
            if "ARM64 user smoke: pass" in text:
                break
        time.sleep(1)
finally:
    proc.kill()
    proc.wait()

text = LOG.read_text(errors="ignore")
assert "ARM64 user smoke: entered" in text
assert "ARM64 user smoke: syscall ok" in text
assert "ARM64 user smoke: pass" in text
print("arm64 userspace smoke check passed")
