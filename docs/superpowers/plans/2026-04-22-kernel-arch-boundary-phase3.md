# Kernel Arch Boundary Phase 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce the Phase 3 MM boundary so shared MM/process code stops depending on x86 paging internals, PMM becomes a shared core plus arch-owned memory-map policy, and arm64 gains the first usable MMU and address-space primitives behind the same interface.

**Architecture:** Split the work into five slices: add a focused regression guard, extract a shared PMM core with x86 and arm64 range providers, add an `arch.h` MM API and adapt x86 behind it, migrate shared callers to the normalized mapping descriptor and temporary page-access model, then bring up arm64 MMU and address-space support on that boundary. Keep x86 behavior stable throughout; arm64 only needs buildable MM primitives in this milestone, not full ELF64 or user-entry ABI work.

**Tech Stack:** Freestanding C, small assembly stubs for x86/arm64 MMU control, GNU Make, Python 3, x86 kernel tests, AArch64 cross-builds

---

## File Structure

- Create: `docs/superpowers/plans/2026-04-22-kernel-arch-boundary-phase3.md`
- Create: `tools/test_kernel_arch_boundary_phase3.py`
- Create: `kernel/mm/pmm_core.h`
- Create: `kernel/mm/pmm_core.c`
- Create: `kernel/test/test_pmm_core.c`
- Create: `kernel/arch/arm64/mm/pmm.h`
- Create: `kernel/arch/arm64/mm/pmm.c`
- Create: `kernel/arch/arm64/mm/mmu.h`
- Create: `kernel/arch/arm64/mm/mmu.c`
- Create: `kernel/arch/arm64/mm/temp_map.c`
- Modify: `kernel/tests.mk`
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/arch/x86/arch.c`
- Modify: `kernel/arch/arm64/arch.c`
- Modify: `kernel/arch/x86/mm/pmm.h`
- Modify: `kernel/arch/x86/mm/pmm.c`
- Modify: `kernel/arch/x86/mm/paging.h`
- Modify: `kernel/arch/x86/mm/paging.c`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `kernel/arch/arm64/start_kernel.c`
- Modify: `kernel/arch/arm64/boot.S`
- Modify: `kernel/arch/arm64/linker.ld`
- Modify: `kernel/kernel.c`
- Modify: `kernel/mm/slab.c`
- Modify: `kernel/mm/fault.c`
- Modify: `kernel/proc/uaccess.c`
- Modify: `kernel/proc/syscall/mem.c`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/elf.c`
- Modify: `kernel/proc/mem_forensics.c`
- Modify: `kernel/gui/desktop.c`
- Modify: `kernel/objects.mk`

### Task 1: Add The Phase 3 Regression Guard

**Files:**
- Create: `tools/test_kernel_arch_boundary_phase3.py`

- [ ] **Step 1: Write the failing repository guard**

