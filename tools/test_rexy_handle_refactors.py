#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "user" / "apps"


def require_markers(path: Path, markers: tuple[str, ...]) -> list[str]:
    text = path.read_text()
    return [marker for marker in markers if marker not in text]


def main() -> int:
    checks = {
        "cpphello.rx": (
            "vec<i32>",
            "slice<i32>",
            "vec_i32_new",
            "vec_i32_push",
            "slice_i32_len",
            "slice_i32_get_or",
        ),
        "sleep.rx": (
            "Result<i32>",
            "result_i32_ok",
            "result_i32_err",
            "result_i32_is_err",
            "result_i32_value_or",
        ),
        "which.rx": ("std::path::join",),
        "env.rx": ("std::path::join",),
        "lsblk.rx": ("std::path::join",),
    }

    failures: list[str] = []
    for relative, markers in checks.items():
        missing = require_markers(APPS / relative, markers)
        if missing:
            failures.append(f"user/apps/{relative} missing: {', '.join(missing)}")

    if failures:
        print("\n".join(failures))
        return 1

    print("Rexy user app handle refactors present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
