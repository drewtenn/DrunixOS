# Linux-Like Memory Map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move Drunix toward Linux-style virtual memory: low user executables, upward `brk`, high downward `mmap`, high grow-down stack, higher-half kernel mappings, dynamic physical memory accounting, and Linux-compatible lazy mapping behavior.

**Architecture:** Do this in staged, bootable checkpoints. First centralize VM layout policy and add tests around current behavior, then improve VMA metadata and placement, then raise x86 physical-memory support, then move the kernel mapping model toward a 3G/1G Linux i386 split. File-backed mappings and user allocator policy become more Linux-like only after the VMA model can represent them.

**Tech Stack:** C, freestanding x86 paging, arm64 layout constants, Drunix PMM/VMA/process subsystems, KTEST, QEMU smoke tests, `/proc/<pid>/maps`.

---

## Current State

Relevant files:

- `kernel/arch/x86/arch_layout.h`: x86 virtual layout constants. Current user base follows the low direct map at `0x10000000`.
- `kernel/arch/arm64/arch_layout.h`: arm64 virtual layout constants.
- `kernel/proc/process.h`: exported user stack, heap, mmap, and process resource structures.
- `kernel/proc/process.c`: process creation, initial image/heap/stack VMA setup.
- `kernel/mm/vma.h` and `kernel/mm/vma.c`: fixed-size sorted VMA table and placement policy.
- `kernel/mm/fault.c`: lazy anonymous heap, mmap, stack, and COW fault handling.
- `kernel/proc/syscall/mem.c`: `brk`, `mmap`, `mmap2`, `munmap`, and `mprotect`.
- `kernel/arch/x86/mm/paging.c`: x86 kernel direct map and user page directory construction.
- `kernel/arch/x86/mm/pmm.c` and `kernel/mm/pmm_core.*`: physical page discovery and PMM cap.
- `user/runtime/malloc.c`: first-fit allocator backed only by `SYS_BRK`.
- `kernel/proc/mem_forensics.*`: `/proc/<pid>/maps`, vmstat, and core-dump memory notes.
- `docs/ch08-memory-management.md`, `docs/ch15-processes.md`, `docs/ch25-demand-paging.md`: memory-management docs.

Linux convergence target for x86:

- User task size: `0xC0000000` for a 3G user / 1G kernel split.
- User ELF base: restore normal i386 Linux-compatible low base such as `0x08048000` or the repo's linked base if already fixed by linker scripts.
- Kernel virtual base: `0xC0000000`, with physical memory reachable through a higher-half direct map instead of requiring user processes to avoid low physical identity mappings.
- Heap: `brk` starts at page-rounded image end and grows upward.
- Mmap: anonymous and file mappings are placed top-down below the stack area, with a guard gap and VMA collision checks.
- Stack: grows down from the top of task size with a guard region.
- Physical memory: PMM manages detected RAM based on boot memory maps, not a fixed 256 MiB compile-time ceiling.

Non-goals for the first pass:

- Full ASLR.
- Swap.
- NUMA.
- Linux `MAP_SHARED` writeback semantics.
- 64-bit x86 long mode.

---

### Task 1: Freeze Current VM Behavior With Tests

**Files:**

- Modify: `kernel/arch/x86/test/test_process.c`
- Modify: `kernel/arch/x86/test/test_arch_x86.c`
- Modify: `kernel/test/test_pmm_core.c`
- Test: `kernel/arch/x86/test/test_process.c`

- [ ] **Step 1: Add explicit layout invariant tests**

Add these cases near the existing VMA tests in `kernel/arch/x86/test/test_process.c`:

```c
static void test_x86_user_layout_invariants(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(tc, USER_STACK_TOP, 0xC0000000u);
	KTEST_EXPECT_TRUE(tc, USER_STACK_BASE < USER_STACK_TOP);
	KTEST_EXPECT_TRUE(tc, USER_MMAP_MIN < USER_STACK_BASE);
	KTEST_EXPECT_TRUE(tc, USER_HEAP_MAX <= USER_STACK_BASE);
	KTEST_EXPECT_TRUE(tc, ARCH_USER_IMAGE_BASE < USER_MMAP_MIN);
}

static void test_brk_refuses_stack_collision(ktest_case_t *tc)
{
	process_t proc;
	process_t *saved = sched_current();

	k_memset(&proc, 0, sizeof(proc));
	init_vma_proc(&proc);
	proc.as = 0;
	sched_set_current_for_test(&proc);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_BRK, USER_STACK_BASE + PAGE_SIZE, 0, 0, 0, 0, 0),
	    proc.brk);

	sched_set_current_for_test(saved);
}
```

Register both cases in the `ktest_case_t` array.

- [ ] **Step 2: Run tests and verify baseline behavior**

Run:

```bash
make check-kernel-unit
```

