# ARM64 Filesystem Init Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the default ARM64 embedded smoke-ELF boot path with a real filesystem-backed ARM64 PID 1 launch, while keeping the smoke path as an explicit fallback.

**Architecture:** Keep the existing shared exec and scheduler flow, but extract two boundaries that are still x86-shaped: initial PID 1 launch and initial user stack construction. On ARM64, embed a tiny root filesystem image into the boot artifact, expose it as a read-only block device, mount it through the existing VFS path, and launch a real AArch64 init binary from that mounted namespace.

**Tech Stack:** Freestanding C, AArch64 assembly, existing VFS/DUFS kernel code, GNU Make, QEMU AArch64, Python regression scripts, existing x86 headless tests

---

## File Structure

- Create: `docs/superpowers/plans/2026-04-23-arm64-filesystem-init.md`
- Create: `tools/test_kernel_arch_boundary_phase7.py`
- Create: `tools/test_arm64_filesystem_init.py`
- Create: `kernel/proc/init_launch.c`
- Create: `kernel/proc/init_launch.h`
- Create: `kernel/arch/arm64/rootfs.c`
- Create: `kernel/arch/arm64/rootfs.h`
- Create: `kernel/arch/arm64/rootfs_blob.S`
- Create: `user/arm64init.c`
- Create: `user/lib/crt0_arm64.S`
- Create: `user/lib/syscall_arm64.c`
- Create: `user/lib/syscall_arm64.h`
- Modify: `Makefile`
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/kernel.c`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/test/test_process.c`
- Modify: `kernel/arch/x86/proc/arch_proc.c`
- Modify: `kernel/arch/arm64/proc/arch_proc.c`
- Modify: `kernel/arch/arm64/proc/smoke.c`
- Modify: `kernel/arch/arm64/start_kernel.c`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `kernel/objects.mk`

## Task 1: Add Red Regression Coverage For Filesystem-Backed ARM64 Init

**Files:**
- Create: `tools/test_kernel_arch_boundary_phase7.py`
- Create: `tools/test_arm64_filesystem_init.py`
- Modify: `Makefile`

- [ ] **Step 1: Write the failing boundary guard**

```python
#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN = {
    ROOT / "kernel/arch/arm64/start_kernel.c": [
        r"\barm64_user_smoke_boot\s*\(\s*\)",
    ],
    ROOT / "kernel/proc/process.c": [
        r"\bbuild_user_stack_frame\s*\(",
    ],
}

REQUIRED = {
    ROOT / "kernel/proc/init_launch.c": [
        r"\bboot_launch_init_process\b",
    ],
    ROOT / "kernel/arch/arch.h": [
        r"\barch_process_build_user_stack\b",
    ],
    ROOT / "kernel/arch/arm64/rootfs.c": [
        r"\barm64_rootfs_register\b",
    ],
    ROOT / "kernel/arch/arm64/arch.mk": [
        r"\brootfs_blob\.o\b",
        r"\barm64init\.elf\b",
        r"\barm64-root\.fs\b",
    ],
}

def check(table, predicate, label):
    for path, patterns in table.items():
        text = path.read_text()
        for pattern in patterns:
            if predicate(re.search(pattern, text)):
                print(f"{label}: {path.relative_to(ROOT)} {pattern}", file=sys.stderr)
                raise SystemExit(1)

check(REQUIRED, lambda m: not m, "missing")
check(FORBIDDEN, bool, "forbidden")
print("phase7 boundary guard passed")
```

- [ ] **Step 2: Run the guard and confirm it fails**

Run: `python3 tools/test_kernel_arch_boundary_phase7.py`  
Expected: FAIL because the new shared init launcher, ARM64 rootfs registration, and arch-owned stack hook do not exist yet, and `start_kernel.c` still calls the smoke boot path directly.

- [ ] **Step 3: Write the failing ARM64 filesystem-init boot test**

