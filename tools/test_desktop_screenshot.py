#!/usr/bin/env python3
"""
Boot the x86 desktop in QEMU, take a monitor screendump, and require the
captured framebuffer to look like the user-space desktop reached first paint.
"""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import socket
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"
IMG_DIR = ROOT / "img"
SERIAL_LOG = LOG_DIR / "serial-desktop-screenshot.log"
DEBUGCON_LOG = LOG_DIR / "debugcon-desktop-screenshot.log"
QEMU_LOG = LOG_DIR / "qemu-desktop-screenshot.stderr"
MONITOR_SOCKET = LOG_DIR / "qemu-desktop-screenshot.monitor"
SCREENSHOT = LOG_DIR / "desktop-boot.ppm"
TEST_DISK = IMG_DIR / "disk-desktop-screenshot.img"
TEST_DUFS = IMG_DIR / "dufs-desktop-screenshot.img"
BOOT_TIMEOUT = 30.0

COLOR_TITLEBAR = (0x37, 0x45, 0x61)
COLOR_TERM_BG = (0x14, 0x14, 0x14)
COLOR_TASKBAR = (0x09, 0x12, 0x1F)
COLOR_TASKBAR_EDGE = (0x1D, 0x31, 0x47)


def qemu_binary() -> str:
    return os.environ.get("QEMU", "qemu-system-i386")


