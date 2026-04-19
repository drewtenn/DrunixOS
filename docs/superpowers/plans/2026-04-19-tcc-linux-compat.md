# TinyCC Linux Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a TinyCC Linux i386 compatibility lane that boots Drunix, runs `/bin/tcc`, compiles a no-libc C program inside Drunix, runs the compiled program, and extracts a `tcc.log` pass marker.

**Architecture:** Follow the existing BusyBox compatibility pattern: host-build a generated static Linux i386 binary, include it in the generated disk image, and drive it from a small native Drunix runner. The runner writes a source file into `/tmp`, invokes TinyCC through Linux-compatible `execve`, captures stdout/stderr through pipes, runs the produced ELF, and records milestone lines under `/dufs/tcc.log`.

**Tech Stack:** Make, POSIX shell, TinyCC 0.9.27, static Linux i386/musl toolchain, Drunix user C runtime, Drunix Linux i386 syscall ABI, QEMU headless test targets.

---

## File Structure

- Create `tools/build_linux_tcc.sh`: downloads and builds a static Linux i386 `tcc` binary, mirroring `tools/build_linux_busybox.sh`.
- Create `user/tcccompat.c`: native Drunix runner that writes `/tmp/tcchello.c`, runs `/bin/tcc -nostdlib -static`, executes `/tmp/tcchello`, and writes `/dufs/tcc.log`.
- Modify `user/Makefile`: add `tcccompat` to the C runtime lane and generated `tcc` to the Linux i386 lane.
- Modify `Makefile`: include `tcc` and `tcccompat` in disk images, add `test-tcc`, and track `tcc` logs/images.
- Modify `tools/check_userland_runtime_lanes.py`: recognize `tcc` as a generated Linux i386 program.
- Modify `.gitignore`: ignore generated `user/tcc` and `user/tcccompat` binaries.
- Modify Linux ABI/kernel files only after the TCC harness exposes a specific missing Linux-compatible behavior.

## Task 1: Add the Host TinyCC Builder

**Files:**
- Create: `tools/build_linux_tcc.sh`
- Modify: `user/Makefile`
- Modify: `tools/check_userland_runtime_lanes.py`
- Modify: `.gitignore`

- [ ] **Step 1: Verify the target does not exist yet**

Run:

```bash
make -C user tcc
```

Expected before this task: FAIL with no rule or missing source for `tcc`.

- [ ] **Step 2: Add the TinyCC build script**

Create `tools/build_linux_tcc.sh`:

```sh
#!/bin/sh
# Build a static Linux i386 TinyCC with the musl cross compiler.

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 OUTPUT [VERSION] [BUILD_DIR]" >&2
    exit 2
fi

out=$1
version=${2:-0.9.27}
build_dir=${3:-build/tcc}
cc=${LINUX_I386_CC:-i486-linux-musl-gcc}
jobs=${JOBS:-2}
url=${TCC_URL:-https://download.savannah.gnu.org/releases/tinycc/tcc-${version}.tar.bz2}

archive="${build_dir}/tcc-${version}.tar.bz2"
src="${build_dir}/tcc-${version}"

mkdir -p "$build_dir" "$(dirname "$out")"

case "$out" in
    /*) ;;
    *) out="$(pwd)/$out" ;;
esac

if [ ! -f "$archive" ]; then
    curl -L --fail --retry 3 -o "$archive" "$url"
fi

if [ ! -d "$src" ]; then
    tar -xf "$archive" -C "$build_dir"
fi

cd "$src"

./configure \
    --cc="$cc" \
    --cpu=i386 \
    --triplet=i386-linux-musl \
    --config-musl \
    --prefix=/usr \
    --bindir=/bin \
    --libdir=/usr/lib \
    --includedir=/usr/include \
    --extra-cflags="-static -Os" \
    --extra-ldflags="-static"

make -j "$jobs" tcc

cp tcc "${out}.tmp"
mv "${out}.tmp" "$out"
```

Run:

```bash
chmod +x tools/build_linux_tcc.sh
```

