# ARM64 Userspace Convergence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up the first real ARM64 EL0 process through a shared userspace boundary, using a built-in Linux/AArch64 smoke-test ELF instead of `/bin/shell`.

**Architecture:** Keep one shared process/exec/syscall orchestration path while pushing user-entry, trap, and ELF ABI ownership into arch adapters. Preserve the existing x86 i386 path behind the same boundary, and use an embedded ARM64 init ELF so the phase does not depend on unported storage or shell work.

**Tech Stack:** Freestanding C, AArch64 assembly, i386 assembly, GNU Make, QEMU AArch64, existing x86 headless kernel tests, focused Python regression guards

---

## File Structure

- Create: `docs/superpowers/plans/2026-04-23-arm64-userspace-convergence.md`
- Create: `tools/test_kernel_arch_boundary_phase6.py`
- Create: `tools/test_arm64_userspace_smoke.py`
- Create: `kernel/arch/arm64/proc/entry.S`
- Create: `kernel/arch/arm64/proc/init_elf.c`
- Create: `kernel/arch/arm64/proc/elf64.h`
- Create: `kernel/arch/arm64/proc/elf64.c`
- Create: `user/arm64_smoketest.c`
- Create: `user/lib/crt0_arm64.S`
- Create: `user/lib/syscall_arm64.h`
- Create: `user/lib/syscall_arm64.c`
- Modify: `Makefile`
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `kernel/arch/arm64/exceptions.c`
- Modify: `kernel/arch/arm64/exceptions_s.S`
- Modify: `kernel/arch/arm64/start_kernel.c`
- Modify: `kernel/arch/arm64/proc/arch_proc.c`
- Modify: `kernel/arch/x86/proc/arch_proc.c`
- Modify: `kernel/proc/elf.h`
- Modify: `kernel/proc/elf.c`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/sched.c`
- Modify: `kernel/proc/sched.h`
- Modify: `kernel/proc/syscall.h`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/proc/core.c`
- Modify: `kernel/test/test_process.c`
- Modify: `user/Makefile`
- Modify: `user/programs.mk`

## Task 1: Add Red Boundary And Boot Tests

**Files:**
- Create: `tools/test_kernel_arch_boundary_phase6.py`
- Create: `tools/test_arm64_userspace_smoke.py`
- Modify: `Makefile`

- [ ] **Step 1: Write the failing shared-boundary guard**

```python
#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN = {
    ROOT / "kernel/proc/elf.h": [r"\bEM_386\b"],
    ROOT / "kernel/proc/syscall.c": [r"INT 0x80", r"\beax,\s*ebx,\s*ecx"],
    ROOT / "kernel/arch/arm64/exceptions.c": [r"uart_puts\\(\"sync exception"],
}

REQUIRED = {
    ROOT / "kernel/proc/elf.c": [r"\barch_elf", r"\barch_process_build_initial_frame\b"],
    ROOT / "kernel/proc/syscall.c": [r"\barch_syscall", r"\barch_syscall_dispatch\b"],
    ROOT / "kernel/arch/arm64/exceptions.c": [r"\barch_current_irq_frame\b", r"\bsched_record_user_fault\b"],
}

def check(path_map, predicate, label):
    for path, patterns in path_map.items():
        text = path.read_text()
        for pattern in patterns:
            if predicate(re.search(pattern, text)):
                print(f"{label}: {path.relative_to(ROOT)} {pattern}", file=sys.stderr)
                raise SystemExit(1)

check(FORBIDDEN, bool, "forbidden")
check(REQUIRED, lambda m: not m, "missing")
print("phase6 boundary guard passed")
```

- [ ] **Step 2: Run guard to verify it fails**

Run: `python3 tools/test_kernel_arch_boundary_phase6.py`
Expected: FAIL because `kernel/proc/elf.h` still exposes `EM_386`, `kernel/proc/syscall.c` still uses the x86 register-shaped dispatcher, and `kernel/arch/arm64/exceptions.c` still halts on sync exceptions.

- [ ] **Step 3: Write the failing ARM64 smoke boot test**