```python
#!/usr/bin/env python3
"""
Focused regression guard for the Phase 3 architecture/MM boundary.

Shared MM, process, and framebuffer code must stop depending on x86 paging
headers, raw CR3 manipulation, and inline invlpg operations.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_INCLUDES = {
    ROOT / "kernel/mm/fault.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/uaccess.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/syscall/mem.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/process.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/elf.c": ['#include "paging.h"'],
    ROOT / "kernel/proc/mem_forensics.c": ['#include "paging.h"'],
    ROOT / "kernel/gui/desktop.c": ['#include "paging.h"'],
}

FORBIDDEN_PATTERNS = {
    ROOT / "kernel/mm/fault.c": [r"\binvlpg\b", r"\bpaging_entry_"],
    ROOT / "kernel/proc/uaccess.c": [r"\binvlpg\b", r"\bpaging_entry_"],
    ROOT / "kernel/proc/syscall/mem.c": [r"\binvlpg\b", r"\bpaging_entry_"],
    ROOT / "kernel/gui/desktop.c": [r"\bcr3\b", r"\bmov %0, %%cr3\b"],
}

REQUIRED_PATTERNS = {
    ROOT / "kernel/mm/fault.c": [r'#include "arch\.h"', r"\barch_mm_query\b"],
    ROOT / "kernel/proc/uaccess.c": [r'#include "arch\.h"', r"\barch_mm_query\b"],
    ROOT / "kernel/proc/process.c": [r'#include "arch\.h"', r"\barch_aspace_"],
    ROOT / "kernel/gui/desktop.c": [r'#include "arch\.h"', r"\barch_mm_present_begin\b"],
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    for path, needles in FORBIDDEN_INCLUDES.items():
        text = path.read_text()
        for needle in needles:
            if needle in text:
                fail(f"{path.relative_to(ROOT)} still contains {needle}")

    for path, patterns in FORBIDDEN_PATTERNS.items():
        text = path.read_text()
        for pattern in patterns:
            if re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} still matches {pattern}")

    for path, patterns in REQUIRED_PATTERNS.items():
        text = path.read_text()
        for pattern in patterns:
            if not re.search(pattern, text):
                fail(f"{path.relative_to(ROOT)} is missing {pattern}")

    print("phase3 boundary guard passed")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the guard to verify it fails**

Run: `python3 tools/test_kernel_arch_boundary_phase3.py`
Expected: FAIL with a message such as `kernel/mm/fault.c still contains #include "paging.h"`

- [ ] **Step 3: Stage the red guard**

```bash
git add tools/test_kernel_arch_boundary_phase3.py
```

- [ ] **Step 4: Commit the failing test checkpoint**

```bash
git commit -m "test: add phase3 arch mm boundary guard"
```

### Task 2: Split PMM Into A Shared Core Plus Arch Range Providers

**Files:**
- Create: `kernel/mm/pmm_core.h`
- Create: `kernel/mm/pmm_core.c`
- Create: `kernel/test/test_pmm_core.c`
- Create: `kernel/arch/arm64/mm/pmm.h`
- Create: `kernel/arch/arm64/mm/pmm.c`
- Modify: `kernel/tests.mk`
- Modify: `kernel/arch/x86/mm/pmm.h`
- Modify: `kernel/arch/x86/mm/pmm.c`
- Modify: `kernel/objects.mk`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `kernel/kernel.c`

- [ ] **Step 1: Add a focused PMM-core kernel test**

```c
/* kernel/test/test_pmm_core.c */
#include "ktest.h"
#include "pmm_core.h"

static void test_pmm_core_respects_reserved_ranges(ktest_case_t *tc)
{
    static const pmm_range_t usable[] = {
        { .base = 0x00100000u, .length = 0x01000000u },
    };
    static const pmm_range_t reserved[] = {
        { .base = 0x00100000u, .length = 0x00002000u },
        { .base = 0x00200000u, .length = 0x00001000u },
    };
    pmm_core_state_t state;

    pmm_core_init(&state, usable, 1u, reserved, 2u);
    KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00100000u), 255u);
    KTEST_EXPECT_EQ(tc, pmm_core_refcount(&state, 0x00200000u), 255u);
    KTEST_EXPECT_GT(tc, pmm_core_free_page_count(&state), 0u);
}
```

```make
# kernel/tests.mk
KTOBJS  = kernel/test/ktest.o \
           kernel/test/test_pmm.o \
           kernel/test/test_pmm_core.o \
           kernel/test/test_kheap.o \
           kernel/test/test_vfs.o \
           kernel/test/test_process.o \
           kernel/test/test_sched.o \
           kernel/test/test_fs.o \
           kernel/test/test_uaccess.o \
           kernel/test/test_desktop.o \
           kernel/test/test_blkdev.o
```

- [ ] **Step 2: Run the x86 test build to verify the new test is red**

Run: `make test KTEST=1`
Expected: FAIL during compile with `pmm_core.h: No such file or directory`