- [ ] **Step 3: Add `tcc` to the generated Linux lane**

In `user/Makefile`, change the top variables to include `tcc`:

```make
PROGS = chello hello shell writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello lsblk linuxhello linuxprobe linuxabi busybox tcc bbcompat dufstest redirtest ext3wtest threadtest
C_PROGS = chello shell bbcompat dufstest redirtest ext3wtest threadtest
CXX_PROGS = hello writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello lsblk
LINUX_PROGS = linuxhello linuxprobe linuxabi busybox tcc
LINUX_ASM_PROGS = linuxhello
LINUX_C_PROGS = linuxprobe linuxabi
LINUX_BUSYBOX_PROGS = busybox
LINUX_TCC_PROGS = tcc
CLEAN_PROGS = $(filter-out $(LINUX_BUSYBOX_PROGS) $(LINUX_TCC_PROGS),$(PROGS))
```

Add the TinyCC configuration variables near the BusyBox variables:

```make
TCC_VERSION ?= 0.9.27
TCC_BUILD_DIR ?= ../build/tcc
```

Add the generated target after the BusyBox target:

```make
$(LINUX_TCC_PROGS):
	../tools/build_linux_tcc.sh $@ $(TCC_VERSION) $(TCC_BUILD_DIR)
```

- [ ] **Step 4: Update the runtime lane guard**

In `tools/check_userland_runtime_lanes.py`, replace:

```python
    linux_generated_progs = {"busybox"}
```

with:

```python
    linux_generated_progs = {"busybox", "tcc"}
```

Also add this required-lane check after the BusyBox check:

```python
    if "tcc" not in linux_set:
        add_failure(failures, "LINUX_PROGS must include tcc as the generated static Linux i386 compiler target")
```

- [ ] **Step 5: Ignore generated TinyCC artifacts**

Add these lines near the other user binary ignores in `.gitignore`:

```gitignore
user/tcc
user/tcccompat
```

- [ ] **Step 6: Run the lane guard**

Run:

```bash
python3 tools/check_userland_runtime_lanes.py
```

Expected: PASS with:

```txt
userland C, C++, and Linux i386 runtime lanes are explicit and complete
```

- [ ] **Step 7: Build TinyCC**

Run:

```bash
make -C user tcc
```

Expected: PASS and `user/tcc` exists.

If the command fails because `i486-linux-musl-gcc` is missing, stop and install or configure the Linux i386 musl cross compiler before continuing. Do not replace this with a Drunix-native or host-native `tcc`; this lane must remain a static Linux i386 compatibility probe.

- [ ] **Step 8: Commit**

Run:

```bash
git add tools/build_linux_tcc.sh user/Makefile tools/check_userland_runtime_lanes.py .gitignore
git commit -m "build: add static Linux TinyCC user target"
```

## Task 2: Add the Native TCC Compatibility Runner

**Files:**
- Create: `user/tcccompat.c`
- Modify: `user/Makefile`

- [ ] **Step 1: Add `tcccompat` to the native C lane**

In `user/Makefile`, update the top variables:

```make
PROGS = chello hello shell writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello lsblk linuxhello linuxprobe linuxabi busybox tcc bbcompat dufstest redirtest ext3wtest threadtest tcccompat
C_PROGS = chello shell bbcompat dufstest redirtest ext3wtest threadtest tcccompat
```

- [ ] **Step 2: Add the runner source**

Create `user/tcccompat.c`:

