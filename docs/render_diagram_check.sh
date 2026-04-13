#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: $0 docs/diagrams/chNN-diagNN.svg [...]" >&2
  exit 1
fi

if ! command -v rsvg-convert >/dev/null 2>&1; then
  echo "error: rsvg-convert not found in PATH" >&2
  exit 1
fi

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/diagram-check.XXXXXX")"

for svg in "$@"; do
  if [ ! -f "$svg" ]; then
    echo "error: missing file: $svg" >&2
    exit 1
  fi
  name="$(basename "${svg%.svg}")"
  png="${tmpdir}/${name}.png"
  rsvg-convert "$svg" -o "$png"
  printf '%s\n' "$png"
done