Expected: all current kernel unit tests pass. If either new test fails, record the actual current layout in this plan before changing behavior.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86/test/test_process.c kernel/arch/x86/test/test_arch_x86.c kernel/test/test_pmm_core.c
git commit -m "test: lock current vm layout invariants"
```

---

### Task 2: Centralize User VM Layout Policy

**Files:**

- Create: `kernel/mm/vm_layout.h`
- Modify: `kernel/arch/x86/arch_layout.h`
- Modify: `kernel/arch/arm64/arch_layout.h`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/mm/vma.c`
- Test: `kernel/arch/x86/test/test_process.c`

- [ ] **Step 1: Create architecture-neutral layout names**

Create `kernel/mm/vm_layout.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef VM_LAYOUT_H
#define VM_LAYOUT_H

#include "arch_layout.h"
#include <stdint.h>

#define VM_TASK_SIZE ((uint32_t)ARCH_USER_VADDR_MAX)
#define VM_USER_BASE ((uint32_t)ARCH_USER_VADDR_MIN)
#define VM_USER_IMAGE_BASE ((uint32_t)ARCH_USER_IMAGE_BASE)
#define VM_STACK_TOP ((uint32_t)ARCH_USER_STACK_TOP)
#define VM_STACK_BASE ((uint32_t)ARCH_USER_STACK_BASE)
#define VM_HEAP_MAX ((uint32_t)ARCH_USER_HEAP_MAX)
#define VM_MMAP_MIN ((uint32_t)ARCH_USER_MMAP_MIN)

/*
 * Keep a gap between top-down mmap regions and the grow-down stack.
 * Linux has a larger configurable guard gap; Drunix starts with 1 MiB
 * because current process address spaces are small and testable.
 */
#define VM_STACK_GUARD_GAP (1024u * 1024u)

static inline uint32_t vm_page_align_down(uint32_t value)
{
	return value & ~(uint32_t)0xFFFu;
}

static inline uint32_t vm_page_align_up(uint32_t value)
{
	return (value + 0xFFFu) & ~(uint32_t)0xFFFu;
}

#endif
```

- [ ] **Step 2: Replace process-level aliases**

In `kernel/proc/process.h`, include `vm_layout.h` and keep the old names as compatibility aliases:

```c
#include "vm_layout.h"

#define USER_STACK_PAGES 4
#define USER_STACK_MAX_PAGES 64u
#define USER_STACK_TOP VM_STACK_TOP
#define USER_STACK_BASE VM_STACK_BASE
#define USER_HEAP_MAX VM_HEAP_MAX
#define USER_MMAP_MIN VM_MMAP_MIN
```

- [ ] **Step 3: Use layout helpers in VMA placement**

In `kernel/mm/vma.c`, replace local page alignment expressions in `vma_map_anonymous()` with:

```c
len = vm_page_align_up(length);
low = vm_page_align_up(vma_brk_const(proc));
if (low < VM_MMAP_MIN)
	low = VM_MMAP_MIN;
high = stack_vma->start;
if (high > VM_STACK_GUARD_GAP)
	high -= VM_STACK_GUARD_GAP;
```

Add `#include "vm_layout.h"` at the top.

- [ ] **Step 4: Run tests**

Run:

```bash
make check-kernel-unit
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/vm_layout.h kernel/arch/x86/arch_layout.h kernel/arch/arm64/arch_layout.h kernel/proc/process.h kernel/mm/vma.c kernel/arch/x86/test/test_process.c
git commit -m "refactor: centralize user vm layout policy"
```

---

### Task 3: Make VMA Records Linux-Like Enough For File Faults

**Files:**

- Modify: `kernel/mm/vma.h`
- Modify: `kernel/mm/vma.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/resources.c`
- Modify: `kernel/proc/mem_forensics.c`
- Test: `kernel/arch/x86/test/test_process.c`

- [ ] **Step 1: Extend VMA metadata**

Update `vm_area_t` in `kernel/mm/vma.h`:

```c
typedef struct {
	uint32_t start;
	uint32_t end;
	uint32_t flags;
	uint32_t kind;
	uint32_t file_offset;
	uint32_t file_size;
	vfs_file_ref_t file_ref;
} vm_area_t;
```

Add `#include "vfs.h"` above the struct definition.

- [ ] **Step 2: Add a file-backed VMA creation API**

Add to `kernel/mm/vma.h`:

```c
int vma_map_file_private(struct process *proc,
                         uint32_t hint,
                         uint32_t length,
                         uint32_t flags,
                         vfs_file_ref_t file_ref,
                         uint32_t file_offset,
                         uint32_t file_size,
                         uint32_t *addr_out);
```

Implement in `kernel/mm/vma.c` by calling the same top-down gap finder as `vma_map_anonymous()`, then filling `file_ref`, `file_offset`, and `file_size` on the inserted VMA. Factor the current gap-finding code into:

```c
static int vma_find_topdown_gap(struct process *proc,
                                uint32_t hint,
                                uint32_t length,
                                uint32_t *addr_out)
```

The helper must return page-aligned addresses and must apply `VM_STACK_GUARD_GAP`.