- [ ] **Step 3: Extract the shared PMM core and make x86 call it**

```c
/* kernel/mm/pmm_core.h */
#ifndef PMM_CORE_H
#define PMM_CORE_H

#include <stdint.h>

#define PAGE_SIZE 4096u
#define PMM_MAX_PAGES 32768u

typedef struct {
    uint32_t base;
    uint32_t length;
} pmm_range_t;

typedef struct {
    uint8_t bitmap[PMM_MAX_PAGES / 8];
    uint8_t refcount[PMM_MAX_PAGES];
} pmm_core_state_t;

void pmm_core_init(pmm_core_state_t *state,
                   const pmm_range_t *usable,
                   uint32_t usable_count,
                   const pmm_range_t *reserved,
                   uint32_t reserved_count);
uint32_t pmm_core_alloc_page(pmm_core_state_t *state);
void pmm_core_free_page(pmm_core_state_t *state, uint32_t phys_addr);
void pmm_core_incref(pmm_core_state_t *state, uint32_t phys_addr);
void pmm_core_decref(pmm_core_state_t *state, uint32_t phys_addr);
uint8_t pmm_core_refcount(const pmm_core_state_t *state, uint32_t phys_addr);
uint32_t pmm_core_free_page_count(const pmm_core_state_t *state);

#endif
```

```c
/* kernel/arch/x86/mm/pmm.c */
#include "pmm.h"
#include "pmm_core.h"

static pmm_core_state_t g_pmm;

void pmm_init(multiboot_info_t *mbi)
{
    pmm_range_t usable[32];
    pmm_range_t reserved[32];
    uint32_t usable_count = 0;
    uint32_t reserved_count = 0;

    x86_pmm_collect_usable_ranges(mbi, usable, &usable_count);
    x86_pmm_collect_reserved_ranges(mbi, reserved, &reserved_count);
    pmm_core_init(&g_pmm, usable, usable_count, reserved, reserved_count);
}

uint32_t pmm_alloc_page(void) { return pmm_core_alloc_page(&g_pmm); }
void pmm_free_page(uint32_t phys_addr) { pmm_core_free_page(&g_pmm, phys_addr); }
void pmm_incref(uint32_t phys_addr) { pmm_core_incref(&g_pmm, phys_addr); }
void pmm_decref(uint32_t phys_addr) { pmm_core_decref(&g_pmm, phys_addr); }
uint8_t pmm_refcount(uint32_t phys_addr) { return pmm_core_refcount(&g_pmm, phys_addr); }
uint32_t pmm_free_page_count(void) { return pmm_core_free_page_count(&g_pmm); }
```

```c
/* kernel/arch/arm64/mm/pmm.c */
#include "pmm.h"
#include "pmm_core.h"

static pmm_core_state_t g_pmm;

void pmm_init(void)
{
    static const pmm_range_t usable[] = {
        { .base = 0x00080000u, .length = 0x07f80000u },
    };
    static const pmm_range_t reserved[] = {
        { .base = 0x00000000u, .length = 0x00080000u },
        { .base = 0x3f000000u, .length = 0x01000000u },
    };

    pmm_core_init(&g_pmm, usable, 1u, reserved, 2u);
}
```

- [ ] **Step 4: Run focused PMM checks and baseline x86 build**

Run: `make check`
Expected: PASS, including the new `test_pmm_core` suite

Run: `make kernel`
Expected: PASS

- [ ] **Step 5: Commit the PMM split**

```bash
git add kernel/mm/pmm_core.h kernel/mm/pmm_core.c kernel/test/test_pmm_core.c \
        kernel/tests.mk kernel/arch/x86/mm/pmm.h kernel/arch/x86/mm/pmm.c \
        kernel/arch/arm64/mm/pmm.h kernel/arch/arm64/mm/pmm.c \
        kernel/objects.mk kernel/arch/arm64/arch.mk kernel/kernel.c
git commit -m "kernel: split shared pmm core"
```

