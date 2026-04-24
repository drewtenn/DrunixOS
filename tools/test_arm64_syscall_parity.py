#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
QEMU = os.environ.get("QEMU_ARM", "qemu-system-aarch64")
MACHINE = os.environ.get("QEMU_ARM_MACHINE", "raspi3b")
LOG = ROOT / "logs" / "serial-arm64-syscall-parity.log"
ERR = ROOT / "logs" / "qemu-arm64-syscall-parity.stderr"

REQUIRED = [
    "ARM64 init: entered",
    "ARM64 syscall: identity ok",
    "ARM64 syscall: getcwd ok",
    "ARM64 syscall: open/read/close ok",
    "ARM64 syscall: metadata ok",
    "ARM64 syscall: dirents ok",
    "ARM64 syscall: time/info ok",
    "ARM64 syscall: memory ok",
    "ARM64 syscall: mutation ok",
    "ARM64 syscall: fd/path ok",
    "ARM64 syscall: signal ok",
    "ARM64 syscall: process ok",
    "ARM64 syscall: clone/wait ok",
    "ARM64 syscall: utility ok",
    "ARM64 syscall: errno ok",
    "ARM64 init: pass",
    "ARM64 init exited with status 0",
    "drunix> ",
]

FORBIDDEN = [
    "unknown syscall",
    "Unhandled syscall",
    "ARM64 syscall: fail",
]


subprocess.run(["make", "ARCH=arm64", "build"], cwd=ROOT, check=True)
LOG.parent.mkdir(exist_ok=True)
LOG.unlink(missing_ok=True)
ERR.unlink(missing_ok=True)
with ERR.open("w") as stderr:
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
    try:
        deadline = time.time() + 25
        while time.time() < deadline:
            if LOG.exists():
                text = LOG.read_text(errors="ignore")
                if all(marker in text for marker in REQUIRED):
                    break
            time.sleep(1)
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()

text = LOG.read_text(errors="ignore")
for marker in REQUIRED:
    assert marker in text, marker
for marker in FORBIDDEN:
    assert marker not in text, marker
print("arm64 syscall parity check passed")
