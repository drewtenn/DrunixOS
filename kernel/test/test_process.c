/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_process.c — unit tests for synthetic process launch frames.
 */

#include "ktest.h"
#include "core.h"
#include "elf.h"
#include "fs.h"
#include "process.h"
#include "sched.h"
#include "gdt.h"
#include "kstring.h"
#include "mem_forensics.h"
#include "paging.h"
#include "pmm.h"
#include "syscall.h"
#include "vfs.h"
#include "vma.h"

extern void process_initial_launch(void);
extern void process_exec_launch(void);

#define TEST_CORE_DUMP_PID    77u
#define TEST_CORE_NOTE_ALIGN  4u
#define TEST_NT_PRPSINFO      3u
#define TEST_NT_DRUNIX_VMSTAT 0x4458564du
#define TEST_NT_DRUNIX_FAULT  0x44584654u
#define TEST_NT_DRUNIX_MAPS   0x44584d50u
#define TEST_LINUX_MAP_FIXED  0x10u
#define TEST_LINUX_AT_NO_AUTOMOUNT 0x0800u
#define TEST_LINUX_AT_EMPTY_PATH 0x1000u
#define TEST_LINUX_STATX_BASIC_STATS 0x7ffu
#define TEST_LINUX_F_DUPFD    0u
#define TEST_LINUX_F_GETFD    1u
#define TEST_LINUX_F_SETFD    2u
#define TEST_LINUX_F_GETFL    3u
#define TEST_LINUX_TIOCGWINSZ 0x5413u

static uint32_t test_align_up(uint32_t val, uint32_t align)
{
    return (val + align - 1u) & ~(align - 1u);
}

static void init_frame_proc(process_t *proc, uint32_t *kstack_words,
                            uint32_t entry, uint32_t user_stack)
{
    k_memset(proc, 0, sizeof(*proc));
    k_memset(kstack_words, 0, 32u * sizeof(*kstack_words));
    proc->entry = entry;
    proc->user_stack = user_stack;
    proc->kstack_top = (uint32_t)(kstack_words + 32);
}

static void init_vma_proc(process_t *proc)
{
    k_memset(proc, 0, sizeof(*proc));
    proc->heap_start = 0x00410000u;
    proc->brk = 0x00418000u;
    proc->stack_low_limit =
        USER_STACK_TOP - (uint32_t)USER_STACK_PAGES * 0x1000u;
    vma_init(proc);
    vma_add(proc, proc->heap_start, proc->brk,
            VMA_FLAG_READ | VMA_FLAG_WRITE |
            VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
            VMA_KIND_HEAP);
    vma_add(proc,
            USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * 0x1000u,
            USER_STACK_TOP,
            VMA_FLAG_READ | VMA_FLAG_WRITE |
            VMA_FLAG_ANON | VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
            VMA_KIND_STACK);
}

static void init_fresh_process_layout_proc(process_t *proc)
{
    k_memset(proc, 0, sizeof(*proc));
    proc->image_start = 0x00400000u;
    proc->image_end = 0x00410000u;
    proc->heap_start = proc->image_end;
    proc->brk = proc->heap_start;
    proc->stack_low_limit =
        USER_STACK_TOP - (uint32_t)USER_STACK_PAGES * 0x1000u;
    vma_init(proc);
    vma_add(proc, proc->image_start, proc->image_end,
            VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC |
            VMA_FLAG_PRIVATE,
            VMA_KIND_IMAGE);
    vma_add(proc, proc->heap_start, proc->brk,
            VMA_FLAG_READ | VMA_FLAG_WRITE |
            VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
            VMA_KIND_HEAP);
    vma_add(proc,
            USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * 0x1000u,
            USER_STACK_TOP,
            VMA_FLAG_READ | VMA_FLAG_WRITE |
            VMA_FLAG_ANON | VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
            VMA_KIND_STACK);
}

static int map_test_page(process_t *proc, uint32_t vaddr, uint32_t flags)
{
    uint32_t phys = pmm_alloc_page();

    if (!phys)
        return -1;
    if (paging_map_page(proc->pd_phys, vaddr, phys, flags) != 0)
        return -1;
    return 0;
}

static int map_test_user_page(process_t *proc, uint32_t vaddr)
{
    uint32_t page = vaddr & ~0xFFFu;

    if (!vma_find(proc, page) &&
        vma_add(proc, page, page + PAGE_SIZE,
                VMA_FLAG_READ | VMA_FLAG_WRITE |
                VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                VMA_KIND_GENERIC) != 0)
        return -1;

    return map_test_page(proc, page, PG_PRESENT | PG_WRITABLE | PG_USER);
}

static uint8_t *mapped_alias(process_t *proc, uint32_t virt)
{
    uint32_t *pte = 0;

    if (!proc || paging_walk(proc->pd_phys, virt, &pte) != 0)
        return 0;

    return (uint8_t *)(paging_entry_addr(*pte) + (virt & 0xFFFu));
}

static process_t *start_syscall_test_process(process_t *proc)
{
    sched_init();
    init_fresh_process_layout_proc(proc);
    proc->pd_phys = paging_create_user_space();
    if (!proc->pd_phys)
        return 0;

    proc->saved_esp = 1; /* syscall tests do not context-switch this task */
    proc->open_files[1].type = FD_TYPE_STDOUT;
    proc->open_files[1].writable = 1;

    if (sched_add(proc) < 1) {
        process_release_user_space(proc);
        return 0;
    }
    return sched_bootstrap();
}

static void stop_syscall_test_process(process_t *proc)
{
    if (proc)
        process_release_user_space(proc);
    sched_init();
}

