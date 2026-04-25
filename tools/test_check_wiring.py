#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def required_commands(arch: str) -> tuple[str, ...]:
    shared = (
        f"python3 tools/test_shell_prompt.py --arch {arch}",
        f"python3 tools/test_user_programs.py --arch {arch}",
        f"python3 tools/test_sleep.py --arch {arch}",
        f"python3 tools/test_ctrl_c.py --arch {arch}",
        f"python3 tools/test_shell_history.py --arch {arch}",
        "python3 tools/test_arch_boundary_reuse.py",
        "python3 tools/test_shared_shell_tests_arch_neutral.py",
        "python3 tools/test_make_targets_arch_neutral.py",
    )
    if arch == "arm64":
        return shared + (
            "python3 tools/test_arm64_userspace_smoke.py",
            "python3 tools/test_arm64_filesystem_init.py",
            "python3 tools/test_arm64_syscall_parity.py",
        )
    return shared


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

    missing = [cmd for cmd in required_commands(args.arch) if cmd not in output]
    if missing:
        print(f"make ARCH={args.arch} check is missing required wiring:")
        for cmd in missing:
            print(f"  {cmd}")
        return 1

    print(f"{args.arch} check wiring includes shared behavior tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
