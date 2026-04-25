#!/usr/bin/env python3
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SHARED_SHELL_TESTS = (
    ROOT / "tools" / "test_shell_prompt.py",
    ROOT / "tools" / "test_user_programs.py",
    ROOT / "tools" / "test_sleep.py",
    ROOT / "tools" / "test_ctrl_c.py",
    ROOT / "tools" / "test_shell_history.py",
)

FORBIDDEN_PATTERNS = (
    (re.compile(r"\bif\s+args\.arch\b"), "branches on args.arch"),
    (re.compile(r"\bargs\.arch\s*=="), "compares args.arch"),
    (re.compile(r"\bSMOKES\s*=\s*\{"), "uses per-arch smoke command lists"),
)


def main() -> int:
    failures: list[str] = []
    for path in SHARED_SHELL_TESTS:
        text = path.read_text()
        for pattern, description in FORBIDDEN_PATTERNS:
            if pattern.search(text):
                failures.append(f"{path.relative_to(ROOT)} {description}")

    if failures:
        print("shared shell tests must keep behavior architecture-neutral:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("shared shell tests keep behavior architecture-neutral")
    return 0


if __name__ == "__main__":
    sys.exit(main())
