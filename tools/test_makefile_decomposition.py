#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
ROOT_MAKEFILE = ROOT / "Makefile"
REQUIRED_INCLUDES = (
    "mk/disk-images.mk",
    "mk/checks.mk",
    "mk/scan-x86.mk",
    "mk/scan-arm64.mk",
    "mk/run-x86.mk",
    "mk/run-arm64.mk",
    "mk/utility-targets.mk",
)
MAX_ROOT_MAKEFILE_LINES = 520


def main() -> int:
    text = ROOT_MAKEFILE.read_text(encoding="utf-8")
    failures: list[str] = []

    for include in REQUIRED_INCLUDES:
        if f"include {include}" not in text:
            failures.append(f"Makefile does not include {include}")

    line_count = len(text.splitlines())
    if line_count > MAX_ROOT_MAKEFILE_LINES:
        failures.append(
            f"Makefile has {line_count} lines; expected at most "
            f"{MAX_ROOT_MAKEFILE_LINES}"
        )

    if failures:
        print("root Makefile decomposition failed:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("root Makefile delegates target families to included makefiles")
    return 0


if __name__ == "__main__":
    sys.exit(main())
