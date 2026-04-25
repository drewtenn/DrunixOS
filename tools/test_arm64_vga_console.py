#!/usr/bin/env python3
"""
Boot the ARM64 kernel in QEMU and require the VGA-style framebuffer console.
"""

from __future__ import annotations

from pathlib import Path
import os
import socket
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"
SERIAL_LOG = LOG_DIR / "serial-arm64-vga.log"
MONITOR_SOCKET = LOG_DIR / "qemu-arm64-vga.monitor"
SCREENSHOT = LOG_DIR / "arm64-vga.ppm"
QEMU_LOG = LOG_DIR / "qemu-arm64-vga.stderr"
QEMU = os.environ.get("QEMU_ARM", "qemu-system-aarch64")
QEMU_MACHINE = os.environ.get("QEMU_ARM_MACHINE", "raspi3b")
BOOT_TIMEOUT = 20.0
REQUIRED_MARKERS = (
    "ARM64 framebuffer console enabled",
    "Drunix ARM64 console",
    "drunix> ",
)


def refresh_arm64_boot_image() -> None:
    for path in (
        ROOT / "kernel/arch/arm64/start_kernel.o",
        ROOT / "kernel-arm64.elf",
        ROOT / "kernel8.img",
    ):
        path.unlink(missing_ok=True)


def build_arm64_vga() -> None:
    refresh_arm64_boot_image()
    build = subprocess.run(
        ["make", "ARCH=arm64", "build"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if build.returncode != 0:
        raise SystemExit(
            "arm64 vga build failed:\n"
            + (build.stderr.strip() or build.stdout.strip() or "no build output")
        )


def read_serial_log() -> str:
    try:
        return SERIAL_LOG.read_text(errors="ignore")
    except FileNotFoundError:
        return ""


def wait_for_markers(proc: subprocess.Popen[bytes]) -> None:
    deadline = time.time() + BOOT_TIMEOUT
    while time.time() < deadline:
        log = read_serial_log()
        if all(marker in log for marker in REQUIRED_MARKERS):
            return
        exit_code = proc.poll()
        if exit_code is not None:
            missing = [marker for marker in REQUIRED_MARKERS if marker not in log]
            raise SystemExit(
                "QEMU exited before ARM64 VGA console markers appeared: "
                + ", ".join(missing)
            )
        time.sleep(0.2)

    missing = [marker for marker in REQUIRED_MARKERS if marker not in read_serial_log()]
    raise SystemExit(
        "timed out waiting for ARM64 VGA console markers: " + ", ".join(missing)
    )


def read_ppm_pixels(path: Path) -> bytes:
    data = path.read_bytes()
    tokens: list[bytes] = []
    i = 0
    length = len(data)
    while len(tokens) < 4 and i < length:
        while i < length and data[i] in b" \t\r\n":
            i += 1
        if i < length and data[i] == ord("#"):
            while i < length and data[i] not in b"\r\n":
                i += 1
            continue
        start = i
        while i < length and data[i] not in b" \t\r\n":
            i += 1
        if start != i:
            tokens.append(data[start:i])

    if len(tokens) < 4 or tokens[0] != b"P6":
        raise SystemExit(f"screendump is not a binary PPM: {path}")
    if i < length and data[i] in b" \t\r\n":
        i += 1
    return data[i:]


def verify_screendump() -> None:
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if SCREENSHOT.exists():
            break
        time.sleep(0.1)
    if not SCREENSHOT.exists():
        raise SystemExit(f"screendump did not create {SCREENSHOT}")
    pixels = read_ppm_pixels(SCREENSHOT)
    if not pixels:
        raise SystemExit(f"screendump has no pixel data: {SCREENSHOT}")
    if not any(byte != 0 for byte in pixels):
        raise SystemExit(f"screendump contains only black pixels: {SCREENSHOT}")


def monitor_command(command: str) -> None:
    deadline = time.time() + 5.0
    last_error: OSError | None = None
    while time.time() < deadline:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
                monitor.settimeout(2.0)
                monitor.connect(str(MONITOR_SOCKET))
                try:
                    monitor.recv(4096)
                except TimeoutError:
                    pass
                monitor.sendall(command.encode("ascii") + b"\n")
                try:
                    monitor.recv(4096)
                except TimeoutError:
                    pass
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise SystemExit(f"failed to send QEMU monitor command {command!r}: {last_error}")


def stop_qemu(proc: subprocess.Popen[bytes]) -> None:
    if proc.poll() is None:
        try:
            monitor_command("quit")
        except SystemExit:
            pass
    if proc.poll() is None:
        proc.kill()
    try:
        proc.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2.0)


def main() -> int:
    build_arm64_vga()
    LOG_DIR.mkdir(exist_ok=True)
    SERIAL_LOG.unlink(missing_ok=True)
    MONITOR_SOCKET.unlink(missing_ok=True)
    SCREENSHOT.unlink(missing_ok=True)
    QEMU_LOG.unlink(missing_ok=True)

    qemu_stderr = QEMU_LOG.open("w")
    proc: subprocess.Popen[bytes] | None = None
    try:
        proc = subprocess.Popen(
            [
                QEMU,
                "-M",
                QEMU_MACHINE,
                "-kernel",
                "kernel-arm64.elf",
                "-serial",
                "null",
                "-serial",
                f"file:{SERIAL_LOG}",
                "-monitor",
                f"unix:{MONITOR_SOCKET},server,nowait",
                "-no-reboot",
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=qemu_stderr,
        )
        wait_for_markers(proc)
        monitor_command(f"screendump {SCREENSHOT}")
        verify_screendump()
        print("arm64 vga console check passed")
        return 0
    except FileNotFoundError as exc:
        raise SystemExit(f"failed to start QEMU command {QEMU!r}: {exc}") from exc
    finally:
        if proc is not None:
            stop_qemu(proc)
        qemu_stderr.close()


if __name__ == "__main__":
    raise SystemExit(main())
