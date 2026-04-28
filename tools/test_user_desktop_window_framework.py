#!/usr/bin/env python3
"""
Require the user-space desktop to render and hit-test app windows through one
shared framework instead of one-off per-app titlebar code.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DESKTOP = ROOT / "user" / "apps" / "desktop.c"


def main() -> int:
    source = DESKTOP.read_text()
    required = (
        "render_window_frame(",
        "window_at_pointer(",
        "wm_server_connect(",
        "drain_wm_server_messages(",
        "render_client_window(",
        "send_client_window_event(",
        "launch_taskbar_app(",
        "taskbar_window_for_app(",
        "client_window_capacity_available(",
        "focused_client_window(",
        "clear_focus_if_hidden(",
    )
    missing = [name for name in required if name not in source]
    if missing:
        print("desktop window framework is missing shared helpers:")
        for name in missing:
            print(f"  {name}")
        return 1

    direct_chrome = source.count("draw_title_button(")
    if direct_chrome > 3:
        print("window titlebar buttons must be drawn only by the shared frame")
        return 1
    if "sys_close(g_wm_fd)" not in source:
        print("desktop child processes must close inherited wm server fds")
        return 1
    if "return launch_taskbar_app(app) == 0;" in source:
        print("taskbar clicks must restore/focus existing client windows before launch")
        return 1
    if "send_client_window_event(msg->window" not in source:
        print("desktop must close client windows it cannot map or track")
        return 1
    if "send_pointer_event_to_client(focused" in source or (
        "buttons != old_buttons || sdx != 0 || sdy != 0" in source
        and "window_at_pointer(g_pointer_x, g_pointer_y)" not in source
    ):
        print("desktop must route pointer motion to the window under the pointer")
        return 1
    if "sys_waitpid(pid, WNOHANG)" in source:
        print("desktop must reap launched apps using the waitpid return pid")
        return 1
    if "if (g_focused_window)\n\t\t\t\t\t\t\tsend_client_window_event" in source:
        print("desktop must not send keys to hidden or minimized focused windows")
        return 1

    forbidden = (
        "render_app_content(",
        "render_app_window(",
        "start_terminal_session(",
        "g_grid[",
        "DRUNIX_TASKBAR_APP_FILES]",
        "DRUNIX_TASKBAR_APP_PROCESSES]",
        "DRUNIX_TASKBAR_APP_HELP]",
    )
    present = [name for name in forbidden if name in source]
    if present:
        print("desktop still contains built-in app window paths:")
        for name in present:
            print(f"  {name}")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
