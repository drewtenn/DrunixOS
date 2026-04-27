# Userland Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move userland sources into role-based directories and make x86 and arm64 emit userland artifacts under `build/user/<arch>/`.

**Architecture:** Keep `user/programs.mk` as the shared program manifest. Compile apps from `user/apps`, runtime code from `user/runtime`, vendored code from `user/third_party`, and generated linker scripts from `build/user/<arch>/linker/user.ld`. Package root filesystems from `build/user/<arch>/bin/<program>` while preserving guest paths such as `/bin/shell`.

**Tech Stack:** GNU Make, Python policy tests, x86 and arm64 cross toolchains, existing Drunix user runtime.

---

## File Structure

- Move app sources to `user/apps/`: all current top-level user programs, including `arm64init.c`, `desktop.c`, `desktop_font.c`, `desktop_font.h`, `linuxabi.c`, `linuxhello.asm`, and `linuxprobe.c`.
- Move runtime sources to `user/runtime/`: all current `user/lib/*` files except vendored nanojpeg.
- Move vendored nanojpeg to `user/third_party/nanojpeg/`.
- Move linker template to `user/linker/user.ld.in`.
- Modify `user/Makefile` to build x86 outputs under `../build/user/x86`.
- Modify `kernel/arch/arm64/arch.mk` to build arm64 user outputs under `build/user/arm64`.
- Modify root `Makefile` to package and debug from `build/user/<arch>/bin`.
- Modify `tools/check_userland_runtime_lanes.py` and `tools/generate_compile_commands.py` for the new source and artifact paths.
- Modify tests that assert old paths: `tools/test_generate_compile_commands.py`, `tools/test_arm64_dev_loop_parity.py`, `tools/test_kernel_arch_boundary_phase7.py`, `tools/check_busybox_compat_harness.py`, and `tools/test_intent_manifest.py`.
- Modify docs that name moved paths: `docs/ch21-libc.md`, `docs/ch30-cpp-userland.md`, `docs/linux-elf-compat.md`, and `docs/contributing/syscalls.md`.
- Simplify `.gitignore` by removing explicit ignored `user/<program>` and generated linker-script entries after artifacts live under `build/`.

### Task 1: Add Layout Policy Checks

**Files:**
- Modify: `tools/check_userland_runtime_lanes.py`
- Test: `tools/check_userland_runtime_lanes.py`

- [ ] **Step 1: Replace hard-coded source checks with the new layout expectations**

In `tools/check_userland_runtime_lanes.py`, add these constants after `USER = ROOT / "user"`:

```python
APPS = USER / "apps"
RUNTIME = USER / "runtime"
THIRD_PARTY = USER / "third_party"
LINKER = USER / "linker"
```

Then replace the source existence loops with:

```python
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
        add_failure(failures, "linker template must not remain at user/user.ld.in")

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

    for prog in linux_progs:
        if (APPS / f"{prog}.cpp").exists():
            add_failure(failures, f"Linux i386 smoke program must not use Drunix C++ runtime sources: user/apps/{prog}.cpp")
```

- [ ] **Step 2: Add Makefile structure assertions**

In the same file, add these checks after the `USER_PROGS` assertion:

```python
    required_root_patterns = {
        "USER_BUILD_ROOT": r"USER_BUILD_ROOT\s*:?\=\s*build/user/\$\(ARCH\)",
        "USER_BIN_DIR": r"USER_BIN_DIR\s*:?\=\s*\$\(USER_BUILD_ROOT\)/bin",
        "USER_BINS": r"USER_BINS\s*:?\=\s*\$\(addprefix \$\(USER_BIN_DIR\)/,\$\(USER_PROGS\)\)",
        "DISK_FILES": r"DISK_FILES\s*:?\=\s*\$\(foreach prog,\$\(USER_PROGS\),\$\(USER_BIN_DIR\)/\$\(prog\) bin/\$\(prog\)\)",
    }
    for label, pattern in required_root_patterns.items():
        if not re.search(pattern, root_text):
            add_failure(failures, f"top-level Makefile must define {label} for build/user/<arch> artifacts")

    required_user_patterns = {
        "USER_ARCH": r"USER_ARCH\s*\?=\s*x86",
        "BUILD_ROOT": r"BUILD_ROOT\s*\?=\s*\.\./build/user/\$\(USER_ARCH\)",
        "BIN_DIR": r"BIN_DIR\s*:?\=\s*\$\(BUILD_ROOT\)/bin",
        "OBJ_DIR": r"OBJ_DIR\s*:?\=\s*\$\(BUILD_ROOT\)/obj",
        "RUNTIME_DIR": r"RUNTIME_DIR\s*:?\=\s*\$\(BUILD_ROOT\)/runtime",
        "LINKER_DIR": r"LINKER_DIR\s*:?\=\s*\$\(BUILD_ROOT\)/linker",
    }
    for label, pattern in required_user_patterns.items():
        if not re.search(pattern, makefile_text):
            add_failure(failures, f"user/Makefile must define {label} for out-of-tree artifacts")
```

