#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def required_check_commands(arch: str) -> tuple[str, ...]:
    return (
        "python3 tools/test_arch_boundary_reuse.py",
        "python3 tools/test_shared_shell_tests_arch_neutral.py",
        "python3 tools/test_make_targets_arch_neutral.py",
        f"python3 tools/test_check_wiring.py --arch {arch}",
        "python3 tools/check_test_intent_coverage.py",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=("arm64", "x86"), required=True)
    args = parser.parse_args()

    result = subprocess.run(
        ["make", "-n", f"ARCH={args.arch}", "check"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        return result.returncode

    missing = [cmd for cmd in required_check_commands(args.arch) if cmd not in output]
    if missing:
        print(f"make ARCH={args.arch} check is missing required wiring:")
        for cmd in missing:
            print(f"  {cmd}")
        return 1

    result = subprocess.run(
        ["make", "-n", f"ARCH={args.arch}", "test-headless"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        return result.returncode

    if re.search(rf"\bmake(?:\[\d+\])?\s+ARCH={args.arch}\s+check\s*$", output, re.MULTILINE):
        print(f"make ARCH={args.arch} test-headless must not delegate back to check")
        return 1

    print(f"{args.arch} check target shape includes guard targets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
