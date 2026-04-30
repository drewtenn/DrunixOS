#!/usr/bin/env python3
"""Boot ARM64 virt and require the virtio-gpu hardware cursor to be exported."""

from __future__ import annotations

from pathlib import Path
import os
import socket
import struct
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"
SERIAL_LOG = LOG_DIR / "serial-arm64-hw-cursor-active.log"
QEMU_LOG = LOG_DIR / "qemu-arm64-hw-cursor-active.stderr"
BOOT_TIMEOUT = 30.0
VNC_DISPLAY = int(os.environ.get("DRUNIX_TEST_VNC_DISPLAY", "79"))
VNC_PORT = 5900 + VNC_DISPLAY


def qemu_binary() -> str:
    return os.environ.get("QEMU_ARM", "qemu-system-aarch64")


def build_image() -> None:
    result = subprocess.run(
        [
            "make",
            "ARCH=arm64",
            "PLATFORM=virt",
            "DRUNIX_ARM64_VIRT_HW_CURSOR=1",
            "build",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(
            "arm64 hw-cursor build failed:\n"
            + (result.stderr.strip() or result.stdout.strip() or "no build output")
        )


def qemu_command() -> list[str]:
    return [
        qemu_binary(),
        "-display",
        f"vnc=127.0.0.1:{VNC_DISPLAY}",
        "-M",
        "virt,gic-version=3",
        "-cpu",
        "cortex-a53",
        "-smp",
        "1",
        "-m",
        "1G",
        "-kernel",
        "kernel-arm64.elf",
        "-drive",
        "file=img/disk.img,if=none,format=raw,id=hd0",
        "-device",
        "virtio-blk-device,drive=hd0",
        "-device",
        "virtio-gpu-device",
        "-device",
        "virtio-keyboard-device",
        "-device",
        "virtio-mouse-device",
        "-serial",
        f"file:{SERIAL_LOG}",
        "-monitor",
        "none",
        "-no-reboot",
    ]


def connect_vnc() -> socket.socket:
    deadline = time.time() + BOOT_TIMEOUT
    last_error: OSError | None = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", VNC_PORT), timeout=2.0)
            sock.settimeout(2.0)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.1)
    raise SystemExit(f"failed to connect to QEMU VNC on port {VNC_PORT}: {last_error}")


def recv_exact(sock: socket.socket, count: int) -> bytes:
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError("VNC connection closed")
        data += chunk
    return data


def vnc_handshake(sock: socket.socket) -> tuple[int, int]:
    version = recv_exact(sock, 12)
    sock.sendall(version)
    security_count = recv_exact(sock, 1)[0]
    security_types = recv_exact(sock, security_count)
    if 1 not in security_types:
        raise SystemExit(f"QEMU VNC did not offer no-auth security: {security_types!r}")
    sock.sendall(b"\x01")
    result = recv_exact(sock, 4)
    if result != b"\x00\x00\x00\x00":
        raise SystemExit(f"QEMU VNC rejected no-auth security: {result!r}")
    sock.sendall(b"\x01")
    server_init = recv_exact(sock, 24)
    width, height = struct.unpack(">HH", server_init[:4])
    name_len = struct.unpack(">I", server_init[20:24])[0]
    recv_exact(sock, name_len)
    return width, height


def wait_for_vnc_cursor() -> None:
    with connect_vnc() as sock:
        width, height = vnc_handshake(sock)
        encodings = [0, -239]  # Raw, Cursor pseudo-encoding.
        sock.sendall(
            struct.pack(">BBH", 2, 0, len(encodings))
            + b"".join(struct.pack(">i", enc) for enc in encodings)
        )
        sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))

        deadline = time.time() + BOOT_TIMEOUT
        while time.time() < deadline:
            try:
                msg_type = recv_exact(sock, 1)[0]
            except socket.timeout:
                sock.sendall(struct.pack(">BBHHHH", 3, 1, 0, 0, width, height))
                continue
            if msg_type != 0:
                continue
            recv_exact(sock, 1)
            rect_count = struct.unpack(">H", recv_exact(sock, 2))[0]
            for _ in range(rect_count):
                x, y, w, h, encoding = struct.unpack(">HHHHi", recv_exact(sock, 12))
                if encoding == -239:
                    mask_stride = (w + 7) // 8
                    recv_exact(sock, w * h * 4 + mask_stride * h)
                    if w == 64 and h == 64:
                        return
                elif encoding == 0:
                    recv_exact(sock, w * h * 4)
                else:
                    raise SystemExit(
                        f"unexpected VNC rectangle encoding {encoding} at {x},{y} {w}x{h}"
                    )
            sock.sendall(struct.pack(">BBHHHH", 3, 1, 0, 0, width, height))

    raise SystemExit("QEMU VNC did not export a 64x64 hardware cursor update")


def wait_for_hw_cursor(proc: subprocess.Popen[bytes]) -> None:
    required = [
        "virtio-gpu: hardware cursor uploaded",
        "virtio-gpu: first cursor move submitted",
    ]
    deadline = time.time() + BOOT_TIMEOUT
    while time.time() < deadline:
        text = SERIAL_LOG.read_text(errors="ignore") if SERIAL_LOG.exists() else ""
        if all(marker in text for marker in required):
            return
        if proc.poll() is not None:
            break
        time.sleep(0.2)

    text = SERIAL_LOG.read_text(errors="ignore") if SERIAL_LOG.exists() else ""
    missing = [marker for marker in required if marker not in text]
    stderr = QEMU_LOG.read_text(errors="ignore") if QEMU_LOG.exists() else ""
    raise SystemExit(
        "desktop did not activate the hardware cursor path; missing: "
        + ", ".join(missing)
        + (f"\nqemu stderr:\n{stderr[-2000:]}" if stderr else "")
        + (f"\nserial tail:\n{text[-2000:]}" if text else "")
    )


def stop_qemu(proc: subprocess.Popen[bytes]) -> None:
    if proc.poll() is None:
        proc.kill()
    proc.wait(timeout=2.0)


def main() -> int:
    build_image()
    LOG_DIR.mkdir(exist_ok=True)
    for path in (SERIAL_LOG, QEMU_LOG):
        path.unlink(missing_ok=True)

    qemu_stderr = QEMU_LOG.open("w")
    proc: subprocess.Popen[bytes] | None = None
    try:
        proc = subprocess.Popen(
            qemu_command(),
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=qemu_stderr,
        )
        wait_for_hw_cursor(proc)
        wait_for_vnc_cursor()
        print("arm64 hardware cursor activation/display check passed")
        return 0
    except FileNotFoundError as exc:
        raise SystemExit(f"failed to start QEMU command {qemu_binary()!r}: {exc}") from exc
    finally:
        if proc is not None:
            stop_qemu(proc)
        qemu_stderr.close()


if __name__ == "__main__":
    raise SystemExit(main())
