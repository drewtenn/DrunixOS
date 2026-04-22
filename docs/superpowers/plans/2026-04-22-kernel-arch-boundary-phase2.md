# Kernel Arch Boundary Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce the Phase 2 architecture boundary for IRQ registration, timer startup, and interrupt-enable sequencing so shared startup and PC drivers no longer depend on x86 `irq.h`, `pit.h`, or direct x86 IRQ bring-up calls.

**Architecture:** Extend `kernel/arch/arch.h` with a small IRQ/timer API, implement it in the existing x86 and arm64 architecture adapters, then migrate shared startup and PC-platform drivers to that API in the same phase. Keep trap setup in its existing architecture-owned path, but move IRQ registry ownership, periodic timer hookup, and the final interrupt-enable step behind the shared boundary.

**Tech Stack:** Freestanding C, GNU Make, Python 3, x86 and AArch64 cross-builds

---

## File Structure

- Create: `tools/test_kernel_arch_boundary_phase2.py`
- Create: `kernel/arch/arm64/irq.h`
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/arch/x86/arch.c`
- Modify: `kernel/arch/x86/irq.h`
- Modify: `kernel/arch/x86/idt.h`
- Modify: `kernel/arch/x86/idt.c`
- Modify: `kernel/arch/x86/pit.h`
- Modify: `kernel/arch/x86/pit.c`
- Modify: `kernel/arch/arm64/arch.c`
- Modify: `kernel/arch/arm64/irq.c`
- Modify: `kernel/arch/arm64/timer.h`
- Modify: `kernel/arch/arm64/timer.c`
- Modify: `kernel/arch/arm64/exceptions.c`
- Modify: `kernel/arch/arm64/start_kernel.c`
- Modify: `kernel/kernel.c`
- Modify: `kernel/platform/pc/keyboard.c`
- Modify: `kernel/platform/pc/mouse.c`

### Task 1: Add The Phase 2 Regression Guard

**Files:**
- Create: `tools/test_kernel_arch_boundary_phase2.py`

- [ ] **Step 1: Write the failing regression guard**

```python
#!/usr/bin/env python3
"""
Focused regression guard for the Phase 2 architecture boundary.

Shared startup and PC platform drivers must stop reaching into x86-private IRQ
and PIT interfaces directly. This check intentionally targets the concrete
callers migrated in Phase 2 rather than trying to infer architecture ownership
for the entire repository.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_INCLUDES = {
    "irq.h": [
        ROOT / "kernel/kernel.c",
        ROOT / "kernel/platform/pc/keyboard.c",
        ROOT / "kernel/platform/pc/mouse.c",
    ],
    "pit.h": [
        ROOT / "kernel/kernel.c",
    ],
}

FORBIDDEN_PATTERNS = {
    r"\birq_dispatch_init\s*\(": [
        ROOT / "kernel/kernel.c",
    ],
    r"\bpit_init\s*\(": [
        ROOT / "kernel/kernel.c",
    ],
    r"\binterrupts_enable\s*\(": [
        ROOT / "kernel/kernel.c",
    ],
    r"\birq_register\s*\(": [
        ROOT / "kernel/platform/pc/keyboard.c",
        ROOT / "kernel/platform/pc/mouse.c",
    ],
    r"\birq_unmask\s*\(": [
        ROOT / "kernel/platform/pc/mouse.c",
    ],
}

REQUIRED_PATTERNS = {
    ROOT / "kernel/kernel.c": [
        r'#include "arch\.h"',
        r"\barch_irq_init\s*\(",
        r"\barch_timer_set_periodic_handler\s*\(\s*sched_tick\s*\)",
        r"\barch_timer_start\s*\(\s*SCHED_HZ\s*\)",
        r"\barch_interrupts_enable\s*\(",
    ],
    ROOT / "kernel/platform/pc/keyboard.c": [
        r'#include "arch\.h"',
        r"\barch_irq_register\s*\(\s*1\s*,\s*keyboard_handler\s*\)",
    ],
    ROOT / "kernel/platform/pc/mouse.c": [
        r'#include "arch\.h"',
        r"\barch_irq_register\s*\(\s*12\s*,\s*mouse_handler\s*\)",
        r"\barch_irq_unmask\s*\(\s*2\s*\)",
        r"\barch_irq_unmask\s*\(\s*12\s*\)",
    ],
    ROOT / "kernel/arch/x86/pit.c": [
        r"\bpit_set_periodic_handler\s*\(",
        r"\bpit_start\s*\(",
        r"\bclock_tick\s*\(",
    ],
    ROOT / "kernel/arch/arm64/start_kernel.c": [
        r"\barch_irq_init\s*\(",
        r"\barch_timer_set_periodic_handler\s*\(",
        r"\barch_timer_start\s*\(\s*10u\s*\)",
        r"\barch_interrupts_enable\s*\(",
    ],
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
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

    for path, patterns in REQUIRED_PATTERNS.items():
        text = path.read_text()
        for pattern in patterns:
            if not re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} is missing {pattern}")

    print("phase2 boundary guard passed")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the guard to verify it fails**

Run: `python3 tools/test_kernel_arch_boundary_phase2.py`
Expected: FAIL with a message such as `kernel/kernel.c still includes irq.h`

- [ ] **Step 3: Keep the failing test in the tree**

```bash
git add tools/test_kernel_arch_boundary_phase2.py
```

- [ ] **Step 4: Commit the red test checkpoint**

```bash
git commit -m "test: add phase2 arch boundary guard"
```

### Task 2: Extend `arch.h` And Implement The x86/arm64 IRQ Adapters

**Files:**
- Create: `kernel/arch/arm64/irq.h`
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/arch/x86/arch.c`
- Modify: `kernel/arch/x86/irq.h`
- Modify: `kernel/arch/x86/idt.h`
- Modify: `kernel/arch/x86/idt.c`
- Modify: `kernel/arch/x86/pit.h`
- Modify: `kernel/arch/x86/pit.c`
- Modify: `kernel/arch/arm64/arch.c`
- Modify: `kernel/arch/arm64/irq.c`
- Modify: `kernel/arch/arm64/timer.h`
- Modify: `kernel/arch/arm64/timer.c`
- Modify: `kernel/arch/arm64/exceptions.c`

- [ ] **Step 1: Extend the shared boundary header**

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARCH_H
#define KERNEL_ARCH_ARCH_H

#include <stdint.h>

typedef void (*arch_irq_handler_fn)(void);

uint32_t arch_time_unix_seconds(void);
uint32_t arch_time_uptime_ticks(void);
void arch_console_write(const char *buf, uint32_t len);
void arch_debug_write(const char *buf, uint32_t len);

void arch_irq_init(void);
void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn);
void arch_irq_mask(uint32_t irq);
void arch_irq_unmask(uint32_t irq);
void arch_timer_set_periodic_handler(arch_irq_handler_fn fn);
void arch_timer_start(uint32_t hz);
void arch_interrupts_enable(void);

#endif
```

- [ ] **Step 2: Refactor the x86 PIT into a timer-start plus callback helper**

```c
/* kernel/arch/x86/pit.h */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PIT_H
#define PIT_H

#include <stdint.h>

typedef void (*pit_handler_fn)(void);

void pit_init(void);
void pit_set_periodic_handler(pit_handler_fn fn);
void pit_start(uint32_t hz);
void pit_handle_irq(void);

#endif
```

```c
/* kernel/arch/x86/pit.c */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pit.c — PIT timer programming plus periodic tick delivery.
 */

#include "pit.h"
#include "clock.h"
#include "io.h"
#include <stdint.h>

#define PIT_INPUT_HZ 1193182u
#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_MODE_RATE_GENERATOR 0x34u

static pit_handler_fn g_pit_handler;

void pit_init(void)
{
    g_pit_handler = 0;
}

void pit_set_periodic_handler(pit_handler_fn fn)
{
    g_pit_handler = fn;
}

void pit_start(uint32_t hz)
{
    uint32_t divisor;

    if (hz == 0)
        return;

    divisor = PIT_INPUT_HZ / hz;
    if (divisor == 0)
        divisor = 1u;

    port_byte_out(PIT_CMD_PORT, PIT_MODE_RATE_GENERATOR);
    port_byte_out(PIT_CH0_PORT, (uint8_t)(divisor & 0xFFu));
    port_byte_out(PIT_CH0_PORT, (uint8_t)((divisor >> 8) & 0xFFu));
}

void pit_handle_irq(void)
{
    clock_tick();
    if (g_pit_handler)
        g_pit_handler();
}
```

- [ ] **Step 3: Move x86 IRQ/timer lifecycle behind the existing x86 adapter**

```c
/* kernel/arch/x86/irq.h */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/*
 * x86-private IRQ dispatch registry.
 *
 * Hardware IRQ lines 0–15 are remapped by the 8259A PIC to vectors 32–47.
 * arch.c wraps this interface for the shared architecture boundary.
 */

#define IRQ_COUNT 16

typedef void (*irq_handler_fn)(void);

void irq_dispatch_init(void);
void irq_unmask(uint8_t irq_num);
void irq_mask(uint8_t irq_num);
void irq_set_pic_masks(uint8_t master_mask, uint8_t slave_mask);
void irq_apply_pic_masks(void);
void irq_register(uint8_t irq_num, irq_handler_fn fn);
void irq_dispatch(uint32_t vector, uint32_t error_code);

#endif
```

```c
/* kernel/arch/x86/idt.h */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef IDT_H
#define IDT_H

void idt_init_early(void);

/*
 * x86-private final interrupt-enable hook. arch.c calls this after the shared
 * boundary has installed the timer callback and early device handlers.
 */
void interrupts_enable(void);

#endif
```

```c
/* kernel/arch/x86/idt.c */
void interrupts_enable(void)
{
    pic_remap();
    __asm__ volatile("sti");
}
```

```c
/* kernel/arch/x86/arch.c */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch.c — Phase 1/2 shared architecture boundary adapters for x86.
 */

#include "../arch.h"
#include "clock.h"
#include "idt.h"
#include "io.h"
#include "irq.h"
#include "pit.h"

#define QEMU_DEBUG_PORT 0xE9

extern void print_bytes(const char *buf, int n);

uint32_t arch_time_unix_seconds(void)
{
    return clock_unix_time();
}

uint32_t arch_time_uptime_ticks(void)
{
    return clock_uptime_ticks();
}

void arch_console_write(const char *buf, uint32_t len)
{
    if (!buf || len == 0)
        return;

    print_bytes(buf, (int)len);
}

void arch_debug_write(const char *buf, uint32_t len)
{
    if (!buf || len == 0)
        return;

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

void arch_irq_init(void)
{
    irq_dispatch_init();
    pit_init();
    irq_register(0, pit_handle_irq);
}

void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn)
{
    if (irq < IRQ_COUNT)
        irq_register((uint8_t)irq, fn);
}

void arch_irq_mask(uint32_t irq)
{
    if (irq < IRQ_COUNT)
        irq_mask((uint8_t)irq);
}

void arch_irq_unmask(uint32_t irq)
{
    if (irq < IRQ_COUNT)
        irq_unmask((uint8_t)irq);
}

void arch_timer_set_periodic_handler(arch_irq_handler_fn fn)
{
    pit_set_periodic_handler(fn);
}

void arch_timer_start(uint32_t hz)
{
    pit_start(hz);
}

void arch_interrupts_enable(void)
{
    interrupts_enable();
}
```

- [ ] **Step 4: Add the arm64 private IRQ registry and timer-start helpers**

```c
/* kernel/arch/arm64/irq.h */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ARM64_IRQ_H
#define ARM64_IRQ_H

#include <stdint.h>

#define ARM64_IRQ_LOCAL_TIMER 0u
#define ARM64_IRQ_COUNT 1u

typedef void (*arm64_irq_handler_fn)(void);

void arm64_irq_init(void);
void arm64_irq_register(uint32_t irq, arm64_irq_handler_fn fn);
void arm64_irq_dispatch(uint32_t irq);
void arm64_irq_enable(void);

#endif
```

```c
/* kernel/arch/arm64/timer.h */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ARM64_TIMER_H
#define ARM64_TIMER_H

#include <stdint.h>

void arm64_timer_start(uint32_t hz);
void arm64_timer_irq(void);
uint64_t arm64_timer_ticks(void);

#endif
```

```c
/* kernel/arch/arm64/timer.c */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * timer.c — ARM Generic Timer support for Milestone 1 bring-up.
 */

#include "timer.h"
#include <stdint.h>

static uint64_t g_ticks_per_interval;
static volatile uint64_t g_tick_count;

static uint64_t cntfrq(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

static void cntp_tval_write(uint64_t value)
{
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(value));
}

static void cntp_ctl_write(uint64_t value)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(value));
}

void arm64_timer_start(uint32_t hz)
{
    if (hz == 0)
        return;

    g_ticks_per_interval = cntfrq() / hz;
    g_tick_count = 0;
    cntp_tval_write(g_ticks_per_interval);
    cntp_ctl_write(1u);
}

void arm64_timer_irq(void)
{
    cntp_tval_write(g_ticks_per_interval);
    g_tick_count++;
}

uint64_t arm64_timer_ticks(void)
{
    return g_tick_count;
}
```

```c
/* kernel/arch/arm64/irq.c */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * irq.c — BCM2836 local interrupt routing for the AArch64 bring-up path.
 */

#include "irq.h"
#include "timer.h"
#include <stdint.h>

#define CORE0_TIMER_IRQCNTL (*(volatile uint32_t *)0x40000040u)
#define CNTPNSIRQ_BIT (1u << 1)

static arm64_irq_handler_fn g_irq_table[ARM64_IRQ_COUNT];

void arm64_irq_init(void)
{
    for (uint32_t i = 0; i < ARM64_IRQ_COUNT; i++)
        g_irq_table[i] = 0;

    CORE0_TIMER_IRQCNTL = CNTPNSIRQ_BIT;
}

void arm64_irq_register(uint32_t irq, arm64_irq_handler_fn fn)
{
    if (irq < ARM64_IRQ_COUNT)
        g_irq_table[irq] = fn;
}

void arm64_irq_dispatch(uint32_t irq)
{
    if (irq >= ARM64_IRQ_COUNT)
        return;

    if (irq == ARM64_IRQ_LOCAL_TIMER)
        arm64_timer_irq();

    if (g_irq_table[irq])
        g_irq_table[irq]();
}

void arm64_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2");
}
```

```c
/* kernel/arch/arm64/exceptions.c */
#include "irq.h"
#include "timer.h"
#include "uart.h"
#include "kprintf.h"
#include <stdint.h>

#define CORE0_IRQ_SOURCE (*(volatile uint32_t *)0x40000060u)
#define CNTPNSIRQ_BIT (1u << 1)

void arm64_irq_handler(uint64_t *frame)
{
    uint32_t source;

    (void)frame;

    source = CORE0_IRQ_SOURCE;
    if (source & CNTPNSIRQ_BIT) {
        arm64_irq_dispatch(ARM64_IRQ_LOCAL_TIMER);
        return;
    }

    g_spurious_irq_count++;
}
```

- [ ] **Step 5: Extend the arm64 shared adapter with the new IRQ/timer wrappers**

```c
/* kernel/arch/arm64/arch.c */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch.c — Phase 1/2 shared architecture boundary adapters for arm64.
 */

#include "../arch.h"
#include "irq.h"
#include "timer.h"
#include "uart.h"

uint32_t arch_time_unix_seconds(void)
{
    return 0;
}

uint32_t arch_time_uptime_ticks(void)
{
    return (uint32_t)arm64_timer_ticks();
}

void arch_console_write(const char *buf, uint32_t len)
{
    if (!buf || len == 0)
        return;

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

void arch_irq_init(void)
{
    arm64_irq_init();
}

void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn)
{
    arm64_irq_register(irq, fn);
}

void arch_irq_mask(uint32_t irq)
{
    (void)irq;
}

void arch_irq_unmask(uint32_t irq)
{
    (void)irq;
}

void arch_timer_set_periodic_handler(arch_irq_handler_fn fn)
{
    arm64_irq_register(ARM64_IRQ_LOCAL_TIMER, fn);
}

void arch_timer_start(uint32_t hz)
{
    arm64_timer_start(hz);
}

void arch_interrupts_enable(void)
{
    arm64_irq_enable();
}
```

- [ ] **Step 6: Run the architecture builds before caller migration**

Run: `make kernel`
Expected: PASS

Run: `make ARCH=arm64 build`
Expected: PASS

- [ ] **Step 7: Commit the adapter scaffold**

```bash
git add kernel/arch/arch.h kernel/arch/x86/arch.c kernel/arch/x86/irq.h \
        kernel/arch/x86/idt.h kernel/arch/x86/idt.c kernel/arch/x86/pit.h \
        kernel/arch/x86/pit.c kernel/arch/arm64/arch.c \
        kernel/arch/arm64/irq.h kernel/arch/arm64/irq.c \
        kernel/arch/arm64/timer.h kernel/arch/arm64/timer.c \
        kernel/arch/arm64/exceptions.c
git commit -m "kernel: add phase2 arch irq adapters"
```

### Task 3: Migrate Shared Startup, PC Drivers, And The arm64 Heartbeat Path

**Files:**
- Modify: `kernel/kernel.c`
- Modify: `kernel/platform/pc/keyboard.c`
- Modify: `kernel/platform/pc/mouse.c`
- Modify: `kernel/arch/arm64/start_kernel.c`
- Test: `tools/test_kernel_arch_boundary_phase2.py`

- [ ] **Step 1: Move shared x86 startup to the new `arch.h` sequence**

```c
/* kernel/kernel.c */
#include "arch.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "ata.h"
#include "blkdev.h"
#include "bcache.h"
#include "gdt.h"
#include "idt.h"
#include "sse.h"
#include "clock.h"
#include "process.h"
#include "sched.h"
```

```c
/* kernel/kernel.c */
klog("BOOT", "bringing up interrupt, timer, and clock subsystems");
clock_init();
arch_irq_init();
arch_timer_set_periodic_handler(sched_tick);
klog("IRQ", "periodic timer callback installed");
```

```c
/* kernel/kernel.c */
/*
 * Enable hardware interrupts only after the architecture-owned IRQ registry,
 * periodic timer callback, and early device handlers are installed. The IDT
 * itself was already loaded above so early breakpoints and traps have a valid
 * destination.
 */
arch_timer_start(SCHED_HZ);
arch_interrupts_enable();
klog("IDT", "interrupts enabled");
klog("BOOT", "interrupt descriptor table live");
```

- [ ] **Step 2: Move PC keyboard and mouse registration to `arch.h`**

```c
/* kernel/platform/pc/keyboard.c */
#include <stdint.h>
#include "arch.h"
#include "chardev.h"
#include "sched.h"
#include "desktop.h"
#include "tty.h"
#include "io.h"
#include "keyboard.h"
```

```c
/* kernel/platform/pc/keyboard.c */
void keyboard_init(void)
{
    arch_irq_register(1, keyboard_handler);
    chardev_register("stdin", &kb_chardev_ops);
    chardev_register("tty0", &kb_chardev_ops);
}
```

```c
/* kernel/platform/pc/mouse.c */
#include "mouse.h"
#include "arch.h"
#include "io.h"
```

```c
/* kernel/platform/pc/mouse.c */
int mouse_init(void)
{
    uint8_t config;

    mouse_stream_reset(&g_stream);
    arch_irq_register(12, mouse_handler);
    arch_irq_unmask(2);
    arch_irq_unmask(12);

    if (ps2_write_command(0xA7) != 0)
        return -1;
    ps2_flush_output();

    int rc = ps2_read_controller_config(&config);
    if (rc != 0)
        return rc;
    config |= 0x02u;
    rc = ps2_write_controller_config(config);
    if (rc != 0)
        return rc;

    if (ps2_write_command(0xA8) != 0)
        return -4;
    if (ps2_mouse_write(0xF4) != 0)
        return -5;
    return 0;
}
```

- [ ] **Step 3: Route the arm64 heartbeat through the new boundary**

```c
/* kernel/arch/arm64/start_kernel.c */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * start_kernel.c — Milestone 1 AArch64 kernel entry point.
 */

#include "../arch.h"
#include "timer.h"
#include "uart.h"
#include "kprintf.h"
#include <stdint.h>

extern char vectors_el1[];

static volatile uint32_t g_heartbeat_ticks;

static void arm64_heartbeat_tick(void)
{
    g_heartbeat_ticks++;
}

static uint64_t arm64_read_currentel(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
    return value;
}

static uint64_t arm64_read_cntfrq(void)
{
    uint64_t value;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

void arm64_start_kernel(void)
{
    char line[64];
    uint32_t last = 0;

    uart_init();
    __asm__ volatile("msr vbar_el1, %0" : : "r"(vectors_el1));
    __asm__ volatile("isb");

    uart_puts("Drunix AArch64 v0 - hello from EL1\n");

    k_snprintf(line,
               sizeof(line),
               "CurrentEL=0x%X (EL%u)\n",
               (unsigned int)arm64_read_currentel(),
               (unsigned int)(arm64_read_currentel() >> 2));
    uart_puts(line);

    k_snprintf(line,
               sizeof(line),
               "CNTFRQ_EL0=%uHz\n",
               (unsigned int)arm64_read_cntfrq());
    uart_puts(line);

    g_heartbeat_ticks = 0;
    arch_irq_init();
    arch_timer_set_periodic_handler(arm64_heartbeat_tick);
    arch_timer_start(10u);
    arch_interrupts_enable();

    for (;;) {
        uint32_t now;

        __asm__ volatile("wfi");
        now = g_heartbeat_ticks;
        while (last < now) {
            last++;
            k_snprintf(line, sizeof(line), "tick %u\n", (unsigned int)last);
            uart_puts(line);
        }
    }
}
```

- [ ] **Step 4: Run the focused boundary guard and baseline verification**

Run: `python3 tools/test_kernel_arch_boundary_phase2.py`
Expected: PASS with `phase2 boundary guard passed`

Run: `python3 tools/test_kernel_layout.py`
Expected: PASS

Run: `python3 tools/test_generate_compile_commands.py`
Expected: PASS

Run: `make kernel`
Expected: PASS

Run: `make ARCH=arm64 build`
Expected: PASS

Run: `make ARCH=arm64 check`
Expected: PASS and `logs/serial-arm.log` contains `tick 5`

- [ ] **Step 5: Commit the Phase 2 integration**

```bash
git add kernel/kernel.c kernel/platform/pc/keyboard.c \
        kernel/platform/pc/mouse.c kernel/arch/arm64/start_kernel.c \
        tools/test_kernel_arch_boundary_phase2.py
git commit -m "kernel: integrate phase2 arch irq boundary"
```