static void init_core_dump_proc(process_t *proc)
{
    init_fresh_process_layout_proc(proc);
    proc->pid = TEST_CORE_DUMP_PID;
    proc->state = PROC_RUNNING;
    proc->parent_pid = 1u;
    proc->pgid = TEST_CORE_DUMP_PID;
    proc->sid = TEST_CORE_DUMP_PID;
    proc->pd_phys = paging_create_user_space();
    k_strncpy(proc->name, "crash", sizeof(proc->name) - 1u);
    k_strncpy(proc->psargs, "crash badptr", sizeof(proc->psargs) - 1u);
    proc->crash.valid = 1u;
    proc->crash.signum = SIGSEGV;
    proc->crash.cr2 = 0xDEADBEEFu;
    proc->crash.frame.vector = 14u;
    proc->crash.frame.error_code = 0x6u;
    proc->crash.frame.eip = 0x00402A16u;
}

static void test_process_build_initial_frame_layout(ktest_case_t *tc)
{
    static process_t proc;
    static uint32_t kstack_words[32];

    init_frame_proc(&proc, kstack_words, 0x00401000u, 0xBFFFE000u);
    process_build_initial_frame(&proc);

    uint32_t *frame = (uint32_t *)proc.saved_esp;
    KTEST_ASSERT_NOT_NULL(tc, frame);

    KTEST_EXPECT_EQ(tc, frame[0], 0u);
    KTEST_EXPECT_EQ(tc, frame[1], 0u);
    KTEST_EXPECT_EQ(tc, frame[2], 0u);
    KTEST_EXPECT_EQ(tc, frame[3], 0u);
    KTEST_EXPECT_EQ(tc, frame[4], (uint32_t)process_initial_launch);
    KTEST_EXPECT_EQ(tc, frame[5], GDT_USER_DS);
    KTEST_EXPECT_EQ(tc, frame[6], GDT_USER_DS);
    KTEST_EXPECT_EQ(tc, frame[7], GDT_USER_DS);
    KTEST_EXPECT_EQ(tc, frame[8], GDT_USER_DS);
    KTEST_EXPECT_EQ(tc, frame[19], 0x00401000u);
    KTEST_EXPECT_EQ(tc, frame[20], GDT_USER_CS);
    KTEST_EXPECT_EQ(tc, frame[21], 0x202u);
    KTEST_EXPECT_EQ(tc, frame[22], 0xBFFFE000u);
    KTEST_EXPECT_EQ(tc, frame[23], GDT_USER_DS);
}

static void test_process_build_exec_frame_layout(ktest_case_t *tc)
{
    static process_t proc;
    static uint32_t kstack_words[32];

    init_frame_proc(&proc, kstack_words, 0x00402000u, 0xBFFFD000u);
    process_build_exec_frame(&proc, 0x00123000u, 0x00ABC000u);

    uint32_t *frame = (uint32_t *)proc.saved_esp;
    KTEST_ASSERT_NOT_NULL(tc, frame);

    KTEST_EXPECT_EQ(tc, frame[0], 0u);
    KTEST_EXPECT_EQ(tc, frame[1], 0u);
    KTEST_EXPECT_EQ(tc, frame[2], 0u);
    KTEST_EXPECT_EQ(tc, frame[3], 0u);
    KTEST_EXPECT_EQ(tc, frame[4], (uint32_t)process_exec_launch);
    KTEST_EXPECT_EQ(tc, frame[5], 0x00123000u);
    KTEST_EXPECT_EQ(tc, frame[6], 0x00ABC000u);
    KTEST_EXPECT_EQ(tc, frame[7], GDT_USER_DS);
    KTEST_EXPECT_EQ(tc, frame[21], 0x00402000u);
    KTEST_EXPECT_EQ(tc, frame[22], GDT_USER_CS);
    KTEST_EXPECT_EQ(tc, frame[23], 0x202u);
    KTEST_EXPECT_EQ(tc, frame[24], 0xBFFFD000u);
    KTEST_EXPECT_EQ(tc, frame[25], GDT_USER_DS);
}

static void test_sched_add_builds_initial_frame_for_never_run_process(ktest_case_t *tc)
{
    static process_t proc;
    static uint32_t kstack_words[32];

    sched_init();
    init_frame_proc(&proc, kstack_words, 0x00403000u, 0xBFFFC000u);
    proc.saved_esp = 0;

    int pid = sched_add(&proc);
    KTEST_ASSERT_TRUE(tc, pid >= 1);

    process_t *slot = sched_find_pid((uint32_t)pid);
    KTEST_ASSERT_NOT_NULL(tc, slot);
    KTEST_EXPECT_EQ(tc, slot->state, PROC_READY);
    KTEST_EXPECT_NE(tc, slot->saved_esp, 0u);

    uint32_t *frame = (uint32_t *)slot->saved_esp;
    KTEST_ASSERT_NOT_NULL(tc, frame);
    KTEST_EXPECT_EQ(tc, frame[4], (uint32_t)process_initial_launch);
    KTEST_EXPECT_EQ(tc, frame[19], 0x00403000u);
    KTEST_EXPECT_EQ(tc, frame[22], 0xBFFFC000u);
}

static void test_process_builds_linux_i386_initial_stack_shape(ktest_case_t *tc)
{
    static process_t proc;
    const char *argv[] = { "linuxprobe", 0 };
    const char *envp[] = { "DRUNIX=1", 0 };
    uint32_t esp = USER_STACK_TOP;
    uint32_t *stack;
    const char *arg0;
    const char *env0;

    k_memset(&proc, 0, sizeof(proc));
    vma_init(&proc);
    proc.pd_phys = paging_create_user_space();
    KTEST_ASSERT_NE(tc, proc.pd_phys, 0u);
    KTEST_ASSERT_EQ(tc,
                    map_test_page(&proc, USER_STACK_TOP - PAGE_SIZE,
                                  PG_PRESENT | PG_WRITABLE | PG_USER),
                    0u);
    KTEST_ASSERT_EQ(tc,
                    (uint32_t)process_build_user_stack_frame_for_test(
                        proc.pd_phys, argv, 1, envp, 1, &esp),
                    0u);
    stack = (uint32_t *)mapped_alias(&proc, esp);
    KTEST_ASSERT_NOT_NULL(tc, stack);

    KTEST_EXPECT_EQ(tc, stack[0], 1u);
    KTEST_EXPECT_EQ(tc, stack[2], 0u);
    KTEST_EXPECT_EQ(tc, stack[4], 0u);
    KTEST_EXPECT_EQ(tc, stack[5], 6u);         /* AT_PAGESZ */
    KTEST_EXPECT_EQ(tc, stack[6], PAGE_SIZE);
    KTEST_EXPECT_EQ(tc, stack[7], 0u);         /* AT_NULL */
    KTEST_EXPECT_EQ(tc, stack[8], 0u);

    arg0 = (const char *)mapped_alias(&proc, stack[1]);
    env0 = (const char *)mapped_alias(&proc, stack[3]);
    KTEST_ASSERT_NOT_NULL(tc, arg0);
    KTEST_ASSERT_NOT_NULL(tc, env0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(arg0, "linuxprobe") == 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(env0, "DRUNIX=1") == 0);

    process_release_user_space(&proc);
}