- [ ] **Step 3: Run the policy check and confirm it fails**

Run:

```bash
python3 tools/check_userland_runtime_lanes.py
```

Expected: FAIL with messages including `user/apps directory is required` and `top-level Makefile must define USER_BUILD_ROOT for build/user/<arch> artifacts`.

- [ ] **Step 4: Commit the failing policy update**

Run:

```bash
git add tools/check_userland_runtime_lanes.py
git commit -m "test: require organized userland layout"
```

### Task 2: Move Userland Sources

**Files:**
- Move: `user/*.c`, `user/*.cpp`, `user/*.asm`, `user/desktop_font.h`, `user/lib/*`, `user/user.ld.in`
- Modify: source comments in moved files
- Test: `tools/check_userland_runtime_lanes.py`

- [ ] **Step 1: Create the new directories**

Run:

```bash
mkdir -p user/apps user/runtime user/third_party user/linker
```

- [ ] **Step 2: Move app sources**

Run:

```bash
git mv user/*.c user/apps/
git mv user/*.cpp user/apps/
git mv user/*.asm user/apps/
git mv user/desktop_font.h user/apps/
```

- [ ] **Step 3: Move runtime sources and linker template**

Run:

```bash
git mv user/lib/nanojpeg user/third_party/nanojpeg
git mv user/lib/* user/runtime/
rmdir user/lib
git mv user/user.ld.in user/linker/user.ld.in
```

- [ ] **Step 4: Update comments inside moved runtime files**

Run:

```bash
perl -pi -e 's|user/lib/|user/runtime/|g; s|user/([A-Za-z0-9_+-]+)\.(c|cpp|asm)|user/apps/$1.$2|g; s|user/user\.ld|build/user/<arch>/linker/user.ld|g' user/apps/* user/runtime/* user/linker/user.ld.in
```

- [ ] **Step 5: Run the policy check and confirm only build-system failures remain**

Run:

```bash
python3 tools/check_userland_runtime_lanes.py
```

Expected: FAIL. The previous missing-directory and missing-source errors are gone. Remaining failures mention Makefile build path expectations.

- [ ] **Step 6: Commit the source move**

Run:

```bash
git add user
git commit -m "refactor: group userland sources by role"
```

### Task 3: Normalize x86 Userland Build Output

**Files:**
- Modify: `user/Makefile`
- Modify: `.gitignore`
- Test: `tools/check_userland_runtime_lanes.py`

- [ ] **Step 1: Replace `user/Makefile` with an out-of-tree x86 build**

Replace `user/Makefile` with:

