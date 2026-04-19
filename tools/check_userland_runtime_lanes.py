#!/usr/bin/env python3

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
USER = ROOT / "user"
MAKEFILE = USER / "Makefile"
PROGRAMS_MK = USER / "programs.mk"
ROOT_MAKEFILE = ROOT / "Makefile"


def make_var(text, name):
    match = re.search(rf"^{re.escape(name)}\s*=\s*(.+)$", text, re.MULTILINE)
    if not match:
        return None
    return match.group(1).split()


def scalar_make_var(text, name):
    match = re.search(rf"^{re.escape(name)}\s*=\s*(.+)$", text, re.MULTILINE)
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
    linux_progs = make_var(text, "LINUX_PROGS")
    linux_binutils_progs = make_var(text, "LINUX_BINUTILS_PROGS")
    linux_gcc_progs = make_var(text, "LINUX_GCC_PROGS")
    linux_gcc_helper_progs = make_var(text, "LINUX_GCC_HELPER_PROGS")
    linux_nano_progs = make_var(text, "LINUX_NANO_PROGS")
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
    if linux_progs is None:
        add_failure(failures, "user/Makefile must define LINUX_PROGS")
        linux_progs = []
    if linux_binutils_progs is None:
        add_failure(failures, "user/Makefile must define LINUX_BINUTILS_PROGS")
        linux_binutils_progs = []
    if linux_gcc_progs is None:
        add_failure(failures, "user/Makefile must define LINUX_GCC_PROGS")
        linux_gcc_progs = []
    if linux_gcc_helper_progs is None:
        add_failure(failures, "user/Makefile must define LINUX_GCC_HELPER_PROGS")
        linux_gcc_helper_progs = []
    if linux_nano_progs is None:
        add_failure(failures, "user/Makefile must define LINUX_NANO_PROGS")
        linux_nano_progs = []
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
    linux_set = set(linux_progs)
    prog_set = set(progs)

    if len(c_progs) != len(c_set):
        add_failure(failures, "C_PROGS contains duplicate entries")
    if len(cxx_progs) != len(cxx_set):
        add_failure(failures, "CXX_PROGS contains duplicate entries")
    if len(linux_progs) != len(linux_set):
        add_failure(failures, "LINUX_PROGS contains duplicate entries")

    overlap = sorted((c_set & cxx_set) | (c_set & linux_set) | (cxx_set & linux_set))
    if overlap:
        add_failure(failures, "programs listed in more than one userland lane: " + " ".join(overlap))

    missing = sorted(prog_set - (c_set | cxx_set | linux_set))
    extra = sorted((c_set | cxx_set | linux_set) - prog_set)
    if missing:
        add_failure(failures, "programs missing from C_PROGS/CXX_PROGS/LINUX_PROGS: " + " ".join(missing))
    if extra:
        add_failure(failures, "lane program not listed in PROGS: " + " ".join(extra))

    if "chello" not in c_set:
        add_failure(failures, "C_PROGS must include chello as the small C runtime smoke program")
    if "linuxhello" not in linux_set:
        add_failure(failures, "LINUX_PROGS must include linuxhello as the static Linux i386 smoke program")
    if "linuxprobe" not in linux_set:
        add_failure(failures, "LINUX_PROGS must include linuxprobe as the static Linux i386 C compatibility probe")
    if "busybox" not in linux_set:
        add_failure(failures, "LINUX_PROGS must include busybox as the generated static Linux i386 userland target")
    if "tcc" not in linux_set:
        add_failure(failures, "LINUX_PROGS must include tcc as the generated static Linux i386 compiler target")
    if "readelf" not in linux_set or "objdump" not in linux_set:
        add_failure(failures, "LINUX_PROGS must include readelf and objdump as generated static Linux i386 binutils targets")
    if set(linux_binutils_progs) != {"readelf", "objdump"}:
        add_failure(failures, "LINUX_BINUTILS_PROGS must be the generated binutils targets: readelf objdump")
    if "gcc" in linux_set:
        add_failure(failures, "LINUX_PROGS must not install gcc through the generic /bin program lane")
    if set(linux_gcc_progs) != {"gcc"}:
        add_failure(failures, "LINUX_GCC_PROGS must be the generated GCC driver target: gcc")
    if set(linux_gcc_helper_progs) != {"gcc-cc1", "gcc-as"}:
        add_failure(failures, "LINUX_GCC_HELPER_PROGS must include the staged GCC helpers: gcc-cc1 gcc-as")
    if "nano" not in linux_set:
        add_failure(failures, "LINUX_PROGS must include nano as the generated static Linux i386 editor target")
    if set(linux_nano_progs) != {"nano"}:
        add_failure(failures, "LINUX_NANO_PROGS must be the generated nano target: nano")

    if "include programs.mk" not in makefile_text:
        add_failure(failures, "user/Makefile must include user/programs.mk as the shared user program manifest")
    if not re.search(r"^\$\(LINUX_BINUTILS_PROGS\):\n\t\.\./tools/build_linux_binutils\.sh \$@ \$\(BINUTILS_VERSION\) \$\(BINUTILS_BUILD_DIR\)", makefile_text, re.MULTILINE):
        add_failure(failures, "user/Makefile must build $(LINUX_BINUTILS_PROGS) with tools/build_linux_binutils.sh")
    if not re.search(r"^\$\(LINUX_GCC_PROGS\):\n\t\.\./tools/build_linux_gcc_tool\.sh \$@ gcc \$\(GCC_TOOLCHAIN_VERSION\) \$\(GCC_TOOLCHAIN_BUILD_DIR\)", makefile_text, re.MULTILINE):
        add_failure(failures, "user/Makefile must build $(LINUX_GCC_PROGS) with tools/build_linux_gcc_tool.sh")
    if not re.search(r"^\$\(LINUX_NANO_PROGS\):\n\t\.\./tools/build_linux_nano\.sh \$@ \$\(NANO_VERSION\) \$\(NANO_BUILD_DIR\)", makefile_text, re.MULTILINE):
        add_failure(failures, "user/Makefile must build $(LINUX_NANO_PROGS) with tools/build_linux_nano.sh")
    if not re.search(r"^include\s+user/programs\.mk$", root_text, re.MULTILINE):
        add_failure(failures, "top-level Makefile must include user/programs.mk for disk image packing")
    if not re.search(r"USER_PROGS\s*:?\=\s*\$\(PROGS\)", root_text):
        add_failure(failures, "top-level Makefile must derive USER_PROGS from the shared user/programs.mk manifest")
    required_gcc_layout = {
        "user/gcc usr/bin/gcc",
        "user/gcc usr/bin/i686-linux-musl-gcc",
        "user/gcc-cc1 usr/libexec/gcc/i686-linux-musl/11.2.1/cc1",
        "user/gcc-as usr/bin/as",
        "user/gcc-as i686-linux-musl/bin/as",
    }
    for entry in required_gcc_layout:
        if entry not in root_text:
            add_failure(failures, f"top-level Makefile must stage GCC tool as {entry}")
    forbidden_gcc_layout = {
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

    for prog in c_progs:
        if not (USER / f"{prog}.c").exists():
            add_failure(failures, f"C program missing C source: user/{prog}.c")
        if (USER / f"{prog}.cpp").exists():
            add_failure(failures, f"C program must not also have C++ source: user/{prog}.cpp")

    for prog in cxx_progs:
        if not (USER / f"{prog}.cpp").exists():
            add_failure(failures, f"C++ program missing C++ source: user/{prog}.cpp")
        if (USER / f"{prog}.c").exists():
            add_failure(failures, f"C++ program must not also have C source: user/{prog}.c")

    linux_c_progs = {"linuxprobe", "linuxabi"}
    linux_generated_progs = {"busybox", "tcc", "readelf", "objdump", "nano"}
    for prog in linux_progs:
        has_asm = (USER / f"{prog}.asm").exists()
        has_c = (USER / f"{prog}.c").exists()
        if prog in linux_generated_progs:
            if has_asm or has_c:
                add_failure(failures, f"Generated Linux i386 program must not have local source: user/{prog}")
        elif prog in linux_c_progs:
            if not has_c:
                add_failure(failures, f"Linux i386 C probe missing source: user/{prog}.c")
            if has_asm:
                add_failure(failures, f"Linux i386 C probe must not also have assembly source: user/{prog}.asm")
        else:
            if not has_asm:
                add_failure(failures, f"Linux i386 smoke program missing assembly source: user/{prog}.asm")
            if has_c:
                add_failure(failures, f"Linux i386 assembly smoke program must not also have C source: user/{prog}.c")
        if (USER / f"{prog}.cpp").exists():
            add_failure(failures, f"Linux i386 smoke program must not use Drunix C++ runtime sources: user/{prog}.cpp")

    if failures:
        for failure in failures:
            print(failure)
        return 1

    print("userland C, C++, and Linux i386 runtime lanes are explicit and complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