```python
#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
QEMU = os.environ.get("QEMU_ARM", "qemu-system-aarch64")
MACHINE = os.environ.get("QEMU_ARM_MACHINE", "raspi3b")
LOG = ROOT / "logs" / "serial-arm64-fs-init.log"
ERR = ROOT / "logs" / "qemu-arm64-fs-init.stderr"

def run_case(make_args, required, forbidden=()):
    subprocess.run(["make", "ARCH=arm64", "build", *make_args], cwd=ROOT, check=True)
    LOG.parent.mkdir(exist_ok=True)
    LOG.unlink(missing_ok=True)
    ERR.unlink(missing_ok=True)
    with ERR.open("w") as stderr:
        proc = subprocess.Popen(
            [
                QEMU,
                "-display", "none",
                "-M", MACHINE,
                "-kernel", "kernel-arm64.elf",
                "-serial", "null",
                "-serial", f"file:{LOG}",
                "-monitor", "none",
                "-no-reboot",
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
        try:
            deadline = time.time() + 20
            while time.time() < deadline:
                if LOG.exists():
                    text = LOG.read_text(errors="ignore")
                    if all(marker in text for marker in required):
                        break
                time.sleep(1)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    text = LOG.read_text(errors="ignore")
    for marker in required:
        assert marker in text, marker
    for marker in forbidden:
        assert marker not in text, marker

run_case([], [
    "ARM64 init: entered",
    "ARM64 init: pass",
    "ARM64 init exited with status 0",
    "drunix> ",
], [
    "ARM64 user smoke: entered",
])

run_case(["INIT_PROGRAM=bin/missing-arm64"], [
    "ARM64 init launch failed: bin/missing-arm64",
], [
    "ARM64 user smoke: entered",
])

run_case(["INIT_PROGRAM=bin/missing-arm64", "ARM64_SMOKE_FALLBACK=1"], [
    "ARM64 init launch failed: bin/missing-arm64",
    "ARM64 user smoke: entered",
    "ARM64 user smoke: pass",
])

print("arm64 filesystem init check passed")
```

- [ ] **Step 4: Run the boot test and confirm it fails**

Run: `python3 tools/test_arm64_filesystem_init.py`  
Expected: FAIL because ARM64 does not mount a filesystem-backed root image yet, does not launch `/bin/arm64init`, and does not have an explicit smoke fallback switch.

- [ ] **Step 5: Wire the new checks into the build**

```make
check-phase7:
	python3 tools/test_kernel_arch_boundary_phase7.py

check-arm64-filesystem-init:
	python3 tools/test_arm64_filesystem_init.py
```

- [ ] **Step 6: Commit**

```bash
git add Makefile tools/test_kernel_arch_boundary_phase7.py tools/test_arm64_filesystem_init.py
git commit -m "test: add arm64 filesystem init red checks"
```

## Task 2: Extract The Shared PID 1 Launch Helper

**Files:**
- Create: `kernel/proc/init_launch.c`
- Create: `kernel/proc/init_launch.h`
- Modify: `kernel/kernel.c`
- Modify: `kernel/objects.mk`
- Test: `tools/test_kernel_arch_boundary_phase7.py`

- [ ] **Step 1: Add a failing boundary requirement for the shared launcher**

```python
REQUIRED[ROOT / "kernel/kernel.c"] = [
    r"\bboot_launch_init_process\s*\(",
]
FORBIDDEN[ROOT / "kernel/kernel.c"] = [
    r"\bvfs_open_file\s*\(\s*DRUNIX_INIT_PROGRAM",
    r"\bprocess_create_file\s*\(",
]
```

- [ ] **Step 2: Run the boundary guard and confirm the new rule fails**

Run: `python3 tools/test_kernel_arch_boundary_phase7.py`  
Expected: FAIL because `kernel/kernel.c` still inlines init lookup and PID 1 launch.

- [ ] **Step 3: Create the shared launcher helper**

```c
int boot_launch_init_process(const char *path,
                             const char *arg0,
                             const char *env0,
                             int attach_desktop_pid)
{
	vfs_file_ref_t file_ref;
	uint32_t elf_size = 0;
	static process_t init_proc;
	static const char *argv[1];
	static const char *envp[1];
	int rc;

	argv[0] = arg0;
	envp[0] = env0;
	if (vfs_open_file(path, &file_ref, &elf_size) != 0) {
		klog("BOOT", "initial program not found");
		return -1;
	}
	rc = process_create_file(&init_proc, file_ref, argv, 1, envp, 1, 0);
	if (rc != 0)
		return rc;
	if (sched_add(&init_proc) < 0)
		return -2;
	return (int)init_proc.pid;
}
```

- [ ] **Step 4: Replace the x86 inlined init launch with the helper**