static void test_vma_add_keeps_regions_sorted_and_findable(ktest_case_t *tc)
{
    static process_t proc;

    k_memset(&proc, 0, sizeof(proc));
    vma_init(&proc);

    KTEST_ASSERT_EQ(tc,
                    vma_add(&proc, 0xBFC00000u, USER_STACK_TOP,
                            VMA_FLAG_READ | VMA_FLAG_WRITE |
                            VMA_FLAG_ANON | VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
                            VMA_KIND_STACK),
                    0u);
    KTEST_ASSERT_EQ(tc,
                    vma_add(&proc, 0x00410000u, 0x00418000u,
                            VMA_FLAG_READ | VMA_FLAG_WRITE |
                            VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                            VMA_KIND_HEAP),
                    0u);

    KTEST_EXPECT_EQ(tc, proc.vma_count, 2u);
    KTEST_EXPECT_EQ(tc, proc.vmas[0].kind, VMA_KIND_HEAP);
    KTEST_EXPECT_EQ(tc, proc.vmas[0].start, 0x00410000u);
    KTEST_EXPECT_EQ(tc, proc.vmas[1].kind, VMA_KIND_STACK);
    KTEST_EXPECT_EQ(tc, proc.vmas[1].start, 0xBFC00000u);

    vm_area_t *heap = vma_find(&proc, 0x00417FFFu);
    vm_area_t *stack = vma_find(&proc, 0xBFFFF000u);

    KTEST_ASSERT_NOT_NULL(tc, heap);
    KTEST_ASSERT_NOT_NULL(tc, stack);
    KTEST_EXPECT_EQ(tc, heap->kind, VMA_KIND_HEAP);
    KTEST_EXPECT_EQ(tc, stack->kind, VMA_KIND_STACK);
    KTEST_EXPECT_NULL(tc, vma_find(&proc, 0x00800000u));
}

static void test_vma_add_rejects_overlapping_regions(ktest_case_t *tc)
{
    static process_t proc;

    k_memset(&proc, 0, sizeof(proc));
    vma_init(&proc);

    KTEST_ASSERT_EQ(tc,
                    vma_add(&proc, 0x00410000u, 0x00418000u,
                            VMA_FLAG_READ | VMA_FLAG_WRITE |
                            VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                            VMA_KIND_HEAP),
                    0u);
    KTEST_EXPECT_EQ(tc,
                    vma_add(&proc, 0x00417000u, 0x0041A000u,
                            VMA_FLAG_READ | VMA_FLAG_WRITE |
                            VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                            VMA_KIND_GENERIC),
                    (uint32_t)-1);
    KTEST_EXPECT_EQ(tc, proc.vma_count, 1u);
}

static void test_vma_map_anonymous_places_regions_below_stack(ktest_case_t *tc)
{
    static process_t proc;
    uint32_t addr = 0;
    uint32_t stack_base =
        USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * 0x1000u;

    init_vma_proc(&proc);

    KTEST_ASSERT_EQ(tc,
                    vma_map_anonymous(&proc, 0, 0x2000u,
                                      VMA_FLAG_READ | VMA_FLAG_WRITE |
                                      VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                                      &addr),
                    0u);
    KTEST_EXPECT_EQ(tc, addr, stack_base - 0x2000u);
    KTEST_EXPECT_EQ(tc, proc.vma_count, 3u);
    KTEST_EXPECT_EQ(tc, proc.vmas[1].start, stack_base - 0x2000u);
    KTEST_EXPECT_EQ(tc, proc.vmas[1].end, stack_base);
    KTEST_EXPECT_EQ(tc, proc.vmas[1].kind, VMA_KIND_GENERIC);
}

static void test_vma_unmap_range_splits_generic_mapping(ktest_case_t *tc)
{
    static process_t proc;

    init_vma_proc(&proc);
    KTEST_ASSERT_EQ(tc,
                    vma_add(&proc, 0x80000000u, 0x80003000u,
                            VMA_FLAG_READ | VMA_FLAG_WRITE |
                            VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                            VMA_KIND_GENERIC),
                    0u);

    KTEST_ASSERT_EQ(tc, vma_unmap_range(&proc, 0x80001000u, 0x80002000u), 0u);
    KTEST_EXPECT_EQ(tc, proc.vma_count, 4u);
    KTEST_EXPECT_EQ(tc, proc.vmas[1].start, 0x80000000u);
    KTEST_EXPECT_EQ(tc, proc.vmas[1].end, 0x80001000u);
    KTEST_EXPECT_EQ(tc, proc.vmas[2].start, 0x80002000u);
    KTEST_EXPECT_EQ(tc, proc.vmas[2].end, 0x80003000u);
}

static void test_vma_unmap_range_rejects_heap_or_stack(ktest_case_t *tc)
{
    static process_t proc;

    init_vma_proc(&proc);
    KTEST_EXPECT_EQ(tc,
                    vma_unmap_range(&proc, 0x00410000u, 0x00411000u),
                    (uint32_t)-1);
    KTEST_EXPECT_EQ(tc, proc.vma_count, 2u);
}

