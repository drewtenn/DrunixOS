#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
ARCHES = ("x86", "arm64")
FORBIDDEN_PUBLIC_TARGET_PREFIXES = ("check-arm64-", "check-x86-")
FORBIDDEN_PUBLIC_TARGETS = {"phase6-check"}


def phony_targets(arch: str) -> set[str]:
    result = subprocess.run(
        ["make", "-pn", f"ARCH={arch}"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="")
        raise SystemExit(result.returncode)

    targets: set[str] = set()
    for line in (result.stdout + result.stderr).splitlines():
        if line.startswith(".PHONY:"):
            targets.update(line.split(":", 1)[1].split())
    return targets


def check_targets(targets: set[str]) -> set[str]:
    return {target for target in targets if target.startswith("check")}


def main() -> int:
    targets_by_arch = {arch: phony_targets(arch) for arch in ARCHES}
    failures: list[str] = []

    for arch, targets in targets_by_arch.items():
        for target in sorted(targets):
            if target in FORBIDDEN_PUBLIC_TARGETS:
                failures.append(f"{arch} exposes alias target {target}")
            if target.startswith(FORBIDDEN_PUBLIC_TARGET_PREFIXES):
                failures.append(f"{arch} exposes architecture-prefixed target {target}")

    check_sets = {arch: check_targets(targets) for arch, targets in targets_by_arch.items()}
    if check_sets["x86"] != check_sets["arm64"]:
        failures.append(
            "check target sets differ: "
            f"x86-only={sorted(check_sets['x86'] - check_sets['arm64'])} "
            f"arm64-only={sorted(check_sets['arm64'] - check_sets['x86'])}"
        )

    if failures:
        print("public check targets must be architecture-neutral:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("public check targets are architecture-neutral")
    return 0


if __name__ == "__main__":
    sys.exit(main())
