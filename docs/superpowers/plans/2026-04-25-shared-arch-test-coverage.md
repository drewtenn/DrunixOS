# Shared Architecture Test Coverage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every public test intent covered on both x86 and ARM64, using shared tests wherever possible and paired arch-specific tests where the mechanism cannot be shared.

**Architecture:** Add a machine-readable test coverage manifest that names each test intent, the public target that runs it, and the implementation used by each architecture. Then migrate the current x86-only in-kernel KTEST suite and ARM-only Python probes toward shared intent coverage, tightening `make check` so both architectures run the same public confidence layers.

**Tech Stack:** GNU Make, Python 3 guard scripts, existing QEMU runners, existing `kernel/test` KTEST framework, existing ARM64 serial-log Python checks.

---

## File Structure

- Create `tools/test_intent_manifest.py`: single source of truth for shared test intents, per-arch implementation commands, and allowed arch-specific alternatives.
- Create `tools/check_test_intent_coverage.py`: verifies every intent has both `x86` and `arm64` coverage and that `make -n ARCH=<arch> check` or `test-headless` contains the listed commands.
- Modify `tools/test_check_wiring.py`: delegate intent coverage checks to `check_test_intent_coverage.py` and keep only target-shape assertions.
- Modify `Makefile`: add `check-test-intent-coverage` to both architecture `check` targets and `.PHONY` lists.
- Modify `kernel/arch/arm64/arch.mk`: add ARM64 build rules for KTEST objects when `KTEST=1`.
- Modify `Makefile` ARM64 branch: add `KTEST=1` support to ARM64 `test-headless` once the shared-compatible KTEST subset links and boots.
- Modify selected `kernel/test/test_*.c`: split x86-only assumptions behind arch-neutral helpers or mark their intent as requiring an ARM-specific equivalent.
- Modify `docs/contributing/testing.md` or create it if missing: document the invariant: new public test intents need coverage for both architectures.

---

### Task 1: Add a Red Test Intent Coverage Guard

**Files:**
- Create: `tools/test_intent_manifest.py`
- Create: `tools/check_test_intent_coverage.py`
- Modify: `Makefile`
- Modify: `tools/test_check_wiring.py`

- [ ] **Step 1: Write the manifest with current known intents**

Create `tools/test_intent_manifest.py`:

```python
#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass


ARCHES = ("x86", "arm64")


@dataclass(frozen=True)
class TestIntent:
    name: str
    target: str
    commands: dict[str, tuple[str, ...]]


INTENTS: tuple[TestIntent, ...] = (
    TestIntent(
        name="static include hygiene",
        target="check",
        commands={
            "x86": ("python3 tools/compile_commands_sources.py compile_commands.json --under kernel",),
            "arm64": ("python3 tools/compile_commands_sources.py compile_commands.json --under kernel",),
        },
    ),
    TestIntent(
        name="interactive shell prompt",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_shell_prompt.py --arch x86",),
            "arm64": ("python3 tools/test_shell_prompt.py --arch arm64",),
        },
    ),
    TestIntent(
        name="user program execution",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_user_programs.py --arch x86",),
            "arm64": ("python3 tools/test_user_programs.py --arch arm64",),
        },
    ),
    TestIntent(
        name="sleep syscall behavior",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_sleep.py --arch x86",),
            "arm64": ("python3 tools/test_sleep.py --arch arm64",),
        },
    ),
    TestIntent(
        name="ctrl-c terminal behavior",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_ctrl_c.py --arch x86",),
            "arm64": ("python3 tools/test_ctrl_c.py --arch arm64",),
        },
    ),
    TestIntent(
        name="shell history behavior",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_shell_history.py --arch x86",),
            "arm64": ("python3 tools/test_shell_history.py --arch arm64",),
        },
    ),
    TestIntent(
        name="kernel unit suite",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk", "KTEST.*SUMMARY pass=[0-9][0-9]* fail=0"),
            "arm64": ("KTEST=1", "KTEST.*SUMMARY pass=[0-9][0-9]* fail=0"),
        },
    ),
    TestIntent(
        name="userspace smoke boot",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_user_programs.py --arch x86",),
            "arm64": ("python3 tools/test_arm64_userspace_smoke.py",),
        },
    ),
    TestIntent(
        name="filesystem init",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_shell_prompt.py --arch x86",),
            "arm64": ("python3 tools/test_arm64_filesystem_init.py",),
        },
    ),
    TestIntent(
        name="syscall parity",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk", "KTEST.*SUMMARY pass=[0-9][0-9]* fail=0"),
            "arm64": ("python3 tools/test_arm64_syscall_parity.py",),
        },
    ),
)
```

