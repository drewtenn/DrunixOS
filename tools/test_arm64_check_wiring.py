#!/usr/bin/env python3
"""Compatibility wrapper for the generic check-wiring test."""

from __future__ import annotations

import sys

from test_check_wiring import main


if __name__ == "__main__":
    sys.argv = [sys.argv[0], "--arch", "arm64"]
    raise SystemExit(main())