- [ ] **Step 3: Preserve VMA metadata across fork/clone**

No special deep-copy code is needed because `proc_resource_clone_for_fork()` copies `proc_address_space_t` by value. Add a test that validates a file-backed VMA keeps `file_offset` and `file_size` after copying:

```c
static void test_vma_file_metadata_survives_address_space_copy(ktest_case_t *tc)
{
	process_t parent;
	process_t child;
	vfs_file_ref_t ref;
	uint32_t addr = 0;

	k_memset(&parent, 0, sizeof(parent));
	k_memset(&child, 0, sizeof(child));
	k_memset(&ref, 0, sizeof(ref));
	init_vma_proc(&parent);

	KTEST_ASSERT_EQ(tc,
	                vma_map_file_private(&parent,
	                                     0,
	                                     PAGE_SIZE,
	                                     VMA_FLAG_READ | VMA_FLAG_PRIVATE,
	                                     ref,
	                                     0x2000u,
	                                     0x1234u,
	                                     &addr),
	                0);

	k_memcpy(child.vmas, parent.vmas, sizeof(parent.vmas));
	child.vma_count = parent.vma_count;

	KTEST_ASSERT_NOT_NULL(tc, vma_find(&child, addr));
	KTEST_EXPECT_EQ(tc, vma_find(&child, addr)->file_offset, 0x2000u);
	KTEST_EXPECT_EQ(tc, vma_find(&child, addr)->file_size, 0x1234u);
}
```

- [ ] **Step 4: Run tests**

Run:

```bash
make check-kernel-unit
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/vma.h kernel/mm/vma.c kernel/proc/process.h kernel/proc/resources.c kernel/proc/mem_forensics.c kernel/arch/x86/test/test_process.c
git commit -m "feat: track file backing in vm areas"
```

---

### Task 4: Convert Private File `mmap` To Lazy Faults

**Files:**

- Modify: `kernel/proc/syscall/mem.c`
- Modify: `kernel/mm/fault.c`
- Modify: `kernel/mm/vma.h`
- Test: `kernel/arch/x86/test/test_process.c`
- Test: `user/apps/linuxabi.c`

- [ ] **Step 1: Change private file mmap to reserve only**

In `syscall_mmap_private_file()` in `kernel/proc/syscall/mem.c`, remove the eager page allocation/read loop. Replace it with:

```c
vma_flags = prot_to_vma_flags(prot) & ~(uint32_t)VMA_FLAG_ANON;
if (vma_map_file_private(cur,
                         hint,
                         map_len,
                         vma_flags,
                         fh->u.file.ref,
                         file_offset,
                         fh->u.file.size,
                         &map_addr) != 0)
	return -1;

*addr_out = map_addr;
return 0;
```

Keep the existing validation for fd type, read permission, offset alignment, and zero length.

- [ ] **Step 2: Add lazy file fault handling**

In `kernel/mm/fault.c`, add:

```c
static int fault_handle_private_file_fault(arch_aspace_t aspace,
                                           uint32_t cr2,
                                           const vm_area_t *vma)
{
	uint32_t fault_page = cr2 & ~0xFFFu;
	uint32_t page_offset = fault_page - vma->start;
	uint32_t file_offset = vma->file_offset + page_offset;
	uint32_t phys = pmm_alloc_page();
	void *page;
	int n;
	uint32_t map_flags = ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
	                     ARCH_MM_MAP_USER;

	if (!phys)
		return -1;
	page = arch_page_temp_map(phys);
	if (!page) {
		pmm_free_page(phys);
		return -1;
	}

	k_memset(page, 0, PAGE_SIZE);
	if (file_offset < vma->file_size) {
		uint32_t readable = vma->file_size - file_offset;
		if (readable > PAGE_SIZE)
			readable = PAGE_SIZE;
		n = vfs_read(vma->file_ref, file_offset, (uint8_t *)page, readable);
		if (n < 0) {
			arch_page_temp_unmap(page);
			pmm_free_page(phys);
			return -1;
		}
	}
	arch_page_temp_unmap(page);

	if (vma->flags & VMA_FLAG_WRITE)
		map_flags |= ARCH_MM_MAP_WRITE;
	if (arch_mm_map(aspace, fault_page, phys, map_flags) != 0) {
		pmm_free_page(phys);
		return -1;
	}
	return 0;
}
```

Add `#include "vfs.h"` if needed. In `fault_handle_lazy_anon_fault()`, dispatch non-anonymous private file VMAs to this function before rejecting them.

- [ ] **Step 3: Add user ABI coverage**

Extend `user/apps/linuxabi.c` around the existing private file `mmap` checks:

```c
check_ok("mmap private file first byte lazy fault",
         ((volatile char *)addr)[0] == 'H' ? 1 : -1);
check_ok("mmap private file second page lazy zero fill",
         ((volatile char *)addr)[4095] == 0 ? 1 : -1);
```

- [ ] **Step 4: Run tests**

Run:

```bash
make check-kernel-unit
make NO_DESKTOP=1 KTEST=0 kernel disk run-x86-smoke
```

Expected: kernel unit tests pass and the Linux ABI smoke app still reports private file mmap success.

- [ ] **Step 5: Commit**

```bash
git add kernel/proc/syscall/mem.c kernel/mm/fault.c kernel/mm/vma.h user/apps/linuxabi.c kernel/arch/x86/test/test_process.c
git commit -m "feat: fault in private file mappings lazily"
```

---

### Task 5: Replace Fixed VMA Array With Growable VMA Storage

**Files:**

- Modify: `kernel/mm/vma.h`
- Modify: `kernel/mm/vma.c`
- Modify: `kernel/proc/process.h`
- Modify: `kernel/proc/resources.c`
- Modify: `kernel/proc/mem_forensics.c`
- Test: `kernel/arch/x86/test/test_process.c`

- [ ] **Step 1: Introduce owned VMA table state**

Replace the fixed array fields in `proc_address_space_t` with:

```c
vm_area_t *vmas;
uint32_t vma_count;
uint32_t vma_capacity;
```

Keep the legacy `process_t::vmas` fields during this task only for tests that construct stack-local processes without `proc_resource_init_fresh()`. Add a helper in `vma.c` that falls back to `proc->vmas` when `proc->as == 0`.

- [ ] **Step 2: Allocate default VMA capacity**

In `proc_resource_init_fresh()` after allocating `proc->as`, allocate:

```c
proc->as->vma_capacity = 32u;
proc->as->vmas =
    (vm_area_t *)alloc_zero(sizeof(vm_area_t) * proc->as->vma_capacity);
if (!proc->as->vmas) {
	proc_resource_put_all(proc);
	return -1;
}
```

In `proc_resource_put_all()`, free `proc->as->vmas` before freeing `proc->as`.

- [ ] **Step 3: Grow VMA capacity**

In `kernel/mm/vma.c`, add:

```c
static int vma_ensure_capacity(struct process *proc, uint32_t needed)
{
	vm_area_t *next;
	uint32_t next_cap;

	if (!proc || !proc->as)
		return needed <= PROCESS_MAX_VMAS ? 0 : -1;
	if (proc->as->vma_capacity >= needed)
		return 0;

	next_cap = proc->as->vma_capacity ? proc->as->vma_capacity : 32u;
	while (next_cap < needed)
		next_cap *= 2u;

	next = (vm_area_t *)kmalloc(sizeof(vm_area_t) * next_cap);
	if (!next)
		return -1;
	k_memset(next, 0, sizeof(vm_area_t) * next_cap);
	k_memcpy(next, proc->as->vmas, sizeof(vm_area_t) * proc->as->vma_count);
	kfree(proc->as->vmas);
	proc->as->vmas = next;
	proc->as->vma_capacity = next_cap;
	return 0;
}
```

Call `vma_ensure_capacity(proc, *count + 1)` before inserting or splitting VMAs.

- [ ] **Step 4: Test more than 16 VMAs**

Add:

```c
static void test_vma_table_grows_beyond_legacy_limit(ktest_case_t *tc)
{
	process_t proc;
	uint32_t addr;

	k_memset(&proc, 0, sizeof(proc));
	KTEST_ASSERT_EQ(tc, proc_resource_init_fresh(&proc), 0);
	init_vma_proc(&proc);
	proc.as->brk = ARCH_USER_IMAGE_BASE + PAGE_SIZE;
	KTEST_ASSERT_EQ(tc,
	                vma_add(&proc,
	                        proc.as->brk,
	                        proc.as->brk,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE,
	                        VMA_KIND_HEAP),
	                0);
	KTEST_ASSERT_EQ(tc,
	                vma_add(&proc,
	                        USER_STACK_BASE,
	                        USER_STACK_TOP,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
	                        VMA_KIND_STACK),
	                0);

	for (uint32_t i = 0; i < 24u; i++) {
		KTEST_ASSERT_EQ(tc,
		                vma_map_anonymous(&proc,
		                                  0,
		                                  PAGE_SIZE,
		                                  VMA_FLAG_READ | VMA_FLAG_ANON |
		                                      VMA_FLAG_PRIVATE,
		                                  &addr),
		                0);
	}
	KTEST_EXPECT_TRUE(tc, proc.as->vma_count > PROCESS_MAX_VMAS);
	proc_resource_put_all(&proc);
}
```

- [ ] **Step 5: Run tests and commit**

Run:

```bash
make check-kernel-unit
```

Commit:

```bash
git add kernel/mm/vma.h kernel/mm/vma.c kernel/proc/process.h kernel/proc/resources.c kernel/proc/mem_forensics.c kernel/arch/x86/test/test_process.c
git commit -m "feat: grow process vma tables dynamically"
```

---

### Task 6: Make PMM Capacity Runtime-Driven

**Files:**

