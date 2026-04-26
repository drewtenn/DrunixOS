#!/usr/bin/env python3
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

from test_intent_manifest import ARCHES, INTENTS


ROOT = Path(__file__).resolve().parents[1]
REGEX_MARKERS = ("[", "]", "(", ")", ".*", "\\d", "\\s")
X86_ONLY_KTEST_SOURCES = (
    "kernel/arch/x86/test/test_pmm.c",
    "kernel/arch/x86/test/test_arch_x86.c",
    "kernel/arch/x86/test/test_process.c",
    "kernel/arch/x86/test/test_uaccess.c",
    "kernel/arch/x86/test/test_desktop.c",
)
ARCH_KTEST_SOURCES = {
    "arm64": ("kernel/arch/arm64/test/test_arch_arm64.c",),
}


def make_output(arch: str, target: str) -> str:
    result = subprocess.run(
        ["make", "-n", f"ARCH={arch}", target],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise SystemExit(result.returncode)
    return output


def command_is_covered(command: str, output: str) -> bool:
    if command in output:
        return True
    if any(marker in command for marker in REGEX_MARKERS):
        return re.search(command, output) is not None
    return False


def source_marker_is_covered(source_path: str, marker: str) -> bool:
    path = ROOT / source_path
    if not path.exists():
        return False
    text = path.read_text(errors="ignore")
    if "/test/" in source_path:
        return re.search(rf"\bKTEST_CASE\(\s*{re.escape(marker)}\s*\)", text) is not None
    return marker in text


def ktest_cases(source_path: str) -> list[str]:
    text = (ROOT / source_path).read_text(errors="ignore")
    return re.findall(r"\bKTEST_CASE\(\s*([A-Za-z_]\w*)\s*\)", text)


def manifest_markers_for(arch: str, source_path: str) -> set[str]:
    markers: set[str] = set()
    for intent in INTENTS:
        for source in intent.sources.get(arch, ()):
            if source.path == source_path:
                markers.update(source.markers)
    return markers


def check_x86_ktest_inventory(failures: list[str]) -> int:
    missing_count = 0
    for source_path in X86_ONLY_KTEST_SOURCES:
        manifest_markers = manifest_markers_for("x86", source_path)
        for case in ktest_cases(source_path):
            if case not in manifest_markers:
                failures.append(
                    f"x86 KTEST inventory: {source_path} missing {case!r}"
                )
                missing_count += 1
    return missing_count


def check_arch_ktest_inventory(failures: list[str]) -> int:
    missing_count = 0
    for arch, source_paths in ARCH_KTEST_SOURCES.items():
        for source_path in source_paths:
            manifest_markers = manifest_markers_for(arch, source_path)
            for case in ktest_cases(source_path):
                if case not in manifest_markers:
                    failures.append(
                        f"{arch} KTEST inventory: {source_path} missing {case!r}"
                    )
                    missing_count += 1
    return missing_count


def main() -> int:
    outputs: dict[tuple[str, str], str] = {}
    failures: list[str] = []
    missing_inventory = check_x86_ktest_inventory(failures)
    missing_arch_inventory = check_arch_ktest_inventory(failures)

    for intent in INTENTS:
        for arch in ARCHES:
            commands = intent.commands.get(arch, ())
            if not commands:
                failures.append(f"{intent.name}: missing {arch} coverage")
                continue

            key = (arch, intent.target)
            if key not in outputs:
                outputs[key] = make_output(arch, intent.target)

            for command in commands:
                if not command_is_covered(command, outputs[key]):
                    failures.append(
                        f"{intent.name}: {arch} {intent.target} missing {command!r}"
                    )

            for source in intent.sources.get(arch, ()):
                for marker in source.markers:
                    if not source_marker_is_covered(source.path, marker):
                        failures.append(
                            f"{intent.name}: {arch} source {source.path} "
                            f"missing {marker!r}"
                        )

    if failures:
        print("test intent coverage is not architecture-complete:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print(f"x86-only KTEST inventory missing count: {missing_inventory}")
    print(f"arch KTEST inventory missing count: {missing_arch_inventory}")
    print("test intent coverage is architecture-complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
