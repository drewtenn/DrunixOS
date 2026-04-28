#!/usr/bin/env python3
"""Policy checks for the user-space window runtime API wiring."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def contains_all(path: Path, needles: tuple[str, ...]) -> list[str]:
    text = path.read_text()
    return [needle for needle in needles if needle not in text]


def contains_function_flow(source: str, name: str, needles: tuple[str, ...]) -> bool:
    match = re.search(rf"{re.escape(name)}\([^)]*\)\s*\{{(?P<body>.*?)\n\}}", source, re.S)
    if not match:
        return False
    body = match.group("body")
    return all(needle in body for needle in needles)


def main() -> int:
    required_files = (
        ROOT / "user" / "runtime" / "drwin.h",
        ROOT / "user" / "runtime" / "drwin.c",
        ROOT / "user" / "runtime" / "drwin_gfx.h",
        ROOT / "user" / "runtime" / "drwin_gfx.c",
    )
    missing_files = [path.relative_to(ROOT) for path in required_files if not path.exists()]
    if missing_files:
        print("user window runtime is missing files:")
        for path in missing_files:
            print(f"  {path}")
        return 1

    x86_missing = contains_all(
        ROOT / "user" / "Makefile",
        (
            "$(RUNTIME_OBJ_DIR)/drwin.o",
            "$(RUNTIME_OBJ_DIR)/drwin_gfx.o",
        ),
    )
    arm64_missing = contains_all(
        ROOT / "kernel" / "arch" / "arm64" / "arch.mk",
        (
            "$(ARM_USER_RUNTIME_OBJ_DIR)/drwin.o",
            "$(ARM_USER_RUNTIME_OBJ_DIR)/drwin_gfx.o",
        ),
    )

    if x86_missing:
        print("x86 user runtime is missing objects:")
        for obj in x86_missing:
            print(f"  {obj}")
        return 1

    if arm64_missing:
        print("arm64 user runtime is missing objects:")
        for obj in arm64_missing:
            print(f"  {obj}")
        return 1

    drwin_source = (ROOT / "user" / "runtime" / "drwin.c").read_text()
    gfx_source = (ROOT / "user" / "runtime" / "drwin_gfx.c").read_text()
    fd_control = (ROOT / "kernel" / "proc" / "syscall" / "fd_control.c").read_text()
    x86_syscall = (ROOT / "kernel" / "arch" / "x86" / "proc" / "syscall.c").read_text()
    arm64_nr = (ROOT / "user" / "runtime" / "syscall_arm64_nr.h").read_text()
    arm64_compat = (ROOT / "user" / "runtime" / "syscall_arm64_compat.c").read_text()
    arm64_kernel = (ROOT / "kernel" / "arch" / "arm64" / "proc" / "syscall.c").read_text()
    user_makefile = (ROOT / "user" / "Makefile").read_text()
    arm64_makefile = (ROOT / "kernel" / "arch" / "arm64" / "arch.mk").read_text()
    programs = (ROOT / "user" / "programs.mk").read_text()
    x86_process_tests = (
        ROOT / "kernel" / "arch" / "x86" / "test" / "test_process.c"
    ).read_text()

    behavior_failures = []
    if "$(RUNTIME_OBJ_DIR)/desktop_font.o" not in user_makefile:
        behavior_failures.append("x86 runtime must link the shared bitmap font")
    if "$(ARM_USER_RUNTIME_OBJ_DIR)/desktop_font.o" not in arm64_makefile:
        behavior_failures.append("arm64 runtime must link the shared bitmap font")
    if "$(APP_OBJ_DIR)/desktop_font.o" in user_makefile:
        behavior_failures.append("desktop must not be the only app linked with the font")
    if "$(ARM_USER_APP_OBJ_DIR)/desktop_font.o" in arm64_makefile:
        behavior_failures.append("arm64 desktop must not be the only app linked with the font")
    if not contains_function_flow(
        drwin_source, "unmap_surface", ("sys_munmap", "mapped_length")
    ):
        behavior_failures.append("drwin_destroy_window must unmap mapped surfaces")
    if not contains_function_flow(
        drwin_source, "request_destroy_window", ("DRWIN_REQ_DESTROY_WINDOW", "write_all")
    ):
        behavior_failures.append("drwin_destroy_window must send a destroy request")
    if not contains_function_flow(
        drwin_source, "drwin_destroy_window", ("request_destroy_window", "forget_surface")
    ):
        behavior_failures.append("drwin_destroy_window must call the surface cleanup path")
    if not contains_function_flow(
        drwin_source, "drwin_create_window", ("has_surface_capacity", "request_destroy_window")
    ):
        behavior_failures.append("drwin_create_window must avoid leaking untracked server windows")
    if not contains_function_flow(
        gfx_source, "drwin_fill_rect", ("int64_t x0", "int64_t y0")
    ):
        behavior_failures.append("drwin_gfx clipping must use wide arithmetic")
    if not contains_function_flow(
        gfx_source, "valid_surface", ("min_pitch", "surface->pitch")
    ):
        behavior_failures.append("drwin_gfx must validate pitch >= width * 4")
    if "uint32_t fg" not in gfx_source or "uint32_t bg" not in gfx_source:
        behavior_failures.append("drwin_draw_text must accept foreground and background colors")
    if "1u << gx" not in gfx_source:
        behavior_failures.append("drwin_gfx must match the shared font bit orientation")
    if (
        "syscall_case_poll(uint32_t ebx, uint32_t ecx, uint32_t edx)" not in fd_control
        or "timeout_ms = (int32_t)edx" not in fd_control
        or "poll_wait_until" not in fd_control
    ):
        behavior_failures.append("kernel poll must accept the timeout argument")
    if "FD_TYPE_PTY_MASTER" not in fd_control or "pty_master_read_available" not in fd_control:
        behavior_failures.append("kernel poll must report pty master/slave readability")
    if "syscall_case_poll(ebx, ecx, edx)" not in x86_syscall:
        behavior_failures.append("x86 syscall dispatcher must pass poll timeout")
    if "ARM64_SYS_PPOLL" not in arm64_nr:
        behavior_failures.append("arm64 runtime must expose ppoll syscall number")
    if "ARM64_SYS_PPOLL" not in arm64_compat or "syscall5" not in arm64_compat:
        behavior_failures.append("arm64 sys_poll must call ppoll instead of stubbing")
    if "ARM64_LINUX_SYS_PPOLL" not in arm64_kernel:
        behavior_failures.append("arm64 kernel syscall dispatcher must handle ppoll")
    if "if (!(pfd.revents & SYS_POLLIN))\n\t\treturn -1;" not in drwin_source:
        behavior_failures.append("drwin_poll_event must report non-input poll readiness as an error")
    if "sigmask != 0" not in arm64_kernel or "sigsetsize != 0" not in arm64_kernel:
        behavior_failures.append("arm64 ppoll must reject unsupported signal masks")
    if "test_linux_poll_timeout_modes" not in x86_process_tests:
        behavior_failures.append("x86 poll timeout modes must have kernel coverage")
    if "SYS_POLL, 0x10100100u, 1u, 25u" not in x86_process_tests:
        behavior_failures.append("x86 poll tests must cover finite timeout dispatch")
    if "(uint32_t)-1" not in x86_process_tests:
        behavior_failures.append("x86 poll tests must cover infinite timeout dispatch")
    for app_name in ("terminal", "files", "processes", "help"):
        app = ROOT / "user" / "apps" / f"{app_name}.c"

        if not app.exists():
            behavior_failures.append(f"user/apps/{app_name}.c client app is required")
            continue
        if app_name not in programs:
            behavior_failures.append(f"user/programs.mk must include {app_name}")
        if "drwin_create_window" not in app.read_text():
            behavior_failures.append(f"{app_name} must create a drwin window")
    terminal_source = (ROOT / "user" / "apps" / "terminal.c").read_text()
    if "sys_open_flags(\"/dev/pts0\"" in terminal_source:
        behavior_failures.append("terminal must discover the allocated pty slave")
    if "sys_close(g_ptmx)" not in terminal_source or "sys_close(wm_fd)" not in terminal_source:
        behavior_failures.append("terminal child must close inherited wm and pty master fds")
    if "if (n == 0)\n\t\t\treturn -1;" not in terminal_source:
        behavior_failures.append("terminal must exit its event loop when the pty reaches EOF")
    if "sys_waitpid(g_shell_pid, 0)" not in terminal_source:
        behavior_failures.append("terminal must reap its shell child")
    if 'sys_execve("/bin/shell"' not in terminal_source:
        behavior_failures.append("terminal must exec the shell with an absolute path")
    if "event_byte" not in terminal_source or "event->value >= 32" in terminal_source:
        behavior_failures.append("terminal must preserve non-NUL key bytes for the pty")
    if behavior_failures:
        print("user window runtime behavior is incomplete:")
        for failure in behavior_failures:
            print(f"  {failure}")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