```c
int init_pid = boot_launch_init_process(
    DRUNIX_INIT_PROGRAM, DRUNIX_INIT_ARG0, DRUNIX_INIT_ENV0, 1);
if (init_pid < 0) {
	klog("PROC", "boot_launch_init_process failed");
	for (;;)
		__asm__ volatile("hlt");
}
```

- [ ] **Step 5: Link the new helper into the x86 kernel object list**

```make
KOBJS = kernel/arch/x86/boot/kernel-entry.o kernel/kernel.o \
        kernel/proc/init_launch.o \
        ...
```

- [ ] **Step 6: Run the boundary guard**

Run: `python3 tools/test_kernel_arch_boundary_phase7.py`  
Expected: PASS for the new `kernel/kernel.c` and `kernel/proc/init_launch.c` requirements, while the overall file still fails on the remaining ARM64 and stack-boundary requirements.

- [ ] **Step 7: Commit**

```bash
git add kernel/proc/init_launch.c kernel/proc/init_launch.h kernel/kernel.c kernel/objects.mk tools/test_kernel_arch_boundary_phase7.py
git commit -m "refactor: extract shared init launch helper"
```

## Task 3: Move Initial User Stack Construction Behind An Arch Hook

**Files:**
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/arch/x86/proc/arch_proc.c`
- Modify: `kernel/arch/arm64/proc/arch_proc.c`
- Modify: `kernel/test/test_process.c`
- Test: `tools/test_kernel_arch_boundary_phase7.py`
- Test: `kernel/test/test_process.c`

- [ ] **Step 1: Add failing x86 stack-layout preservation coverage**

```c
static void test_x86_initial_user_stack_builder_preserves_linux_layout(ktest_case_t *tc)
{
	const char *argv[] = {"shell", "-c", "echo"};
	const char *envp[] = {"PATH=/bin"};
	uint32_t esp = 0;

	KTEST_ASSERT_EQ(tc,
	                arch_process_build_user_stack(test_pd_phys,
	                                              argv,
	                                              3,
	                                              envp,
	                                              1,
	                                              &esp),
	                0);
	KTEST_ASSERT_TRUE(tc, esp < USER_STACK_TOP);
	KTEST_ASSERT_EQ(tc, test_read_u32_le(mapped_page, esp & 0xFFFu), 3u);
}
```

- [ ] **Step 2: Run the x86 test target and confirm it fails**

Run: `make test KTEST=1`  
Expected: FAIL because `arch_process_build_user_stack()` does not exist yet.

- [ ] **Step 3: Add the new arch hook to the boundary**

```c
int arch_process_build_user_stack(arch_aspace_t aspace,
                                  const char *const *argv,
                                  int argc,
                                  const char *const *envp,
                                  int envc,
                                  uintptr_t *stack_out);
```

- [ ] **Step 4: Replace the shared i386-only stack builder call**

```c
uintptr_t initial_sp = 0;
if (arch_process_build_user_stack(aspace, argv, argc, envp, envc, &initial_sp) != 0)
	return -6;
proc->user_stack = (uint32_t)initial_sp;
```

- [ ] **Step 5: Move the current x86 stack builder into `kernel/arch/x86/proc/arch_proc.c`**

```c
int arch_process_build_user_stack(arch_aspace_t aspace,
                                  const char *const *argv,
                                  int argc,
                                  const char *const *envp,
                                  int envc,
                                  uintptr_t *stack_out)
{
	uint32_t esp = 0;
	int rc = arch_x86_build_user_stack_frame((uint32_t)aspace,
	                                         argv,
	                                         argc,
	                                         envp,
	                                         envc,
	                                         &esp);
	if (rc == 0 && stack_out)
		*stack_out = esp;
	return rc;
}
```

- [ ] **Step 6: Add the ARM64 stack builder with AArch64 argv/envp alignment**

```c
int arch_process_build_user_stack(arch_aspace_t aspace,
                                  const char *const *argv,
                                  int argc,
                                  const char *const *envp,
                                  int envc,
                                  uintptr_t *stack_out)
{
	uint64_t sp = ARM64_USER_STACK_TOP;
	/* copy strings, align to 16 bytes, write argc/argv/envp/auxv using 64-bit pointers */
	if (!stack_out)
		return -1;
	*stack_out = sp;
	return 0;
}
```

- [ ] **Step 7: Run the x86 kernel tests and the boundary guard**

Run: `make test-headless`  
Expected: PASS.

Run: `python3 tools/test_kernel_arch_boundary_phase7.py`  
Expected: PASS for the new `arch_process_build_user_stack` requirement, while the file still fails on the ARM64 rootfs and boot-path requirements.

- [ ] **Step 8: Commit**

```bash
git add kernel/arch/arch.h kernel/proc/process.c kernel/proc/process.h kernel/arch/x86/proc/arch_proc.c kernel/arch/arm64/proc/arch_proc.c kernel/test/test_process.c tools/test_kernel_arch_boundary_phase7.py
git commit -m "refactor: make initial user stack construction arch-owned"
```

## Task 4: Promote The ARM64 Shared Runtime From Compile-Only To Linked Code

**Files:**
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `Makefile`
- Modify: `kernel/objects.mk`
- Test: `make ARCH=arm64 build`
- Test: `make ARCH=arm64 check`

- [ ] **Step 1: Move the required shared runtime objects into the linked ARM64 kernel**

```make
kernel/drivers/%.arm64.o: kernel/drivers/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/fs/%.arm64.o: kernel/fs/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

