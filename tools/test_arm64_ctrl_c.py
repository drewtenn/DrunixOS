#!/usr/bin/env python3
"""Compatibility wrapper for the shared Ctrl-C regression."""

from __future__ import annotations

import sys

from test_ctrl_c import main


if __name__ == "__main__":
    sys.argv = [sys.argv[0], "--arch", "arm64"]
    raise SystemExit(main())
