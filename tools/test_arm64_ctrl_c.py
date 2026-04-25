#!/usr/bin/env python3
"""
Regression test for Ctrl-C delivery to a foreground ARM64 process.
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
    with ShellSession("arm64") as session:
        session.read_until("drunix shell --", 30.0)
        session.read_until("drunix:", 10.0)

        session.buffer.clear()
        send_bytes(session, b"\x03")
        session.send_line("echo prompt-still-alive")
        session.read_until("prompt-still-alive", 10.0)
        session.read_until("drunix:", 10.0)

        session.buffer.clear()
        session.send_line("/bin/sleeper")
        session.read_until("[sleeper] 1", 10.0)

        send_bytes(session, b"\x03")
        session.read_until("drunix:", 10.0)

    print("arm64 ctrl-c check passed")


if __name__ == "__main__":
    main()