```python
#!/usr/bin/env python3
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT / "logs" / "serial-arm-userspace.log"

subprocess.run(["make", "ARCH=arm64", "build"], cwd=ROOT, check=True)
LOG.unlink(missing_ok=True)
proc = subprocess.Popen(
    [
        "qemu-system-aarch64",
        "-display", "none",
        "-M", "raspi3b",
        "-kernel", "kernel-arm64.elf",
        "-serial", "null",
        "-serial", f"file:{LOG}",
        "-monitor", "none",
        "-no-reboot",
    ],
    cwd=ROOT,
)
try:
    for _ in range(20):
        if LOG.exists():
            text = LOG.read_text(errors="ignore")
            if "ARM64 user smoke: pass" in text:
                break
        time.sleep(1)
finally:
    proc.kill()
    proc.wait()

text = LOG.read_text(errors="ignore")
assert "ARM64 user smoke: entered" in text
assert "ARM64 user smoke: syscall ok" in text
assert "ARM64 user smoke: pass" in text
print("arm64 userspace smoke check passed")
```

- [ ] **Step 4: Run smoke test to verify it fails**

Run: `python3 tools/test_arm64_userspace_smoke.py`
Expected: FAIL because none of the serial success markers exist yet.

- [ ] **Step 5: Wire the new checks into Make without making them pass yet**

```make
check-phase6:
	python3 tools/test_kernel_arch_boundary_phase6.py

check-arm64-userspace:
	python3 tools/test_arm64_userspace_smoke.py
```

- [ ] **Step 6: Commit**

```bash
git add tools/test_kernel_arch_boundary_phase6.py tools/test_arm64_userspace_smoke.py Makefile
git commit -m "test: add arm64 userspace convergence red checks"
```

## Task 2: Define Shared Userspace ABI Hooks

**Files:**
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/syscall.h`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/proc/sched.h`
- Modify: `kernel/proc/sched.c`
- Test: `tools/test_kernel_arch_boundary_phase6.py`

- [ ] **Step 1: Add the failing process/syscall interface expectations to the guard**

```python
REQUIRED[ROOT / "kernel/arch/arch.h"] = [
    r"\barch_syscall_number\b",
    r"\barch_syscall_arg0\b",
    r"\barch_syscall_arg1\b",
    r"\barch_syscall_arg2\b",
    r"\barch_syscall_arg3\b",
    r"\barch_syscall_arg4\b",
    r"\barch_syscall_arg5\b",
    r"\barch_syscall_set_result\b",
    r"\barch_trap_frame_is_syscall\b",
    r"\barch_trap_frame_fault_addr\b",
]
```

- [ ] **Step 2: Run guard to verify it fails on missing hook declarations**

Run: `python3 tools/test_kernel_arch_boundary_phase6.py`
Expected: FAIL with missing `arch_syscall_*` and fault helpers.

- [ ] **Step 3: Add the minimal shared arch hook declarations**

```c
uint64_t arch_syscall_number(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg0(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg1(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg2(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg3(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg4(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg5(const arch_trap_frame_t *frame);
void arch_syscall_set_result(arch_trap_frame_t *frame, uint64_t value);
int arch_trap_frame_is_syscall(const arch_trap_frame_t *frame);
uint64_t arch_trap_frame_fault_addr(const arch_trap_frame_t *frame);
```

- [ ] **Step 4: Convert the shared syscall entry contract to an arch-frame-based dispatcher**

```c
uint64_t syscall_dispatch_from_frame(arch_trap_frame_t *frame)
{
	uint64_t nr = arch_syscall_number(frame);
	uint64_t ret = syscall_handler(
	    nr,
	    arch_syscall_arg0(frame),
	    arch_syscall_arg1(frame),
	    arch_syscall_arg2(frame),
	    arch_syscall_arg3(frame),
	    arch_syscall_arg4(frame),
	    arch_syscall_arg5(frame));
	arch_syscall_set_result(frame, ret);
	return ret;
}
```

- [ ] **Step 5: Update scheduler and fault prototypes to take arch-owned trap frames only**

```c
void sched_record_user_fault(const arch_trap_frame_t *frame,
                             uint64_t fault_addr,
                             int signum);
```