```make
include programs.mk

USER_ARCH ?= x86
BUILD_ROOT ?= ../build/user/$(USER_ARCH)
BIN_DIR := $(BUILD_ROOT)/bin
OBJ_DIR := $(BUILD_ROOT)/obj
RUNTIME_DIR := $(BUILD_ROOT)/runtime
LINKER_DIR := $(BUILD_ROOT)/linker
APP_SRC_DIR := apps
RUNTIME_SRC_DIR := runtime
THIRD_PARTY_SRC_DIR := third_party
LINKER_SRC_DIR := linker

APP_OBJ_DIR := $(OBJ_DIR)/apps
RUNTIME_OBJ_DIR := $(OBJ_DIR)/runtime
THIRD_PARTY_OBJ_DIR := $(OBJ_DIR)/third_party
NANOJPEG_OBJ_DIR := $(THIRD_PARTY_OBJ_DIR)/nanojpeg

CLEAN_PROGS = $(addprefix $(BIN_DIR)/,$(PROGS))

all: $(CLEAN_PROGS)

print-progs:
	@printf '%s\n' $(PROGS)

CC     = x86_64-elf-gcc
CXX    = x86_64-elf-g++
LD     = x86_64-elf-ld
AR     = x86_64-elf-ar
NASM   = nasm
CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
         -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall \
         -Werror -fdebug-prefix-map=$(abspath ..)=.
CXXFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
           -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall \
           -Werror -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
           -fno-threadsafe-statics -fdebug-prefix-map=$(abspath ..)=.
NASMFLAGS = -Werror
CXXLIBS = $(shell $(CXX) -m32 -print-libgcc-file-name)
USER_INCLUDES = -I $(RUNTIME_SRC_DIR) -I ../shared -I $(APP_SRC_DIR)

C_RUNTIME_OBJS = $(RUNTIME_OBJ_DIR)/crt0.o $(RUNTIME_OBJ_DIR)/cxx_init.o $(RUNTIME_OBJ_DIR)/syscall.o $(RUNTIME_OBJ_DIR)/malloc.o \
                 $(RUNTIME_OBJ_DIR)/string.o $(RUNTIME_OBJ_DIR)/ctype.o $(RUNTIME_OBJ_DIR)/stdlib.o $(RUNTIME_OBJ_DIR)/stdio.o \
                 $(RUNTIME_OBJ_DIR)/unistd.o $(RUNTIME_OBJ_DIR)/time.o
C_RUNTIME_LIB_OBJS = $(filter-out $(RUNTIME_OBJ_DIR)/crt0.o,$(C_RUNTIME_OBJS))
CXX_RUNTIME_OBJS = $(RUNTIME_OBJ_DIR)/cxxrt.o $(RUNTIME_OBJ_DIR)/cxxabi.o
C_LINK_OBJS = $(C_RUNTIME_OBJS)
CXX_LINK_OBJS = $(C_RUNTIME_OBJS) $(CXX_RUNTIME_OBJS)

C_BINS = $(addprefix $(BIN_DIR)/,$(C_PROGS))
CXX_BINS = $(addprefix $(BIN_DIR)/,$(CXX_PROGS))

$(LINKER_DIR)/user.ld: $(LINKER_SRC_DIR)/user.ld.in
	@mkdir -p $(dir $@)
	sed 's|@USER_LOAD_ADDR@|$(USER_LOAD_ADDR)|' $< > $@

$(RUNTIME_OBJ_DIR)/crt0.o: $(RUNTIME_SRC_DIR)/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -f elf32 -o $@ $<

$(RUNTIME_OBJ_DIR)/%.o: $(RUNTIME_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(RUNTIME_SRC_DIR) -c -o $@ $<

$(RUNTIME_OBJ_DIR)/%.o: $(RUNTIME_SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I $(RUNTIME_SRC_DIR) -c -o $@ $<

$(APP_OBJ_DIR)/%.o: $(APP_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(USER_INCLUDES) -c -o $@ $<

$(APP_OBJ_DIR)/%.o: $(APP_SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(USER_INCLUDES) -c -o $@ $<

$(APP_OBJ_DIR)/%.o: $(APP_SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -f elf32 -o $@ $<

$(RUNTIME_DIR)/libc.a: $(C_RUNTIME_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(C_RUNTIME_LIB_OBJS)

$(C_BINS): $(BIN_DIR)/%: $(APP_OBJ_DIR)/%.o $(C_LINK_OBJS) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LINKER_DIR)/user.ld -o $@ $(C_LINK_OBJS) $(APP_OBJ_DIR)/$*.o

$(CXX_BINS): $(BIN_DIR)/%: $(APP_OBJ_DIR)/%.o $(CXX_LINK_OBJS) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LINKER_DIR)/user.ld -o $@ $(CXX_LINK_OBJS) $(APP_OBJ_DIR)/$*.o $(CXXLIBS)

DESKTOP_NANOJPEG_OBJS = $(NANOJPEG_OBJ_DIR)/nanojpeg.o $(NANOJPEG_OBJ_DIR)/nanojpeg_shim.o

$(NANOJPEG_OBJ_DIR)/nanojpeg.o: $(THIRD_PARTY_SRC_DIR)/nanojpeg/nanojpeg.c $(THIRD_PARTY_SRC_DIR)/nanojpeg/nanojpeg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DNJ_USE_LIBC=0 '-DNULL=((void*)0)' -Wno-unused-function -Wno-sign-compare -Wno-misleading-indentation -Wno-unused-parameter -Wno-implicit-fallthrough -c -o $@ $<

$(NANOJPEG_OBJ_DIR)/nanojpeg_shim.o: $(THIRD_PARTY_SRC_DIR)/nanojpeg/nanojpeg_shim.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(RUNTIME_SRC_DIR) -c -o $@ $<

$(APP_OBJ_DIR)/desktop_kbdmap.o: ../shared/kbdmap.c ../shared/kbdmap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I../shared -c -o $@ $<

$(BIN_DIR)/desktop: $(APP_OBJ_DIR)/desktop.o $(APP_OBJ_DIR)/desktop_font.o $(APP_OBJ_DIR)/desktop_kbdmap.o $(DESKTOP_NANOJPEG_OBJS) $(C_LINK_OBJS) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LINKER_DIR)/user.ld -o $@ $(C_LINK_OBJS) $(APP_OBJ_DIR)/desktop.o $(APP_OBJ_DIR)/desktop_font.o $(APP_OBJ_DIR)/desktop_kbdmap.o $(DESKTOP_NANOJPEG_OBJS)

clean:
	rm -rf $(BUILD_ROOT)
```

