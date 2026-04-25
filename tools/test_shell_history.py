#!/usr/bin/env python3
"""
Architecture-neutral shell command-history recall regression.
"""

from __future__ import annotations

import argparse

from run_shell_session import ShellSession


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=("arm64", "x86"), required=True)
    args = parser.parse_args()

    marker = "history-recalled-ok"
    with ShellSession(args.arch) as session:
        session.read_until("drunix shell --", 30.0)
        session.wait_for_prompt(10.0)

        session.buffer.clear()
        session.send_command(f"echo {marker}")
        session.read_output_line(marker, 10.0)
        session.wait_for_prompt(10.0)

        session.buffer.clear()
        session.send_bytes(b"\x1b[A\n")
        session.read_output_line(marker, 10.0)
        session.wait_for_prompt(10.0)

    print(f"{args.arch} shell history check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
