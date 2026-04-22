# Kernel Arch Boundary Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce the Phase 1 architecture boundary for shared time and console access so common kernel code no longer depends directly on x86 `clock.h`, `print_string()`, or `io.h`.

**Architecture:** Add a narrow shared interface in `kernel/arch/arch.h`, implement it separately in `kernel/arch/x86/arch.c` and `kernel/arch/arm64/arch.c`, then migrate every current shared caller in the same change. Guard the phase with a focused repository test that fails if shared code reintroduces x86-only interfaces.

**Tech Stack:** Freestanding C, GNU Make, Python 3, x86 and AArch64 cross-builds

---

## File Structure

- Create: `kernel/arch/arch.h`
- Create: `kernel/arch/x86/arch.c`
- Create: `kernel/arch/arm64/arch.c`
- Create: `tools/test_kernel_arch_boundary_phase1.py`
- Modify: `kernel/objects.mk`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `kernel/lib/klog.c`
- Modify: `kernel/drivers/tty.c`
- Modify: `kernel/fs/fs.c`
- Modify: `kernel/fs/ext3/main.c`
- Modify: `kernel/fs/ext3/mutation.c`
- Modify: `kernel/proc/syscall/info.c`
- Modify: `kernel/proc/syscall/time.c`
- Modify: `kernel/proc/syscall/vfs/path.c`
- Modify: `kernel/proc/syscall/vfs/stat.c`

### Task 1: Add The Phase 1 Regression Guard

**Files:**
- Create: `tools/test_kernel_arch_boundary_phase1.py`

- [ ] **Step 1: Write the failing regression test**

```python
#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_INCLUDES = {
    "clock.h": [
        ROOT / "kernel/lib/klog.c",
        ROOT / "kernel/fs/fs.c",
        ROOT / "kernel/fs/ext3/main.c",
        ROOT / "kernel/fs/ext3/mutation.c",
        ROOT / "kernel/proc/syscall/info.c",
        ROOT / "kernel/proc/syscall/time.c",
        ROOT / "kernel/proc/syscall/vfs/path.c",
        ROOT / "kernel/proc/syscall/vfs/stat.c",
    ],
    "io.h": [ROOT / "kernel/lib/klog.c"],
}

FORBIDDEN_PATTERNS = {
    r"\\bprint_string\\s*\\(": [
        ROOT / "kernel/lib/klog.c",
        ROOT / "kernel/drivers/tty.c",
    ],
}


def fail(msg: str) -> None:
    print(msg, file=sys.stderr)
    raise SystemExit(1)


for header, paths in FORBIDDEN_INCLUDES.items():
    needle = f'#include "{header}"'
    for path in paths:
        if needle in path.read_text():
            fail(f"{path.relative_to(ROOT)} still includes {header}")

for pattern, paths in FORBIDDEN_PATTERNS.items():
    regex = re.compile(pattern)
    for path in paths:
        if regex.search(path.read_text()):
            fail(f"{path.relative_to(ROOT)} still matches {pattern}")

print("phase1 boundary guard passed")
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python3 tools/test_kernel_arch_boundary_phase1.py`
Expected: FAIL with a message such as `kernel/lib/klog.c still includes clock.h`

- [ ] **Step 3: Keep the failing test in the tree**

```bash
git add tools/test_kernel_arch_boundary_phase1.py
```

- [ ] **Step 4: Commit the red test checkpoint**

```bash
git commit -m "test: add phase1 arch boundary guard"
```

### Task 2: Add The Shared Arch Interface And Per-Architecture Adapters

**Files:**
- Create: `kernel/arch/arch.h`
- Create: `kernel/arch/x86/arch.c`
- Create: `kernel/arch/arm64/arch.c`
- Modify: `kernel/objects.mk`
- Modify: `kernel/arch/arm64/arch.mk`

- [ ] **Step 1: Add the shared boundary header**

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARCH_H
#define KERNEL_ARCH_ARCH_H

#include <stdint.h>

uint32_t arch_time_unix_seconds(void);
uint32_t arch_time_uptime_ticks(void);
void arch_console_write(const char *buf, uint32_t len);
void arch_debug_write(const char *buf, uint32_t len);

#endif
```

- [ ] **Step 2: Add the x86 adapter**

```c
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch.c — Phase 1 shared arch boundary adapters for x86.
 */

#include "../arch.h"
#include "clock.h"
#include "io.h"
#include <stdint.h>

#define QEMU_DEBUG_PORT 0xE9

extern void print_bytes(const char *buf, int n);

uint32_t arch_time_unix_seconds(void) { return clock_unix_time(); }
uint32_t arch_time_uptime_ticks(void) { return clock_uptime_ticks(); }

void arch_console_write(const char *buf, uint32_t len)
{
    if (!buf || len == 0)
        return;
    print_bytes(buf, (int)len);
}

void arch_debug_write(const char *buf, uint32_t len)
{
#ifdef KLOG_TO_DEBUGCON
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] == '\n')
            port_byte_out(QEMU_DEBUG_PORT, '\r');
        port_byte_out(QEMU_DEBUG_PORT, (unsigned char)buf[i]);
    }
#else
    (void)buf;
    (void)len;
#endif
}
```

- [ ] **Step 3: Add the arm64 adapter**

```c
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch.c — Phase 1 shared arch boundary adapters for arm64.
 */

#include "../arch.h"
#include "timer.h"
#include "uart.h"

