#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(
        description="List compile_commands.json source files under a directory."
    )
    parser.add_argument("compile_commands", type=Path)
    parser.add_argument("--under", default="", help="repository-relative prefix")
    args = parser.parse_args()

    root = args.compile_commands.resolve().parent
    prefix = args.under.strip("/")
    commands = json.loads(args.compile_commands.read_text())
    seen = set()

    for entry in commands:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = (Path(entry.get("directory", root)) / source).resolve()
        try:
            rel = source.relative_to(root).as_posix()
        except ValueError:
            rel = source.as_posix()
        if prefix and not (rel == prefix or rel.startswith(prefix + "/")):
            continue
        if rel in seen:
            continue
        seen.add(rel)
        print(rel)


if __name__ == "__main__":
    main()
