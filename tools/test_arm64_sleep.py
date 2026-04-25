#!/usr/bin/env python3
"""
Regression test for ARM64 timed sleeps through the user shell.
"""

from __future__ import annotations

from run_shell_session import ShellSession


def main() -> None:
    with ShellSession("arm64") as session:
        session.read_until("drunix shell --", 30.0)
        session.read_until("drunix:", 10.0)

        session.buffer.clear()
        session.send_line("/bin/sleep 1")
        session.read_until("drunix:", 15.0)

        session.buffer.clear()
        session.send_line("echo arm64-sleep-ok")
        session.read_until("arm64-sleep-ok", 5.0)
        session.read_until("drunix:", 5.0)

    print("arm64 sleep check passed")


if __name__ == "__main__":
    main()
