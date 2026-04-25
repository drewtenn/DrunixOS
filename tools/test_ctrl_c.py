#!/usr/bin/env python3
"""
Architecture-neutral Ctrl-C delivery regression through the user shell.
"""

from __future__ import annotations

import argparse

from run_shell_session import ShellSession


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=("arm64", "x86"), required=True)
    args = parser.parse_args()

    with ShellSession(args.arch) as session:
        session.read_until("drunix shell --", 30.0)
        session.wait_for_prompt(10.0)

        session.buffer.clear()
        session.send_bytes(b"\x03")
        session.wait_for_prompt(10.0)

        session.buffer.clear()
        session.send_command("echo prompt-still-alive", timeout=10.0)
        session.read_output_line("prompt-still-alive", 10.0)
        session.wait_for_prompt(10.0)

        session.buffer.clear()
        session.send_command("sleeper")
        session.read_until("[sleeper] 1", 10.0)

        session.send_bytes(b"\x03")
        session.wait_for_prompt(10.0)

    print(f"{args.arch} ctrl-c check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
