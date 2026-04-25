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

TASK5_REQUIRED = {
    ROOT / "kernel/arch/arm64/rootfs.c": [
        r"\barm64_rootfs_register\b",
        r"\barm64_rootfs_read_sector\b",
        r"\blba\s*>=\s*sectors\b",
        r"\bblkdev_register_disk\b",
    ],
    ROOT / "kernel/arch/arm64/rootfs.h": [
        r"\barm64_rootfs_register\s*\(",
    ],
    ROOT / "kernel/arch/arm64/rootfs_blob.S": [
        r"\barm64_rootfs_start\b",
        r"\barm64_rootfs_end\b",
        r'\.incbin\s+"build/arm64-root\.fs"',
    ],
    ROOT / "user/arm64init.c": [
        r"\barm64_sys_write\s*\(",
        r"ARM64 init: entered\\n",
        r"ARM64 init: pass\\n",
    ],
    ROOT / "user/lib/crt0_arm64.S": [
        r"\b_start\b",
        r"\b__drunix_run_constructors\b",
        r"\b__drunix_run_destructors\b",
        r"\bbl\s+sys_exit\b",
    ],
    ROOT / "user/lib/syscall_arm64.c": [
        r"\barm64_sys_write\s*\(",
        # The svc trampolines used to live here; they now live in
        # user/lib/syscall_arm64_asm.h and are static-inline so each
        # caller still emits the same code in-place.
        r'#include\s+"syscall_arm64_asm\.h"',
    ],
    ROOT / "user/lib/syscall_arm64_asm.h": [
        r"\bregister\s+long\s+x8\b",
        r"\bsvc\s+#0\b",
    ],
    ROOT / "user/lib/syscall_arm64.h": [
        r"\barm64_sys_write\s*\(",
    ],
    ROOT / "kernel/arch/arm64/arch.mk": [
        r"\brootfs\.o\b",
        r"\brootfs_blob\.o\b",
        r"\barm64init\.elf\b",
        r"\barm64-root\.fs\b",
    ],
    ROOT / "Makefile": [
        r"(?m)^build:\s+kernel-arm64\.elf\s+kernel8\.img\s+build/arm64-root\.fs\s+\$\(ARM_COMPILE_ONLY_OBJS\)",
    ],
}

TASK6_REQUIRED = {
    ROOT / "kernel/arch/arm64/start_kernel.c": [
        r"\barm64_rootfs_register\s*\(",
        r"\bvfs_reset\s*\(",
        r"\bdufs_register\s*\(",
        r'\bvfs_mount_with_source\s*\(\s*"/"\s*,\s*"dufs"\s*,\s*"/dev/sda1"\s*\)',
        r"\bboot_launch_init_process\s*\(",
        r"\bBOOT_LAUNCH_INIT_STANDALONE\b",
        r"\bDRUNIX_ARM64_SMOKE_FALLBACK\b",
        r"\bsched_bootstrap\s*\(",
        r"\barch_process_launch\s*\(",
    ],
    ROOT / "kernel/arch/arm64/proc/smoke.c": [
        r"\barm64_report_init_exit\s*\(",
        r"\b__wrap_syscall_case_exit_exit_group\s*\(",
    ],
    ROOT / "Makefile": [
        r"(?m)^ARM64_SMOKE_FALLBACK \?= 0$",
        r"-DDRUNIX_ARM64_SMOKE_FALLBACK=",
        r"(?ms)^ifeq \(\$\(ARCH\),arm64\).*?^INIT_PROGRAM \?= bin/shell$",
        r"(?ms)^ifeq \(\$\(ARCH\),arm64\).*?^INIT_ARG0 \?= shell$",
        r"(?ms)^ifeq \(\$\(ARCH\),arm64\).*?^ROOT_FS \?= dufs$",
        r"--wrap=syscall_case_exit_exit_group",
    ],
}

