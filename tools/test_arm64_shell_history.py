#!/usr/bin/env python3
"""
Regression test for ARM64 shell command history recall.
"""

from __future__ import annotations

from run_shell_session import ShellSession


def send_bytes(session: ShellSession, data: bytes) -> None:
    if session.sock is not None:
        session.sock.sendall(data)
        return
    assert session.proc is not None and session.proc.stdin is not None
    session.proc.stdin.write(data)
    session.proc.stdin.flush()


def main() -> None:
    marker = "history-recalled-ok"
    with ShellSession("arm64") as session:
        session.read_until("drunix shell --", 30.0)
        session.read_until("drunix:", 10.0)

        session.buffer.clear()
        session.send_line(f"echo {marker}")
        session.read_until(marker, 10.0)
        session.read_until("drunix:", 10.0)

        session.buffer.clear()
        send_bytes(session, b"\x1b[A\n")
        session.read_until(marker, 10.0)
        session.read_until("drunix:", 10.0)

    print("arm64 shell history check passed")


if __name__ == "__main__":
    main()