```c
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * tcccompat.c - unattended TinyCC/Linux i386 compatibility runner.
 */

#include "lib/stdio.h"
#include "lib/string.h"
#include "lib/syscall.h"

#define OUT_CAP 8192

static int log_fd = -1;

static const char hello_source[] =
    "static void sys_write(const char *s, unsigned n) {\n"
    "    int r;\n"
    "    __asm__ volatile(\"int $0x80\"\n"
    "        : \"=a\"(r)\n"
    "        : \"a\"(4), \"b\"(1), \"c\"(s), \"d\"(n)\n"
    "        : \"memory\");\n"
    "}\n"
    "static void sys_exit(int code) {\n"
    "    __asm__ volatile(\"int $0x80\"\n"
    "        :\n"
    "        : \"a\"(1), \"b\"(code)\n"
    "        : \"memory\");\n"
    "    for (;;) { }\n"
    "}\n"
    "void _start(void) {\n"
    "    sys_write(\"TCCHELLO OK\\n\", 12);\n"
    "    sys_exit(0);\n"
    "}\n";

static int text_contains(const char *haystack, const char *needle)
{
    int hlen;
    int nlen;

    if (!needle || needle[0] == '\0')
        return 1;
    if (!haystack)
        return 0;

    hlen = (int)strlen(haystack);
    nlen = (int)strlen(needle);
    if (nlen > hlen)
        return 0;

    for (int i = 0; i <= hlen - nlen; i++) {
        if (strncmp(haystack + i, needle, (unsigned int)nlen) == 0)
            return 1;
    }
    return 0;
}

static void emit(const char *s)
{
    int len = (int)strlen(s);

    sys_fwrite(1, s, len);
    if (log_fd >= 0)
        sys_fwrite(log_fd, s, len);
}

static void emit_result(const char *label, int ok, int code, const char *out)
{
    char buf[128];

    snprintf(buf, sizeof(buf), "TCCCOMPAT: %s %s exit=%d\n",
             label, ok ? "ok" : "fail", code);
    emit(buf);
    if (!ok && out) {
        emit("TCCCOMPAT OUTPUT BEGIN\n");
        emit(out);
        emit("TCCCOMPAT OUTPUT END\n");
    }
}

static int wait_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    if (WIFSTOPPED(status))
        return 128 + WSTOPSIG(status);
    return 255;
}

static int write_file(const char *path, const char *text)
{
    int fd = sys_create(path);
    int len = (int)strlen(text);

    if (fd < 0)
        return -1;
    if (sys_fwrite(fd, text, len) != len) {
        sys_close(fd);
        return -1;
    }
    return sys_close(fd);
}

static int file_exists(const char *path)
{
    int fd = sys_open(path);

    if (fd < 0)
        return 0;
    sys_close(fd);
    return 1;
}

static int run_capture(const char *path, char **argv, char *out, int out_cap)
{
    int out_pipe[2];
    int pid;
    int used = 0;
    char *envp[] = { "PATH=/bin", "TMPDIR=/tmp", 0 };

    if (sys_pipe(out_pipe) != 0)
        return 255;

    pid = sys_fork();
    if (pid == 0) {
        sys_dup2(out_pipe[1], 1);
        sys_dup2(out_pipe[1], 2);
        sys_close(out_pipe[0]);
        sys_close(out_pipe[1]);
        sys_execve(path, argv, envp);
        sys_write("exec failed\n");
        sys_exit(127);
    }

    if (pid < 0) {
        sys_close(out_pipe[0]);
        sys_close(out_pipe[1]);
        return 255;
    }

    sys_close(out_pipe[1]);
    for (;;) {
        char chunk[128];
        int n = sys_read(out_pipe[0], chunk, sizeof(chunk));
        if (n <= 0)
            break;
        if (used < out_cap - 1) {
            int room = out_cap - 1 - used;
            int copy = n < room ? n : room;
            memcpy(out + used, chunk, (unsigned int)copy);
            used += copy;
        }
    }
    out[used] = '\0';
    sys_close(out_pipe[0]);

    return wait_exit_code(sys_waitpid(pid, 0));
}

int main(void)
{
    char out[OUT_CAP];
    int ok_version;
    int ok_write;
    int ok_compile;
    int ok_run;
    int code;
    char *version_argv[] = { "tcc", "-v", 0 };
    char *compile_argv[] = {
        "tcc",
        "-nostdlib",
        "-static",
        "/tmp/tcchello.c",
        "-o",
        "/tmp/tcchello",
        0
    };
    char *run_argv[] = { "tcchello", 0 };

    sys_unlink("/dufs/tcc.log");
    log_fd = sys_create("/dufs/tcc.log");
    emit("TCCCOMPAT BEGIN\n");

    sys_mkdir("/tmp");
    sys_unlink("/tmp/tcchello.c");
    sys_unlink("/tmp/tcchello");

    code = run_capture("/bin/tcc", version_argv, out, sizeof(out));
    ok_version = code == 0 && text_contains(out, "tcc version");
    emit_result("version", ok_version, code, out);

    ok_write = write_file("/tmp/tcchello.c", hello_source) == 0;
    emit_result("source write", ok_write, ok_write ? 0 : 1, 0);

    code = ok_write ? run_capture("/bin/tcc", compile_argv, out, sizeof(out)) : 1;
    ok_compile = ok_write && code == 0 && file_exists("/tmp/tcchello");
    emit_result("compile", ok_compile, code, out);

    code = ok_compile ? run_capture("/tmp/tcchello", run_argv, out, sizeof(out)) : 1;
    ok_run = ok_compile && code == 0 && text_contains(out, "TCCHELLO OK\n");
    emit_result("run", ok_run, code, out);

    if (ok_version && ok_write && ok_compile && ok_run)
        emit("TCCCOMPAT PASS\n");
    else
        emit("TCCCOMPAT FAIL\n");

    emit("TCCCOMPAT DONE\n");
    if (log_fd >= 0)
        sys_close(log_fd);

    return ok_version && ok_write && ok_compile && ok_run ? 0 : 1;
}
```