uint32_t arch_time_unix_seconds(void) { return 0; }
uint32_t arch_time_uptime_ticks(void) { return (uint32_t)arm64_timer_ticks(); }

void arch_console_write(const char *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] == '\n')
            uart_putc('\r');
        uart_putc(buf[i]);
    }
}

void arch_debug_write(const char *buf, uint32_t len)
{
    arch_console_write(buf, len);
}
```

- [ ] **Step 4: Wire the adapters into both builds**

```make
# kernel/objects.mk
KOBJS = kernel/arch/x86/boot/kernel-entry.o kernel/kernel.o \
        kernel/module.o kernel/module_exports.o \
        kernel/lib/klog.o \
        kernel/lib/kstring.o kernel/lib/kprintf.o kernel/lib/ksort.o \
        kernel/arch/x86/arch.o \
        kernel/arch/x86/gdt.o kernel/arch/x86/gdt_flush.o \
        kernel/arch/x86/idt.o kernel/arch/x86/isr.o kernel/arch/x86/sse.o kernel/arch/x86/df_test.o

# kernel/arch/arm64/arch.mk
ARM_KOBJS := kernel/arch/arm64/boot.o \
             kernel/arch/arm64/arch.o \
             kernel/arch/arm64/exceptions.o \
             kernel/arch/arm64/exceptions_s.o \
             kernel/arch/arm64/irq.o \
             kernel/arch/arm64/timer.o \
             kernel/arch/arm64/uart.o \
             kernel/arch/arm64/start_kernel.o
```

- [ ] **Step 5: Run the architecture builds before caller migration**

Run: `make kernel`
Expected: PASS

Run: `make ARCH=arm64 build`
Expected: PASS

- [ ] **Step 6: Commit the adapter scaffold**

```bash
git add kernel/arch/arch.h kernel/arch/x86/arch.c kernel/arch/arm64/arch.c \
        kernel/objects.mk kernel/arch/arm64/arch.mk
git commit -m "kernel: add phase1 arch boundary adapters"
```

### Task 3: Migrate Shared Callers To `arch.h`

**Files:**
- Modify: `kernel/lib/klog.c`
- Modify: `kernel/drivers/tty.c`
- Modify: `kernel/fs/fs.c`
- Modify: `kernel/fs/ext3/main.c`
- Modify: `kernel/fs/ext3/mutation.c`
- Modify: `kernel/proc/syscall/info.c`
- Modify: `kernel/proc/syscall/time.c`
- Modify: `kernel/proc/syscall/vfs/path.c`
- Modify: `kernel/proc/syscall/vfs/stat.c`
- Test: `tools/test_kernel_arch_boundary_phase1.py`

- [ ] **Step 1: Replace shared `clock.h` and `io.h` includes with `arch.h`**

```c
/* klog.c */
#include "klog.h"
#include "arch.h"
#include "sched.h"
#include "kprintf.h"
#include "kstring.h"

/* time.c */
#include "syscall_internal.h"
#include "arch.h"
#include "process.h"
#include "sched.h"
#include "uaccess.h"
```

- [ ] **Step 2: Switch `klog.c` to the new boundary**

```c
static void klog_debugcon_puts(const char *s)
{
    if (!s)
        return;
    arch_debug_write(s, k_strlen(s));
}

static void klog_puts(const char *s)
{
    if (!s)
        return;
    if (desktop_console_mirror_enabled())
        arch_console_write(s, k_strlen(s));
    klog_debugcon_puts(s);
}

rec.ticks = arch_time_uptime_ticks();
```

- [ ] **Step 3: Switch `tty.c` to the new boundary**

```c
static void tty_feedback(const char *buf, uint32_t len)
{
    desktop_state_t *desktop = desktop_is_active() ? desktop_global() : 0;

    if (desktop && desktop_write_console_output(desktop, buf, len) == (int)len)
        return;

    arch_console_write(buf, len);
}

arch_console_write("\b \b", 3u);
arch_console_write(&c, 1u);
```

- [ ] **Step 4: Switch all shared time callers to the new boundary**

```c
/* fs/ext3/mutation.c */
now = arch_time_unix_seconds();
in.mtime = in.ctime = arch_time_unix_seconds();

/* proc/syscall/time.c */
ts[0] = arch_time_unix_seconds();
ticks = arch_time_uptime_ticks();

/* proc/syscall/info.c */
info_put_u32(info, 0u, arch_time_unix_seconds());
```

- [ ] **Step 5: Run the focused boundary test and verify green**

Run: `python3 tools/test_kernel_arch_boundary_phase1.py`
Expected: PASS with `phase1 boundary guard passed`

- [ ] **Step 6: Run baseline verification**

Run: `python3 tools/test_kernel_layout.py`
Expected: PASS

Run: `python3 tools/test_generate_compile_commands.py`
Expected: PASS

Run: `make kernel`
Expected: PASS

Run: `make ARCH=arm64 build`
Expected: PASS

- [ ] **Step 7: Commit the Phase 1 integration**

```bash
git add kernel/lib/klog.c kernel/drivers/tty.c kernel/fs/fs.c \
        kernel/fs/ext3/main.c kernel/fs/ext3/mutation.c \
        kernel/proc/syscall/info.c kernel/proc/syscall/time.c \
        kernel/proc/syscall/vfs/path.c kernel/proc/syscall/vfs/stat.c \
        tools/test_kernel_arch_boundary_phase1.py
git commit -m "kernel: integrate phase1 arch time and console boundary"
```