def build_desktop_image() -> None:
    result = subprocess.run(
        ["make", "ARCH=x86", "kernel", "disk"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(
            "x86 desktop build failed:\n"
            + (result.stderr.strip() or result.stdout.strip() or "no build output")
        )


def prepare_images() -> None:
    LOG_DIR.mkdir(exist_ok=True)
    IMG_DIR.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "img" / "disk.img", TEST_DISK)
    shutil.copy2(ROOT / "img" / "dufs.img", TEST_DUFS)


def qemu_command() -> list[str]:
    return [
        qemu_binary(),
        "-display",
        os.environ.get("DRUNIX_DESKTOP_SCREENSHOT_DISPLAY", "none"),
        "-drive",
        f"format=raw,file={TEST_DISK},if=ide,index=0",
        "-drive",
        f"format=raw,file={TEST_DUFS},if=ide,index=1",
        "-cdrom",
        "os.iso",
        "-boot",
        "d",
        "-no-reboot",
        "-no-shutdown",
        "-global",
        "isa-debugcon.iobase=0xe9",
        "-serial",
        f"file:{SERIAL_LOG}",
        "-debugcon",
        f"file:{DEBUGCON_LOG}",
        "-monitor",
        f"unix:{MONITOR_SOCKET},server,nowait",
    ]


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


def read_ppm(path: Path) -> tuple[int, int, bytes]:
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
        raise ValueError(f"screendump is not a binary PPM: {path}")
    if i < length and data[i] in b" \t\r\n":
        i += 1
    width = int(tokens[1])
    height = int(tokens[2])
    maxval = int(tokens[3])
    if maxval != 255:
        raise ValueError(f"screendump uses unsupported maxval {maxval}")
    pixels = data[i:]
    expected = width * height * 3
    if len(pixels) < expected:
        raise ValueError(
            f"screendump is truncated: expected {expected} bytes, got {len(pixels)}"
        )
    return width, height, pixels[:expected]


def close_rgb(a: tuple[int, int, int], b: tuple[int, int, int], tolerance: int = 4) -> bool:
    return all(abs(a[i] - b[i]) <= tolerance for i in range(3))


def pixel_at(width: int, pixels: bytes, x: int, y: int) -> tuple[int, int, int]:
    offset = (y * width + x) * 3
    return pixels[offset], pixels[offset + 1], pixels[offset + 2]


def region_ratio(
    width: int,
    height: int,
    pixels: bytes,
    x: int,
    y: int,
    w: int,
    h: int,
    color: tuple[int, int, int],
) -> float:
    x0 = max(0, x)
    y0 = max(0, y)
    x1 = min(width, x + w)
    y1 = min(height, y + h)
    total = max(0, x1 - x0) * max(0, y1 - y0)
    if total == 0:
        return 0.0
    matches = 0
    for py in range(y0, y1):
        for px in range(x0, x1):
            if close_rgb(pixel_at(width, pixels, px, py), color):
                matches += 1
    return matches / total


def unique_sampled_colors(width: int, height: int, pixels: bytes) -> int:
    colors: set[tuple[int, int, int]] = set()
    step_x = max(1, width // 64)
    step_y = max(1, height // 48)
    for y in range(0, height, step_y):
        for x in range(0, width, step_x):
            colors.add(pixel_at(width, pixels, x, y))
    return len(colors)


def desktop_screenshot_error(path: Path) -> str | None:
    width, height, pixels = read_ppm(path)
    if width < 720 or height < 520:
        return f"screendump is too small for the desktop layout: {width}x{height}"
    if not any(byte != 0 for byte in pixels):
        return "screendump contains only black pixels"
    if unique_sampled_colors(width, height, pixels) < 8:
        return "screendump does not contain enough distinct desktop colors"

    title_ratio = region_ratio(width, height, pixels, 90, 66, 420, 14, COLOR_TITLEBAR)
    if title_ratio < 0.72:
        return f"terminal titlebar is not visible enough: ratio={title_ratio:.2f}"

    terminal_bg_ratio = region_ratio(
        width, height, pixels, 80, 100, 560, 280, COLOR_TERM_BG
    )
    if terminal_bg_ratio < 0.80:
        return f"terminal body is not visible enough: ratio={terminal_bg_ratio:.2f}"

    taskbar_y = height - 48
    taskbar_ratio = region_ratio(
        width, height, pixels, 180, taskbar_y + 4, width - 360, 36, COLOR_TASKBAR
    )
    if taskbar_ratio < 0.80:
        return f"desktop taskbar is not visible enough: ratio={taskbar_ratio:.2f}"

    edge_ratio = region_ratio(width, height, pixels, 0, taskbar_y, width, 1, COLOR_TASKBAR_EDGE)
    if edge_ratio < 0.90:
        return f"desktop taskbar edge is not visible enough: ratio={edge_ratio:.2f}"

    return None


def wait_for_desktop_screenshot(proc: subprocess.Popen[bytes]) -> None:
    deadline = time.time() + BOOT_TIMEOUT
    last_error = "desktop screendump was not attempted"
    attempt = 0
    while time.time() < deadline:
        if proc.poll() is not None:
            break
        attempt += 1
        attempt_path = SCREENSHOT.with_name(f"{SCREENSHOT.stem}.attempt{attempt}.ppm")
        attempt_path.unlink(missing_ok=True)
        monitor_command(f"screendump {attempt_path}")
        wait_for_screendump_file(attempt_path)
        try:
            error = desktop_screenshot_error(attempt_path)
        except (OSError, ValueError) as exc:
            error = str(exc)
        if error is None:
            attempt_path.replace(SCREENSHOT)
            remove_attempt_screenshots()
            return
        last_error = error
        time.sleep(1.0)

    stderr = QEMU_LOG.read_text(errors="ignore") if QEMU_LOG.exists() else ""
    debugcon = DEBUGCON_LOG.read_text(errors="ignore") if DEBUGCON_LOG.exists() else ""
    raise SystemExit(
        "desktop did not reach a valid screenshot before timeout: "
        + last_error
        + (f"\nqemu stderr:\n{stderr[-2000:]}" if stderr else "")
        + (f"\ndebugcon tail:\n{debugcon[-2000:]}" if debugcon else "")
    )


def remove_attempt_screenshots() -> None:
    for stale in LOG_DIR.glob(f"{SCREENSHOT.stem}.attempt*.ppm"):
        stale.unlink(missing_ok=True)


def wait_for_screendump_file(path: Path) -> None:
    deadline = time.time() + 2.0
    last_size = -1
    while time.time() < deadline:
        try:
            size = path.stat().st_size
        except FileNotFoundError:
            time.sleep(0.05)
            continue
        if size > 0 and size == last_size:
            return
        last_size = size
        time.sleep(0.05)
    if not path.exists():
        raise FileNotFoundError(path)


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
    build_desktop_image()
    prepare_images()

    for path in (
        SERIAL_LOG,
        DEBUGCON_LOG,
        QEMU_LOG,
        MONITOR_SOCKET,
        SCREENSHOT,
    ):
        path.unlink(missing_ok=True)

    remove_attempt_screenshots()

    qemu_stderr = QEMU_LOG.open("w")
    proc: subprocess.Popen[bytes] | None = None
    try:
        proc = subprocess.Popen(
            qemu_command(),
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=qemu_stderr,
        )
        wait_for_desktop_screenshot(proc)
        print(f"desktop screenshot check passed: {SCREENSHOT.relative_to(ROOT)}")
        return 0
    except FileNotFoundError as exc:
        raise SystemExit(f"failed to start QEMU command {qemu_binary()!r}: {exc}") from exc
    finally:
        if proc is not None:
            stop_qemu(proc)
        qemu_stderr.close()


if __name__ == "__main__":
    raise SystemExit(main())