- [ ] **Step 2: Write the failing coverage checker**

Create `tools/check_test_intent_coverage.py`:

```python
#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from test_intent_manifest import ARCHES, INTENTS


ROOT = Path(__file__).resolve().parents[1]


def make_output(arch: str, target: str) -> str:
    result = subprocess.run(
        ["make", "-n", f"ARCH={arch}", target],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise SystemExit(result.returncode)
    return output


def main() -> int:
    outputs: dict[tuple[str, str], str] = {}
    failures: list[str] = []

    for intent in INTENTS:
        for arch in ARCHES:
            commands = intent.commands.get(arch, ())
            if not commands:
                failures.append(f"{intent.name}: missing {arch} coverage")
                continue
            key = (arch, intent.target)
            if key not in outputs:
                outputs[key] = make_output(arch, intent.target)
            for command in commands:
                if command not in outputs[key]:
                    failures.append(
                        f"{intent.name}: {arch} {intent.target} missing {command!r}"
                    )

    if failures:
        print("test intent coverage is not architecture-complete:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("test intent coverage is architecture-complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Wire the guard into both check targets**

In both x86 and ARM64 `.PHONY` lists, add `check-test-intent-coverage`.

In both x86 and ARM64 target sections, add:

```make
check-test-intent-coverage:
	python3 tools/check_test_intent_coverage.py
```

Append `check-test-intent-coverage` to both `check:` dependency lists after `check-test-wiring`.

- [ ] **Step 4: Run the guard and verify it fails for ARM64 KTEST**

Run:

```bash
python3 tools/check_test_intent_coverage.py
```

Expected: FAIL with a message including:

```text
kernel unit suite: arm64 test-headless missing 'KTEST=1'
```

- [ ] **Step 5: Commit the red guard**

```bash
git add Makefile tools/test_intent_manifest.py tools/check_test_intent_coverage.py tools/test_check_wiring.py
git commit -m "test: guard cross-architecture test intent coverage"
```

---

### Task 2: Make ARM64 Build a KTEST Kernel

**Files:**
- Modify: `Makefile`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `kernel/kernel.c` if needed for ARM64-safe KTEST boot placement

- [ ] **Step 1: Add an explicit red build assertion**

Run:

```bash
make -n ARCH=arm64 KTEST=1 kernel
```

Expected before implementation: output does not link `kernel/test/ktest.o` or any `kernel/test/test_*.o` into `kernel-arm64.elf`.

- [ ] **Step 2: Add ARM64 KTEST object rules**

In `kernel/arch/arm64/arch.mk`, add:

```make
ARM_KTEST_OBJS := $(patsubst kernel/test/%.o,kernel/test/%.arm64.o,$(KTOBJS))

kernel/test/%.arm64.o: kernel/test/%.c
	$(ARM_CC) $(ARM_CFLAGS) -Wno-stack-usage $(DEPFLAGS) $(ARM_INC) -I kernel/test -c $< -o $@
```

- [ ] **Step 3: Link ARM64 KTEST objects when KTEST=1**

In the ARM64 section of `Makefile`, change:

```make
kernel-arm64.elf: $(ARM_KOBJS) $(ARM_SHARED_KOBJS) Makefile kernel/arch/arm64/arch.mk
	$(ARM_LD) $(ARM_LDFLAGS) --wrap=syscall_case_exit_exit_group -o $@ $(ARM_KOBJS) $(ARM_SHARED_KOBJS)
```

to:

```make
ifeq ($(KTEST),1)
ARM_TEST_OBJS := $(ARM_KTEST_OBJS)
else
ARM_TEST_OBJS :=
endif

kernel-arm64.elf: $(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_TEST_OBJS) Makefile kernel/arch/arm64/arch.mk
	$(ARM_LD) $(ARM_LDFLAGS) --wrap=syscall_case_exit_exit_group -o $@ $(ARM_KOBJS) $(ARM_SHARED_KOBJS) $(ARM_TEST_OBJS)