- Modify: `kernel/mm/pmm_core.h`
- Modify: `kernel/mm/pmm_core.c`
- Modify: `kernel/arch/x86/mm/pmm.c`
- Modify: `kernel/arch/arm64/mm/pmm.c`
- Modify: `kernel/test/test_pmm_core.c`
- Modify: `kernel/arch/x86/test/test_pmm.c`

- [ ] **Step 1: Split PMM metadata from fixed storage**

Change `pmm_core_state_t` in `kernel/mm/pmm_core.h`:

```c
typedef struct {
	uint8_t *bitmap;
	uint8_t *refcounts;
	uint32_t max_pages;
} pmm_core_state_t;
```

Change APIs that currently assume `PMM_MAX_PAGES` to use `state->max_pages`.

- [ ] **Step 2: Add explicit PMM storage initialization**

Add to `kernel/mm/pmm_core.h`:

```c
void pmm_core_bind_storage(pmm_core_state_t *state,
                           void *bitmap,
                           void *refcounts,
                           uint32_t max_pages);
uint32_t pmm_core_bitmap_bytes(uint32_t max_pages);
uint32_t pmm_core_refcount_bytes(uint32_t max_pages);
```

Implementation rules:

- `pmm_core_bitmap_bytes(max_pages)` returns `(max_pages + 7u) / 8u`.
- `pmm_core_refcount_bytes(max_pages)` returns `max_pages`.
- `pmm_core_init()` marks all pages used, then frees usable ranges.

- [ ] **Step 3: Allocate x86 PMM metadata from an early reserved window**

In `kernel/arch/x86/mm/pmm.c`, compute the highest managed page from the multiboot memory map, capped at `0x40000000` for the first Linux-like lowmem pass. Reserve metadata immediately after `_kernel_end` page-aligned:

```c
static uint32_t x86_detect_managed_pages(const multiboot_info_t *mbi)
{
	uint64_t highest = 0;
	/* Walk mmap entries, track end of type 1 usable ranges. */
	/* Cap at 0x40000000 for 1 GiB lowmem stage. */
	return (uint32_t)(highest / PAGE_SIZE);
}
```

Bind `g_pmm` with the reserved metadata buffers before applying usable and reserved ranges.

- [ ] **Step 4: Test 512 MiB and 1 GiB synthetic maps**

Add KTEST cases that initialize PMM with synthetic usable ranges of `512 MiB` and `1 GiB`, then assert:

```c
KTEST_EXPECT_EQ(tc, state.max_pages, expected_pages);
KTEST_EXPECT_TRUE(tc, pmm_core_free_page_count(&state) > 0u);
```

- [ ] **Step 5: Run tests and QEMU with larger memory**

Run:

```bash
make check-kernel-unit
make NO_DESKTOP=1 KTEST=0 QEMU_MEMORY="-m 512M" kernel disk run-x86-smoke
```

Expected: tests pass and the x86 smoke boot works with 512 MiB.

- [ ] **Step 6: Commit**

```bash
git add kernel/mm/pmm_core.h kernel/mm/pmm_core.c kernel/arch/x86/mm/pmm.c kernel/arch/arm64/mm/pmm.c kernel/test/test_pmm_core.c kernel/arch/x86/test/test_pmm.c
git commit -m "feat: size physical memory manager at boot"
```

---

### Task 7: Introduce Higher-Half Kernel Mapping On x86

**Files:**

- Modify: `kernel/arch/x86/arch_layout.h`
- Modify: `kernel/arch/x86/mm/paging.h`
- Modify: `kernel/arch/x86/mm/paging.c`
- Modify: `kernel/arch/x86/boot/boot.asm` or the current x86 entry assembly file
- Modify: x86 linker script used by the kernel
- Modify: `kernel/arch/x86/proc/arch_proc.c`
- Test: `kernel/arch/x86/test/test_arch_x86.c`

- [ ] **Step 1: Add higher-half layout constants without switching yet**

In `kernel/arch/x86/arch_layout.h`, add:

```c
#define ARCH_KERNEL_VIRT_BASE ((uintptr_t)0xC0000000u)
#define ARCH_KERNEL_DIRECT_PHYS_MAX ((uintptr_t)0x40000000u)
#define ARCH_KERNEL_PHYS_TO_VIRT(p) ((uintptr_t)(p) + ARCH_KERNEL_VIRT_BASE)
#define ARCH_KERNEL_VIRT_TO_PHYS(v) ((uintptr_t)(v) - ARCH_KERNEL_VIRT_BASE)
```

Keep `ARCH_USER_VADDR_MAX` at `0xC0000000u`.

- [ ] **Step 2: Map both low identity and higher-half during transition**

In `paging_init()`, map the kernel low identity range as today, then also map physical `0..ARCH_KERNEL_DIRECT_PHYS_MAX` at virtual `ARCH_KERNEL_VIRT_BASE..ARCH_KERNEL_VIRT_BASE + ARCH_KERNEL_DIRECT_PHYS_MAX`.

Use a helper:

```c
static int paging_map_kernel_page(uint32_t *pd,
                                  uint32_t virt,
                                  uint32_t phys,
                                  uint32_t flags)
{
	uint32_t pdi = virt >> 22;
	uint32_t pti = (virt >> 12) & 0x3FFu;
	uint32_t *pt;

	if (!(pd[pdi] & PG_PRESENT)) {
		uint32_t pt_phys = pmm_alloc_page();
		if (!pt_phys)
			return -1;
		pt = (uint32_t *)pt_phys;
		k_memset(pt, 0, PAGE_SIZE);
		pd[pdi] = paging_entry_build(pt_phys, PG_PRESENT | PG_WRITABLE);
	} else {
		pt = (uint32_t *)paging_entry_addr(pd[pdi]);
	}

	pt[pti] = paging_entry_build(phys, flags | PG_PRESENT | PG_WRITABLE);
	return 0;
}
```

- [ ] **Step 3: Switch kernel pointers one subsystem at a time**

Replace physical-address-as-pointer assumptions in paging and process code with `arch_page_temp_map()` or `ARCH_KERNEL_PHYS_TO_VIRT()`. Start with:

- Page directory access in `paging_lookup_slot()`.
- User page directory creation in `paging_create_user_space()`.
- ELF page initialization in `arch_x86_load_elf()`.

- [ ] **Step 4: Test supervisor-only kernel aliases**

In `kernel/arch/x86/test/test_arch_x86.c`, add a test that creates a user address space and asserts the PDEs at and above `0xC0000000` are present without `PG_USER`, while low user addresses are free for user mappings.

- [ ] **Step 5: Run tests**

Run:

```bash
make check-kernel-unit
make NO_DESKTOP=1 KTEST=0 kernel disk run-x86-smoke
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86/arch_layout.h kernel/arch/x86/mm/paging.h kernel/arch/x86/mm/paging.c kernel/arch/x86/proc/arch_proc.c kernel/arch/x86/test/test_arch_x86.c
git commit -m "feat: add x86 higher-half kernel mappings"
```

---

### Task 8: Lower x86 User Image Base Toward Linux

**Files:**

- Modify: `kernel/arch/x86/arch_layout.h`
- Modify: `Makefile`
- Modify: `user/Makefile`
- Modify: x86 user linker script if present
- Modify: `kernel/proc/process.h`
- Modify: `docs/ch15-processes.md`
- Test: `kernel/arch/x86/test/test_process.c`
- Test: `user/apps/linuxabi.c`

- [ ] **Step 1: Change x86 user base**

After Task 7 proves low addresses no longer collide with supervisor identity mappings, set:

```c
#define ARCH_USER_VADDR_MIN ((uintptr_t)0x00010000u)
#define ARCH_USER_IMAGE_BASE ((uintptr_t)0x08048000u)
#define ARCH_USER_VADDR_MAX ((uintptr_t)0xC0000000u)
```

- [ ] **Step 2: Update user link address**

In top-level `Makefile` and `user/Makefile`, set:

```make
X86_USER_LOAD_ADDR ?= 0x08048000
USER_LOAD_ADDR ?= 0x08048000
```

- [ ] **Step 3: Add load-range validation**

In `arch_x86_validate_elf_load_range()`, assert every `PT_LOAD` page satisfies:

```c
if (vaddr < ARCH_USER_VADDR_MIN || vend > ARCH_USER_VADDR_MAX)
	return -1;
```

- [ ] **Step 4: Run tests and user smoke**

Run:

```bash
rm -rf build/user/x86
make check-kernel-unit
make NO_DESKTOP=1 KTEST=0 kernel disk run-x86-smoke
```

Expected: pass, and `/proc/<pid>/maps` labels executable images near `0x08048000`.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86/arch_layout.h Makefile user/Makefile kernel/proc/process.h docs/ch15-processes.md kernel/arch/x86/test/test_process.c user/apps/linuxabi.c
git commit -m "feat: move x86 user programs to linux-style base"
```

---

### Task 9: Make User `malloc` Use `mmap` For Large Allocations

**Files:**

- Modify: `user/runtime/malloc.c`
- Modify: `user/runtime/syscall.h`
- Modify: `user/runtime/syscall.c`
- Test: `user/apps/linuxabi.c`

- [ ] **Step 1: Add mmap syscall wrapper if missing**

In `user/runtime/syscall.h`:

```c
void *sys_mmap(void *addr,
               unsigned int length,
               int prot,
               int flags,
               int fd,
               unsigned int offset);