static void test_vma_protect_range_splits_and_requires_full_coverage(ktest_case_t *tc)
{
    static process_t proc;

    init_vma_proc(&proc);
    KTEST_ASSERT_EQ(tc,
                    vma_add(&proc, 0x80000000u, 0x80003000u,
                            VMA_FLAG_READ | VMA_FLAG_WRITE |
                            VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                            VMA_KIND_GENERIC),
                    0u);

    KTEST_ASSERT_EQ(tc,
                    vma_protect_range(&proc, 0x80001000u, 0x80002000u,
                                      VMA_FLAG_READ |
                                      VMA_FLAG_ANON | VMA_FLAG_PRIVATE),
                    0u);
    KTEST_EXPECT_EQ(tc, proc.vma_count, 5u);
    KTEST_EXPECT_EQ(tc, proc.vmas[2].start, 0x80001000u);
    KTEST_EXPECT_EQ(tc, proc.vmas[2].end, 0x80002000u);
    KTEST_EXPECT_EQ(tc,
                    proc.vmas[2].flags,
                    VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE);
    KTEST_EXPECT_EQ(tc,
                    vma_protect_range(&proc, 0x80003000u, 0x80005000u,
                                      VMA_FLAG_READ |
                                      VMA_FLAG_ANON | VMA_FLAG_PRIVATE),
                    (uint32_t)-1);
}

static void test_mem_forensics_collects_basic_region_totals(ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;
    uint32_t stack_reserved =
        (uint32_t)USER_STACK_MAX_PAGES * 0x1000u;

    init_vma_proc(&proc);
    proc.image_start = 0x00400000u;
    proc.image_end = 0x00410000u;
    k_strncpy(proc.name, "shell", sizeof(proc.name) - 1);

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.region_count, 3u);
    KTEST_EXPECT_EQ(tc, report.image_reserved_bytes, 0x00010000u);
    KTEST_EXPECT_EQ(tc, report.heap_reserved_bytes,  0x00008000u);
    KTEST_EXPECT_EQ(tc,
                    report.stack_reserved_bytes,
                    (uint32_t)USER_STACK_MAX_PAGES * 0x1000u);
}

static void test_mem_forensics_core_note_sizes_are_nonzero(ktest_case_t *tc)
{
    KTEST_EXPECT_TRUE(tc, mem_forensics_vmstat_note_size() > 0u);
    KTEST_EXPECT_TRUE(tc, mem_forensics_fault_note_size() > 0u);
}