### Task 3: Add The Shared MM Boundary And Adapt x86 Behind It

**Files:**
- Modify: `kernel/arch/arch.h`
- Modify: `kernel/arch/x86/arch.c`
- Modify: `kernel/arch/x86/mm/paging.h`
- Modify: `kernel/arch/x86/mm/paging.c`

- [ ] **Step 1: Extend `arch.h` with the MM types and operations**

```c
/* kernel/arch/arch.h */
typedef uintptr_t arch_aspace_t;

typedef struct {
    uint64_t phys_addr;
    uint32_t flags;
} arch_mm_mapping_t;

#define ARCH_MM_MAP_PRESENT   0x0001u
#define ARCH_MM_MAP_READ      0x0002u
#define ARCH_MM_MAP_WRITE     0x0004u
#define ARCH_MM_MAP_EXEC      0x0008u
#define ARCH_MM_MAP_USER      0x0010u
#define ARCH_MM_MAP_COW       0x0020u

void arch_mm_init(void);
arch_aspace_t arch_aspace_kernel(void);
arch_aspace_t arch_aspace_create(void);
arch_aspace_t arch_aspace_clone(arch_aspace_t src);
void arch_aspace_switch(arch_aspace_t aspace);
void arch_aspace_destroy(arch_aspace_t aspace);
int arch_mm_map(arch_aspace_t aspace, uintptr_t virt, uint64_t phys, uint32_t flags);
int arch_mm_unmap(arch_aspace_t aspace, uintptr_t virt);
int arch_mm_query(arch_aspace_t aspace, uintptr_t virt, arch_mm_mapping_t *out);
int arch_mm_update(arch_aspace_t aspace, uintptr_t virt, uint32_t clear_flags, uint32_t set_flags);
void arch_mm_invalidate_page(arch_aspace_t aspace, uintptr_t virt);
void *arch_page_temp_map(uint64_t phys_addr);
void arch_page_temp_unmap(void *ptr);
uint32_t arch_mm_present_begin(void);
void arch_mm_present_end(uint32_t state);
```

- [ ] **Step 2: Add a red compile checkpoint for the new API**

Run: `make kernel`
Expected: FAIL with missing symbols such as `arch_mm_init` or `arch_mm_query`

- [ ] **Step 3: Implement the x86 adapter as wrappers over existing paging code**

```c
/* kernel/arch/x86/arch.c */
#include "../arch.h"
#include "mm/paging.h"

void arch_mm_init(void)
{
    paging_init();
}

arch_aspace_t arch_aspace_kernel(void)
{
    return (arch_aspace_t)PAGE_DIR_ADDR;
}

arch_aspace_t arch_aspace_create(void)
{
    return (arch_aspace_t)paging_create_user_space();
}

arch_aspace_t arch_aspace_clone(arch_aspace_t src)
{
    return (arch_aspace_t)paging_clone_user_space((uint32_t)src);
}

void arch_aspace_switch(arch_aspace_t aspace)
{
    paging_switch_directory((uint32_t)aspace);
}
```

```c
/* kernel/arch/x86/mm/paging.h */
int paging_query_page(uint32_t pd_phys, uint32_t virt, arch_mm_mapping_t *out);
int paging_update_page(uint32_t pd_phys,
                       uint32_t virt,
                       uint32_t clear_flags,
                       uint32_t set_flags);
void *paging_temp_map(uint32_t phys_addr);
void paging_temp_unmap(void *ptr);
uint32_t paging_present_begin(void);
void paging_present_end(uint32_t flags);
```

```c
/* kernel/arch/x86/mm/paging.c */
int paging_query_page(uint32_t pd_phys, uint32_t virt, arch_mm_mapping_t *out)
{
    uint32_t *pte;
    if (paging_walk(pd_phys, virt, &pte) != 0)
        return -1;

    out->phys_addr = paging_entry_addr(*pte);
    out->flags = ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ;
    if (*pte & PG_WRITABLE)
        out->flags |= ARCH_MM_MAP_WRITE;
    if (*pte & PG_USER)
        out->flags |= ARCH_MM_MAP_USER;
    if (*pte & PG_COW)
        out->flags |= ARCH_MM_MAP_COW;
    return 0;
}
```