- [ ] **Step 6: Run the boundary guard**

Run: `python3 tools/test_kernel_arch_boundary_phase6.py`
Expected: PASS for the newly added interface requirements, but the full file still fails on the remaining x86-only call sites.

- [ ] **Step 7: Commit**

```bash
git add kernel/arch/arch.h kernel/proc/process.h kernel/proc/syscall.h kernel/proc/syscall.c kernel/proc/sched.h kernel/proc/sched.c tools/test_kernel_arch_boundary_phase6.py
git commit -m "refactor: add shared userspace arch hooks"
```

## Task 3: Split ELF Loading Into Shared Flow Plus Arch ABI Helpers

**Files:**
- Create: `kernel/arch/arm64/proc/elf64.h`
- Create: `kernel/arch/arm64/proc/elf64.c`
- Modify: `kernel/arch/x86/proc/arch_proc.c`
- Modify: `kernel/proc/elf.h`
- Modify: `kernel/proc/elf.c`
- Modify: `kernel/proc/process.c`
- Test: `kernel/test/test_process.c`
- Test: `tools/test_kernel_arch_boundary_phase6.py`

- [ ] **Step 1: Add a failing process test for ARM64 ELF machine rejection and x86 preservation**

```c
static void test_elf_machine_validation_is_arch_owned(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(tc, arch_elf_machine_supported(ELF_CLASS_32, EM_386), 1);
	KTEST_EXPECT_EQ(tc, arch_elf_machine_supported(ELF_CLASS_64, EM_AARCH64), 0);
}
```

- [ ] **Step 2: Run the focused process test and verify it fails**

Run: `make test KTEST=1`
Expected: FAIL because `arch_elf_machine_supported()` and the new ELF class constants do not exist yet.

- [ ] **Step 3: Define shared ELF class/machine enums and an arch validation surface**

```c
typedef enum {
	ELF_CLASS_NONE = 0,
	ELF_CLASS_32 = 1,
	ELF_CLASS_64 = 2,
} elf_class_t;

#define EM_386 3
#define EM_AARCH64 183

int arch_elf_machine_supported(elf_class_t elf_class, uint16_t machine);
int arch_elf_load_user_image(vfs_file_ref_t file_ref,
                             arch_aspace_t aspace,
                             uintptr_t *entry_out,
                             uintptr_t *image_start_out,
                             uintptr_t *heap_start_out);
```

- [ ] **Step 4: Keep the shared load orchestration and route x86/arm64 specifics through arch helpers**

```c
int elf_load_file(vfs_file_ref_t file_ref,
                  arch_aspace_t aspace,
                  uintptr_t *entry_out,
                  uintptr_t *image_start_out,
                  uintptr_t *heap_start_out)
{
	return arch_elf_load_user_image(
	    file_ref, aspace, entry_out, image_start_out, heap_start_out);
}
```

- [ ] **Step 5: Implement the initial ARM64 ELF64 loader around PT_LOAD only**

```c
int arch_elf_load_user_image(vfs_file_ref_t file_ref,
                             arch_aspace_t aspace,
                             uintptr_t *entry_out,
                             uintptr_t *image_start_out,
                             uintptr_t *heap_start_out)
{
	Elf64_Ehdr eh;
	/* read header, validate ELFCLASS64 + EM_AARCH64, iterate PT_LOAD,
	   map pages with ARCH_MM_MAP_USER|READ|WRITE|EXEC as needed, copy bytes,
	   zero BSS, compute entry/image/heap */
	return 0;
}
```

- [ ] **Step 6: Change process creation to use `arch_aspace_t`/`uintptr_t` loader outputs instead of i386-only `uint32_t` assumptions where the values are architecture-owned**

```c
arch_aspace_t aspace = arch_aspace_create();
uintptr_t entry = 0;
uintptr_t image_start = 0;
uintptr_t heap_start = 0;
```

- [ ] **Step 7: Run x86 tests and the boundary guard**

Run: `make test-headless`
Expected: PASS

Run: `python3 tools/test_kernel_arch_boundary_phase6.py`
Expected: PASS for the ELF ownership checks.

