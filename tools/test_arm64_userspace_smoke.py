#!/usr/bin/env python3
from pathlib import Path
import subprocess
import time
import os

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT / "logs" / "serial-arm-userspace.log"
QEMU_LOG = ROOT / "logs" / "qemu-arm-userspace.stderr"
QEMU = os.environ.get("QEMU_ARM", "qemu-system-aarch64")
QEMU_MACHINE = os.environ.get("QEMU_ARM_MACHINE", "raspi3b")
BOOT_TIMEOUT = 20

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
QEMU_LOG.unlink(missing_ok=True)
qemu_stderr = QEMU_LOG.open("w")
saw_pass = False
timed_out = False
exit_code = None
try:
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
        stderr=qemu_stderr,
    )
except FileNotFoundError as exc:
    qemu_stderr.close()
    raise SystemExit(f"failed to start QEMU command {QEMU!r}: {exc}") from exc
try:
    deadline = time.time() + BOOT_TIMEOUT
    while time.time() < deadline:
        if LOG.exists():
            text = LOG.read_text(errors="ignore")
            if "ARM64 user smoke: pass" in text:
                saw_pass = True
                break
        exit_code = proc.poll()
        if exit_code is not None:
            break
        time.sleep(1)
    else:
        timed_out = True
finally:
    if proc.poll() is None:
        proc.kill()
    proc.wait()
    qemu_stderr.close()

stderr_text = QEMU_LOG.read_text(errors="ignore").strip()
log_exists = LOG.exists()

if saw_pass:
    print("arm64 userspace smoke check passed")
elif exit_code is not None:
    if not log_exists:
        raise SystemExit(
            f"QEMU exited before creating serial log: {LOG}\n"
            + (f"stderr:\n{stderr_text}" if stderr_text else "stderr: <empty>")
        )
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
            f"QEMU exited before the smoke markers appeared: {', '.join(missing)}\n"
            + (f"stderr:\n{stderr_text}" if stderr_text else "stderr: <empty>")
        )
elif timed_out and not log_exists:
    raise SystemExit(
        f"timed out waiting for QEMU to create serial log: {LOG}\n"
        + (f"stderr:\n{stderr_text}" if stderr_text else "stderr: <empty>")
    )
else:
    text = LOG.read_text(errors="ignore") if log_exists else ""
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
            + (f"\nstderr:\n{stderr_text}" if stderr_text else "\nstderr: <empty>")
        )

    print("arm64 userspace smoke check passed")