- [ ] **Step 4: Re-run x86 build and the Phase 3 guard**

Run: `make kernel`
Expected: PASS

Run: `python3 tools/test_kernel_arch_boundary_phase3.py`
Expected: still FAIL, now on shared callers that have not been migrated yet

- [ ] **Step 5: Commit the x86 MM boundary scaffold**

```bash
git add kernel/arch/arch.h kernel/arch/x86/arch.c \
        kernel/arch/x86/mm/paging.h kernel/arch/x86/mm/paging.c
git commit -m "kernel: add phase3 arch mm boundary"
```

### Task 4: Migrate Shared MM, Process, And Framebuffer Callers

**Files:**
- Modify: `kernel/kernel.c`
- Modify: `kernel/mm/slab.c`
- Modify: `kernel/mm/fault.c`
- Modify: `kernel/proc/uaccess.c`
- Modify: `kernel/proc/syscall/mem.c`
- Modify: `kernel/proc/process.c`
- Modify: `kernel/proc/elf.c`
- Modify: `kernel/proc/mem_forensics.c`
- Modify: `kernel/gui/desktop.c`

- [ ] **Step 1: Change `fault.c` and `uaccess.c` to use the normalized mapping descriptor**

```c
/* kernel/mm/fault.c */
#include "arch.h"

static int fault_break_cow(arch_aspace_t aspace, uint32_t fault_page)
{
    arch_mm_mapping_t mapping;
    uint32_t new_phys;
    void *dst_page;
    void *src_page;

    if (arch_mm_query(aspace, fault_page, &mapping) != 0)
        return -1;
    if ((mapping.flags & ARCH_MM_MAP_COW) == 0)
        return -1;

    new_phys = pmm_alloc_page();
    if (!new_phys)
        return -1;

    dst_page = arch_page_temp_map(new_phys);
    src_page = arch_page_temp_map((uint32_t)mapping.phys_addr);
    k_memcpy(dst_page, src_page, PAGE_SIZE);
    arch_page_temp_unmap(src_page);
    arch_page_temp_unmap(dst_page);
    if (arch_mm_map(aspace, fault_page, new_phys,
                    ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
                        ARCH_MM_MAP_WRITE | ARCH_MM_MAP_USER) != 0) {
        pmm_free_page(new_phys);
        return -1;
    }
    pmm_decref((uint32_t)mapping.phys_addr);
    return 0;
}
```

```c
/* kernel/proc/uaccess.c */
static int uaccess_translate(process_t *proc,
                             uint32_t user_addr,
                             int write_access,
                             uint8_t **kptr_out)
{
    arch_mm_mapping_t mapping;
    void *page_ptr;

    if (arch_mm_query(proc->aspace, user_addr & ~0xFFFu, &mapping) != 0)
        return -1;
    if ((mapping.flags & ARCH_MM_MAP_USER) == 0)
        return -1;

    page_ptr = arch_page_temp_map(mapping.phys_addr);
    *kptr_out = (uint8_t *)page_ptr + (user_addr & 0xFFFu);
    return 0;
}
```

- [ ] **Step 2: Migrate process, ELF, and syscall memory paths off raw page directories**

```c
/* kernel/proc/process.c */
proc->aspace = arch_aspace_create();
if (!proc->aspace)
    return -1;

if (arch_mm_map(proc->aspace, USER_STACK_TOP - PAGE_SIZE, phys,
                ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
                    ARCH_MM_MAP_WRITE | ARCH_MM_MAP_USER) != 0) {
    pmm_free_page(phys);
    arch_aspace_destroy(proc->aspace);
    return -1;
}
```