static void test_core_dump_writes_drunix_notes_in_order(ktest_case_t *tc)
{
    static process_t proc;
    static uint8_t note_buf[2048];
    static char expected_vmstat[512];
    static char expected_fault[512];
    static char expected_maps[1024];
    Elf32_Ehdr ehdr;
    Elf32_Phdr phdr;
    Elf32_Nhdr nhdr;
    uint32_t expected_vmstat_len = 0;
    uint32_t expected_fault_len = 0;
    uint32_t expected_maps_len = 0;
    uint32_t ino = 0;
    uint32_t size = 0;
    uint32_t off = 0;
    int n;

    vfs_reset();
    dufs_register();
    KTEST_ASSERT_EQ(tc, (uint32_t)vfs_mount("/", "dufs"), 0u);
    (void)fs_unlink("core.77");

    init_core_dump_proc(&proc);
    KTEST_ASSERT_NE(tc, proc.pd_phys, 0u);
    KTEST_ASSERT_EQ(tc,
                    (uint32_t)map_test_page(&proc, proc.image_start,
                                            PG_PRESENT | PG_USER | PG_WRITABLE),
                    0u);
    KTEST_ASSERT_EQ(tc,
                    (uint32_t)map_test_page(&proc, USER_STACK_TOP - 0x1000u,
                                            PG_PRESENT | PG_USER | PG_WRITABLE),
                    0u);

    KTEST_ASSERT_EQ(tc,
                    (uint32_t)mem_forensics_render_vmstat(&proc,
                                                         expected_vmstat,
                                                         sizeof(expected_vmstat),
                                                         &expected_vmstat_len),
                    0u);
    KTEST_ASSERT_EQ(tc,
                    (uint32_t)mem_forensics_render_fault(&proc,
                                                        expected_fault,
                                                        sizeof(expected_fault),
                                                        &expected_fault_len),
                    0u);
    KTEST_ASSERT_EQ(tc,
                    (uint32_t)mem_forensics_render_maps(&proc,
                                                       expected_maps,
                                                       sizeof(expected_maps),
                                                       &expected_maps_len),
                    0u);

    KTEST_ASSERT_EQ(tc, (uint32_t)core_dump_process(&proc, SIGSEGV), 0u);
    KTEST_ASSERT_EQ(tc, (uint32_t)vfs_open("core.77", &ino, &size), 0u);

    n = fs_read(ino, 0u, (uint8_t *)&ehdr, (uint32_t)sizeof(ehdr));
    KTEST_ASSERT_EQ(tc, (uint32_t)n, (uint32_t)sizeof(ehdr));
    KTEST_EXPECT_EQ(tc, ehdr.e_type, ET_CORE);

    n = fs_read(ino, ehdr.e_phoff, (uint8_t *)&phdr, (uint32_t)sizeof(phdr));
    KTEST_ASSERT_EQ(tc, (uint32_t)n, (uint32_t)sizeof(phdr));
    KTEST_EXPECT_EQ(tc, phdr.p_type, PT_NOTE);
    KTEST_ASSERT_TRUE(tc, phdr.p_filesz < sizeof(note_buf));

    n = fs_read(ino, phdr.p_offset, note_buf, phdr.p_filesz);
    KTEST_ASSERT_EQ(tc, (uint32_t)n, phdr.p_filesz);

    k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
    KTEST_EXPECT_EQ(tc, nhdr.n_type, NT_PRSTATUS);
    KTEST_EXPECT_EQ(tc, nhdr.n_namesz, 5u);
    KTEST_EXPECT_TRUE(tc, k_strcmp((const char *)(note_buf + off + sizeof(nhdr)),
                                   "CORE") == 0);
    off += (uint32_t)sizeof(nhdr) + test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
           test_align_up(nhdr.n_descsz, TEST_CORE_NOTE_ALIGN);

    k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
    KTEST_EXPECT_EQ(tc, nhdr.n_type, TEST_NT_PRPSINFO);
    KTEST_EXPECT_EQ(tc, nhdr.n_namesz, 5u);
    KTEST_EXPECT_TRUE(tc, k_strcmp((const char *)(note_buf + off + sizeof(nhdr)),
                                   "CORE") == 0);
    off += (uint32_t)sizeof(nhdr) + test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
           test_align_up(nhdr.n_descsz, TEST_CORE_NOTE_ALIGN);

    k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
    KTEST_EXPECT_EQ(tc, nhdr.n_type, TEST_NT_DRUNIX_VMSTAT);
    KTEST_EXPECT_EQ(tc, nhdr.n_descsz, expected_vmstat_len);
    KTEST_EXPECT_TRUE(tc, k_strcmp((const char *)(note_buf + off + sizeof(nhdr)),
                                   "DRUNIX") == 0);
    KTEST_EXPECT_TRUE(tc,
                      k_memcmp(note_buf + off + (uint32_t)sizeof(nhdr) +
                                   test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN),
                               expected_vmstat,
                               expected_vmstat_len) == 0);
    off += (uint32_t)sizeof(nhdr) + test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
           test_align_up(nhdr.n_descsz, TEST_CORE_NOTE_ALIGN);

    k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
    KTEST_EXPECT_EQ(tc, nhdr.n_type, TEST_NT_DRUNIX_FAULT);
    KTEST_EXPECT_EQ(tc, nhdr.n_descsz, expected_fault_len);
    KTEST_EXPECT_TRUE(tc, k_strcmp((const char *)(note_buf + off + sizeof(nhdr)),
                                   "DRUNIX") == 0);
    KTEST_EXPECT_TRUE(tc,
                      k_memcmp(note_buf + off + (uint32_t)sizeof(nhdr) +
                                   test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN),
                               expected_fault,
                               expected_fault_len) == 0);
    off += (uint32_t)sizeof(nhdr) + test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
           test_align_up(nhdr.n_descsz, TEST_CORE_NOTE_ALIGN);

    k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
    KTEST_EXPECT_EQ(tc, nhdr.n_type, TEST_NT_DRUNIX_MAPS);
    KTEST_EXPECT_EQ(tc, nhdr.n_descsz, expected_maps_len);
    KTEST_EXPECT_TRUE(tc, k_strcmp((const char *)(note_buf + off + sizeof(nhdr)),
                                   "DRUNIX") == 0);
    KTEST_EXPECT_TRUE(tc,
                      k_memcmp(note_buf + off + (uint32_t)sizeof(nhdr) +
                                   test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN),
                               expected_maps,
                               expected_maps_len) == 0);
    off += (uint32_t)sizeof(nhdr) + test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
           test_align_up(nhdr.n_descsz, TEST_CORE_NOTE_ALIGN);

    KTEST_EXPECT_EQ(tc, off, phdr.p_filesz);

    KTEST_ASSERT_EQ(tc, (uint32_t)fs_unlink("core.77"), 0u);
    process_release_user_space(&proc);
    vfs_reset();
}

static void test_mem_forensics_collects_fresh_process_layout(ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;
    uint32_t stack_reserved =
        (uint32_t)USER_STACK_MAX_PAGES * 0x1000u;

    init_fresh_process_layout_proc(&proc);
    k_strncpy(proc.name, "shell", sizeof(proc.name) - 1);

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.region_count, 3u);
    KTEST_EXPECT_EQ(tc, report.regions[0].kind, MEM_FORENSICS_REGION_IMAGE);
    KTEST_EXPECT_EQ(tc, report.regions[1].kind, MEM_FORENSICS_REGION_HEAP);
    KTEST_EXPECT_EQ(tc, report.regions[2].kind, MEM_FORENSICS_REGION_STACK);
    KTEST_EXPECT_EQ(tc, report.image_reserved_bytes, 0x00010000u);
    KTEST_EXPECT_EQ(tc, report.heap_reserved_bytes, 0u);
    KTEST_EXPECT_EQ(tc, report.stack_reserved_bytes, stack_reserved);
    KTEST_EXPECT_EQ(tc, report.mmap_reserved_bytes, 0u);
    KTEST_EXPECT_EQ(tc,
                    report.total_reserved_bytes,
                    0x00010000u + stack_reserved);
}

static void test_mem_forensics_collects_full_vma_table_with_fallback_image(ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;
    uint32_t expected_total = 0x00010000u;

    k_memset(&proc, 0, sizeof(proc));
    proc.image_start = 0x00400000u;
    proc.image_end = 0x00410000u;
    proc.brk = 0x00500000u;
    vma_init(&proc);

    for (uint32_t i = 0; i < PROCESS_MAX_VMAS; i++) {
        uint32_t start = 0x01000000u + i * 0x2000u;
        KTEST_ASSERT_EQ(tc,
                        vma_add(&proc, start, start + 0x1000u,
                                VMA_FLAG_READ | VMA_FLAG_WRITE |
                                VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                                VMA_KIND_GENERIC),
                        0u);
        expected_total += 0x1000u;
    }

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.region_count, PROCESS_MAX_VMAS + 1u);
    KTEST_EXPECT_EQ(tc, report.regions[0].kind, MEM_FORENSICS_REGION_IMAGE);
    KTEST_EXPECT_EQ(tc, report.image_reserved_bytes, 0x00010000u);
    KTEST_EXPECT_EQ(tc, report.mmap_reserved_bytes, (uint32_t)PROCESS_MAX_VMAS * 0x1000u);
    KTEST_EXPECT_EQ(tc, report.total_reserved_bytes, expected_total);
}

