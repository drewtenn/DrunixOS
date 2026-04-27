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


def make_dry_run(target: str, args: tuple[str, ...]) -> str:
    result = subprocess.run(
        ["make", "--no-print-directory", "-n", "-B", *args, target],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="")
        raise SystemExit(result.returncode)
    return result.stdout


def require_werror(name: str, flags: str, failures: list[str]) -> None:
    if "-Werror" not in flags.split():
        failures.append(f"{name} does not include -Werror: {flags}")


def require_optimized(name: str, flags: str, failures: list[str]) -> None:
    words = flags.split()
    if "-O2" not in words:
        failures.append(f"{name} does not include -O2: {flags}")
    if "-Og" in words:
        failures.append(f"{name} still includes -Og: {flags}")


def require_debug_optimized(name: str, flags: str, failures: list[str]) -> None:
    words = flags.split()
    if "-Og" not in words:
        failures.append(f"{name} does not include -Og: {flags}")
    if "-O2" in words:
        failures.append(f"{name} still includes -O2: {flags}")


def require_debug_target_uses_og(label: str,
                                 target: str,
                                 args: tuple[str, ...],
                                 failures: list[str]) -> None:
    output = make_dry_run(target, args)
    if " -Og " not in output:
        failures.append(f"{label} does not compile with -Og")
    if " -O2 " in output:
        failures.append(f"{label} still compiles with -O2")


def main() -> int:
    failures: list[str] = []

    root_makefile = ROOT / "Makefile"
    user_makefile = ROOT / "user" / "Makefile"

    x86_kernel = make_vars(ROOT, root_makefile, ("CFLAGS", "NASMFLAGS"),
                           ("ARCH=x86",))
    for name, flags in x86_kernel.items():
        require_werror(f"x86 kernel {name}", flags, failures)
    require_optimized("x86 kernel CFLAGS", x86_kernel["CFLAGS"], failures)

    arm64 = make_vars(
        ROOT,
        root_makefile,
        ("ARM_CFLAGS", "ARM_USER_CFLAGS", "ARM_USER_CXXFLAGS"),
        ("ARCH=arm64",),
    )
    for name, flags in arm64.items():
        require_werror(f"arm64 {name}", flags, failures)
    require_optimized("arm64 kernel ARM_CFLAGS", arm64["ARM_CFLAGS"], failures)
    require_optimized("arm64 user ARM_USER_CFLAGS",
                      arm64["ARM_USER_CFLAGS"], failures)
    require_optimized("arm64 user ARM_USER_CXXFLAGS",
                      arm64["ARM_USER_CXXFLAGS"], failures)

    x86_user = make_vars(ROOT / "user", user_makefile,
                         ("CFLAGS", "CXXFLAGS", "NASMFLAGS"))
    for name, flags in x86_user.items():
        require_werror(f"x86 user {name}", flags, failures)
    require_optimized("x86 user CFLAGS", x86_user["CFLAGS"], failures)
    require_optimized("x86 user CXXFLAGS", x86_user["CXXFLAGS"], failures)

    x86_debug_kernel = make_vars(ROOT, root_makefile, ("CFLAGS",),
                                 ("ARCH=x86", "BUILD_MODE=debug"))
    require_debug_optimized("x86 debug kernel CFLAGS",
                            x86_debug_kernel["CFLAGS"], failures)

    arm64_debug = make_vars(
        ROOT,
        root_makefile,
        ("ARM_CFLAGS", "ARM_USER_CFLAGS", "ARM_USER_CXXFLAGS"),
        ("ARCH=arm64", "BUILD_MODE=debug"),
    )
    require_debug_optimized("arm64 debug kernel ARM_CFLAGS",
                            arm64_debug["ARM_CFLAGS"], failures)
    require_debug_optimized("arm64 debug user ARM_USER_CFLAGS",
                            arm64_debug["ARM_USER_CFLAGS"], failures)
    require_debug_optimized("arm64 debug user ARM_USER_CXXFLAGS",
                            arm64_debug["ARM_USER_CXXFLAGS"], failures)

    x86_debug_user = make_vars(ROOT / "user", user_makefile,
                               ("CFLAGS", "CXXFLAGS"),
                               ("BUILD_MODE=debug",))
    require_debug_optimized("x86 debug user CFLAGS",
                            x86_debug_user["CFLAGS"], failures)
    require_debug_optimized("x86 debug user CXXFLAGS",
                            x86_debug_user["CXXFLAGS"], failures)

    for target in ("debug", "debug-user", "debug-fresh"):
        args = ("ARCH=x86",)
        if target == "debug-user":
            args += ("APP=shell",)
        require_debug_target_uses_og(f"x86 {target} target", target, args,
                                     failures)

    for target in ("debug", "debug-user", "debug-fresh"):
        args = ("ARCH=arm64",)
        if target == "debug-user":
            args += ("APP=shell",)
        require_debug_target_uses_og(f"arm64 {target} target", target, args,
                                     failures)

    if failures:
        print("build warning and optimization policy failed:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("build warning and optimization policy is enforced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
