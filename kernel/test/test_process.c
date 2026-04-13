/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_process.c — unit tests for synthetic process launch frames.
 */

#include "ktest.h"
#include "process.h"
#include "sched.h"
#include "gdt.h"
#include "kstring.h"
#include "vma.h"

extern void process_initial_launch(void);
extern void process_exec_launch(void);

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
    proc->brk = 0x00418000u;
    vma_init(proc);
    vma_add(proc, 0x00410000u, proc->brk,
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

static ktest_case_t cases[] = {
    KTEST_CASE(test_process_build_initial_frame_layout),
    KTEST_CASE(test_process_build_exec_frame_layout),
    KTEST_CASE(test_sched_add_builds_initial_frame_for_never_run_process),
    KTEST_CASE(test_vma_add_keeps_regions_sorted_and_findable),
    KTEST_CASE(test_vma_add_rejects_overlapping_regions),
    KTEST_CASE(test_vma_map_anonymous_places_regions_below_stack),
    KTEST_CASE(test_vma_unmap_range_splits_generic_mapping),
    KTEST_CASE(test_vma_unmap_range_rejects_heap_or_stack),
    KTEST_CASE(test_vma_protect_range_splits_and_requires_full_coverage),
};

static ktest_suite_t suite = KTEST_SUITE("process", cases);

ktest_suite_t *ktest_suite_process(void) { return &suite; }