- [ ] **Step 2: Keep x86 linker address configurable from the root make**

In `user/Makefile`, add this default near `USER_ARCH`:

```make
USER_LOAD_ADDR ?= 0x08000000
```

- [ ] **Step 3: Simplify `.gitignore` userland entries**

Remove explicit ignored entries for `user/<program>`, `user/<program>.o`, `user/lib/libc.a`, `/user/user.ld`, and `/user/user_arm64.ld`. Keep `/build/` because it now covers the generated userland artifacts.

- [ ] **Step 4: Run the policy check**

Run:

```bash
python3 tools/check_userland_runtime_lanes.py
```

Expected: FAIL only for root `Makefile` path expectations because the top-level build still points at `user/<program>`.

- [ ] **Step 5: Commit the x86 user makefile changes**

Run:

```bash
git add user/Makefile .gitignore
git commit -m "build: move x86 user artifacts out of source tree"
```

### Task 4: Update Root Makefile For Shared User Artifact Paths

**Files:**
- Modify: `Makefile`
- Test: `tools/check_userland_runtime_lanes.py`

- [ ] **Step 1: Replace root user artifact variables**

In `Makefile`, replace:

```make
USER_PROGS    := $(PROGS)
USER_BINS     := $(addprefix user/,$(USER_PROGS))
DISK_FILES    := $(foreach prog,$(USER_PROGS),user/$(prog) bin/$(prog)) \
```

with:

```make
USER_PROGS    := $(PROGS)
USER_BUILD_ROOT := build/user/$(ARCH)
USER_BIN_DIR  := $(USER_BUILD_ROOT)/bin
USER_BINS     := $(addprefix $(USER_BIN_DIR)/,$(USER_PROGS))
DISK_FILES    := $(foreach prog,$(USER_PROGS),$(USER_BIN_DIR)/$(prog) bin/$(prog)) \
```

- [ ] **Step 2: Replace x86 user build delegation**

Replace:

```make
.PHONY: $(USER_BINS)
$(USER_BINS): user/user.ld
	$(MAKE) -C user $(@F)

user/user.ld: user/user.ld.in Makefile
	sed 's|@USER_LOAD_ADDR@|$(X86_USER_LOAD_ADDR)|' $< > $@

user/user_arm64.ld: user/user.ld.in Makefile
	sed 's|@USER_LOAD_ADDR@|0x02000000|' $< > $@

user/lib/libc.a user/lib/tcc_crt0.o:
	$(MAKE) -C user $(@F:%=lib/%)
```

with:

```make
.PHONY: $(USER_BINS)
$(USER_BINS):
	$(MAKE) -C user USER_ARCH=$(ARCH) USER_LOAD_ADDR=$(X86_USER_LOAD_ADDR) ../$@
```

- [ ] **Step 3: Update x86 debug-user symbol path**

Replace the debug-user comment and command path:

```make
#                The binary is expected at user/$(APP); symbols are added at the
	$(call qemu_debug,-ex "add-symbol-file user/$(APP) $(X86_USER_LOAD_ADDR)")
```

with:

```make
#                The binary is expected at $(USER_BIN_DIR)/$(APP); symbols are added at the
	$(call qemu_debug,-ex "add-symbol-file $(USER_BIN_DIR)/$(APP) $(X86_USER_LOAD_ADDR)")
```

- [ ] **Step 4: Update x86 clean rules**

Replace:

```make
	$(RM) user/user.ld user/user_arm64.ld
```

with:

```make
	rm -rf build/user
```

Apply the same replacement in both x86 and arm64 `clean` targets.

- [ ] **Step 5: Update x86 compile command arguments**

In the x86 `compile_commands.json` rule, replace:

```make
		--user-cflags="-m32 -ffreestanding -nostdlib -fno-pie -no-pie -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall -Werror" \
```

with:

```make
		--user-cflags="-m32 -ffreestanding -nostdlib -fno-pie -no-pie -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall -Werror -I user/runtime -I shared -I user/apps" \
		--user-build-root="$(USER_BUILD_ROOT)" \
```

- [ ] **Step 6: Run the policy check**

Run:

```bash
python3 tools/check_userland_runtime_lanes.py
```

Expected: PASS with `native userland C and C++ runtime lanes are explicit and complete`.

- [ ] **Step 7: Commit the root Makefile x86 path changes**

Run:

