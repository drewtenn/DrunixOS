#!/usr/bin/env python3

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
USER = ROOT / "user"
APPS = USER / "apps"
RUNTIME = USER / "runtime"
THIRD_PARTY = USER / "third_party"
LINKER = USER / "linker"
MAKEFILE = USER / "Makefile"
PROGRAMS_MK = USER / "programs.mk"
ROOT_MAKEFILE = ROOT / "Makefile"


def make_var(text, name):
    match = re.search(rf"^{re.escape(name)}[ \t]*(?:\?|:)?=[ \t]*(.*)$", text, re.MULTILINE)
    if not match:
        return None
    return match.group(1).split()


def scalar_make_var(text, name):
    match = re.search(rf"^{re.escape(name)}[ \t]*(?:\?|:)?=[ \t]*(.*)$", text, re.MULTILINE)
    if not match:
        return None
    return match.group(1).strip()


def add_failure(failures, message):
    failures.append(message)


def main():
    text = PROGRAMS_MK.read_text() + "\n" + MAKEFILE.read_text()
    makefile_text = MAKEFILE.read_text()
    root_text = ROOT_MAKEFILE.read_text()
    failures = []

    progs = make_var(text, "PROGS")
    c_progs = make_var(text, "C_PROGS")
    cxx_progs = make_var(text, "CXX_PROGS")
    rexc_progs = make_var(text, "REXC_PROGS")
    linux_progs = make_var(text, "LINUX_PROGS")
    c_runtime = make_var(text, "C_RUNTIME_OBJS")
    cxx_runtime = make_var(text, "CXX_RUNTIME_OBJS")
    c_link = scalar_make_var(text, "C_LINK_OBJS")
    cxx_link = scalar_make_var(text, "CXX_LINK_OBJS")

    if progs is None:
        add_failure(failures, "user/Makefile must define PROGS")
        progs = []
    if c_progs is None:
        add_failure(failures, "user/Makefile must define C_PROGS")
        c_progs = []
    if cxx_progs is None:
        add_failure(failures, "user/Makefile must define CXX_PROGS")
        cxx_progs = []
    if rexc_progs is None:
        add_failure(failures, "user/Makefile must define REXC_PROGS")
        rexc_progs = []
    if linux_progs is None:
        add_failure(failures, "user/Makefile must define LINUX_PROGS")
        linux_progs = []
    if c_runtime is None:
        add_failure(failures, "user/Makefile must define C_RUNTIME_OBJS")
        c_runtime = []
    if cxx_runtime is None:
        add_failure(failures, "user/Makefile must define CXX_RUNTIME_OBJS")
        cxx_runtime = []
    if c_link is None:
        add_failure(failures, "user/Makefile must define C_LINK_OBJS")
        c_link = ""
    if cxx_link is None:
        add_failure(failures, "user/Makefile must define CXX_LINK_OBJS")
        cxx_link = ""

    c_set = set(c_progs)
    cxx_set = set(cxx_progs)
    rexc_set = set(rexc_progs)
    linux_set = set(linux_progs)
    prog_set = set(progs)

    if len(c_progs) != len(c_set):
        add_failure(failures, "C_PROGS contains duplicate entries")
    if len(cxx_progs) != len(cxx_set):
        add_failure(failures, "CXX_PROGS contains duplicate entries")
    if len(rexc_progs) != len(rexc_set):
        add_failure(failures, "REXC_PROGS contains duplicate entries")
    if len(linux_progs) != len(linux_set):
        add_failure(failures, "LINUX_PROGS contains duplicate entries")

    overlap = sorted(
        (c_set & cxx_set)
        | (c_set & rexc_set)
        | (c_set & linux_set)
        | (cxx_set & rexc_set)
        | (cxx_set & linux_set)
        | (rexc_set & linux_set)
    )
    if overlap:
        add_failure(failures, "programs listed in more than one userland lane: " + " ".join(overlap))

    missing = sorted(prog_set - (c_set | cxx_set | rexc_set | linux_set))
    extra = sorted((c_set | cxx_set | rexc_set | linux_set) - prog_set)
    if missing:
        add_failure(failures, "programs missing from C_PROGS/CXX_PROGS/REXC_PROGS/LINUX_PROGS: " + " ".join(missing))
    if extra:
        add_failure(failures, "lane program not listed in PROGS: " + " ".join(extra))

    if "chello" not in c_set:
        add_failure(failures, "C_PROGS must include chello as the small C runtime smoke program")
    if linux_set:
        add_failure(
            failures,
            "LINUX_PROGS must stay empty; generated Linux compatibility payloads are deprecated",
        )

    if "include programs.mk" not in makefile_text:
        add_failure(failures, "user/Makefile must include user/programs.mk as the shared user program manifest")
    required_rexc_progs = {"hello", "echo", "printenv", "cat", "yes", "basename"}
    missing_rexc_progs = sorted(required_rexc_progs - rexc_set)
    if missing_rexc_progs:
        add_failure(
            failures,
            "user/programs.mk must list canonical Rexc programs in REXC_PROGS: "
            + " ".join(missing_rexc_progs),
        )
    if not re.search(r"^REXC_BUILD_DIR[ \t]*\?=[ \t]*\.\./external/rexc/build[ \t]*$", makefile_text, re.MULTILINE):
        add_failure(failures, "user/Makefile must define REXC_BUILD_DIR for the local Rexc build")
    if not re.search(r"^REXC[ \t]*\?=[ \t]*\$\(REXC_BUILD_DIR\)/rexc[ \t]*$", makefile_text, re.MULTILINE):
        add_failure(failures, "user/Makefile must define REXC as the Rexc compiler driver")
    if "cmake --build $(REXC_BUILD_DIR) --target rexc" not in makefile_text:
        add_failure(failures, "user/Makefile must know how to build the local Rexc compiler driver")
    if not re.search(r"^include\s+user/programs\.mk$", root_text, re.MULTILINE):
        add_failure(failures, "top-level Makefile must include user/programs.mk for disk image packing")
    if not re.search(r"USER_PROGS\s*:?\=\s*\$\(PROGS\)", root_text):
        add_failure(failures, "top-level Makefile must derive USER_PROGS from the shared user/programs.mk manifest")
    required_root_patterns = {
        "USER_BUILD_ROOT": r"^USER_BUILD_ROOT[ \t]*:=[ \t]*build/user/\$\(ARCH\)[ \t]*$",
        "USER_BIN_DIR": r"^USER_BIN_DIR[ \t]*:=[ \t]*\$\(USER_BUILD_ROOT\)/bin[ \t]*$",
        "USER_BINS": r"^USER_BINS[ \t]*:=[ \t]*\$\(addprefix \$\(USER_BIN_DIR\)/,\$\(USER_PROGS\)\)[ \t]*$",
        "DISK_FILES": r"^DISK_FILES[ \t]*:=[ \t]*\$\(foreach prog,\$\(USER_PROGS\),\$\(USER_BIN_DIR\)/\$\(prog\) bin/\$\(prog\)\)[ \t]*$",
    }
    for label, pattern in required_root_patterns.items():
        if not re.search(pattern, root_text, re.MULTILINE):
            add_failure(failures, f"top-level Makefile must define {label} for build/user/<arch> artifacts")

    required_user_patterns = {
        "USER_ARCH": r"^USER_ARCH[ \t]*\?=[ \t]*x86[ \t]*$",
        "BUILD_ROOT": r"^BUILD_ROOT[ \t]*\?=[ \t]*\.\./build/user/\$\(USER_ARCH\)[ \t]*$",
        "BIN_DIR": r"^BIN_DIR[ \t]*:=[ \t]*\$\(BUILD_ROOT\)/bin[ \t]*$",
        "OBJ_DIR": r"^OBJ_DIR[ \t]*:=[ \t]*\$\(BUILD_ROOT\)/obj[ \t]*$",
        "RUNTIME_DIR": r"^RUNTIME_DIR[ \t]*:=[ \t]*\$\(BUILD_ROOT\)/runtime[ \t]*$",
        "LINKER_DIR": r"^LINKER_DIR[ \t]*:=[ \t]*\$\(BUILD_ROOT\)/linker[ \t]*$",
    }
    for label, pattern in required_user_patterns.items():
        if not re.search(pattern, makefile_text, re.MULTILINE):
            add_failure(failures, f"user/Makefile must define {label} for out-of-tree artifacts")
    forbidden_gcc_layout = {
        "user/gcc usr/bin/gcc",
        "user/gcc usr/bin/i686-linux-musl-gcc",
        "user/gcc-cc1 usr/libexec/gcc/i686-linux-musl/11.2.1/cc1",
        "user/gcc-as usr/bin/as",
        "user/gcc-as i686-linux-musl/bin/as",
        "user/gcc bin/gcc",
        "user/gcc bin/i686-linux-musl-gcc",
        "user/gcc-cc1 bin/cc1",
        "user/gcc-as bin/as",
        "user/gcc-as bin/i686-linux-musl-as",
        "user/gcc-as usr/i686-linux-musl/bin/as",
    }
    for entry in forbidden_gcc_layout:
        if entry in root_text:
            add_failure(failures, f"top-level Makefile must not stage GCC tool as {entry}")

    if "$(C_RUNTIME_OBJS)" not in c_link:
        add_failure(failures, "C_LINK_OBJS must include $(C_RUNTIME_OBJS)")
    if "$(C_RUNTIME_OBJS)" not in cxx_link or "$(CXX_RUNTIME_OBJS)" not in cxx_link:
        add_failure(failures, "CXX_LINK_OBJS must include both C and C++ runtime objects")

    required_dirs = {
        "user/apps": APPS,
        "user/runtime": RUNTIME,
        "user/third_party": THIRD_PARTY,
        "user/linker": LINKER,
    }
    for label, path in required_dirs.items():
        if not path.is_dir():
            add_failure(failures, f"{label} directory is required")

    if not (LINKER / "user.ld.in").exists():
        add_failure(failures, "linker template must live at user/linker/user.ld.in")
    if (USER / "user.ld.in").exists():
        add_failure(failures, "legacy flat linker template must not remain under user")

    for prog in c_progs:
        if not (APPS / f"{prog}.c").exists():
            add_failure(failures, f"C program missing C source: user/apps/{prog}.c")
        if (APPS / f"{prog}.cpp").exists():
            add_failure(failures, f"C program must not also have C++ source: user/apps/{prog}.cpp")

    for prog in cxx_progs:
        if not (APPS / f"{prog}.cpp").exists():
            add_failure(failures, f"C++ program missing C++ source: user/apps/{prog}.cpp")
        if (APPS / f"{prog}.c").exists():
            add_failure(failures, f"C++ program must not also have C source: user/apps/{prog}.c")

    for prog in rexc_progs:
        if not (APPS / f"{prog}.rx").exists():
            add_failure(failures, f"Rexc program missing Rexc source: user/apps/{prog}.rx")
        if (APPS / f"{prog}.c").exists():
            add_failure(failures, f"Rexc program must not also have C source: user/apps/{prog}.c")
        if (APPS / f"{prog}.cpp").exists():
            add_failure(failures, f"Rexc program must not also have C++ source: user/apps/{prog}.cpp")

    for prog in linux_progs:
        if (APPS / f"{prog}.cpp").exists():
            add_failure(failures, f"Linux i386 smoke program must not use Drunix C++ runtime sources: user/apps/{prog}.cpp")

    if failures:
        for failure in failures:
            print(failure)
        return 1

    print("native userland runtime lanes are explicit and complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