static void test_mem_forensics_classifies_unmapped_fault(ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;

    init_vma_proc(&proc);
    proc.crash.valid = 1;
    proc.crash.signum = SIGSEGV;
    proc.crash.cr2 = 0xDEADBEEFu;
    proc.crash.frame.vector = 14u;
    proc.crash.frame.error_code = 0x6u;

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.fault.valid, 1u);
    KTEST_EXPECT_EQ(tc, report.fault.classification,
                    MEM_FORENSICS_FAULT_UNMAPPED);
}

static void test_mem_forensics_classifies_lazy_miss_for_shadow_heap_mapping(
    ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;
    uint32_t *pte = 0;

    init_vma_proc(&proc);
    proc.pd_phys = paging_create_user_space();
    KTEST_ASSERT_NE(tc, proc.pd_phys, 0u);
    KTEST_ASSERT_EQ(tc, paging_walk(proc.pd_phys, proc.heap_start, &pte), 0u);
    KTEST_ASSERT_EQ(tc, *pte & (PG_PRESENT | PG_USER), PG_PRESENT);

    proc.crash.valid = 1;
    proc.crash.signum = SIGSEGV;
    proc.crash.cr2 = proc.heap_start;
    proc.crash.frame.vector = 14u;
    proc.crash.frame.error_code = 0x7u;

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.fault.classification,
                    MEM_FORENSICS_FAULT_LAZY_MISS);

    process_release_user_space(&proc);
}

static void test_mem_forensics_classifies_cow_write_fault(ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;
    uint32_t phys;
    uint32_t *pte = 0;

    init_vma_proc(&proc);
    proc.pd_phys = paging_create_user_space();
    KTEST_ASSERT_NE(tc, proc.pd_phys, 0u);

    phys = pmm_alloc_page();
    KTEST_ASSERT_NE(tc, phys, 0u);
    KTEST_ASSERT_EQ(tc,
                    paging_map_page(proc.pd_phys, proc.heap_start, phys,
                                    PG_PRESENT | PG_USER),
                    0u);
    KTEST_ASSERT_EQ(tc, paging_walk(proc.pd_phys, proc.heap_start, &pte), 0u);
    *pte |= PG_COW;

    proc.crash.valid = 1;
    proc.crash.signum = SIGSEGV;
    proc.crash.cr2 = proc.heap_start;
    proc.crash.frame.vector = 14u;
    proc.crash.frame.error_code = 0x7u;

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.fault.classification,
                    MEM_FORENSICS_FAULT_COW_WRITE);

    process_release_user_space(&proc);
}

static void test_mem_forensics_classifies_protection_fault(ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;

    k_memset(&proc, 0, sizeof(proc));
    vma_init(&proc);
    KTEST_ASSERT_EQ(tc,
                    vma_add(&proc, 0x80000000u, 0x80001000u,
                            VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
                            VMA_KIND_GENERIC),
                    0u);

    proc.crash.valid = 1;
    proc.crash.signum = SIGSEGV;
    proc.crash.cr2 = 0x80000000u;
    proc.crash.frame.vector = 14u;
    proc.crash.frame.error_code = 0x7u;

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.fault.classification,
                    MEM_FORENSICS_FAULT_PROTECTION);
}

static void test_mem_forensics_classifies_unknown_fault_vector(ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;

    init_vma_proc(&proc);
    proc.crash.valid = 1;
    proc.crash.signum = SIGSEGV;
    proc.crash.cr2 = proc.heap_start;
    proc.crash.frame.vector = 13u;
    proc.crash.frame.error_code = 0u;

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.fault.classification,
                    MEM_FORENSICS_FAULT_UNKNOWN);
}

static void test_mem_forensics_classifies_stack_limit_fault(ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;

    init_vma_proc(&proc);
    proc.crash.valid = 1;
    proc.crash.signum = SIGSEGV;
    proc.crash.cr2 = proc.stack_low_limit - PAGE_SIZE;
    proc.crash.frame.vector = 14u;
    proc.crash.frame.error_code = 0x6u;
    proc.crash.frame.user_esp = proc.stack_low_limit;

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.fault.classification,
                    MEM_FORENSICS_FAULT_STACK_LIMIT);
}

static void test_mem_forensics_counts_present_heap_pages(ktest_case_t *tc)
{
    static process_t proc;
    mem_forensics_report_t report;
    uint32_t phys;

    init_vma_proc(&proc);
    proc.pd_phys = paging_create_user_space();
    KTEST_ASSERT_NE(tc, proc.pd_phys, 0u);

    phys = pmm_alloc_page();
    KTEST_ASSERT_NE(tc, phys, 0u);
    KTEST_ASSERT_EQ(tc,
                    paging_map_page(proc.pd_phys, proc.heap_start, phys,
                                    PG_PRESENT | PG_USER | PG_WRITABLE),
                    0u);

    KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
    KTEST_EXPECT_EQ(tc, report.heap_mapped_bytes, 0x1000u);

    process_release_user_space(&proc);
}