```bash
git add Makefile
git commit -m "build: package user binaries from shared build root"
```

### Task 5: Normalize arm64 Userland Build Output

**Files:**
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `Makefile`
- Test: `tools/test_arm64_dev_loop_parity.py`

- [ ] **Step 1: Update arm64 user build directory variables**

In `kernel/arch/arm64/arch.mk`, replace the current `ARM_USER_BUILD_DIR` block with:

```make
ARM_USER_BUILD_DIR := build/user/arm64
ARM_USER_BIN_DIR := $(ARM_USER_BUILD_DIR)/bin
ARM_USER_OBJ_DIR := $(ARM_USER_BUILD_DIR)/obj
ARM_USER_RUNTIME_OBJ_DIR := $(ARM_USER_OBJ_DIR)/runtime
ARM_USER_APP_OBJ_DIR := $(ARM_USER_OBJ_DIR)/apps
ARM_USER_RUNTIME_DIR := $(ARM_USER_BUILD_DIR)/runtime
ARM_USER_LINKER_DIR := $(ARM_USER_BUILD_DIR)/linker
ARM_USER_LINKER := $(ARM_USER_LINKER_DIR)/user.ld
```

- [ ] **Step 2: Update arm64 object and binary lists**

Replace the runtime object and binary list definitions with:

```make
ARM_USER_C_RUNTIME_OBJS := $(ARM_USER_RUNTIME_OBJ_DIR)/crt0.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/cxx_init.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/syscall.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/malloc.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/string.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/ctype.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/stdlib.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/stdio.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/unistd.o \
                           $(ARM_USER_RUNTIME_OBJ_DIR)/time.o
ARM_USER_C_RUNTIME_LIB_OBJS := $(filter-out $(ARM_USER_RUNTIME_OBJ_DIR)/crt0.o,$(ARM_USER_C_RUNTIME_OBJS))
ARM_USER_CXX_RUNTIME_OBJS := $(ARM_USER_RUNTIME_OBJ_DIR)/cxxrt.o \
                             $(ARM_USER_RUNTIME_OBJ_DIR)/cxxabi.o
ARM_USER_C_BINS := $(addprefix $(ARM_USER_BIN_DIR)/,$(C_PROGS))
ARM_USER_CXX_BINS := $(addprefix $(ARM_USER_BIN_DIR)/,$(CXX_PROGS))
ARM_USER_NATIVE_BINS := $(ARM_USER_C_BINS) $(ARM_USER_CXX_BINS)
ARM_USER_ROOTFS_FILES := $(foreach prog,$(C_PROGS) $(CXX_PROGS),$(ARM_USER_BIN_DIR)/$(prog) bin/$(prog)) \
                         build/arm64init.elf bin/arm64init \
                         tools/hello.txt hello.txt \
                         tools/readme.txt readme.txt \
                         $(ARM_BUSYBOX_ROOTFS_FILES) \
                         $(ARM_EXTRA_ROOTFS_FILES)
```

- [ ] **Step 3: Update arm64 source paths and includes**

Replace `user/lib` with `user/runtime` in `ARM64_SYSCALL_HEADERS`, include flags, and build rules. Replace app source rules from `user/%.c` and `user/%.cpp` to `user/apps/%.c` and `user/apps/%.cpp`.

Use these patterns:

```make
ARM64_SYSCALL_HEADERS := user/runtime/syscall_arm64.h \
                         user/runtime/syscall_arm64_asm.h \
                         user/runtime/syscall_arm64_nr.h \
                         user/runtime/ustrlen.h

build/arm64init.o: user/apps/arm64init.c $(ARM64_SYSCALL_HEADERS)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -I user/runtime -c $< -o $@

build/crt0_arm64.o: user/runtime/crt0_arm64.S
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

build/syscall_arm64.o: user/runtime/syscall_arm64.c $(ARM64_SYSCALL_HEADERS)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -I user/runtime -c $< -o $@

$(ARM_USER_LINKER): user/linker/user.ld.in Makefile
	@mkdir -p $(dir $@)
	sed 's|@USER_LOAD_ADDR@|0x02000000|' $< > $@

$(ARM_USER_RUNTIME_OBJ_DIR)/crt0.o: user/runtime/crt0_arm64.S
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -c $< -o $@

$(ARM_USER_RUNTIME_OBJ_DIR)/syscall.o: user/runtime/syscall_arm64_compat.c user/runtime/syscall.h $(ARM64_SYSCALL_HEADERS)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I user/apps -I user/runtime -c $< -o $@

$(ARM_USER_RUNTIME_OBJ_DIR)/%.o: user/runtime/%.c
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I user/apps -I user/runtime -c $< -o $@

$(ARM_USER_RUNTIME_OBJ_DIR)/%.o: user/runtime/%.cpp
	@mkdir -p $(dir $@)
	$(ARM_CXX) $(ARM_USER_CXXFLAGS) -I user/apps -I user/runtime -c $< -o $@

$(ARM_USER_APP_OBJ_DIR)/%.o: user/apps/%.c
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_USER_CFLAGS) -I user/apps -I user/runtime -c $< -o $@

$(ARM_USER_APP_OBJ_DIR)/%.o: user/apps/%.cpp
	@mkdir -p $(dir $@)
	$(ARM_CXX) $(ARM_USER_CXXFLAGS) -I user/apps -I user/runtime -c $< -o $@
```

