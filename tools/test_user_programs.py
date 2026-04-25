#!/usr/bin/env python3
"""
Architecture-neutral user-program smoke tests.

The test intentionally exercises normal shell launching rather than `exec`,
because fork/exec/wait is the behavior a working interactive shell needs.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re

from run_shell_session import ShellSession


ROOT = Path(__file__).resolve().parents[1]

SMOKES = {
    "arm64": (
        ("/bin/chello", "Hello from C userland!"),
        ("/bin/hello", "Hello from ring 3!"),
        ("/bin/cpphello", "new[] sum=6"),
    ),
    "x86": (
        ("chello", "Hello from C userland!"),
        ("hello", "Hello from ring 3!"),
        ("cpphello", "new[] sum=6"),
    ),
}


def native_programs() -> list[str]:
    text = (ROOT / "user" / "programs.mk").read_text()
    match = re.search(r"^PROGS\s*=\s*(.+)$", text, re.MULTILINE)
    if not match:
        raise RuntimeError("user/programs.mk missing PROGS")
    return match.group(1).split()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=("x86", "arm64"), required=True)
    args = parser.parse_args()

    with ShellSession(args.arch) as session:
        session.read_until("drunix shell --", 30.0)
        session.read_until("drunix:", 10.0)

        for program in native_programs():
            session.buffer.clear()
            session.send_line(f"which {program}")
            session.read_until(f"/bin/{program}", 15.0)
            session.read_until("drunix:", 15.0)

        for command, marker in SMOKES[args.arch]:
            session.buffer.clear()
            session.send_line(command)
            session.read_until(marker, 15.0)
            session.read_until("drunix:", 15.0)

    print(f"{args.arch} user program smoke passed")


if __name__ == "__main__":
    main()