int sys_munmap(void *addr, unsigned int length);
```

In `user/runtime/syscall.c`, wrap the existing Linux syscall numbers used by `linuxabi.c`.

- [ ] **Step 2: Route large allocations through mmap**

In `user/runtime/malloc.c`, add:

```c
#define MMAP_THRESHOLD (128u * 1024u)
#define HDR_FLAG_MMAP 0x2u
```

Before the `sbrk()` extension path in `malloc()`:

```c
if (size >= MMAP_THRESHOLD) {
	uint32_t total = (uint32_t)((size + HDR_SIZE + 0xFFFu) & ~0xFFFu);
	block_hdr_t *blk = (block_hdr_t *)sys_mmap(0,
	                                           total,
	                                           PROT_READ | PROT_WRITE,
	                                           MAP_PRIVATE | MAP_ANONYMOUS,
	                                           -1,
	                                           0);
	if ((uintptr_t)blk > 0xFFFFF000u)
		return 0;
	blk->size = total - HDR_SIZE;
	blk->flags = FLAG_USED | HDR_FLAG_MMAP;
	return payload_of(blk);
}
```

In `free()`:

```c
if (h->flags & HDR_FLAG_MMAP) {
	sys_munmap(h, h->size + HDR_SIZE);
	return;
}
```

- [ ] **Step 3: Add ABI app coverage**

In `user/apps/linuxabi.c`, allocate and touch a large block:

```c
char *big = (char *)malloc(256 * 1024);
check_ok("malloc large mmap allocation succeeds", big ? 1 : -1);
if (big) {
	big[0] = 'a';
	big[(256 * 1024) - 1] = 'z';
	check_eq("malloc large mmap first byte", big[0], 'a');
	check_eq("malloc large mmap last byte", big[(256 * 1024) - 1], 'z');
	free(big);
}
```

- [ ] **Step 4: Run tests**

Run:

```bash
make NO_DESKTOP=1 KTEST=0 kernel disk run-x86-smoke
```

Expected: Linux ABI app passes.

- [ ] **Step 5: Commit**

```bash
git add user/runtime/malloc.c user/runtime/syscall.h user/runtime/syscall.c user/apps/linuxabi.c
git commit -m "feat: allocate large user blocks with mmap"
```

---

### Task 10: Add Linux-Like Resource Limits And Commit Accounting

**Files:**

- Modify: `kernel/proc/process.h`
- Create: `kernel/mm/commit.h`
- Create: `kernel/mm/commit.c`
- Modify: `kernel/proc/syscall/mem.c`
- Modify: `kernel/mm/fault.c`
- Modify: `kernel/proc/mem_forensics.c`
- Test: `kernel/arch/x86/test/test_process.c`

- [ ] **Step 1: Add address-space limits**

In `proc_address_space_t`, add:

```c
uint32_t rlimit_as;
uint32_t rlimit_data;
uint32_t committed_pages;
```

Set defaults in `proc_resource_init_fresh()`:

```c
proc->as->rlimit_as = VM_TASK_SIZE;
proc->as->rlimit_data = 64u * 1024u * 1024u;
proc->as->committed_pages = 0;
```

- [ ] **Step 2: Add commit accounting helpers**

Create `kernel/mm/commit.h`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef MM_COMMIT_H
#define MM_COMMIT_H

#include "process.h"
#include <stdint.h>

int vm_commit_reserve(process_t *proc, uint32_t bytes);
void vm_commit_release(process_t *proc, uint32_t bytes);

#endif
```

Create `kernel/mm/commit.c`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "commit.h"
#include "pmm.h"

int vm_commit_reserve(process_t *proc, uint32_t bytes)
{
	uint32_t pages;
	uint32_t free_pages;

	if (!proc || !proc->as)
		return -1;
	pages = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
	free_pages = pmm_free_page_count();
	if (pages > free_pages)
		return -1;
	if (proc->as->committed_pages > UINT32_MAX - pages)
		return -1;
	proc->as->committed_pages += pages;
	return 0;
}