- [ ] **Step 3: Build the runner**

Run:

```bash
make -C user tcccompat
```

Expected: PASS and `user/tcccompat` exists.

- [ ] **Step 4: Run the lane guard**

Run:

```bash
python3 tools/check_userland_runtime_lanes.py
```

Expected: PASS with:

```txt
userland C, C++, and Linux i386 runtime lanes are explicit and complete
```

- [ ] **Step 5: Commit**

Run:

```bash
git add user/Makefile user/tcccompat.c
git commit -m "test: add TinyCC compatibility runner"
```

## Task 3: Wire the Top-Level TCC Test Target

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Verify the top-level target does not exist**

Run:

```bash
make test-tcc
```

Expected before this task: FAIL with:

```txt
make: *** No rule to make target `test-tcc'.  Stop.
```

The exact punctuation may vary by `make`, but the target must be missing.

- [ ] **Step 2: Add TCC programs to the root build lists**

In the top-level `Makefile`, update `USER_PROGS`:

```make
USER_PROGS    := shell chello hello writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello lsblk linuxhello linuxprobe linuxabi busybox tcc bbcompat dufstest redirtest ext3wtest threadtest tcccompat
```

Update `TEST_SUFFIXES`:

```make
TEST_SUFFIXES := ktest df bbcompat linuxabi ext3w threadtest tcc
```

Update `TEST_LOGS`:

```make
TEST_LOGS     := $(foreach suffix,$(TEST_SUFFIXES),serial-$(suffix).log debugcon-$(suffix).log) \
                 bbcompat.log linuxabi.log ext3wtest.log threadtest.log tcc.log ext3-host-readback.txt
```

- [ ] **Step 3: Add `test-tcc`**

Add this target after `test-threadtest`:

```make
test-tcc:
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/tcccompat INIT_ARG0=tcccompat kernel disk
	$(call prepare_test_images,tcc,tcc.log)
	$(call qemu_headless_for,tcc,120)
	$(PYTHON) tools/dufs_extract.py dufs-tcc.img tcc.log tcc.log
	cat tcc.log
	grep -q "TCCCOMPAT: version ok" tcc.log
	grep -q "TCCCOMPAT: compile ok" tcc.log
	grep -q "TCCCOMPAT: run ok" tcc.log
	grep -q "TCCCOMPAT PASS" tcc.log
	! grep -q "TCCCOMPAT FAIL" tcc.log
	! grep -Eq "unknown syscall|Unhandled syscall" debugcon-tcc.log
