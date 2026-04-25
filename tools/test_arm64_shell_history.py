#!/usr/bin/env python3
"""Compatibility wrapper for the shared shell-history regression."""

from __future__ import annotations

import sys

from test_shell_history import main


if __name__ == "__main__":
    sys.argv = [sys.argv[0], "--arch", "arm64"]
    raise SystemExit(main())
