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
BOOT_TIMEOUT = 20


def stderr_message(stderr_text):
    return f"stderr:\n{stderr_text}" if stderr_text else "stderr: <empty>"


def fail(message, stderr_text):
    raise SystemExit(message + "\n" + stderr_message(stderr_text))


def read_serial_log():
    if not LOG.exists():
        return None
    try:
        return LOG.read_text(errors="ignore")
    except FileNotFoundError:
        return None


def run_case(make_args, required, forbidden=()):
    build = subprocess.run(
        ["make", "ARCH=arm64", "build", *make_args],
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
        except FileNotFoundError as exc:
            raise SystemExit(f"failed to start QEMU command {QEMU!r}: {exc}") from exc
        try:
            deadline = time.time() + BOOT_TIMEOUT
            while time.time() < deadline:
                text = read_serial_log()
                if text is not None:
                    if all(marker in text for marker in required):
                        break
                if proc.poll() is not None:
                    break
                time.sleep(1)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    stderr_text = ERR.read_text(errors="ignore").strip()
    text = read_serial_log()
    log_exists = text is not None
    if text is None:
        text = ""

    missing = [marker for marker in required if marker not in text]
    if missing:
        if not log_exists:
            fail(
                "QEMU exited before creating serial log: "
                + str(LOG)
                + f"\nQEMU return code: {proc.returncode}",
                stderr_text,
            )
        fail(
            "missing ARM64 filesystem init markers: " + ", ".join(missing),
            stderr_text,
        )

    forbidden_seen = [marker for marker in forbidden if marker in text]
    if forbidden_seen:
        fail(
            "forbidden ARM64 filesystem init markers present: "
            + ", ".join(forbidden_seen),
            stderr_text,
        )


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
