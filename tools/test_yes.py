#!/usr/bin/env python3
"""
Architecture-neutral smoke test for /bin/yes.

The program is intentionally endless, so this test reads a couple of output
lines, interrupts it with Ctrl-C, then verifies the shell is usable again.
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
        session.send_command("/bin/yes rexy yes", timeout=10.0)
        session.read_until("rexy yes\r\n", 5.0)
        session.buffer.clear()
        session.read_until("rexy yes\r\n", 5.0)

        session.send_bytes(b"\x03")
        session.wait_for_prompt(10.0)

        session.buffer.clear()
        session.send_command("echo yes-stopped", timeout=10.0)
        session.read_output_line("yes-stopped", 10.0)
        session.wait_for_prompt(10.0)

    print(f"{args.arch} yes smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