```

- [ ] **Step 4: Run the dry-run build assertion**

Run:

```bash
make -n ARCH=arm64 KTEST=1 kernel | rg "kernel/test/ktest.arm64.o|kernel/test/test_console_terminal.arm64.o"
```

Expected: PASS, both object names appear.

- [ ] **Step 5: Run the real ARM64 KTEST build**

Run:

```bash
make ARCH=arm64 KTEST=1 kernel
```

Expected: if it fails, the failures identify the first x86-only KTEST dependencies to split in Task 3. If it passes, continue to Task 4.

- [ ] **Step 6: Commit the ARM64 KTEST build plumbing**

```bash
git add Makefile kernel/arch/arm64/arch.mk
git commit -m "test: build kernel unit tests for arm64"
```

---

### Task 3: Split KTEST Suites into Shared and Arch-Specific Intents

**Files:**
- Modify: `kernel/tests.mk`
- Modify: selected `kernel/test/test_*.c`
- Create if needed: `kernel/test/test_arch_x86.c`
- Create if needed: `kernel/test/test_arch_arm64.c`

- [ ] **Step 1: Classify current KTEST object dependencies**

Run:

```bash
for f in kernel/test/test_*.c; do printf '%s: ' "$f"; rg -n "arch/x86|GDT|PG_|SYS_|sda1|sdb1|VGA|desktop|mouse|ATA|i386|TLS" "$f" | wc -l; done
```

Expected: a count per KTEST file. Files with nonzero counts need either helper extraction or paired arch-specific tests.

- [ ] **Step 2: Introduce shared and arch-specific KTEST lists**

Replace `kernel/tests.mk` with:

```make
KTEST_SHARED_OBJS = kernel/test/ktest.o \
                    kernel/test/test_console_terminal.o \
                    kernel/test/test_pmm_core.o \
                    kernel/test/test_kheap.o \
                    kernel/test/test_vfs.o \
                    kernel/test/test_sched.o \
                    kernel/test/test_fs.o \
                    kernel/test/test_blkdev.o

KTEST_X86_OBJS = kernel/test/test_pmm.o \
                 kernel/test/test_process.o \
                 kernel/test/test_uaccess.o \
                 kernel/test/test_desktop.o

KTEST_ARM64_OBJS =

ifeq ($(ARCH),arm64)
KTOBJS = $(KTEST_SHARED_OBJS) $(KTEST_ARM64_OBJS)
else
KTOBJS = $(KTEST_SHARED_OBJS) $(KTEST_X86_OBJS)
endif
```

- [ ] **Step 3: Run x86 KTEST dry-run**

Run:

```bash
make -n ARCH=x86 KTEST=1 kernel | rg "kernel/test/test_process.o|kernel/test/test_console_terminal.o"
```

Expected: PASS, both x86-specific and shared objects appear.

- [ ] **Step 4: Run ARM64 KTEST dry-run**

Run:

```bash
make -n ARCH=arm64 KTEST=1 kernel | rg "kernel/test/test_console_terminal.arm64.o|kernel/test/test_process.arm64.o"
```

Expected: `test_console_terminal.arm64.o` appears and `test_process.arm64.o` does not appear yet.

- [ ] **Step 5: Build both KTEST kernels**

Run:

```bash
make ARCH=x86 KTEST=1 kernel
make ARCH=arm64 KTEST=1 kernel
```

Expected: both builds complete or expose a specific shared-suite compile failure. Fix shared-suite compile failures by replacing x86-only includes with existing generic headers.

- [ ] **Step 6: Commit the KTEST split**

```bash
git add kernel/tests.mk kernel/test
git commit -m "test: split shared and architecture-specific kernel suites"
```

---

### Task 4: Add ARM64 Headless KTEST Execution

**Files:**
- Modify: `Makefile`
- Create: `tools/test_arm64_ktest.py`
- Modify: `tools/test_intent_manifest.py`

- [ ] **Step 1: Create the ARM64 KTEST runner**

Create `tools/test_arm64_ktest.py`:

```python
#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
QEMU = os.environ.get("QEMU_ARM", "qemu-system-aarch64")
MACHINE = os.environ.get("QEMU_ARM_MACHINE", "raspi3b")
LOG = ROOT / "logs" / "serial-arm64-ktest.log"
ERR = ROOT / "logs" / "qemu-arm64-ktest.stderr"
SUMMARY = "KTEST: SUMMARY pass="
SUCCESS = "fail=0"