```

Update `.PHONY` to include `test-tcc`:

```make
.PHONY: all build kernel iso images disk fresh check \
        run run-stdio run-grub-menu run-fresh \
        debug debug-user debug-fresh \
        test test-fresh test-headless test-halt test-busybox-compat test-linux-abi test-threadtest test-tcc test-ext3-linux-compat test-ext3-host-write-interop test-all \
        validate-ext3-linux \
        pdf epub docs \
        rebuild clean
```

- [ ] **Step 4: Run a syntax-level target check**

Run:

```bash
make -n test-tcc
```

Expected: PASS. The printed commands should include:

```txt
INIT_PROGRAM=bin/tcccompat
dufs-tcc.img
tcc.log
debugcon-tcc.log
```

- [ ] **Step 5: Commit**

Run:

```bash
git add Makefile
git commit -m "test: wire TinyCC compatibility target"
```

## Task 4: Run the TCC Harness and Close Compatibility Gaps

**Files:**
- Modify only files implicated by the observed failure.
- Likely files: `kernel/proc/syscall.c`, `kernel/proc/syscall.h`, `kernel/fs/vfs.c`, `kernel/fs/ext3.c`, `tools/check_linux_i386_syscall_abi.py`, `docs/linux-elf-compat.md`, `user/tcccompat.c`

- [ ] **Step 1: Run the full TCC test**

Run:

```bash
make test-tcc
```

Expected at the start of this task: either PASS, or FAIL with useful evidence in `tcc.log` and/or `debugcon-tcc.log`.

- [ ] **Step 2: If the test passes, commit the passing state**

If `make test-tcc` passes, run:

```bash
git status --short
git add Makefile user/Makefile user/tcccompat.c tools/build_linux_tcc.sh tools/check_userland_runtime_lanes.py .gitignore
git commit -m "test: pass TinyCC Linux compatibility smoke"
```

Then skip to Task 5.

- [ ] **Step 3: If there is an unknown syscall, identify it**

Run:

```bash
rg -n "unknown syscall|Unhandled syscall" debugcon-tcc.log
```

Expected on syscall failure: one or more log lines containing the syscall number.

Then inspect the existing Linux i386 dispatch style:

```bash
rg -n "SYS_.*|syscall_case_.*|case SYS_" kernel/proc/syscall.h kernel/proc/syscall.c | head -80
```

Add only the missing syscall behavior needed by TCC, following the existing `syscall_case_*` pattern. If the syscall is public Linux ABI, update `kernel/proc/syscall.h` with the Linux i386 number and add or update a guard in `tools/check_linux_i386_syscall_abi.py`.

- [ ] **Step 4: If TCC fails on file/path behavior, isolate the operation**

Run:

```bash
cat tcc.log
rg -n "open|stat|access|unlink|rename|tmp|No such|permission|failed|FAIL" tcc.log debugcon-tcc.log
```

Expected on file/path failure: TCC output names the missing file or operation. Fix the VFS/ext3 behavior in the smallest compatible place:

- `kernel/fs/vfs.c` for path resolution, mount routing, metadata, or descriptor-level semantics.
- `kernel/fs/ext3.c` for ext3 create/write/truncate/unlink/rename behavior.
- `kernel/proc/syscall.c` for Linux struct layout translation, flags, errno encoding, or syscall argument handling.

- [ ] **Step 5: Add a focused regression before each compatibility fix**

For raw Linux syscall behavior, add a direct assertion to `user/linuxabi.c`.

For a static Linux libc behavior, add a focused probe to `user/linuxprobe.c`.

For a compiler-only behavior, add a narrow assertion to `user/tcccompat.c` and keep the pass markers unchanged.

Example `user/linuxabi.c` style for a syscall return-value regression:

```c
expect_eq("open missing file returns -ENOENT",
          sc2(SYS_OPEN, (long)"/definitely-missing", 0),
          -ENOENT);
