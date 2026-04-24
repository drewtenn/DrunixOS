#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
QEMU = os.environ.get("QEMU_ARM", "qemu-system-aarch64")
MACHINE = os.environ.get("QEMU_ARM_MACHINE", "raspi3b")
LOG = ROOT / "logs" / "serial-arm64-fs-init.log"
ERR = ROOT / "logs" / "qemu-arm64-fs-init.stderr"


def run_case(make_args, required, forbidden=()):
    subprocess.run(["make", "ARCH=arm64", "build", *make_args], cwd=ROOT, check=True)
    LOG.parent.mkdir(exist_ok=True)
    LOG.unlink(missing_ok=True)
    ERR.unlink(missing_ok=True)
    with ERR.open("w") as stderr:
        proc = subprocess.Popen(
            [
                QEMU,
                "-display", "none",
                "-M", MACHINE,
                "-kernel", "kernel-arm64.elf",
                "-serial", "null",
                "-serial", f"file:{LOG}",
                "-monitor", "none",
                "-no-reboot",
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
        try:
            deadline = time.time() + 20
            while time.time() < deadline:
                if LOG.exists():
                    text = LOG.read_text(errors="ignore")
                    if all(marker in text for marker in required):
                        break
                time.sleep(1)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    text = LOG.read_text(errors="ignore")
    for marker in required:
        assert marker in text, marker
    for marker in forbidden:
        assert marker not in text, marker


run_case([], [
    "ARM64 init: entered",
    "ARM64 init: pass",
    "ARM64 init exited with status 0",
    "drunix> ",
], [
    "ARM64 user smoke: entered",
])

run_case(["INIT_PROGRAM=bin/missing-arm64"], [
    "ARM64 init launch failed: bin/missing-arm64",
], [
    "ARM64 user smoke: entered",
])

run_case(["INIT_PROGRAM=bin/missing-arm64", "ARM64_SMOKE_FALLBACK=1"], [
    "ARM64 init launch failed: bin/missing-arm64",
    "ARM64 user smoke: entered",
    "ARM64 user smoke: pass",
])

print("arm64 filesystem init check passed")