static void test_linux_syscalls_fill_uname_time_and_fstat64(ktest_case_t *tc)
{
    static process_t seed;
    process_t *cur = start_syscall_test_process(&seed);
    uint8_t *page;

    KTEST_ASSERT_NOT_NULL(tc, cur);
    KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x00800000u), 0u);
    page = mapped_alias(cur, 0x00800000u);
    KTEST_ASSERT_NOT_NULL(tc, page);

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_UNAME, 0x00800000u, 0, 0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc, page[0], (uint8_t)'D');
    KTEST_EXPECT_EQ(tc, page[1], (uint8_t)'r');
    KTEST_EXPECT_EQ(tc, page[260], (uint8_t)'i');
    KTEST_EXPECT_EQ(tc, page[261], (uint8_t)'4');

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_GETTIMEOFDAY, 0x00800200u,
                                    0x00800210u, 0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc, page[0x210], 0u);
    KTEST_EXPECT_EQ(tc, page[0x211], 0u);

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_FSTAT64, 1, 0x00800400u,
                                    0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc, page[0x410], 0x80u);
    KTEST_EXPECT_EQ(tc, page[0x411], 0x21u);

    page[0x300] = '\0';
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_STATX, 1, 0x00800300u,
                                    TEST_LINUX_AT_EMPTY_PATH |
                                        TEST_LINUX_AT_NO_AUTOMOUNT,
                                    TEST_LINUX_STATX_BASIC_STATS,
                                    0x00800500u, 0),
                    0u);
    KTEST_EXPECT_EQ(tc, page[0x500], 0xffu);
    KTEST_EXPECT_EQ(tc, page[0x501], 0x07u);
    KTEST_EXPECT_EQ(tc, page[0x51c], 0x80u);
    KTEST_EXPECT_EQ(tc, page[0x51d], 0x21u);

    stop_syscall_test_process(cur);
}

static void test_linux_syscalls_support_busybox_identity_and_rt_sigmask(ktest_case_t *tc)
{
    static process_t seed;
    process_t *cur = start_syscall_test_process(&seed);
    uint8_t *page;

    KTEST_ASSERT_NOT_NULL(tc, cur);
    KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x00800000u), 0u);
    page = mapped_alias(cur, 0x00800000u);
    KTEST_ASSERT_NOT_NULL(tc, page);

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_GETUID32, 0, 0, 0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_GETGID32, 0, 0, 0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_GETEUID32, 0, 0, 0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_GETEGID32, 0, 0, 0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_SETGID32, 0, 0, 0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_SETUID32, 0, 0, 0, 0, 0, 0),
                    0u);

    page[0x100] = (uint8_t)(1u << 2);  /* block signal 2 */
    page[0x101] = 0;
    page[0x102] = 0;
    page[0x103] = 0;
    page[0x104] = 0xAA;
    page[0x105] = 0xAA;
    page[0x106] = 0xAA;
    page[0x107] = 0xAA;
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_RT_SIGPROCMASK, 0,
                                    0x00800100u, 0x00800200u, 8, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc, cur->sig_blocked, 1u << 2);
    KTEST_EXPECT_EQ(tc, page[0x200], 0u);
    KTEST_EXPECT_EQ(tc, page[0x204], 0u);

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_RT_SIGPROCMASK, 1,
                                    0x00800100u, 0x00800200u, 8, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc, cur->sig_blocked, 0u);
    KTEST_EXPECT_EQ(tc, page[0x200], 1u << 2);

    stop_syscall_test_process(cur);
}

static void test_linux_syscalls_support_busybox_stdio_helpers(ktest_case_t *tc)
{
    static process_t seed;
    process_t *cur = start_syscall_test_process(&seed);
    uint8_t *page;
    uint32_t *u32;

    KTEST_ASSERT_NOT_NULL(tc, cur);
    KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x00800000u), 0u);
    page = mapped_alias(cur, 0x00800000u);
    KTEST_ASSERT_NOT_NULL(tc, page);
    u32 = (uint32_t *)page;

    u32[0] = 0x00800100u;
    u32[1] = 3u;
    u32[2] = 0x00800110u;
    u32[3] = 2u;
    page[0x100] = (uint8_t)'o';
    page[0x101] = (uint8_t)'k';
    page[0x102] = (uint8_t)'\n';
    page[0x110] = (uint8_t)'.';
    page[0x111] = (uint8_t)'\n';
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_WRITEV, 1, 0x00800000u,
                                    2, 0, 0, 0),
                    5u);

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_GETCWD, 0x00800500u,
                                    32, 0, 0, 0, 0),
                    2u);
    KTEST_EXPECT_EQ(tc, page[0x500], (uint8_t)'/');
    KTEST_EXPECT_EQ(tc, page[0x501], 0u);

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_IOCTL, 1, TEST_LINUX_TIOCGWINSZ,
                                    0x00800200u, 0, 0, 0),
                    0u);
    KTEST_EXPECT_TRUE(tc, page[0x200] != 0u || page[0x201] != 0u);
    KTEST_EXPECT_TRUE(tc, page[0x202] != 0u || page[0x203] != 0u);

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_FCNTL64, 1, TEST_LINUX_F_GETFD,
                                    0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_FCNTL64, 1, TEST_LINUX_F_SETFD,
                                    1, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_FCNTL64, 1, TEST_LINUX_F_GETFL,
                                    0, 0, 0, 0),
                    1u);
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_FCNTL64, 1, TEST_LINUX_F_DUPFD,
                                    4, 0, 0, 0),
                    4u);
    KTEST_EXPECT_EQ(tc, cur->open_files[4].type, FD_TYPE_STDOUT);

    cur->open_files[3].type = FD_TYPE_FILE;
    cur->open_files[3].writable = 0;
    cur->open_files[3].u.file.inode_num = 1u;
    cur->open_files[3].u.file.size = 100u;
    cur->open_files[3].u.file.offset = 10u;

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS__LLSEEK, 3, 0, 5,
                                    0x00800300u, 1, 0),
                    0u);
    KTEST_EXPECT_EQ(tc, u32[0x300 / 4], 15u);
    KTEST_EXPECT_EQ(tc, u32[0x304 / 4], 0u);
    KTEST_EXPECT_EQ(tc, cur->open_files[3].u.file.offset, 15u);

    u32[0x400 / 4] = 5u;
    u32[0x404 / 4] = 0u;
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_SENDFILE64, 1, 3,
                                    0x00800400u, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc, u32[0x400 / 4], 5u);

    KTEST_EXPECT_EQ(tc, syscall_handler(SYS_GETTID, 0, 0, 0, 0, 0, 0),
                    cur->pid);

    stop_syscall_test_process(cur);
}