def main() -> int:
    build = subprocess.run(
        ["make", "ARCH=arm64", "KTEST=1", "kernel"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if build.returncode != 0:
        print(build.stdout, end="")
        print(build.stderr, end="")
        return build.returncode

    LOG.parent.mkdir(exist_ok=True)
    LOG.unlink(missing_ok=True)
    ERR.unlink(missing_ok=True)
    with ERR.open("w") as stderr:
        proc = subprocess.Popen(
            [
                QEMU,
                "-display",
                "none",
                "-M",
                MACHINE,
                "-kernel",
                "kernel-arm64.elf",
                "-serial",
                "null",
                "-serial",
                f"file:{LOG}",
                "-monitor",
                "none",
                "-no-reboot",
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
        try:
            deadline = time.time() + 30
            while time.time() < deadline:
                if LOG.exists():
                    text = LOG.read_text(errors="ignore")
                    if SUMMARY in text:
                        break
                if proc.poll() is not None:
                    break
                time.sleep(1)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()

    text = LOG.read_text(errors="ignore") if LOG.exists() else ""
    if SUMMARY not in text:
        print("missing ARM64 KTEST summary")
        print(ERR.read_text(errors="ignore"), end="")
        return 1
    summary_lines = [line for line in text.splitlines() if SUMMARY in line]
    if not any(SUCCESS in line for line in summary_lines):
        print("ARM64 KTEST summary did not report fail=0")
        print("\n".join(summary_lines))
        return 1
    print("arm64 kernel unit tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Wire ARM64 `test-headless` through the KTEST runner**

Change the ARM64 `test-headless` target from:

```make
test-headless: check-shared-shell check-userspace-smoke check-filesystem-init check-syscall-parity
```

to:

```make
test-headless: check-kernel-unit check-shared-shell check-userspace-smoke check-filesystem-init check-syscall-parity

check-kernel-unit:
	python3 tools/test_arm64_ktest.py
```

Add `check-kernel-unit` to both `.PHONY` lists. For x86, add:

```make
check-kernel-unit: test-headless
```

Do not add `check-kernel-unit` to x86 `test-headless`, because that would recurse.

- [ ] **Step 3: Update the manifest to look for the ARM64 runner**

In `tools/test_intent_manifest.py`, change the ARM64 kernel unit suite commands to:

```python
"arm64": ("python3 tools/test_arm64_ktest.py",),
```

- [ ] **Step 4: Run the intent coverage guard**

Run:

```bash
python3 tools/check_test_intent_coverage.py
```

Expected: PASS for intent wiring.

- [ ] **Step 5: Run both headless suites**

Run:

```bash
make ARCH=x86 test-headless
make ARCH=arm64 test-headless
```

Expected: x86 reports `KTEST.*SUMMARY ... fail=0`; ARM64 prints `arm64 kernel unit tests passed` and its existing ARM checks pass.

- [ ] **Step 6: Commit ARM64 KTEST execution**

```bash
git add Makefile tools/test_arm64_ktest.py tools/test_intent_manifest.py
git commit -m "test: run kernel unit tests on arm64"
```

---

### Task 5: Pair Remaining X86-Only KTEST Intents with ARM64 Equivalents

**Files:**
- Modify: `kernel/test/test_process.c`
- Modify: `kernel/test/test_uaccess.c`
- Modify: `kernel/test/test_pmm.c`
- Create or modify ARM64-specific KTEST files as needed
- Modify: `kernel/tests.mk`
- Modify: `tools/test_intent_manifest.py`

- [ ] **Step 1: Inventory x86-only KTEST cases**

Run:

```bash
rg -n "KTEST_CASE\\(" kernel/test/test_process.c kernel/test/test_uaccess.c kernel/test/test_pmm.c kernel/test/test_desktop.c
```

Expected: list of cases that are currently included only under `KTEST_X86_OBJS`.

- [ ] **Step 2: Add paired intent entries before porting**

For each x86-only suite, add manifest entries like:

```python
TestIntent(
    name="copy-on-write process memory",
    target="test-headless",
    commands={
        "x86": ("test_uaccess_cow",),
        "arm64": ("test_arm64_cow",),
    },
),
```

Use exact case names from the source files. If an ARM64 equivalent is still missing, this guard must fail.

- [ ] **Step 3: Run the guard and verify it fails**

Run:

```bash
python3 tools/check_test_intent_coverage.py
```

Expected: FAIL for each missing ARM64 equivalent case.

- [ ] **Step 4: Implement the smallest ARM64 equivalent cases first**

Start with tests that exercise generic code and only need ARM64 page-table helpers:

```c
KTEST_CASE(test_arm64_pmm_alloc_free_reuses_pages)
KTEST_CASE(test_arm64_vma_add_rejects_overlapping_regions)
KTEST_CASE(test_arm64_process_resources_start_with_single_refs)
```

Place the cases in `kernel/test/test_arch_arm64.c`, register them in `KTEST_ARM64_OBJS`, and keep the assertions behavior-equivalent to the x86 cases.

- [ ] **Step 5: Run ARM64 KTEST after each small group**

Run:

```bash
make ARCH=arm64 test-headless
```

Expected: `arm64 kernel unit tests passed`.

- [ ] **Step 6: Repeat until the manifest has no unpaired x86-only intents**

Run:

```bash
python3 tools/check_test_intent_coverage.py
```

Expected: PASS.

- [ ] **Step 7: Commit paired KTEST coverage**

```bash
git add kernel/test kernel/tests.mk tools/test_intent_manifest.py
git commit -m "test: pair architecture-specific kernel test intents"
```

---

### Task 6: Document the Testing Contract

**Files:**
- Create: `docs/contributing/testing.md`
- Modify: `README.md`

- [ ] **Step 1: Add testing contract documentation**

Create `docs/contributing/testing.md`:

```markdown
# Testing Contract

Drunix keeps public test intents architecture-complete. A test should be shared
between x86 and ARM64 whenever the behavior under test is shared. When a test
cannot be shared because the mechanism is architecture-specific, the same
intent must have an x86 implementation and an ARM64 implementation.

`tools/test_intent_manifest.py` is the source of truth for this contract.
`make ARCH=x86 check` and `make ARCH=arm64 check` both run
`check-test-intent-coverage`, which verifies that every listed intent has
coverage on both architectures.

Before adding a new public test target or KTEST case:

1. Add or update the intent in `tools/test_intent_manifest.py`.
2. Prefer one shared test command for both architectures.
3. If the test must be architecture-specific, add paired commands or case names.
4. Run `python3 tools/check_test_intent_coverage.py`.
5. Run both `make ARCH=x86 test-headless` and `make ARCH=arm64 test-headless`
   when the change affects runtime behavior.
```

- [ ] **Step 2: Link it from README**

In the README test section, add:

```markdown
The cross-architecture testing contract is documented in
`docs/contributing/testing.md`. Public test intent must either be shared between
x86 and ARM64 or have paired architecture-specific coverage.
```

- [ ] **Step 3: Verify docs references**

Run:

```bash
rg -n "Testing Contract|test_intent_manifest|check-test-intent-coverage" README.md docs/contributing/testing.md
```

Expected: all three phrases appear.

- [ ] **Step 4: Commit docs**

```bash
git add README.md docs/contributing/testing.md
git commit -m "docs: document cross-architecture testing contract"
```

---

### Task 7: Final Verification

**Files:**
- No source edits expected.

- [ ] **Step 1: Run target-surface guard**

Run:

```bash
python3 tools/test_make_targets_arch_neutral.py
```

Expected: `public phony targets are architecture-neutral`

- [ ] **Step 2: Run intent coverage guard**

Run:

```bash
python3 tools/check_test_intent_coverage.py
```

Expected: `test intent coverage is architecture-complete`

- [ ] **Step 3: Run wiring guards**

Run:

```bash
python3 tools/test_check_wiring.py --arch x86
python3 tools/test_check_wiring.py --arch arm64
```

Expected: both commands pass.

- [ ] **Step 4: Run headless tests on both architectures**

Run:

```bash
make ARCH=x86 test-headless
make ARCH=arm64 test-headless
```

Expected: x86 KTEST summary reports `fail=0`; ARM64 KTEST runner reports `arm64 kernel unit tests passed`; existing shell/userspace/filesystem/syscall checks pass.

- [ ] **Step 5: Run full checks on both architectures**

Run:

```bash
make ARCH=x86 check
make ARCH=arm64 check
```

Expected: both checks pass.

- [ ] **Step 6: Commit final verification fixes if any**

If verification required edits:

```bash
git add Makefile tools kernel docs README.md
git commit -m "test: finish shared architecture coverage"
```

If no edits were required, do not create an empty commit.

---

## Self-Review

- Spec coverage: The plan covers shared tests, paired arch-specific tests, build wiring, runtime execution, target guards, and documentation.
- Placeholder scan: No `TBD`, `TODO`, or unspecified “add tests” steps remain.
- Type consistency: The plan consistently uses `TestIntent`, `INTENTS`, `check-test-intent-coverage`, `test-headless`, `KTEST_SHARED_OBJS`, `KTEST_X86_OBJS`, and `KTEST_ARM64_OBJS`.
