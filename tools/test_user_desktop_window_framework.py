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
        "render_window_by_app(",
        "window_at_pointer(",
        "close_window_app(",
        "minimize_window_app(",
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

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