ARM64_SHARED_RUNTIME_OBJS = [
    "kernel/proc/syscall.arm64.o",
    "kernel/proc/syscall/helpers.arm64.o",
    "kernel/proc/syscall/console.arm64.o",
    "kernel/proc/syscall/task.arm64.o",
    "kernel/proc/syscall/tty.arm64.o",
    "kernel/proc/syscall/fd.arm64.o",
    "kernel/proc/syscall/fd_control.arm64.o",
    "kernel/proc/syscall/vfs/open.arm64.o",
    "kernel/proc/syscall/vfs/path.arm64.o",
    "kernel/proc/syscall/vfs/stat.arm64.o",
    "kernel/proc/syscall/vfs/dirents.arm64.o",
    "kernel/proc/syscall/vfs/mutation.arm64.o",
    "kernel/proc/syscall/time.arm64.o",
    "kernel/proc/syscall/info.arm64.o",
    "kernel/proc/syscall/mem.arm64.o",
    "kernel/proc/syscall/process.arm64.o",
    "kernel/proc/syscall/signal.arm64.o",
]

ARM64_COMPILE_ONLY_OBJS = []

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


def check_late_phase7_boundaries():
    rootfs_path = ROOT / "kernel/arch/arm64/rootfs.c"
    start_path = ROOT / "kernel/arch/arm64/start_kernel.c"

    if not rootfs_path.exists():
        return

    check(LATE_REQUIRED, lambda m: not m, "missing")
    start_text = read_source(start_path)
    if not re.search(r"\bboot_launch_init_process\s*\(", start_text):
        return
    fallback_blocks = (
        r"(?ms)#\s*if\s+DRUNIX_ARM64_SMOKE_FALLBACK\b"
        r".*?\barm64_user_smoke_boot\s*\(\s*\).*?"
        r"#\s*endif"
    )
    unguarded_text = re.sub(fallback_blocks, "", start_text)
    if re.search(r"\barm64_user_smoke_boot\s*\(\s*\)", unguarded_text):
        print(
            f"forbidden: {start_path.relative_to(ROOT)} unguarded arm64_user_smoke_boot",
            file=sys.stderr,
        )
        raise SystemExit(1)


def extract_make_variable(text, name):
    lines = text.splitlines()
    values = []
    for idx, line in enumerate(lines):
        match = re.match(rf"^{name}\s*(?::=|\+=)", line)
        if not match:
            continue
        parts = [re.split(r"(?::=|\+=)", line, maxsplit=1)[1]]
        while parts[-1].rstrip().endswith("\\") and idx + 1 < len(lines):
            idx += 1
            parts.append(lines[idx])
        values.append("\n".join(parts))
    return "\n".join(values)


def check_arm64_shared_runtime_linkage():
    path = ROOT / "kernel/arch/arm64/arch.mk"
    text = read_source(path)
    shared = extract_make_variable(text, "ARM_SHARED_KOBJS")
    compile_only = extract_make_variable(text, "ARM_COMPILE_ONLY_OBJS")

    for obj in ARM64_SHARED_RUNTIME_OBJS:
        if obj not in shared:
            print(f"missing: {path.relative_to(ROOT)} ARM_SHARED_KOBJS {obj}", file=sys.stderr)
            raise SystemExit(1)
        if obj in compile_only:
            print(
                f"forbidden: {path.relative_to(ROOT)} ARM_COMPILE_ONLY_OBJS {obj}",
                file=sys.stderr,
            )
            raise SystemExit(1)
    for obj in ARM64_COMPILE_ONLY_OBJS:
        if obj not in compile_only:
            print(
                f"missing: {path.relative_to(ROOT)} ARM_COMPILE_ONLY_OBJS {obj}",
                file=sys.stderr,
            )
            raise SystemExit(1)

    makefile = ROOT / "Makefile"
    make_text = read_source(makefile)
    if not re.search(
        r"(?m)^build:\s+kernel-arm64\.elf\s+kernel8\.img\s+"
        r"(?:build/arm64-root\.fs\s+)?\$\(ARM_COMPILE_ONLY_OBJS\)",
        make_text,
    ):
        print(
            f"missing: {makefile.relative_to(ROOT)} ARM64 build compile-only dependency",
            file=sys.stderr,
        )
        raise SystemExit(1)


check(REQUIRED, lambda m: not m, "missing")
check(FORBIDDEN, bool, "forbidden")
check_init_launch_body()
check_process_create_file_body()
check_arm64_user_stack_body()
check_arm64_shared_runtime_linkage()
check(TASK5_REQUIRED, lambda m: not m, "missing")
check(TASK6_REQUIRED, lambda m: not m, "missing")
check_late_phase7_boundaries()
print("phase7 boundary guard passed")