kernel/fs/vfs/%.arm64.o: kernel/fs/vfs/%.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEPFLAGS) $(ARM_INC) -c $< -o $@

ARM_SHARED_KOBJS := kernel/lib/klog.arm64.o \
                    kernel/mm/vma.arm64.o \
                    kernel/mm/kheap.arm64.o \
                    kernel/mm/slab.arm64.o \
                    kernel/drivers/blkdev.arm64.o \
                    kernel/drivers/blkdev_part.arm64.o \
                    kernel/drivers/chardev.arm64.o \
                    kernel/drivers/tty.arm64.o \
                    kernel/proc/process.arm64.o \
                    kernel/proc/resources.arm64.o \
                    kernel/proc/task_group.arm64.o \
                    kernel/proc/sched.arm64.o \
                    kernel/proc/uaccess.arm64.o \
                    kernel/proc/syscall.arm64.o \
                    kernel/proc/syscall/helpers.arm64.o \
                    kernel/proc/syscall/console.arm64.o \
                    kernel/proc/syscall/task.arm64.o \
                    kernel/proc/syscall/tty.arm64.o \
                    kernel/proc/core.arm64.o \
                    kernel/proc/mem_forensics.arm64.o \
                    kernel/proc/pipe.arm64.o \
                    kernel/proc/init_launch.arm64.o \
                    kernel/fs/fs.arm64.o \
                    kernel/fs/vfs/core.arm64.o \
                    kernel/fs/vfs/lookup.arm64.o \
                    kernel/fs/vfs/mutation.arm64.o \
                    kernel/fs/procfs.arm64.o \
                    kernel/fs/sysfs.arm64.o
```

- [ ] **Step 2: Make `kernel-arm64.elf` link the promoted shared object list**

```make
kernel-arm64.elf: $(ARM_KOBJS) $(ARM_SHARED_KOBJS)
	$(ARM_LD) $(ARM_LDFLAGS) -o $@ $(ARM_KOBJS) $(ARM_SHARED_KOBJS)
```

- [ ] **Step 3: Keep the remaining ARM64-only compile guards separate**

```make
ARM_COMPILE_ONLY_OBJS := kernel/proc/syscall/fd.arm64.o \
                         kernel/proc/syscall/fd_control.arm64.o \
                         kernel/proc/syscall/vfs/open.arm64.o \
                         kernel/proc/syscall/vfs/path.arm64.o \
                         kernel/proc/syscall/vfs/stat.arm64.o \
                         kernel/proc/syscall/vfs/dirents.arm64.o \
                         kernel/proc/syscall/vfs/mutation.arm64.o \
                         kernel/proc/syscall/process.arm64.o \
                         kernel/proc/syscall/info.arm64.o \
                         kernel/proc/syscall/signal.arm64.o \
                         kernel/proc/syscall/mem.arm64.o
```

- [ ] **Step 4: Run the ARM64 build**

Run: `make ARCH=arm64 build`  
Expected: PASS, producing `kernel-arm64.elf` with the shared runtime linked instead of compile-checked only.

- [ ] **Step 5: Run the existing ARM64 console check**

Run: `make ARCH=arm64 check`  
Expected: PASS, still showing the console banner and `drunix> ` prompt even before the filesystem-backed PID 1 path is enabled.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/arm64/arch.mk Makefile kernel/objects.mk
git commit -m "build: link shared runtime into arm64 kernel"
```