- [ ] **Step 8: Commit**

```bash
git add kernel/arch/arm64/proc/elf64.h kernel/arch/arm64/proc/elf64.c kernel/arch/x86/proc/arch_proc.c kernel/proc/elf.h kernel/proc/elf.c kernel/proc/process.c kernel/test/test_process.c tools/test_kernel_arch_boundary_phase6.py
git commit -m "refactor: split user ELF loading by architecture"
```

## Task 4: Add ARM64 EL0 Entry, Syscall Trap, And User-Fault Attribution

**Files:**
- Create: `kernel/arch/arm64/proc/entry.S`
- Modify: `kernel/arch/arm64/exceptions_s.S`
- Modify: `kernel/arch/arm64/exceptions.c`
- Modify: `kernel/arch/arm64/proc/frame.h`
- Modify: `kernel/arch/arm64/proc/arch_proc.c`
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/proc/syscall.c`
- Modify: `kernel/proc/sched.c`
- Modify: `kernel/proc/core.c`
- Test: `tools/test_kernel_arch_boundary_phase6.py`

- [ ] **Step 1: Add a failing guard for ARM64 sync exceptions halting instead of routing EL0 traps**

```python
FORBIDDEN[ROOT / "kernel/arch/arm64/exceptions.c"] = [
    r'utillu_puts\\(\"sync exception',
    r"\barm64_halt_forever\b",
]
REQUIRED[ROOT / "kernel/arch/arm64/proc/arch_proc.c"] = [
    r"\barch_process_build_initial_frame\b",
    r"\barch_syscall_number\b",
    r"\barch_syscall_set_result\b",
]
```

- [ ] **Step 2: Run the guard to verify it fails**

Run: `python3 tools/test_kernel_arch_boundary_phase6.py`
Expected: FAIL because ARM64 still halts on sync exceptions.

- [ ] **Step 3: Define the ARM64 trap frame shape used by exceptions and scheduler handoff**

```c
typedef struct arch_trap_frame {
	uint64_t x[31];
	uint64_t sp_el0;
	uint64_t elr_el1;
	uint64_t spsr_el1;
	uint64_t esr_el1;
	uint64_t far_el1;
} arch_trap_frame_t;
```

- [ ] **Step 4: Add the first EL0 entry trampoline**

```asm
.global arm64_enter_user
arm64_enter_user:
	/* x0 = trap frame pointer */
	ldp x1, x2, [x0, #...]
	msr sp_el0, x1
	msr elr_el1, x2
	msr spsr_el1, x3
	eret
```

- [ ] **Step 5: Route EL0 synchronous exceptions into the shared syscall/fault path**

```c
void arm64_sync_handler(arch_trap_frame_t *frame)
{
	uint32_t ec = (uint32_t)((frame->esr_el1 >> 26) & 0x3Fu);
	if (ec == ESR_EC_SVC64) {
		(void)syscall_dispatch_from_frame(frame);
		return;
	}
	if (arch_irq_frame_is_user((uintptr_t)frame)) {
		sched_record_user_fault(frame,
		                        arch_trap_frame_fault_addr(frame),
		                        SIGSEGV);
		schedule();
		return;
	}
	uart_puts("arm64 kernel sync exception\n");
	arm64_halt_forever();
}
```

- [ ] **Step 6: Implement ARM64 syscall register extraction helpers**

```c
uint64_t arch_syscall_number(const arch_trap_frame_t *frame) { return frame->x[8]; }
uint64_t arch_syscall_arg0(const arch_trap_frame_t *frame) { return frame->x[0]; }
uint64_t arch_syscall_arg5(const arch_trap_frame_t *frame) { return frame->x[5]; }
void arch_syscall_set_result(arch_trap_frame_t *frame, uint64_t value) { frame->x[0] = value; }
```

- [ ] **Step 7: Re-run the boundary guard**

Run: `python3 tools/test_kernel_arch_boundary_phase6.py`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add kernel/arch/arm64/proc/entry.S kernel/arch/arm64/exceptions_s.S kernel/arch/arm64/exceptions.c kernel/arch/arm64/proc/frame.h kernel/arch/arm64/proc/arch_proc.c kernel/arch/arch.h kernel/proc/syscall.c kernel/proc/sched.c kernel/proc/core.c tools/test_kernel_arch_boundary_phase6.py
git commit -m "feat: add arm64 EL0 entry and syscall trap path"
```

## Task 5: Build And Embed The ARM64 Smoke-Test ELF

**Files:**
- Create: `user/arm64_smoketest.c`
- Create: `user/lib/crt0_arm64.S`
- Create: `user/lib/syscall_arm64.h`
- Create: `user/lib/syscall_arm64.c`
- Create: `kernel/arch/arm64/proc/init_elf.c`
- Modify: `user/Makefile`
- Modify: `user/programs.mk`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `Makefile`
- Test: `tools/test_arm64_userspace_smoke.py`

- [ ] **Step 1: Write the failing smoke-test program**

```c
#include "lib/syscall_arm64.h"

void _start(void)
{
	arm64_sys_write(1, "ARM64 user smoke: entered\n", 26);
	long pid = arm64_sys_getpid();
	if (pid > 0)
		arm64_sys_write(1, "ARM64 user smoke: syscall ok\n", 29);
	arm64_sys_exit(0);
}
```

- [ ] **Step 2: Run the ARM64 smoke boot test and confirm it still fails**

Run: `python3 tools/test_arm64_userspace_smoke.py`
Expected: FAIL because the user binary is not built or embedded yet.

- [ ] **Step 3: Add a minimal ARM64 user runtime**

```asm
.global _start
_start:
	bl main
	mov x8, #93
	mov x0, #0
	svc #0
```

```c
static inline long arm64_syscall3(long nr, long a0, long a1, long a2)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x8 __asm__("x8") = nr;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
	return x0;
}
```

- [ ] **Step 4: Add ARM64 user build rules and embed the resulting ELF into the kernel**

```make
ARM64_USER_CC ?= aarch64-linux-gnu-gcc
ARM64_USER_PROGS := user/arm64_smoketest

kernel/arch/arm64/proc/init_elf.o: user/arm64_smoketest
	$(ARM_OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@
```

- [ ] **Step 5: Expose the embedded ELF blob to the ARM64 boot path**

```c
extern const uint8_t _binary_user_arm64_smoketest_start[];
extern const uint8_t _binary_user_arm64_smoketest_end[];

const uint8_t *arm64_init_elf_start(void) { return _binary_user_arm64_smoketest_start; }
size_t arm64_init_elf_size(void) { return (size_t)(_binary_user_arm64_smoketest_end - _binary_user_arm64_smoketest_start); }
```

- [ ] **Step 6: Run the smoke boot test again**

Run: `python3 tools/test_arm64_userspace_smoke.py`
Expected: still FAIL, but now because the kernel does not launch the embedded ELF yet rather than because the asset is missing.

- [ ] **Step 7: Commit**

```bash
git add user/arm64_smoketest.c user/lib/crt0_arm64.S user/lib/syscall_arm64.h user/lib/syscall_arm64.c user/Makefile user/programs.mk kernel/arch/arm64/proc/init_elf.c kernel/arch/arm64/arch.mk Makefile
git commit -m "build: add embedded arm64 userspace smoke binary"
```

## Task 6: Launch The Embedded ARM64 User Process From Boot

**Files:**
- Modify: `kernel/arch/arm64/start_kernel.c`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/sched.c`
- Modify: `kernel/arch/arm64/proc/arch_proc.c`
- Test: `tools/test_arm64_userspace_smoke.py`

- [ ] **Step 1: Add a failing boot-path expectation to the smoke test**

```python
assert "ARM64 user smoke: entered" in text
assert "ARM64 user smoke: syscall ok" in text
assert "ARM64 user smoke: pass" in text
```

- [ ] **Step 2: Run the smoke test and confirm it fails before boot wiring**

Run: `python3 tools/test_arm64_userspace_smoke.py`
Expected: FAIL because `start_kernel` still starts only the kernel terminal.

- [ ] **Step 3: Add an ARM64 init-process creation path that consumes the embedded ELF blob**

```c
int process_create_embedded_elf(process_t *proc,
                                const void *image,
                                size_t image_size,
                                const char *const *argv,
                                int argc);
```

- [ ] **Step 4: Launch the embedded smoke-test process from ARM64 boot before dropping into the idle terminal loop**

```c
static void arm64_launch_init_process(void)
{
	static process_t init_proc;
	const char *argv[] = {"arm64_smoketest", 0};

	if (process_create_embedded_elf(&init_proc,
	                                arm64_init_elf_start(),
	                                arm64_init_elf_size(),
	                                argv,
	                                1) == 0 &&
	    sched_add(&init_proc) > 0) {
		uart_puts("ARM64 user smoke: launch\n");
	}
}
```

- [ ] **Step 5: Mark clean userspace completion in the kernel log path**

```c
if (proc->pid == 1 && proc->exit_status == 0)
	arch_console_write("ARM64 user smoke: pass\n", 24);
```

- [ ] **Step 6: Run the smoke test and make it pass**

Run: `python3 tools/test_arm64_userspace_smoke.py`
Expected: PASS

- [ ] **Step 7: Run ARM64 verification**

Run: `make ARCH=arm64 build`
Expected: PASS

Run: `make ARCH=arm64 check`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add kernel/arch/arm64/start_kernel.c kernel/proc/process.c kernel/proc/process.h kernel/proc/sched.c kernel/arch/arm64/proc/arch_proc.c tools/test_arm64_userspace_smoke.py
git commit -m "feat: launch embedded arm64 userspace smoke test"
```

## Task 7: Full Verification, Docs Sync, And Phase Push

**Files:**
- Modify: `Makefile`
- Modify: `docs/ch03-kernel-entry.md`
- Test: `tools/test_kernel_layout.py`
- Test: `tools/test_generate_compile_commands.py`
- Test: `tools/test_kernel_arch_boundary_phase6.py`
- Test: `tools/test_arm64_userspace_smoke.py`

- [ ] **Step 1: Add the new phase checks to the convenient verification targets**

```make
phase6-check:
	python3 tools/test_kernel_arch_boundary_phase6.py
	python3 tools/test_arm64_userspace_smoke.py
```

- [ ] **Step 2: Update the user-facing boot narrative docs**

```md
arm64 now boots to a serial console and can launch a built-in EL0 smoke-test
process using Linux/AArch64 syscall conventions. The interactive shell remains
an x86-only path until the following phase.
```

- [ ] **Step 3: Run the full final verification suite**

Run: `python3 tools/test_kernel_layout.py`
Expected: `OK`

Run: `python3 tools/test_generate_compile_commands.py`
Expected: `OK`

Run: `python3 tools/test_kernel_arch_boundary_phase6.py`
Expected: `phase6 boundary guard passed`

Run: `make kernel`
Expected: PASS

Run: `make test-headless`
Expected: PASS

Run: `make ARCH=arm64 build`
Expected: PASS

Run: `python3 tools/test_arm64_userspace_smoke.py`
Expected: `arm64 userspace smoke check passed`

Run: `make ARCH=arm64 check`
Expected: PASS

- [ ] **Step 4: Commit the final verification and docs adjustments**

```bash
git add Makefile docs/ch03-kernel-entry.md
git commit -m "docs: record arm64 userspace smoke bring-up"
```

- [ ] **Step 5: Push the completed phase branch**

```bash
git push -u origin <working-branch>
```

Expected: remote branch updated successfully.

## Self-Review Notes

- Spec coverage:
  - Shared userspace ABI hooks: Task 2
  - ELF loader split: Task 3
  - ARM64 EL0 entry/syscall/fault path: Task 4
  - Embedded smoke-test ELF: Tasks 5-6
  - x86 preservation and full verification: Tasks 3, 7
- Placeholder scan:
  - No `TBD` or `TODO` markers remain.
  - Every verification step includes an exact command and expected result.
- Type consistency:
  - Shared trap-frame and syscall helpers consistently use `arch_trap_frame_t`, `arch_aspace_t`, and `uintptr_t` at the architecture boundary.