```

Run the focused test that covers the regression:

```bash
make test-linux-abi
```

or:

```bash
make test-tcc
```

Expected before the fix: FAIL for the new assertion or TCC milestone.

- [ ] **Step 6: Implement the compatibility fix**

Use existing helper functions instead of creating a parallel compatibility layer. Keep Linux behavior in Linux ABI translation code and filesystem behavior in VFS/ext3.

For new Linux syscall numbers, update all of:

```txt
kernel/proc/syscall.h
kernel/proc/syscall.c
tools/check_linux_i386_syscall_abi.py
docs/linux-elf-compat.md
```

For VFS/ext3 behavior, update the focused filesystem file and add a targeted test in `user/ext3wtest.c` if the behavior is filesystem-persistent.

- [ ] **Step 7: Verify the fix**

Run:

```bash
make test-tcc
```

Expected after all compatibility fixes for this milestone:

```txt
TCCCOMPAT: version ok
TCCCOMPAT: compile ok
TCCCOMPAT: run ok
TCCCOMPAT PASS
```

Also run the relevant nearby suite:

```bash
make test-linux-abi
```

Expected:

```txt
LINUXABI SUMMARY passed
```

with `fail=0` or no `LINUXABI FAIL` markers, matching the current test format.

- [ ] **Step 8: Commit each compatibility fix**

After each small compatibility fix passes its focused test, commit it separately:

For a syscall compatibility fix, run:

```bash
git status --short
git add kernel/proc/syscall.h kernel/proc/syscall.c tools/check_linux_i386_syscall_abi.py docs/linux-elf-compat.md user/linuxabi.c
git commit -m "compat: support TinyCC syscall dependency"
```

For a filesystem compatibility fix, run:

```bash
git status --short
git add kernel/fs/vfs.c kernel/fs/ext3.c user/ext3wtest.c user/tcccompat.c
git commit -m "fs: support TinyCC temporary file workflow"
```

If only a subset of those files changed, remove unchanged paths from the `git add` command after checking `git status --short`.

## Task 5: Final Verification and Documentation

**Files:**
- Modify: `docs/linux-elf-compat.md`
- Modify: `docs/superpowers/specs/2026-04-19-tcc-linux-compat-design.md` only if the implementation meaningfully changed scope.

- [ ] **Step 1: Document the TCC compatibility milestone**

In `docs/linux-elf-compat.md`, add a short section near the existing Linux i386 compatibility notes:

```markdown
## TinyCC Compatibility Probe

Drunix includes a TinyCC compatibility lane that runs a static Linux i386
`/bin/tcc` inside the OS. The `test-tcc` target compiles a no-libc C source
file in `/tmp`, runs the produced ELF, extracts `/dufs/tcc.log`, and fails if
the compiler path triggers an unknown Linux syscall.

This is a Linux ABI probe rather than a Drunix-specific compiler port. Missing
behavior found by this lane should be fixed in the Linux syscall, VFS, ext3, or
process compatibility layers.
```

- [ ] **Step 2: Run the full relevant verification set**

Run:

```bash
python3 tools/check_userland_runtime_lanes.py
make test-linux-abi
make test-busybox-compat
make test-tcc
```

Expected:

```txt
userland C, C++, and Linux i386 runtime lanes are explicit and complete
LINUXABI SUMMARY passed
BBCOMPAT SUMMARY passed 255/255
TCCCOMPAT PASS
```

Also verify:

```bash
! grep -Eq "unknown syscall|Unhandled syscall" debugcon-linuxabi.log
! grep -Eq "unknown syscall|Unhandled syscall" debugcon-bbcompat.log
! grep -Eq "unknown syscall|Unhandled syscall" debugcon-tcc.log
```

Expected: all three commands exit 0.

- [ ] **Step 3: Commit docs**

Run:

```bash
git add docs/linux-elf-compat.md
git commit -m "docs: describe TinyCC compatibility probe"
```

- [ ] **Step 4: Final status check**

Run:

```bash
git status --short
git log --oneline -5
```

Expected: clean working tree except for intentionally ignored build artifacts.