- [ ] **Step 4: Update arm64 archive and link rules**

Use:

```make
$(ARM_USER_RUNTIME_DIR)/libc.a: $(ARM_USER_C_RUNTIME_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(ARM_AR) rcs $@ $(ARM_USER_C_RUNTIME_LIB_OBJS)

$(ARM_USER_C_BINS): $(ARM_USER_BIN_DIR)/%: $(ARM_USER_APP_OBJ_DIR)/%.o $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_LINKER)
	@mkdir -p $(dir $@)
	$(ARM_LD) -nostdlib -T $(ARM_USER_LINKER) -o $@ $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_APP_OBJ_DIR)/$*.o

$(ARM_USER_CXX_BINS): $(ARM_USER_BIN_DIR)/%: $(ARM_USER_APP_OBJ_DIR)/%.o $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_CXX_RUNTIME_OBJS) $(ARM_USER_LINKER)
	@mkdir -p $(dir $@)
	$(ARM_LD) -nostdlib -T $(ARM_USER_LINKER) -o $@ $(ARM_USER_C_RUNTIME_OBJS) $(ARM_USER_CXX_RUNTIME_OBJS) $(ARM_USER_APP_OBJ_DIR)/$*.o $(ARM_USER_CXXLIBS)

build/arm64init.elf: build/crt0_arm64.o build/syscall_arm64.o build/arm64init.o $(ARM_USER_RUNTIME_OBJ_DIR)/syscall.o $(ARM_USER_RUNTIME_OBJ_DIR)/cxx_init.o $(ARM_USER_LINKER) kernel/arch/arm64/arch.mk
	$(ARM_LD) -nostdlib -e _start -T $(ARM_USER_LINKER) -o $@ build/crt0_arm64.o $(ARM_USER_RUNTIME_OBJ_DIR)/syscall.o build/syscall_arm64.o $(ARM_USER_RUNTIME_OBJ_DIR)/cxx_init.o build/arm64init.o
```

- [ ] **Step 5: Update arm64 debug-user and compile command paths**

In the arm64 `debug-user` rule in `Makefile`, replace:

```make
	$(call arm64_qemu_debug,-ex "add-symbol-file build/arm64-user/$(APP) 0x02000000")
```

with:

```make
	$(call arm64_qemu_debug,-ex "add-symbol-file $(ARM_USER_BIN_DIR)/$(APP) 0x02000000")
```

In the arm64 `compile_commands.json` rule, replace:

```make
		--user-cflags="$(ARM_USER_CFLAGS) -I user -I user/lib" \
```

with:

```make
		--user-cflags="$(ARM_USER_CFLAGS) -I user/apps -I user/runtime" \
		--user-build-root="$(ARM_USER_BUILD_DIR)" \
```

- [ ] **Step 6: Update the arm64 dev-loop parity test**

In `tools/test_arm64_dev_loop_parity.py`, replace:

```python
    if "add-symbol-file build/arm64-user/shell 0x02000000" not in debug_user_output:
```

with:

```python
    if "add-symbol-file build/user/arm64/bin/shell 0x02000000" not in debug_user_output:
```

- [ ] **Step 7: Run arm64 dry-run parity**

Run:

```bash
python3 tools/test_arm64_dev_loop_parity.py
```

Expected: PASS with `arm64 dev-loop parity check passed`.

- [ ] **Step 8: Commit arm64 normalization**

Run:

```bash
git add Makefile kernel/arch/arm64/arch.mk tools/test_arm64_dev_loop_parity.py
git commit -m "build: normalize arm64 user artifact layout"
```

### Task 6: Update Compile Commands Tooling

**Files:**
- Modify: `tools/generate_compile_commands.py`
- Modify: `tools/test_generate_compile_commands.py`
- Test: `tools/test_generate_compile_commands.py`

- [ ] **Step 1: Extend the generator API**

In `tools/generate_compile_commands.py`, add this parser argument:

