#!/usr/bin/env python3
"""
Architecture-neutral timed sleep regression through the user shell.
"""

from __future__ import annotations

import argparse
import time

from run_shell_session import ShellSession


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=("arm64", "x86"), required=True)
    args = parser.parse_args()

    marker = f"{args.arch}-sleep-ok"

    with ShellSession(args.arch) as session:
        session.read_until("drunix shell --", 30.0)
        session.wait_for_prompt(10.0)

        session.buffer.clear()
        start = time.monotonic()
        session.send_command("sleep 1")
        session.wait_for_prompt(15.0)
        elapsed = time.monotonic() - start
        if elapsed < 0.8:
            raise AssertionError(f"sleep returned too early after {elapsed:.2f}s")

        session.buffer.clear()
        session.send_command(f"echo {marker}")
        session.read_output_line(marker, 5.0)
        session.wait_for_prompt(5.0)

    print(f"{args.arch} sleep check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