## Task 5: Build And Embed A Minimal ARM64 Root Filesystem Image

**Files:**
- Create: `kernel/arch/arm64/rootfs.c`
- Create: `kernel/arch/arm64/rootfs.h`
- Create: `kernel/arch/arm64/rootfs_blob.S`
- Create: `user/arm64init.c`
- Create: `user/lib/crt0_arm64.S`
- Create: `user/lib/syscall_arm64.c`
- Create: `user/lib/syscall_arm64.h`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `Makefile`
- Test: `make ARCH=arm64 build`

- [ ] **Step 1: Add a tiny ARM64 PID 1 program with serial success markers**

```c
#include "syscall_arm64.h"

int main(void)
{
	arm64_sys_write(1, "ARM64 init: entered\n", 20);
	arm64_sys_write(1, "ARM64 init: pass\n", 17);
	return 0;
}
```

- [ ] **Step 2: Add the minimal ARM64 user runtime**

```asm
.global _start
_start:
	bl main
	mov x8, #93
	mov x0, x0
	svc #0
1:
	b 1b
```

```c
long arm64_sys_write(int fd, const char *buf, unsigned long len)
{
	register long x0 __asm__("x0") = fd;
	register long x1 __asm__("x1") = (long)buf;
	register long x2 __asm__("x2") = (long)len;
	register long x8 __asm__("x8") = 64;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
	return x0;
}
```

- [ ] **Step 3: Build a raw DUFS root image containing only `/bin/arm64init`**

```make
build/arm64init.o: user/arm64init.c user/lib/syscall_arm64.h
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

build/crt0_arm64.o: user/lib/crt0_arm64.S
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

build/syscall_arm64.o: user/lib/syscall_arm64.c user/lib/syscall_arm64.h
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

build/arm64init.elf: build/crt0_arm64.o build/syscall_arm64.o build/arm64init.o
	$(ARM_LD) -nostdlib -e _start -Ttext 0x00400000 -o $@ $^

build/arm64-root.fs: build/arm64init.elf tools/mkfs.py
	$(PYTHON) tools/mkfs.py $@ 32768 build/arm64init.elf bin/arm64init
```

- [ ] **Step 4: Embed the root image and expose it as a read-only block device**

```c
extern const uint8_t arm64_rootfs_start[];
extern const uint8_t arm64_rootfs_end[];

static int arm64_rootfs_read_sector(uint32_t lba, uint8_t *buf)
{
	uint32_t off = lba * 512u;
	uint32_t size = (uint32_t)(arm64_rootfs_end - arm64_rootfs_start);
	if (!buf || off + 512u > size)
		return -1;
	k_memcpy(buf, arm64_rootfs_start + off, 512u);
	return 0;
}

int arm64_rootfs_register(void)
{
	static const blkdev_ops_t ops = {
	    .read_sector = arm64_rootfs_read_sector,
	    .write_sector = 0,
	};
	return blkdev_register("sda1", &ops);
}
```

- [ ] **Step 5: Add the blob build rule**

```asm
.section .rodata
.global arm64_rootfs_start
.global arm64_rootfs_end
arm64_rootfs_start:
    .incbin "build/arm64-root.fs"
arm64_rootfs_end:
```

- [ ] **Step 6: Build the ARM64 kernel and embedded rootfs**

Run: `make ARCH=arm64 build`  
Expected: PASS, producing `build/arm64init.elf`, `build/arm64-root.fs`, and a `kernel-arm64.elf` that contains the rootfs blob.

- [ ] **Step 7: Commit**

```bash
git add kernel/arch/arm64/rootfs.c kernel/arch/arm64/rootfs.h kernel/arch/arm64/rootfs_blob.S user/arm64init.c user/lib/crt0_arm64.S user/lib/syscall_arm64.c user/lib/syscall_arm64.h kernel/arch/arm64/arch.mk Makefile
git commit -m "feat: embed arm64 rootfs and init program"
```

## Task 6: Boot ARM64 Through The Mounted Rootfs And Gate Smoke Fallback Explicitly

**Files:**
- Modify: `kernel/arch/arm64/start_kernel.c`
- Modify: `kernel/arch/arm64/proc/smoke.c`
- Modify: `kernel/proc/init_launch.c`
- Modify: `Makefile`
- Modify: `tools/test_kernel_arch_boundary_phase7.py`
- Test: `python3 tools/test_arm64_filesystem_init.py`
- Test: `make ARCH=arm64 check`
- Test: `make test-headless`

