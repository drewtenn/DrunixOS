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

SMOKES = (
    ("chello", "Hello from C userland!"),
    ("hello", "Hello from ring 3!"),
    ("/bin/echo rexy echo smoke", "rexy echo smoke"),
    ("/bin/printenv PATH", "/bin"),
    ("/bin/cat hello.txt", "Hello from the filesystem!"),
    ("/bin/basename /usr/bin/leaf.txt .txt", "leaf\r\n"),
    ("/bin/dirname /usr/bin/leaf.txt", "/usr/bin\r\n"),
    ("/bin/cmp hello.txt readme.txt", "hello.txt readme.txt differ: char 1, line 1\r\n"),
    ("/bin/head -n 1 readme.txt", "DUFS test file.\r\n"),
    ("/bin/tail -n 1 readme.txt", "Line three.\r\n"),
    ("/bin/uniq readme.txt", "Line two.\r\n"),
    ("/bin/sort readme.txt", "Line two.\r\n"),
    ("/bin/wc -l readme.txt", "3 readme.txt\r\n"),
    ("/bin/writer", "hello from the pipe writer\r\n"),
    ("/bin/writer | /bin/reader", "[reader] received 27 bytes\r\n"),
    ("/bin/date", "UTC"),
    ("/bin/grep DUFS readme.txt", "DUFS test file.\r\n"),
    ("/bin/echo tee-line | /bin/tee /tmp/tee-smoke.txt", "tee-line\r\n"),
    ("/bin/kill -l 15", "TERM\r\n"),
    ("cpphello", "new[] sum=6"),
)


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
        session.wait_for_prompt(10.0)

        for program in native_programs():
            session.buffer.clear()
            session.send_command(f"which {program}")
            session.read_until(f"/bin/{program}", 15.0)
            session.wait_for_prompt(15.0)

        for command, marker in SMOKES:
            session.buffer.clear()
            session.send_command(command)
            session.read_until(marker, 15.0)
            session.wait_for_prompt(15.0)

    print(f"{args.arch} user program smoke passed")


if __name__ == "__main__":
    main()