void vm_commit_release(process_t *proc, uint32_t bytes)
{
	uint32_t pages;

	if (!proc || !proc->as)
		return;
	pages = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
	if (pages > proc->as->committed_pages)
		proc->as->committed_pages = 0;
	else
		proc->as->committed_pages -= pages;
}
```

- [ ] **Step 3: Enforce limits in brk and mmap**

In `syscall_case_brk()`, before accepting growth:

```c
if (new_brk > *brk_ptr) {
	uint32_t grow = new_brk - *brk_ptr;
	if (new_brk - heap_vma->start > cur->as->rlimit_data)
		return *brk_ptr;
	if (vm_commit_reserve(cur, grow) != 0)
		return *brk_ptr;
}
```

On shrink, call `vm_commit_release(cur, *brk_ptr - new_brk)`.

In mmap reservation paths, reserve commit for anonymous private writable mappings. Release commit in `munmap`.

- [ ] **Step 4: Test brk/data limit**

Add:

```c
static void test_brk_obeys_data_limit(ktest_case_t *tc)
{
	process_t proc;
	process_t *saved = sched_current();

	k_memset(&proc, 0, sizeof(proc));
	KTEST_ASSERT_EQ(tc, proc_resource_init_fresh(&proc), 0);
	init_vma_proc(&proc);
	proc.as->heap_start = 0x08050000u;
	proc.as->brk = proc.as->heap_start;
	proc.as->rlimit_data = PAGE_SIZE;
	KTEST_ASSERT_EQ(tc,
	                vma_add(&proc,
	                        proc.as->heap_start,
	                        proc.as->brk,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE,
	                        VMA_KIND_HEAP),
	                0);
	sched_set_current_for_test(&proc);

	KTEST_EXPECT_EQ(tc,
	                syscall_handler(SYS_BRK,
	                                proc.as->heap_start + 2u * PAGE_SIZE,
	                                0,
	                                0,
	                                0,
	                                0,
	                                0),
	                proc.as->heap_start);

	sched_set_current_for_test(saved);
	proc_resource_put_all(&proc);
}
```

- [ ] **Step 5: Run tests and commit**

Run:

```bash
make check-kernel-unit
```

Commit:

```bash
git add kernel/proc/process.h kernel/mm/commit.h kernel/mm/commit.c kernel/proc/syscall/mem.c kernel/mm/fault.c kernel/proc/mem_forensics.c kernel/arch/x86/test/test_process.c
git commit -m "feat: add basic vm commit accounting"
```

---

### Task 11: Update `/proc` Memory Reporting To Match Linux Concepts

**Files:**

- Modify: `kernel/proc/mem_forensics.h`
- Modify: `kernel/proc/mem_forensics.c`
- Modify: `docs/ch15-processes.md`
- Modify: `docs/ch25-demand-paging.md`
- Test: `kernel/arch/x86/test/test_process.c`

- [ ] **Step 1: Report reserved versus resident memory**

Ensure `mem_forensics_collect()` reports:

- image reserved bytes and mapped bytes
- heap reserved bytes and mapped bytes
- mmap reserved bytes and mapped bytes
- stack reserved bytes and mapped bytes
- total committed bytes

Use existing page table walks for resident counts and VMA lengths for reserved counts.

- [ ] **Step 2: Print Linux-like map labels**

In `/proc/<pid>/maps`, label:

```text
[heap]
[stack]
/bin/name
anon
file
```

Keep existing Drunix details in `vmstat`.

- [ ] **Step 3: Add tests**

Extend existing mem-forensics tests to assert:

```c
KTEST_EXPECT_TRUE(tc, report.heap_reserved_bytes >= report.heap_mapped_bytes);
KTEST_EXPECT_TRUE(tc, report.stack_reserved_bytes >= report.stack_mapped_bytes);
KTEST_EXPECT_TRUE(tc, report.mmap_reserved_bytes >= report.mmap_mapped_bytes);
```

- [ ] **Step 4: Run tests and commit**

Run:

```bash
make check-kernel-unit
```

Commit:

```bash
git add kernel/proc/mem_forensics.h kernel/proc/mem_forensics.c docs/ch15-processes.md docs/ch25-demand-paging.md kernel/arch/x86/test/test_process.c
git commit -m "docs: report linux-like process memory regions"
```

---

### Task 12: Full Verification And Merge Readiness

**Files:**

- Modify: docs touched by previous tasks.
- No new code unless verification exposes a defect.

- [ ] **Step 1: Clean build outputs**

Run:

```bash
rm -rf build
```

- [ ] **Step 2: Run x86 verification**

Run:

```bash
make check
```

Expected: pass.

- [ ] **Step 3: Run arm64 verification**

Run:

```bash
make ARCH=arm64 check
```

Expected: pass.

- [ ] **Step 4: Run larger-memory x86 smoke**

Run:

```bash
make NO_DESKTOP=1 KTEST=0 QEMU_MEMORY="-m 512M" kernel disk run-x86-smoke
```

Expected: pass.

- [ ] **Step 5: Inspect maps manually**

Boot x86 and run:

```text
cat /proc/1/maps
```

Expected shape:

```text
08048000-... image
...-... [heap]
...-... anon/file mmap region below stack
bfff...-c0000000 [stack]
```

- [ ] **Step 6: Commit docs or fixes**

```bash
git status --short
git diff --check
git add docs kernel user Makefile
git commit -m "docs: document linux-like memory layout"
```

Skip the commit if there are no changes.

---

## Self-Review

Spec coverage:

- Linux-like low user image: Task 8.
- Linux-like `brk` heap: Tasks 1, 2, 8, 10.
- Linux-like high top-down `mmap`: Tasks 2, 3, 4, 9.
- Linux-like grow-down stack with guard: Tasks 2 and 11.
- Higher-half kernel prerequisite: Task 7.
- PMM no longer capped at 256 MiB: Task 6.
- Lazy file mappings: Task 4.
- User allocator convergence: Task 9.
- Reporting and documentation: Tasks 11 and 12.

Implementation order matters:

- Do not lower `ARCH_USER_IMAGE_BASE` before Task 7. The current low identity kernel mapping model makes low user addresses collide with supervisor-only entries.
- Do not make `mmap` heavily used by `malloc` before Tasks 3 and 4. The VMA model needs file/backing metadata and a stronger placement helper first.
- Do not raise QEMU memory expectations before Task 6. The current PMM cap prevents Drunix from managing all RAM that QEMU may expose.