```python
    p.add_argument("--user-build-root", default="build/user/x86")
```

Add `user_build_root` to `build_commands(...)` parameters:

```python
    user_build_root,
```

Pass it from `main()`:

```python
        user_build_root=args.user_build_root,
```

- [ ] **Step 2: Update user source and output path generation**

Replace the user loops in `build_commands()` with:

```python
    user_build_root = user_build_root.rstrip("/")

    for obj in user_c_runtime_objs:
        obj_path = Path(obj)
        source_name = object_to_source(obj_path.name)
        source = f"user/runtime/{source_name}"
        output = f"{user_build_root}/obj/runtime/{obj_path.stem}.o"
        add_command(commands, root, user_cc, user_cflags, source, output)

    for prog in user_c_progs:
        source = f"user/apps/{prog}.c"
        output = f"{user_build_root}/obj/apps/{prog}.o"
        add_command(commands, root, user_cc, user_cflags, source, output)

    for prog in linux_c_progs:
        source = f"user/apps/{prog}.c"
        output = f"{user_build_root}/bin/{prog}"
        add_command(commands, root, linux_cc, linux_cflags, source, output)
```

- [ ] **Step 3: Update generator tests to create new paths**

In `tools/test_generate_compile_commands.py`, replace:

```python
            (root / "user/lib").mkdir(parents=True)
            (root / "user/shell.c").write_text("int main(void) { return 0; }\n")
            (root / "user/linuxabi.c").write_text("int main(void) { return 0; }\n")
            (root / "user/lib/stdio.c").write_text("int stdio;\n")
```

with:

```python
            (root / "user/apps").mkdir(parents=True)
            (root / "user/runtime").mkdir(parents=True)
            (root / "user/apps/shell.c").write_text("int main(void) { return 0; }\n")
            (root / "user/apps/linuxabi.c").write_text("int main(void) { return 0; }\n")
            (root / "user/runtime/stdio.c").write_text("int stdio;\n")
```

Replace output assertions:

```python
            self.assertIn("-o user/shell.o", by_file["shell.c"]["command"])
            self.assertIn("-o user/lib/stdio.o", by_file["stdio.c"]["command"])
```

with:

```python
            self.assertIn("-o build/user/x86/obj/apps/shell.o", by_file["shell.c"]["command"])
            self.assertIn("-o build/user/x86/obj/runtime/stdio.o", by_file["stdio.c"]["command"])
```

- [ ] **Step 4: Run compile command tests**

Run:

```bash
python3 tools/test_generate_compile_commands.py
```

Expected: PASS.

- [ ] **Step 5: Commit compile command tooling**

Run:

```bash
git add tools/generate_compile_commands.py tools/test_generate_compile_commands.py
git commit -m "tools: emit compile commands for organized userland"
```

### Task 7: Update Path-Sensitive Tests And Documentation

**Files:**
- Modify: `tools/test_kernel_arch_boundary_phase7.py`
- Modify: `tools/check_busybox_compat_harness.py`
- Modify: `tools/test_intent_manifest.py`
- Modify: `docs/ch21-libc.md`
- Modify: `docs/ch30-cpp-userland.md`
- Modify: `docs/linux-elf-compat.md`
- Modify: `docs/contributing/syscalls.md`
- Test: listed Python checks

- [ ] **Step 1: Update arm64 boundary paths**

In `tools/test_kernel_arch_boundary_phase7.py`, replace path keys:

```python
ROOT / "user/arm64init.c"
ROOT / "user/lib/crt0_arm64.S"
ROOT / "user/lib/syscall_arm64.c"
ROOT / "user/lib/syscall_arm64_asm.h"
ROOT / "user/lib/syscall_arm64.h"
```

with:

```python
ROOT / "user/apps/arm64init.c"
ROOT / "user/runtime/crt0_arm64.S"
ROOT / "user/runtime/syscall_arm64.c"
ROOT / "user/runtime/syscall_arm64_asm.h"
ROOT / "user/runtime/syscall_arm64.h"
```

- [ ] **Step 2: Update BusyBox compatibility harness paths**

In `tools/check_busybox_compat_harness.py`, replace messages and source checks for `user/bbcompat.c` with `user/apps/bbcompat.c`:

```python
BB_COMPAT = ROOT / "user" / "apps" / "bbcompat.c"
```

Use `BB_COMPAT.exists()` and `BB_COMPAT.read_text()` instead of constructing the old path inline.

- [ ] **Step 3: Update intent manifest expected paths**

In `tools/test_intent_manifest.py`, replace string entries:

```python
"user/arm64init.c"
"user/bbcompat.c"
```

with:

```python
"user/apps/arm64init.c"
"user/apps/bbcompat.c"
```

