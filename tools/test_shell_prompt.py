#!/usr/bin/env python3
"""
Boot an architecture build and prove the initial process is the user shell.
"""

from __future__ import annotations

import argparse

from run_shell_session import ShellSession


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=("arm64", "x86"), required=True)
    args = parser.parse_args()

    with ShellSession(args.arch) as session:
        session.read_until("drunix shell --", timeout=20.0)
        session.wait_for_prompt(timeout=5.0)
        marker = f"{args.arch}-shell-ok"
        session.send_command(f"echo {marker}", timeout=5.0)
        session.read_output_line(marker, timeout=5.0)

    print(f"{args.arch} shell prompt check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