static void test_process_restore_user_tls_switches_global_gdt_slot(ktest_case_t *tc)
{
    static process_t first;
    static process_t second;
    uint32_t base = 0;
    uint32_t limit = 0;
    int limit_in_pages = 0;
    int present = 0;

    k_memset(&first, 0, sizeof(first));
    k_memset(&second, 0, sizeof(second));

    first.user_tls_present = 1;
    first.user_tls_base = 0x11111000u;
    first.user_tls_limit = 0x00000FFFu;
    first.user_tls_limit_in_pages = 0;

    second.user_tls_present = 1;
    second.user_tls_base = 0x22222000u;
    second.user_tls_limit = 0x000FFFFFu;
    second.user_tls_limit_in_pages = 1;

    process_restore_user_tls(&first);
    gdt_get_user_tls_for_test(&base, &limit, &limit_in_pages, &present);
    KTEST_EXPECT_EQ(tc, present, 1);
    KTEST_EXPECT_EQ(tc, base, first.user_tls_base);
    KTEST_EXPECT_EQ(tc, limit, first.user_tls_limit);
    KTEST_EXPECT_EQ(tc, limit_in_pages, first.user_tls_limit_in_pages);

    process_restore_user_tls(&second);
    gdt_get_user_tls_for_test(&base, &limit, &limit_in_pages, &present);
    KTEST_EXPECT_EQ(tc, present, 1);
    KTEST_EXPECT_EQ(tc, base, second.user_tls_base);
    KTEST_EXPECT_EQ(tc, limit, second.user_tls_limit);
    KTEST_EXPECT_EQ(tc, limit_in_pages, second.user_tls_limit_in_pages);

    second.user_tls_present = 0;
    process_restore_user_tls(&second);
    gdt_get_user_tls_for_test(&base, &limit, &limit_in_pages, &present);
    KTEST_EXPECT_EQ(tc, present, 0);
}

static void test_linux_syscalls_install_tls_and_map_mmap2(ktest_case_t *tc)
{
    static process_t seed;
    process_t *cur = start_syscall_test_process(&seed);
    uint8_t *page;
    uint32_t *desc;
    uint32_t addr;

    KTEST_ASSERT_NOT_NULL(tc, cur);
    KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x00801000u), 0u);
    page = mapped_alias(cur, 0x00801000u);
    KTEST_ASSERT_NOT_NULL(tc, page);

    desc = (uint32_t *)page;
    desc[0] = 0xFFFFFFFFu;
    desc[1] = 0x00801800u;
    desc[2] = 0x000FFFFFu;
    desc[3] = (1u << 0) | (1u << 4) | (1u << 6);

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_SET_THREAD_AREA, 0x00801000u,
                                    0, 0, 0, 0, 0),
                    0u);
    KTEST_EXPECT_EQ(tc, desc[0], GDT_USER_TLS_ENTRY);
    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_SET_TID_ADDRESS, 0x00801080u,
                                    0, 0, 0, 0, 0),
                    cur->pid);

    addr = syscall_handler(SYS_MMAP2, 0, PAGE_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS,
                           (uint32_t)-1, 0);
    KTEST_ASSERT_NE(tc, addr, (uint32_t)-1);
    KTEST_EXPECT_TRUE(tc, vma_find(cur, addr) != 0);

    KTEST_EXPECT_EQ(tc,
                    syscall_handler(SYS_MMAP2, 0, PAGE_SIZE,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS |
                                    TEST_LINUX_MAP_FIXED,
                                    (uint32_t)-1, 0),
                    (uint32_t)-1);

    stop_syscall_test_process(cur);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_process_build_initial_frame_layout),
    KTEST_CASE(test_process_build_exec_frame_layout),
    KTEST_CASE(test_sched_add_builds_initial_frame_for_never_run_process),
    KTEST_CASE(test_process_builds_linux_i386_initial_stack_shape),
    KTEST_CASE(test_vma_add_keeps_regions_sorted_and_findable),
    KTEST_CASE(test_vma_add_rejects_overlapping_regions),
    KTEST_CASE(test_vma_map_anonymous_places_regions_below_stack),
    KTEST_CASE(test_vma_unmap_range_splits_generic_mapping),
    KTEST_CASE(test_vma_unmap_range_rejects_heap_or_stack),
    KTEST_CASE(test_vma_protect_range_splits_and_requires_full_coverage),
    KTEST_CASE(test_mem_forensics_collects_basic_region_totals),
    KTEST_CASE(test_mem_forensics_core_note_sizes_are_nonzero),
    KTEST_CASE(test_core_dump_writes_drunix_notes_in_order),
    KTEST_CASE(test_mem_forensics_collects_fresh_process_layout),
    KTEST_CASE(test_mem_forensics_collects_full_vma_table_with_fallback_image),
    KTEST_CASE(test_mem_forensics_classifies_unmapped_fault),
    KTEST_CASE(test_mem_forensics_classifies_lazy_miss_for_shadow_heap_mapping),
    KTEST_CASE(test_mem_forensics_classifies_cow_write_fault),
    KTEST_CASE(test_mem_forensics_classifies_protection_fault),
    KTEST_CASE(test_mem_forensics_classifies_unknown_fault_vector),
    KTEST_CASE(test_mem_forensics_classifies_stack_limit_fault),
    KTEST_CASE(test_mem_forensics_counts_present_heap_pages),
    KTEST_CASE(test_linux_syscalls_fill_uname_time_and_fstat64),
    KTEST_CASE(test_linux_syscalls_support_busybox_identity_and_rt_sigmask),
    KTEST_CASE(test_linux_syscalls_support_busybox_stdio_helpers),
    KTEST_CASE(test_process_restore_user_tls_switches_global_gdt_slot),
    KTEST_CASE(test_linux_syscalls_install_tls_and_map_mmap2),
};

static ktest_suite_t suite = KTEST_SUITE("process", cases);

ktest_suite_t *ktest_suite_process(void) { return &suite; }
