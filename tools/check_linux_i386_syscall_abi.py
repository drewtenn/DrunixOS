#!/usr/bin/env python3
"""Verify that Drunix's public syscall ABI follows Linux i386 numbering."""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
KERNEL_HEADER = ROOT / "kernel" / "proc" / "syscall.h"
USER_SYSCALL_C = ROOT / "user" / "lib" / "syscall.c"


EXPECTED_NUMBERS = {
    "SYS_EXIT": 1,
    "SYS_FORK": 2,
    "SYS_READ": 3,
    "SYS_WRITE": 4,
    "SYS_OPEN": 5,
    "SYS_CLOSE": 6,
    "SYS_WAITPID": 7,
    "SYS_CREAT": 8,
    "SYS_CHMOD": 15,
    "SYS_LCHOWN": 16,
    "SYS_UNLINK": 10,
    "SYS_EXECVE": 11,
    "SYS_CHDIR": 12,
    "SYS_ACCESS": 33,
    "SYS_SYNC": 36,
    "SYS_LSEEK": 19,
    "SYS_GETPID": 20,
    "SYS_KILL": 37,
    "SYS_RENAME": 38,
    "SYS_MKDIR": 39,
    "SYS_RMDIR": 40,
    "SYS_DUP": 41,
    "SYS_PIPE": 42,
    "SYS_BRK": 45,
    "SYS_IOCTL": 54,
    "SYS_SETPGID": 57,
    "SYS_UMASK": 60,
    "SYS_DUP2": 63,
    "SYS_GETPPID": 64,
    "SYS_SETSID": 66,
    "SYS_SIGACTION": 67,
    "SYS_GETTIMEOFDAY": 78,
    "SYS_READLINK": 85,
    "SYS_MMAP": 90,
    "SYS_MUNMAP": 91,
    "SYS_GETPRIORITY": 96,
    "SYS_SETPRIORITY": 97,
    "SYS_STAT": 106,
    "SYS_LSTAT64": 107,
    "SYS_WAIT4": 114,
    "SYS_SYSINFO": 116,
    "SYS_UNAME": 122,
    "SYS_CLONE": 120,
    "SYS_SIGRETURN": 119,
    "SYS_MPROTECT": 125,
    "SYS_SIGPROCMASK": 126,
    "SYS_GETPGID": 132,
    "SYS__LLSEEK": 140,
    "SYS_GETDENTS": 141,
    "SYS__NEWSELECT": 142,
    "SYS_READV": 145,
    "SYS_WRITEV": 146,
    "SYS_GETSID": 147,
    "SYS_YIELD": 158,
    "SYS_NANOSLEEP": 162,
    "SYS_POLL": 168,
    "SYS_RT_SIGACTION": 174,
    "SYS_RT_SIGPROCMASK": 175,
    "SYS_GETCWD": 183,
    "SYS_VFORK": 190,
    "SYS_MMAP2": 192,
    "SYS_TRUNCATE64": 193,
    "SYS_FTRUNCATE64": 194,
    "SYS_STAT64": 195,
    "SYS_FSTAT64": 197,
    "SYS_GETUID32": 199,
    "SYS_GETGID32": 200,
    "SYS_GETEUID32": 201,
    "SYS_GETEGID32": 202,
    "SYS_SETUID32": 213,
    "SYS_CHOWN32": 212,
    "SYS_SETGID32": 214,
    "SYS_GETDENTS64": 220,
    "SYS_FCNTL64": 221,
    "SYS_GETTID": 224,
    "SYS_SENDFILE64": 239,
    "SYS_SCHED_GETAFFINITY": 242,
    "SYS_SET_THREAD_AREA": 243,
    "SYS_EXIT_GROUP": 252,
    "SYS_SET_TID_ADDRESS": 258,
    "SYS_CLOCK_GETTIME": 265,
    "SYS_STATFS64": 268,
    "SYS_FSTATFS64": 269,
    "SYS_OPENAT": 295,
    "SYS_MKDIRAT": 296,
    "SYS_FCHOWNAT": 298,
    "SYS_FUTIMESAT": 299,
    "SYS_FSTATAT64": 300,
    "SYS_UNLINKAT": 301,
    "SYS_RENAMEAT": 302,
    "SYS_LINKAT": 303,
    "SYS_SYMLINKAT": 304,
    "SYS_READLINKAT": 305,
    "SYS_FCHMODAT": 306,
    "SYS_FACCESSAT": 307,
    "SYS_UTIMENSAT": 320,
    "SYS_STATX": 383,
    "SYS_CLOCK_GETTIME64": 403,
}