```c
/* kernel/proc/syscall/mem.c */
if (arch_mm_update(cur->aspace, page, ARCH_MM_MAP_WRITE,
                   (prot & LINUX_PROT_WRITE) ? ARCH_MM_MAP_WRITE : 0u) != 0)
    return (uint32_t)-1;
```

```c
/* kernel/proc/elf.c */
void *page = arch_page_temp_map(phys);
k_memset(page, 0, PAGE_SIZE);
arch_page_temp_unmap(page);
```

- [ ] **Step 3: Remove direct CR3 manipulation from framebuffer presentation**

```c
/* kernel/gui/desktop.c */
static uint32_t desktop_framebuffer_present_begin(void)
{
    return arch_mm_present_begin();
}

static void desktop_framebuffer_present_end(uint32_t state)
{
    arch_mm_present_end(state);
}
```

- [ ] **Step 4: Run the focused guard and x86 regression suite**

Run: `python3 tools/test_kernel_arch_boundary_phase3.py`
Expected: PASS

Run: `make check`
Expected: PASS

- [ ] **Step 5: Commit the shared-caller migration**

```bash
git add kernel/kernel.c kernel/mm/slab.c kernel/mm/fault.c \
        kernel/proc/uaccess.c kernel/proc/syscall/mem.c \
        kernel/proc/process.c kernel/proc/elf.c kernel/proc/mem_forensics.c \
        kernel/gui/desktop.c tools/test_kernel_arch_boundary_phase3.py
git commit -m "kernel: migrate shared mm callers to arch boundary"
```

### Task 5: Bring Up arm64 MMU And Address-Space Primitives

**Files:**
- Create: `kernel/arch/arm64/mm/mmu.h`
- Create: `kernel/arch/arm64/mm/mmu.c`
- Create: `kernel/arch/arm64/mm/temp_map.c`
- Modify: `kernel/arch/arm64/arch.c`
- Modify: `kernel/arch/arm64/arch.mk`
- Modify: `kernel/arch/arm64/start_kernel.c`
- Modify: `kernel/arch/arm64/boot.S`
- Modify: `kernel/arch/arm64/linker.ld`

- [ ] **Step 1: Add the red arm64 build checkpoint**

Run: `make ARCH=arm64 build`
Expected: FAIL with missing objects or undefined references such as `arch_mm_init`

- [ ] **Step 2: Add arm64 MMU bootstrap and address-space helpers**

```c
/* kernel/arch/arm64/mm/mmu.h */
#ifndef ARM64_MMU_H
#define ARM64_MMU_H

#include <stdint.h>
#include "../../arch.h"

void arm64_mmu_init(void);
arch_aspace_t arm64_aspace_kernel(void);
arch_aspace_t arm64_aspace_create(void);
arch_aspace_t arm64_aspace_clone(arch_aspace_t src);
void arm64_aspace_switch(arch_aspace_t aspace);
void arm64_aspace_destroy(arch_aspace_t aspace);
int arm64_mm_map(arch_aspace_t aspace, uintptr_t virt, uint64_t phys, uint32_t flags);
int arm64_mm_unmap(arch_aspace_t aspace, uintptr_t virt);
int arm64_mm_query(arch_aspace_t aspace, uintptr_t virt, arch_mm_mapping_t *out);
int arm64_mm_update(arch_aspace_t aspace,
                    uintptr_t virt,
                    uint32_t clear_flags,
                    uint32_t set_flags);
void arm64_tlbi_page(arch_aspace_t aspace, uintptr_t virt);

#endif
```

