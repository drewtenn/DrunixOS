#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def make_vars(cwd: Path,
              makefile: Path,
              variables: tuple[str, ...],
              args: tuple[str, ...] = ()) -> dict[str, str]:
    with tempfile.TemporaryDirectory() as tmpdir:
        print_mk = Path(tmpdir) / "print-vars.mk"
        lines = ["print-warning-vars:"]
        for variable in variables:
            lines.append(f"\t@printf '%s\\n' '{variable}=$({variable})'")
        print_mk.write_text("\n".join(lines) + "\n", encoding="utf-8")

        result = subprocess.run(
            ["make", "--no-print-directory", "-s", "-f", str(makefile), "-f",
             str(print_mk), *args, "print-warning-vars"],
            cwd=cwd,
            capture_output=True,
            text=True,
        )

    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="")
        raise SystemExit(result.returncode)

    values: dict[str, str] = {}
    for line in result.stdout.splitlines():
        name, value = line.split("=", 1)
        values[name] = value
    return values


def require_werror(name: str, flags: str, failures: list[str]) -> None:
    if "-Werror" not in flags.split():
        failures.append(f"{name} does not include -Werror: {flags}")


def main() -> int:
    failures: list[str] = []

    root_makefile = ROOT / "Makefile"
    user_makefile = ROOT / "user" / "Makefile"

    x86_kernel = make_vars(ROOT, root_makefile, ("CFLAGS", "NASMFLAGS"),
                           ("ARCH=x86",))
    for name, flags in x86_kernel.items():
        require_werror(f"x86 kernel {name}", flags, failures)

    arm64 = make_vars(
        ROOT,
        root_makefile,
        ("ARM_CFLAGS", "ARM_USER_CFLAGS", "ARM_USER_CXXFLAGS"),
        ("ARCH=arm64",),
    )
    for name, flags in arm64.items():
        require_werror(f"arm64 {name}", flags, failures)

    x86_user = make_vars(ROOT / "user", user_makefile,
                         ("CFLAGS", "CXXFLAGS", "NASMFLAGS"))
    for name, flags in x86_user.items():
        require_werror(f"x86 user {name}", flags, failures)

    if failures:
        print("build warning policy must promote warnings to errors:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("build warning policy promotes warnings to errors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
