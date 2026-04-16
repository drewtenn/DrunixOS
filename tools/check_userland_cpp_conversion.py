#!/usr/bin/env python3

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
USER = ROOT / "user"
MAKEFILE = USER / "Makefile"
EXCLUDED = {"shell"}


def user_programs():
    text = MAKEFILE.read_text()
    match = re.search(r"^PROGS\s*=\s*(.+)$", text, re.MULTILINE)
    if not match:
        raise RuntimeError("user/Makefile does not define PROGS")
    return match.group(1).split()


def main():
    failures = []
    for prog in user_programs():
        if prog in EXCLUDED:
            continue
        cpp = USER / f"{prog}.cpp"
        c = USER / f"{prog}.c"
        if not cpp.exists():
            failures.append(f"missing C++ source: user/{prog}.cpp")
        if c.exists():
            failures.append(f"utility still has C source: user/{prog}.c")

    if failures:
        for failure in failures:
            print(failure)
        return 1

    print("all non-shell userland utilities are C++ sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