- [ ] **Step 1: Add the ARM64 boot path that mounts the embedded rootfs and launches PID 1**

```c
if (arm64_rootfs_register() != 0)
	uart_puts("ARM64 rootfs register failed\n");
vfs_reset();
dufs_register();
if (vfs_mount_with_source("/", "dufs", "/dev/sda1") != 0) {
	uart_puts("ARM64 root mount failed\n");
	arm64_console_loop();
}
if (vfs_mount("/dev", "devfs") != 0)
	uart_puts("ARM64 devfs mount failed\n");
if (vfs_mount("/proc", "procfs") != 0)
	uart_puts("ARM64 procfs mount failed\n");
if (vfs_mount_with_source("/sys", "sysfs", "sysfs") != 0)
	uart_puts("ARM64 sysfs mount failed\n");
```

- [ ] **Step 2: Launch PID 1 through the shared helper, not through the smoke loader**

```c
int init_pid = boot_launch_init_process(
    DRUNIX_INIT_PROGRAM, DRUNIX_INIT_ARG0, DRUNIX_INIT_ENV0, 0);
if (init_pid < 0) {
	uart_puts("ARM64 init launch failed: ");
	uart_puts(DRUNIX_INIT_PROGRAM);
	uart_puts("\n");
#if DRUNIX_ARM64_SMOKE_FALLBACK
	if (arm64_user_smoke_boot() != 0)
		uart_puts("ARM64 user smoke: boot failed\n");
#endif
	arm64_console_loop();
}
```

- [ ] **Step 3: Report clean PID 1 exit back to the serial console**

```c
void arm64_report_init_exit(uint32_t status)
{
	char line[64];
	k_snprintf(line, sizeof(line), "ARM64 init exited with status %u\n", status);
	uart_puts(line);
}
```

- [ ] **Step 4: Gate the fallback behind an explicit build flag**

```make
ARM64_SMOKE_FALLBACK ?= 0
CFLAGS += -DDRUNIX_ARM64_SMOKE_FALLBACK=$(ARM64_SMOKE_FALLBACK)

ifeq ($(ARCH),arm64)
INIT_PROGRAM ?= bin/arm64init
INIT_ARG0 ?= arm64init
ROOT_FS ?= dufs
endif
```

- [ ] **Step 5: Run the new ARM64 regression script**

Run: `python3 tools/test_arm64_filesystem_init.py`  
Expected: PASS, covering the default real PID 1 path, the missing-init failure path, and the explicit smoke fallback path.

- [ ] **Step 6: Run the ARM64 console check and the x86 headless suite**

Run: `make ARCH=arm64 check`  
Expected: PASS.

Run: `make test-headless`  
Expected: PASS.

- [ ] **Step 7: Run the full milestone verification set**

Run: `python3 tools/test_kernel_arch_boundary_phase7.py`  
Expected: PASS.

Run: `make ARCH=arm64 build`  
Expected: PASS.

Run: `python3 tools/test_arm64_filesystem_init.py`  
Expected: PASS.

Run: `make ARCH=arm64 check`  
Expected: PASS.

Run: `make test-headless`  
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add kernel/arch/arm64/start_kernel.c kernel/arch/arm64/proc/smoke.c kernel/proc/init_launch.c Makefile tools/test_kernel_arch_boundary_phase7.py
git commit -m "feat: boot arm64 pid1 from embedded rootfs"
```

## Self-Review

Spec coverage:
- Shared init-launch ownership is covered in Task 2 and consumed again in Task 6.
- ARM64 filesystem-backed PID 1 from a mounted namespace is covered in Tasks 5 and 6.
- Explicit smoke fallback discipline is covered in Tasks 1 and 6.
- x86 preservation is covered by Task 3 x86 stack-layout tests and Task 6 `make test-headless`.

Placeholder scan:
- No `TODO`, `TBD`, or “similar to” references remain.
- Each code-changing step includes the concrete function or rule shape to add.

Type consistency:
- The plan uses one new stack hook name consistently: `arch_process_build_user_stack`.
- The shared init launcher stays consistently named `boot_launch_init_process`.
- The ARM64 rootfs registrar stays consistently named `arm64_rootfs_register`.