PRIVATE_NUMBERS = {
    "SYS_DRUNIX_CLEAR",
    "SYS_DRUNIX_SCROLL_UP",
    "SYS_DRUNIX_SCROLL_DOWN",
    "SYS_DRUNIX_MODLOAD",
    "SYS_DRUNIX_GETDENTS_PATH",
}


def fail(message: str) -> None:
    print(f"check_linux_i386_syscall_abi: {message}", file=sys.stderr)
    sys.exit(1)


def parse_defines(text: str) -> dict[str, int]:
    values: dict[str, int] = {}
    define_re = re.compile(r"^#define\s+(SYS_[A-Z0-9_]+)\s+([0-9]+)u?\s*$")
    for line in text.splitlines():
        match = define_re.match(line.strip())
        if match:
            values[match.group(1)] = int(match.group(2))
    return values


def function_body(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", text)
    if not match:
        fail(f"missing user wrapper {name}()")
    start = match.end()
    depth = 1
    pos = start
    while pos < len(text) and depth:
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
        pos += 1
    if depth != 0:
        fail(f"could not parse body for {name}()")
    return text[start : pos - 1]


def require(pattern: str, text: str, message: str) -> None:
    if not re.search(pattern, text, flags=re.S):
        fail(message)


def main() -> int:
    header = KERNEL_HEADER.read_text()
    user_syscall = USER_SYSCALL_C.read_text()
    defines = parse_defines(header)

    for name, expected in EXPECTED_NUMBERS.items():
        actual = defines.get(name)
        if actual != expected:
            fail(f"{name} must be Linux i386 syscall {expected}, got {actual}")

    for name in ("SYS_FWRITE", "SYS_CREATE", "SYS_EXEC", "SYS_WAIT", "SYS_SLEEP"):
        if name in defines:
            fail(f"{name} is a Drunix-private spelling; use the Linux i386 public name instead")

    for name in PRIVATE_NUMBERS:
        actual = defines.get(name)
        if actual is None:
            fail(f"missing private syscall {name}")
        if actual < 4000:
            fail(f"{name} must live in the Drunix-private syscall range >= 4000")

    write_body = function_body(user_syscall, "sys_fwrite")
    require(r'"a"\(4\)', write_body, "sys_fwrite() must call Linux SYS_write number 4")
    require(r'"b"\(fd\)', write_body, "sys_fwrite() must pass fd in EBX")
    require(r'"c"\(buf\)', write_body, "sys_fwrite() must pass buf in ECX")
    require(r'"d"\(count\)', write_body, "sys_fwrite() must pass count in EDX")

    sys_write_n_body = function_body(user_syscall, "sys_write_n")
    require(r"sys_fwrite\s*\(\s*1\s*,\s*buf\s*,\s*count\s*\)",
            sys_write_n_body,
            "sys_write_n() must route through fd 1 using sys_fwrite()")

    execve_body = function_body(user_syscall, "sys_execve")
    require(r'"a"\(11\)', execve_body, "sys_execve() must call Linux SYS_execve number 11")
    require(r'"b"\(filename\)', execve_body, "sys_execve() must pass path in EBX")
    require(r'"c"\(argv\)', execve_body, "sys_execve() must pass argv in ECX")
    require(r'"d"\(envp\)', execve_body, "sys_execve() must pass envp in EDX")
    if '"S"(envp)' in execve_body or '"D"(envc)' in execve_body:
        fail("sys_execve() must not pass Drunix-only argc/envc registers")

    print("Linux i386 syscall ABI guard passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
