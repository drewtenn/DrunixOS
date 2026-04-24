#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN = {
    ROOT / "kernel/kernel.c": [
        r"\bvfs_open_file\s*\(\s*DRUNIX_INIT_PROGRAM",
        r"\bprocess_create_file\s*\(",
        r"\bdesktop_attach_shell_pid\s*\(",
    ],
    ROOT / "kernel/proc/init_launch.c": [
        r"\bstatic\s+const\s+char\s+\*argv\s*\[",
        r"\bstatic\s+const\s+char\s+\*envp\s*\[",
    ],
    ROOT / "kernel/arch/arm64/start_kernel.c": [
        r"\barm64_user_smoke_boot\s*\(\s*\)",
    ],
    ROOT / "kernel/proc/process.c": [
        r"\bbuild_user_stack_frame\s*\(",
    ],
}

REQUIRED = {
    ROOT / "kernel/kernel.c": [
        r"\bboot_launch_init_process\s*\(",
        r"\bBOOT_LAUNCH_INIT_ATTACH_DESKTOP\b",
    ],
    ROOT / "kernel/proc/init_launch.h": [
        r"\bboot_launch_init_process\s*\(\s*const char \*[^,]+,\s*const char \*[^,]+,\s*const char \*[^,]+,\s*boot_launch_init_mode_t\s+\w+\s*\)",
        r"\bBOOT_LAUNCH_INIT_ATTACH_DESKTOP\b",
    ],
    ROOT / "kernel/proc/init_launch.c": [
        r"\bboot_launch_init_process\s*\(\s*const char \*[^,]+,\s*const char \*[^,]+,\s*const char \*[^,]+,\s*boot_launch_init_mode_t\s+\w+\s*\)",
        r"\bvfs_open_file\s*\(",
        r"\bprocess_create_file\s*\(",
        r"\bdesktop_attach_shell_pid\s*\(",
        r"\bdesktop_render\s*\(",
    ],
    ROOT / "kernel/arch/arch.h": [
        r"\barch_process_build_user_stack\b",
    ],
    ROOT / "kernel/arch/arm64/rootfs.c": [
        r"\barm64_rootfs_register\b",
    ],
    ROOT / "kernel/arch/arm64/arch.mk": [
        r"\brootfs_blob\.o\b",
        r"\barm64init\.elf\b",
        r"\barm64-root\.fs\b",
    ],
}

_COMMENT_RE = re.compile(r"/\*.*?\*/|//.*?$", re.DOTALL | re.MULTILINE)
_STACK_LOCAL_PROCESS_RE = re.compile(
    r"(?m)^\s*(?!static\b)process_t\s+[A-Za-z_]\w*(?:\s*=\s*[^;]+)?;"
)


def strip_comments(text):
    return _COMMENT_RE.sub("", text)


def read_source(path):
    try:
        return strip_comments(path.read_text())
    except FileNotFoundError:
        print(f"missing: {path.relative_to(ROOT)}", file=sys.stderr)
        raise SystemExit(1)


def extract_function_body(text, name):
    anchor = re.search(rf"\b{name}\s*\(", text)
    if not anchor:
        return None
    start = text.find("{", anchor.end())
    if start == -1:
        return None
    idx = start + 1
    depth = 1
    while idx < len(text):
        ch = text[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:idx]
        idx += 1
    return None


def check_init_launch_body():
    path = ROOT / "kernel/proc/init_launch.c"
    text = read_source(path)
    body = extract_function_body(text, "boot_launch_init_process")
    if body is None:
        print(f"missing: {path.relative_to(ROOT)} boot_launch_init_process body", file=sys.stderr)
        raise SystemExit(1)
    if _STACK_LOCAL_PROCESS_RE.search(body):
        print(
            f"forbidden: {path.relative_to(ROOT)} stack-local process_t in boot_launch_init_process",
            file=sys.stderr,
        )
        raise SystemExit(1)


def check(table, predicate, label):
    for path, patterns in table.items():
        text = read_source(path)
        for pattern in patterns:
            if predicate(re.search(pattern, text)):
                print(f"{label}: {path.relative_to(ROOT)} {pattern}", file=sys.stderr)
                raise SystemExit(1)


check(REQUIRED, lambda m: not m, "missing")
check(FORBIDDEN, bool, "forbidden")
check_init_launch_body()
print("phase7 boundary guard passed")