- [ ] **Step 4: Update documentation paths**

Use these replacements:

```bash
perl -pi -e 's|user/lib/|user/runtime/|g; s|user/linuxabi\.c|user/apps/linuxabi.c|g; s|user/bbcompat\.c|user/apps/bbcompat.c|g; s|user/crt0\.S|user/runtime/crt0_arm64.S|g' docs/ch21-libc.md docs/ch30-cpp-userland.md docs/linux-elf-compat.md docs/contributing/syscalls.md
```

Then manually check `docs/ch21-libc.md` and ensure the CRT sentence names `user/runtime/crt0.asm` for x86 and `user/runtime/crt0_arm64.S` for AArch64.

- [ ] **Step 5: Run path-sensitive checks**

Run:

```bash
python3 tools/test_kernel_arch_boundary_phase7.py
python3 tools/check_busybox_compat_harness.py
python3 tools/test_intent_manifest.py
```

Expected: all PASS.

- [ ] **Step 6: Commit tests and docs**

Run:

```bash
git add tools/test_kernel_arch_boundary_phase7.py tools/check_busybox_compat_harness.py tools/test_intent_manifest.py docs/ch21-libc.md docs/ch30-cpp-userland.md docs/linux-elf-compat.md docs/contributing/syscalls.md
git commit -m "docs: update userland source paths"
```

### Task 8: Verify Builds And Final Cleanup

**Files:**
- Inspect: `Makefile`, `user/Makefile`, `kernel/arch/arm64/arch.mk`, `tools/*.py`, `docs/*.md`, `.gitignore`
- Test: Make and Python commands

- [ ] **Step 1: Check the user program manifest**

Run:

```bash
make -C user print-progs
```

Expected: prints the program list from `user/programs.mk`, including `shell`, `desktop`, and `lsblk`.

- [ ] **Step 2: Build one x86 C program**

Run:

```bash
make build/user/x86/bin/shell
```

Expected: creates `build/user/x86/bin/shell` and `build/user/x86/obj/apps/shell.o`.

- [ ] **Step 3: Build one x86 C++ program**

Run:

```bash
make build/user/x86/bin/hello
```

Expected: creates `build/user/x86/bin/hello` and links with C++ runtime objects under `build/user/x86/obj/runtime`.

- [ ] **Step 4: Build the x86 disk image**

Run:

```bash
make disk
```

Expected: builds `img/disk.img` using binaries from `build/user/x86/bin`.

- [ ] **Step 5: Generate x86 compile commands**

Run:

```bash
make compile-commands
```

Expected: `compile_commands.json` includes user entries for `user/apps/*.c` and `user/runtime/*.c` with outputs under `build/user/x86/obj`.

- [ ] **Step 6: Dry-run arm64 debug-user**

Run:

```bash
make -B -n ARCH=arm64 debug-user APP=shell
```

Expected: output includes `add-symbol-file build/user/arm64/bin/shell 0x02000000`.

- [ ] **Step 7: Run policy and unit checks**

Run:

```bash
python3 tools/check_userland_runtime_lanes.py
python3 tools/test_generate_compile_commands.py
python3 tools/test_make_targets_arch_neutral.py
python3 tools/test_arm64_dev_loop_parity.py
python3 tools/check_busybox_compat_harness.py
```

Expected: all PASS.

- [ ] **Step 8: Scan for stale old userland paths**

Run:

```bash
rg -n "user/(lib|[A-Za-z0-9_+-]+\\.(c|cpp|asm)|user\\.ld|arm64-user)|build/arm64-user|user/\\$\\(APP\\)|user/<app>|user/<program>" Makefile user kernel tools docs .gitignore
```

Expected: no stale references except in the committed spec and plan files under `docs/superpowers/`.

- [ ] **Step 9: Commit final verification fixes**

If verification required edits, run:

```bash
git add Makefile user kernel tools docs .gitignore
git commit -m "fix: finish userland layout migration"
```

If no edits were required, do not create an empty commit.

## Self-Review

- Spec coverage: Tasks cover source layout, artifact layout, x86 build changes, arm64 build changes, root filesystem packaging, debug symbol paths, compile command generation, policy tests, documentation, `.gitignore`, and validation.
- Placeholder scan: The plan contains no unresolved marker tokens or open-ended implementation steps. Commands and expected outcomes are explicit.
- Type and variable consistency: The plan uses `build/user/<arch>`, `USER_BUILD_ROOT`, `USER_BIN_DIR`, `ARM_USER_BIN_DIR`, `OBJ_DIR`, `RUNTIME_DIR`, and `LINKER_DIR` consistently across build and test tasks.
