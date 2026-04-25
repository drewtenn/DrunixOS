#!/usr/bin/env python3
"""
Generate a compile_commands.json database for Drunix C analysis targets.

The normal build is Make-based and cross-compiled, so this keeps scanner
metadata in the repo instead of requiring developers to wrap a build with Bear.
"""

import argparse
import json
import shlex
from pathlib import Path


def split_words(value):
    return [word for word in value.split() if word]


def object_to_source(obj):
    if obj.endswith(".arm64.o"):
        return obj[:-8] + ".c"
    return obj[:-2] + ".c" if obj.endswith(".o") else obj


def add_command(commands, root, compiler, flags, source, output, extra_flags=""):
    source_path = root / source
    if not source_path.exists():
        return

    parts = [compiler]
    parts.extend(split_words(flags))
    parts.extend(split_words(extra_flags))
    parts.extend(["-c", source, "-o", output])

    commands.append(
        {
            "directory": str(root),
            "file": str(source_path),
            "command": " ".join(shlex.quote(part) for part in parts),
        }
    )


def build_commands(
    *,
    root,
    kernel_objs,
    kernel_cc,
    kernel_cflags,
    kernel_inc,
    user_cc,
    user_cflags,
    linux_cc,
    linux_cflags,
    user_c_runtime_objs,
    user_c_progs,
    linux_c_progs,
):
    commands = []

    for obj in kernel_objs:
        source = object_to_source(obj)
        add_command(commands, root, kernel_cc, kernel_cflags, source, obj, kernel_inc)

    for obj in user_c_runtime_objs:
        source = "user/" + object_to_source(obj)
        output = "user/" + obj
        add_command(commands, root, user_cc, user_cflags, source, output)

    for prog in user_c_progs:
        source = f"user/{prog}.c"
        output = f"user/{prog}.o"
        add_command(commands, root, user_cc, user_cflags, source, output)

    for prog in linux_c_progs:
        source = f"user/{prog}.c"
        output = f"user/{prog}"
        add_command(commands, root, linux_cc, linux_cflags, source, output)

    commands.sort(key=lambda entry: entry["file"])
    return commands


def parser():
    p = argparse.ArgumentParser()
    p.add_argument("--root", default=".")
    p.add_argument("--output", default="compile_commands.json")
    p.add_argument("--kernel-objs", default="")
    p.add_argument("--kernel-cc", required=True)
    p.add_argument("--kernel-cflags", default="")
    p.add_argument("--kernel-inc", default="")
    p.add_argument("--user-cc", required=True)
    p.add_argument("--user-cflags", default="")
    p.add_argument("--linux-cc", required=True)
    p.add_argument("--linux-cflags", default="")
    p.add_argument("--user-c-runtime-objs", default="")
    p.add_argument("--user-c-progs", default="")
    p.add_argument("--linux-c-progs", default="")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    root = Path(args.root).resolve()
    output = Path(args.output)
    if not output.is_absolute():
        output = root / output

    commands = build_commands(
        root=root,
        kernel_objs=split_words(args.kernel_objs),
        kernel_cc=args.kernel_cc,
        kernel_cflags=args.kernel_cflags,
        kernel_inc=args.kernel_inc,
        user_cc=args.user_cc,
        user_cflags=args.user_cflags,
        linux_cc=args.linux_cc,
        linux_cflags=args.linux_cflags,
        user_c_runtime_objs=split_words(args.user_c_runtime_objs),
        user_c_progs=split_words(args.user_c_progs),
        linux_c_progs=split_words(args.linux_c_progs),
    )

    output.write_text(json.dumps(commands, indent=2) + "\n")
    print(f"wrote {output} with {len(commands)} commands")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