```c
/* kernel/arch/arm64/arch.c */
#include "mm/mmu.h"

void arch_mm_init(void) { arm64_mmu_init(); }
arch_aspace_t arch_aspace_kernel(void) { return arm64_aspace_kernel(); }
arch_aspace_t arch_aspace_create(void) { return arm64_aspace_create(); }
arch_aspace_t arch_aspace_clone(arch_aspace_t src) { return arm64_aspace_clone(src); }
void arch_aspace_switch(arch_aspace_t aspace) { arm64_aspace_switch(aspace); }
void arch_aspace_destroy(arch_aspace_t aspace) { arm64_aspace_destroy(aspace); }
int arch_mm_map(arch_aspace_t aspace, uintptr_t virt, uint64_t phys, uint32_t flags)
{
    return arm64_mm_map(aspace, virt, phys, flags);
}
```

```c
/* kernel/arch/arm64/mm/mmu.c */
void arm64_mmu_init(void)
{
    arm64_build_bootstrap_tables();
    arm64_program_mair_tcr_ttbr();
    arm64_enable_mmu_and_caches();
}
```

- [ ] **Step 3: Wire arm64 early boot to initialize PMM and MMU before shared callers need them**

```c
/* kernel/arch/arm64/start_kernel.c */
void arm64_start_kernel(void)
{
    uart_init();
    __asm__ volatile("msr vbar_el1, %0" : : "r"(vectors_el1));
    __asm__ volatile("isb");

    pmm_init();
    arch_mm_init();
    arch_irq_init();
    arch_timer_set_periodic_handler(arm64_heartbeat_tick);
    arch_timer_start(10u);
    arch_interrupts_enable();
    for (;;)
        __asm__ volatile("wfi");
}
```

```make
# kernel/arch/arm64/arch.mk
ARM_KOBJS := kernel/arch/arm64/boot.o \
             kernel/arch/arm64/arch.o \
             kernel/arch/arm64/mm/pmm.o \
             kernel/arch/arm64/mm/mmu.o \
             kernel/arch/arm64/mm/temp_map.o \
             kernel/arch/arm64/exceptions.o \
             kernel/arch/arm64/exceptions_s.o \
             kernel/arch/arm64/irq.o \
             kernel/arch/arm64/timer.o \
             kernel/arch/arm64/uart.o \
             kernel/arch/arm64/start_kernel.o \
             kernel/lib/kprintf.arm64.o \
             kernel/lib/kstring.arm64.o
```

- [ ] **Step 4: Run full verification for x86 and arm64**

Run: `python3 tools/test_kernel_arch_boundary_phase3.py`
Expected: PASS

Run: `python3 tools/test_kernel_layout.py`
Expected: PASS

Run: `python3 tools/test_generate_compile_commands.py`
Expected: PASS

Run: `make kernel`
Expected: PASS

Run: `make check`
Expected: PASS

Run: `make ARCH=arm64 build`
Expected: PASS

- [ ] **Step 5: Commit the arm64 MMU bring-up**

```bash
git add kernel/arch/arm64/mm/pmm.h kernel/arch/arm64/mm/pmm.c \
        kernel/arch/arm64/mm/mmu.h kernel/arch/arm64/mm/mmu.c \
        kernel/arch/arm64/mm/temp_map.c kernel/arch/arm64/arch.c \
        kernel/arch/arm64/arch.mk kernel/arch/arm64/start_kernel.c \
        kernel/arch/arm64/boot.S kernel/arch/arm64/linker.ld
git commit -m "kernel: bring up arm64 mmu primitives"
```

## Self-Review

- Spec coverage: the plan covers the regression guard, shared PMM core, normalized mapping descriptor, shared-caller migration, framebuffer CR3 cleanup, and arm64 MMU/address-space bring-up from the approved Phase 3 spec.
- Placeholder scan: there are no `TODO`, `TBD`, or “implement later” placeholders; each task has concrete files, commands, and commit points.
- Type consistency: the plan uses `arch_aspace_t`, `arch_mm_mapping_t`, `arch_mm_query()`, `arch_mm_map()`, `arch_mm_update()`, `arch_page_temp_map()`, and `arch_mm_present_begin()/end()` consistently across the boundary and the caller-migration tasks.
