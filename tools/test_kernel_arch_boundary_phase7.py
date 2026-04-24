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
    ROOT / "kernel/proc/process.c": [
        r"\bbuild_user_stack_frame\s*\(",
    ],
}

LATE_FORBIDDEN = {
    ROOT / "kernel/arch/arm64/start_kernel.c": [
        r"\barm64_user_smoke_boot\s*\(\s*\)",
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
    ROOT / "kernel/arch/arm64/proc/arch_proc.c": [
        r"\barch_process_build_user_stack\b",
    ],
}

LATE_REQUIRED = {
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
    r"""
    (?ms)
    (?:^|;)
    \s*
    (?!static\b)
    (?:
        (?:const|volatile|register|restrict)\s+
    )*
    process_t
    \b
    (?!\s*\*)
    [^;]*;
    """,
    re.VERBOSE,
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


def check_process_create_file_body():
    path = ROOT / "kernel/proc/process.c"
    text = read_source(path)
    body = extract_function_body(text, "process_create_file")
    if body is None:
        print(f"missing: {path.relative_to(ROOT)} process_create_file body", file=sys.stderr)
        raise SystemExit(1)

    expected = (
        r"uintptr_t\s+initial_stack\s*=\s*USER_STACK_TOP;"
        r"[\s\S]*?\barch_process_build_user_stack\s*\(\s*aspace,\s*argv,\s*argc,\s*envp,\s*envc,\s*&initial_stack\s*\)"
        r"[\s\S]*?\bproc->user_stack\s*=\s*\(uint32_t\)initial_stack;"
    )
    if not re.search(expected, body):
        print(
            f"missing: {path.relative_to(ROOT)} process_create_file delegation contract",
            file=sys.stderr,
        )
        raise SystemExit(1)


def check_arm64_user_stack_body():
    path = ROOT / "kernel/arch/arm64/proc/arch_proc.c"
    text = read_source(path)
    body = extract_function_body(text, "arch_process_build_user_stack")
    if body is None:
        print(f"missing: {path.relative_to(ROOT)} arch_process_build_user_stack body", file=sys.stderr)
        raise SystemExit(1)

    patterns = [
        r"\buint64_t\s+uargv_ptrs\s*\[PROCESS_ARGV_MAX_COUNT\];",
        r"\buint64_t\s+uenv_ptrs\s*\[PROCESS_ENV_MAX_COUNT\];",
        r"\buint64_t\s+\*\s*tail_k;",
        r"\bstack_qwords\s*=\s*1u\s*\+\s*\(uint32_t\)argc\s*\+\s*1u\s*\+\s*\(uint32_t\)envc\s*\+\s*1u\s*\+\s*4u;",
        r"\bpad\s*=\s*\(16u\s*-\s*\(frame_off\s*&\s*15u\)\)\s*&\s*15u;",
        (
            r"tail_k\[idx\+\+\]\s*=\s*LINUX_AT_PAGESZ;"
            r"[\s\S]*?tail_k\[idx\+\+\]\s*=\s*PAGE_SIZE;"
            r"[\s\S]*?tail_k\[idx\+\+\]\s*=\s*LINUX_AT_NULL;"
            r"[\s\S]*?tail_k\[idx\+\+\]\s*=\s*0;"
        ),
    ]
    for pattern in patterns:
        if not re.search(pattern, body):
            print(
                f"missing: {path.relative_to(ROOT)} arch64 user-stack trait {pattern}",
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
check_process_create_file_body()
check_arm64_user_stack_body()
check(LATE_REQUIRED, lambda m: not m, "missing")
check(LATE_FORBIDDEN, bool, "forbidden")
print("phase7 boundary guard passed")
