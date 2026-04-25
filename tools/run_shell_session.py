#!/usr/bin/env python3
"""
Shared QEMU shell-session harness.

The harness owns the architecture-specific QEMU wiring, while tests interact
with a small byte-stream API: wait for text, send a line, then wait again.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import os
import select
import socket
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"


@dataclass(frozen=True)
class ShellArchConfig:
    arch: str
    build_args: tuple[str, ...]
    qemu_args: tuple[str, ...]
    serial_socket: Path | None
    qemu_stderr: Path


def arch_config(arch: str) -> ShellArchConfig:
    if arch == "arm64":
        return ShellArchConfig(
            arch=arch,
            build_args=("make", "ARCH=arm64", "build"),
            qemu_args=(
                os.environ.get("QEMU_ARM", "qemu-system-aarch64"),
                "-display",
                "none",
                "-M",
                os.environ.get("QEMU_ARM_MACHINE", "raspi3b"),
                "-kernel",
                "kernel-arm64.elf",
                "-serial",
                "null",
                "-serial",
                "stdio",
                "-device",
                "usb-kbd",
                "-monitor",
                "none",
                "-no-reboot",
            ),
            serial_socket=None,
            qemu_stderr=LOG_DIR / "qemu-arm64-shell.stderr",
        )

    if arch == "x86":
        raise NotImplementedError(
            "x86 shell sessions need a VGA/keyboard adapter; no shared "
            "byte-stream path exists yet"
        )

    raise ValueError(f"unsupported architecture: {arch}")


class ShellSession:
    def __init__(self, arch: str, *, build: bool = True) -> None:
        self.config = arch_config(arch)
        self.build = build
        self.proc: subprocess.Popen[bytes] | None = None
        self.sock: socket.socket | None = None
        self.stderr_file = None
        self.buffer = bytearray()

    def __enter__(self) -> "ShellSession":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop()

    def start(self) -> None:
        if self.build:
            build = subprocess.run(
                list(self.config.build_args),
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            if build.returncode != 0:
                raise RuntimeError(
                    "build failed:\n"
                    + (build.stderr.strip() or build.stdout.strip() or "no output")
                )

        LOG_DIR.mkdir(exist_ok=True)
        if self.config.serial_socket is not None:
            self.config.serial_socket.unlink(missing_ok=True)
        self.config.qemu_stderr.unlink(missing_ok=True)
        self.stderr_file = self.config.qemu_stderr.open("w")
        self.proc = subprocess.Popen(
            list(self.config.qemu_args),
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_file,
        )
        if self.config.serial_socket is not None:
            self.sock = self._connect_serial()

    def _connect_serial(self) -> socket.socket:
        deadline = time.time() + 10.0
        last_error: OSError | None = None
        while time.time() < deadline:
            if self.proc and self.proc.poll() is not None:
                raise RuntimeError(f"QEMU exited early with status {self.proc.returncode}")
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(0.2)
                sock.connect(str(self.config.serial_socket))
                return sock
            except OSError as exc:
                last_error = exc
                time.sleep(0.05)
        raise RuntimeError(f"failed to connect serial socket: {last_error}")

    def stop(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.kill()
            try:
                self.proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2.0)
            self.proc = None

        if self.stderr_file is not None:
            self.stderr_file.close()
            self.stderr_file = None

    def read_until(self, marker: str, timeout: float = 20.0) -> str:
        marker_bytes = marker.encode("utf-8")
        deadline = time.time() + timeout
        while time.time() < deadline:
            if marker_bytes in self.buffer:
                return self.text()
            if self.proc and self.proc.poll() is not None:
                raise RuntimeError(f"QEMU exited early with status {self.proc.returncode}")
            if self.sock is not None:
                try:
                    chunk = self.sock.recv(4096)
                except socket.timeout:
                    continue
            else:
                assert self.proc is not None and self.proc.stdout is not None
                ready, _, _ = select.select([self.proc.stdout], [], [], 0.2)
                if not ready:
                    continue
                chunk = os.read(self.proc.stdout.fileno(), 4096)
            if not chunk:
                time.sleep(0.05)
                continue
            self.buffer.extend(chunk)
        raise TimeoutError(f"timed out waiting for {marker!r}\n\n{self.text()}")

    def send_line(self, line: str) -> None:
        data = line.encode("utf-8") + b"\n"
        if self.sock is not None:
            self.sock.sendall(data)
            return
        assert self.proc is not None and self.proc.stdin is not None
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def text(self) -> str:
        return self.buffer.decode("utf-8", errors="replace")
