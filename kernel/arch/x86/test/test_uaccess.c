/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_uaccess.c — unit tests for validated user/kernel memory copies.
 */

#include "ktest.h"
#include "fault.h"
#include "uaccess.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "process.h"
#include "resources.h"
#include "pipe.h"
#include "sched.h"
#include "task_group.h"
#include "kstring.h"

#define TEST_IMAGE_START 0x00400000u
#define TEST_IMAGE_END 0x00409000u
#define TEST_IMAGE_PAGE 0x00408000u
#define TEST_BSS_ADDR 0x004086A0u
#define TEST_STACK_PAGE (USER_STACK_TOP - PAGE_SIZE)
#define TEST_STACK_ADDR (TEST_STACK_PAGE + 0x120u)
#define TEST_GROWN_STACK_PAGE (TEST_STACK_PAGE - PAGE_SIZE)
#define TEST_GROWN_STACK_ADDR (TEST_GROWN_STACK_PAGE + 0x80u)
#define PF_ERR_PRESENT 0x1u
#define PF_ERR_WRITE 0x2u
#define PF_ERR_USER 0x4u

static int init_test_proc(process_t *proc)
{
	k_memset(proc, 0, sizeof(*proc));
	vma_init(proc);
	proc->pd_phys = paging_create_user_space();
	return proc->pd_phys ? 0 : -1;
}

static void destroy_test_proc(process_t *proc)
{
	process_release_user_space(proc);
}

static uint32_t map_user_page(process_t *proc, uint32_t virt, uint8_t fill)
{
	uint32_t phys = pmm_alloc_page();
	uint32_t page = virt & ~0xFFFu;

	if (!phys)
		return 0;

	if (!vma_find(proc, page) && vma_add(proc,
	                                     page,
	                                     page + PAGE_SIZE,
	                                     VMA_FLAG_READ | VMA_FLAG_WRITE |
	                                         VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
	                                     VMA_KIND_GENERIC) != 0) {
		pmm_free_page(phys);
		return 0;
	}

	k_memset((void *)phys, fill, PAGE_SIZE);
	if (paging_map_page(
	        proc->pd_phys, virt, phys, PG_PRESENT | PG_WRITABLE | PG_USER) !=
	    0) {
		pmm_free_page(phys);
		return 0;
	}

	return phys;
}

static uint8_t *mapped_alias(process_t *proc, uint32_t virt)
{
	uint32_t *pte = 0;

	if (!proc || paging_walk(proc->pd_phys, virt, &pte) != 0)
		return 0;

	return (uint8_t *)(paging_entry_addr(*pte) + (virt & 0xFFFu));
}

static int init_fork_test_proc(process_t *proc,
                               uint32_t *kstack_words,
                               uint32_t kstack_word_count)
{
	if (init_test_proc(proc) != 0)
		return -1;

	proc->image_start = TEST_IMAGE_START;
	proc->image_end = TEST_IMAGE_END;
	proc->heap_start = TEST_IMAGE_END;
	proc->brk = TEST_IMAGE_END;
	proc->stack_low_limit = TEST_STACK_PAGE;
	proc->kstack_top = (uint32_t)(kstack_words + kstack_word_count);

	k_memset(kstack_words, 0, kstack_word_count * sizeof(*kstack_words));

	if (vma_add(proc,
	            proc->image_start,
	            proc->image_end,
	            VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC |
	                VMA_FLAG_PRIVATE,
	            VMA_KIND_IMAGE) != 0)
		goto fail;

	if (vma_add(proc,
	            USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * PAGE_SIZE,
	            USER_STACK_TOP,
	            VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
	            VMA_KIND_STACK) != 0)
		goto fail;

	if (!map_user_page(proc, TEST_IMAGE_PAGE, 0x00) ||
	    !map_user_page(proc, TEST_STACK_PAGE, 0x00))
		goto fail;

	return 0;

fail:
	destroy_test_proc(proc);
	return -1;
}

static void destroy_forked_child(process_t *proc)
{
	process_release_kstack(proc);
	destroy_test_proc(proc);
}

static unsigned exhaust_kernel_heap(void **ptrs, unsigned cap, uint32_t chunk)
{
	unsigned count = 0;

	while (count < cap) {
		ptrs[count] = kmalloc(chunk);
		if (!ptrs[count])
			break;
		count++;
	}

	return count;
}

static void release_kernel_heap_allocs(void **ptrs, unsigned count)
{
	for (unsigned i = 0; i < count; i++)
		kfree(ptrs[i]);
}

static void test_copy_from_user_reads_mapped_bytes(ktest_case_t *tc)
{
	static process_t proc;
	uint8_t buf[8];
	uint8_t *src;

	if (init_test_proc(&proc) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (!map_user_page(&proc, 0x400000u, 0x00)) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&proc);
		return;
	}

	src = mapped_alias(&proc, 0x400080u);
	if (!src) {
		KTEST_EXPECT_NOT_NULL(tc, src);
		destroy_test_proc(&proc);
		return;
	}
	src[0] = 0x11;
	src[1] = 0x22;
	src[2] = 0x33;
	src[3] = 0x44;

	KTEST_EXPECT_EQ(tc, uaccess_copy_from_user(&proc, buf, 0x400080u, 4u), 0u);
	KTEST_EXPECT_EQ(tc, buf[0], 0x11u);
	KTEST_EXPECT_EQ(tc, buf[1], 0x22u);
	KTEST_EXPECT_EQ(tc, buf[2], 0x33u);
	KTEST_EXPECT_EQ(tc, buf[3], 0x44u);

	destroy_test_proc(&proc);
}

static void test_copy_to_user_spans_pages(ktest_case_t *tc)
{
	static process_t proc;
	uint8_t src[200];
	uint8_t *dst0;
	uint8_t *dst1;
	uint32_t start = 0x401000u + PAGE_SIZE - 50u;

	if (init_test_proc(&proc) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (!map_user_page(&proc, 0x401000u, 0xAA) ||
	    !map_user_page(&proc, 0x402000u, 0xBB)) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&proc);
		return;
	}

	for (uint32_t i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)i;

	KTEST_EXPECT_EQ(
	    tc, uaccess_copy_to_user(&proc, start, src, sizeof(src)), 0u);

	dst0 = mapped_alias(&proc, start);
	dst1 = mapped_alias(&proc, 0x402000u);
	if (!dst0 || !dst1) {
		KTEST_EXPECT_NOT_NULL(tc, dst0);
		KTEST_EXPECT_NOT_NULL(tc, dst1);
		destroy_test_proc(&proc);
		return;
	}
	KTEST_EXPECT_EQ(tc, dst0[0], 0u);
	KTEST_EXPECT_EQ(tc, dst0[49], 49u);
	KTEST_EXPECT_EQ(tc, dst1[0], 50u);
	KTEST_EXPECT_EQ(tc, dst1[149], 199u);

	destroy_test_proc(&proc);
}

static void test_copy_string_from_user_spans_pages(ktest_case_t *tc)
{
	static process_t proc;
	char buf[32];
	static const char msg[] = "cross-page";
	uint32_t start = 0x410000u + PAGE_SIZE - 6u;

	if (init_test_proc(&proc) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (!map_user_page(&proc, 0x410000u, 0x00) ||
	    !map_user_page(&proc, 0x411000u, 0x00)) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&proc);
		return;
	}

	if (uaccess_copy_to_user(&proc, start, msg, sizeof(msg)) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&proc);
		return;
	}

	KTEST_EXPECT_EQ(
	    tc, uaccess_copy_string_from_user(&proc, buf, sizeof(buf), start), 0u);
	KTEST_EXPECT_EQ(tc, k_strcmp(buf, msg), 0u);

	destroy_test_proc(&proc);
}

static void test_prepare_rejects_kernel_and_unmapped_ranges(ktest_case_t *tc)
{
	static process_t proc;

	if (init_test_proc(&proc) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	KTEST_EXPECT_EQ(tc, uaccess_prepare(&proc, 0x1000u, 1u, 0), (uint32_t)-1);

	if (!map_user_page(&proc, 0x420000u, 0x00)) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&proc);
		return;
	}

	KTEST_EXPECT_EQ(tc,
	                uaccess_prepare(&proc, 0x420000u + PAGE_SIZE - 2u, 4u, 0),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(
	    tc, uaccess_prepare(&proc, USER_STACK_TOP - 2u, 4u, 0), (uint32_t)-1);

	destroy_test_proc(&proc);
}

static void test_copy_to_user_breaks_cow_clone(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	uint32_t *parent_pte = 0;
	uint32_t *child_pte = 0;
	uint32_t before_phys;
	static const char parent_msg[] = "parent";
	static const char child_msg[] = "child";

	if (init_test_proc(&parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (!map_user_page(&parent, 0x430000u, 0x00)) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	if (uaccess_copy_to_user(
	        &parent, 0x430000u, parent_msg, sizeof(parent_msg)) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	k_memset(&child, 0, sizeof(child));
	child.pd_phys = paging_clone_user_space(parent.pd_phys);
	child.vma_count = parent.vma_count;
	k_memcpy(child.vmas, parent.vmas, sizeof(parent.vmas));
	if (!child.pd_phys) {
		KTEST_EXPECT_NOT_NULL(tc, child.pd_phys);
		destroy_test_proc(&parent);
		return;
	}

	if (paging_walk(parent.pd_phys, 0x430000u, &parent_pte) != 0 ||
	    paging_walk(child.pd_phys, 0x430000u, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&child);
		destroy_test_proc(&parent);
		return;
	}

	before_phys = *parent_pte & ~0xFFFu;
	KTEST_EXPECT_EQ(tc, before_phys, *child_pte & ~0xFFFu);
	KTEST_EXPECT_TRUE(tc, (*parent_pte & PG_COW) != 0);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_COW) != 0);
	KTEST_EXPECT_EQ(tc, pmm_refcount(before_phys), 2u);

	KTEST_EXPECT_EQ(
	    tc,
	    uaccess_copy_to_user(&child, 0x430000u, child_msg, sizeof(child_msg)),
	    0u);

	if (paging_walk(parent.pd_phys, 0x430000u, &parent_pte) != 0 ||
	    paging_walk(child.pd_phys, 0x430000u, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&child);
		destroy_test_proc(&parent);
		return;
	}
	KTEST_EXPECT_NE(tc, *parent_pte & ~0xFFFu, *child_pte & ~0xFFFu);
	KTEST_EXPECT_EQ(tc, pmm_refcount(*parent_pte & ~0xFFFu), 1u);
	KTEST_EXPECT_EQ(tc, pmm_refcount(*child_pte & ~0xFFFu), 1u);
	KTEST_EXPECT_EQ(
	    tc, k_strcmp((char *)mapped_alias(&parent, 0x430000u), parent_msg), 0u);
	KTEST_EXPECT_EQ(
	    tc, k_strcmp((char *)mapped_alias(&child, 0x430000u), child_msg), 0u);

	destroy_test_proc(&child);
	destroy_test_proc(&parent);
}

static void
test_release_user_space_drops_only_child_cow_reference(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	uint32_t *parent_pte = 0;
	uint32_t *child_pte = 0;
	uint32_t shared_phys;
	static const char msg[] = "still-parent";

	if (init_test_proc(&parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (!map_user_page(&parent, 0x440000u, 0x00)) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	if (uaccess_copy_to_user(&parent, 0x440000u, msg, sizeof(msg)) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	k_memset(&child, 0, sizeof(child));
	child.pd_phys = paging_clone_user_space(parent.pd_phys);
	child.vma_count = parent.vma_count;
	k_memcpy(child.vmas, parent.vmas, sizeof(parent.vmas));
	if (!child.pd_phys) {
		KTEST_EXPECT_NOT_NULL(tc, child.pd_phys);
		destroy_test_proc(&parent);
		return;
	}

	if (paging_walk(parent.pd_phys, 0x440000u, &parent_pte) != 0 ||
	    paging_walk(child.pd_phys, 0x440000u, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&child);
		destroy_test_proc(&parent);
		return;
	}

	shared_phys = *parent_pte & ~0xFFFu;
	KTEST_EXPECT_EQ(tc, shared_phys, *child_pte & ~0xFFFu);
	KTEST_EXPECT_EQ(tc, pmm_refcount(shared_phys), 2u);

	destroy_test_proc(&child);

	KTEST_EXPECT_EQ(tc, pmm_refcount(shared_phys), 1u);
	KTEST_EXPECT_EQ(
	    tc, k_strcmp((char *)mapped_alias(&parent, 0x440000u), msg), 0u);

	destroy_test_proc(&parent);
}

static void
test_process_fork_stack_pages_break_cow_on_write_fault(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];
	static const char parent_msg[] = "parent-stack";
	static const char child_msg[] = "child-stack";
	uint32_t stack_page = USER_STACK_TOP - PAGE_SIZE;
	uint32_t stack_addr = stack_page + 0x120u;
	uint32_t *parent_pte = 0;
	uint32_t *child_pte = 0;

	if (init_test_proc(&parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	parent.stack_low_limit = stack_page;
	parent.kstack_top = (uint32_t)(parent_kstack_words + 64);
	k_memset(parent_kstack_words, 0, sizeof(parent_kstack_words));

	if (vma_add(&parent,
	            stack_page,
	            USER_STACK_TOP,
	            VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
	            VMA_KIND_STACK) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	if (!map_user_page(&parent, stack_page, 0x00)) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	if (uaccess_copy_to_user(
	        &parent, stack_addr, parent_msg, sizeof(parent_msg)) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	k_memset(&child, 0, sizeof(child));
	if (process_fork(&child, &parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	if (paging_walk(parent.pd_phys, stack_addr, &parent_pte) != 0 ||
	    paging_walk(child.pd_phys, stack_addr, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		process_release_kstack(&child);
		destroy_test_proc(&child);
		destroy_test_proc(&parent);
		return;
	}

	KTEST_EXPECT_EQ(tc, *parent_pte & ~0xFFFu, *child_pte & ~0xFFFu);
	KTEST_EXPECT_TRUE(tc, (*parent_pte & PG_COW) != 0);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_COW) != 0);
	KTEST_EXPECT_EQ(tc, pmm_refcount(*parent_pte & ~0xFFFu), 2u);
	KTEST_EXPECT_EQ(
	    tc,
	    k_strcmp((char *)mapped_alias(&parent, stack_addr), parent_msg),
	    0u);

	KTEST_EXPECT_EQ(
	    tc,
	    paging_handle_fault(child.pd_phys,
	                        stack_addr,
	                        PF_ERR_PRESENT | PF_ERR_WRITE | PF_ERR_USER,
	                        stack_addr + 16u,
	                        &child),
	    0u);

	if (paging_walk(parent.pd_phys, stack_addr, &parent_pte) != 0 ||
	    paging_walk(child.pd_phys, stack_addr, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		process_release_kstack(&child);
		destroy_test_proc(&child);
		destroy_test_proc(&parent);
		return;
	}

	KTEST_EXPECT_NE(tc, *parent_pte & ~0xFFFu, *child_pte & ~0xFFFu);
	KTEST_EXPECT_EQ(tc, pmm_refcount(*parent_pte & ~0xFFFu), 1u);
	KTEST_EXPECT_EQ(tc, pmm_refcount(*child_pte & ~0xFFFu), 1u);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_COW) == 0);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_WRITABLE) != 0);

	KTEST_EXPECT_EQ(
	    tc,
	    uaccess_copy_to_user(&child, stack_addr, child_msg, sizeof(child_msg)),
	    0u);
	KTEST_EXPECT_EQ(
	    tc,
	    k_strcmp((char *)mapped_alias(&parent, stack_addr), parent_msg),
	    0u);
	KTEST_EXPECT_EQ(
	    tc, k_strcmp((char *)mapped_alias(&child, stack_addr), child_msg), 0u);

	process_release_kstack(&child);
	destroy_test_proc(&child);
	destroy_test_proc(&parent);
}

static void
test_process_fork_child_writes_image_data_before_exec(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];
	static const char parent_msg[] = "parent-bss";
	static const char child_msg[] = "child-bss";
	uint32_t *parent_pte = 0;
	uint32_t *child_pte = 0;
	int child_live = 0;

	if (init_fork_test_proc(&parent, parent_kstack_words, 64u) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (uaccess_copy_to_user(
	        &parent, TEST_BSS_ADDR, parent_msg, sizeof(parent_msg)) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	k_memset(&child, 0, sizeof(child));
	if (process_fork(&child, &parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}
	child_live = 1;

	if (paging_walk(parent.pd_phys, TEST_BSS_ADDR, &parent_pte) != 0 ||
	    paging_walk(child.pd_phys, TEST_BSS_ADDR, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		goto cleanup;
	}

	KTEST_EXPECT_EQ(tc, *parent_pte & ~0xFFFu, *child_pte & ~0xFFFu);
	KTEST_EXPECT_TRUE(tc, (*parent_pte & PG_COW) != 0);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_COW) != 0);
	KTEST_EXPECT_EQ(tc, pmm_refcount(*parent_pte & ~0xFFFu), 2u);

	KTEST_EXPECT_EQ(tc,
	                uaccess_copy_to_user(
	                    &child, TEST_BSS_ADDR, child_msg, sizeof(child_msg)),
	                0u);

	if (paging_walk(parent.pd_phys, TEST_BSS_ADDR, &parent_pte) != 0 ||
	    paging_walk(child.pd_phys, TEST_BSS_ADDR, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		goto cleanup;
	}

	KTEST_EXPECT_NE(tc, *parent_pte & ~0xFFFu, *child_pte & ~0xFFFu);
	KTEST_EXPECT_EQ(tc, pmm_refcount(*parent_pte & ~0xFFFu), 1u);
	KTEST_EXPECT_EQ(tc, pmm_refcount(*child_pte & ~0xFFFu), 1u);
	KTEST_EXPECT_EQ(
	    tc,
	    k_strcmp((char *)mapped_alias(&parent, TEST_BSS_ADDR), parent_msg),
	    0u);
	KTEST_EXPECT_EQ(
	    tc,
	    k_strcmp((char *)mapped_alias(&child, TEST_BSS_ADDR), child_msg),
	    0u);

cleanup:
	if (child_live)
		destroy_forked_child(&child);
	destroy_test_proc(&parent);
}

static void test_process_fork_preserves_user_io_mappings(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];
	uint32_t io_virt = 0x480000u;
	uint32_t io_phys = 0xE0000000u;
	uint32_t *parent_pte = 0;
	uint32_t *child_pte = 0;
	int child_live = 0;

	if (init_fork_test_proc(&parent, parent_kstack_words, 64u) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (vma_add(&parent,
	            io_virt,
	            io_virt + PAGE_SIZE,
	            VMA_FLAG_READ | VMA_FLAG_WRITE,
	            VMA_KIND_GENERIC) != 0 ||
	    paging_map_page(parent.pd_phys,
	                    io_virt,
	                    io_phys,
	                    PG_PRESENT | PG_WRITABLE | PG_USER | PG_IO) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	k_memset(&child, 0, sizeof(child));
	if (process_fork(&child, &parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}
	child_live = 1;

	if (paging_walk(parent.pd_phys, io_virt, &parent_pte) != 0 ||
	    paging_walk(child.pd_phys, io_virt, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		goto cleanup;
	}

	KTEST_EXPECT_EQ(tc, *parent_pte & PG_ENTRY_ADDR_MASK, io_phys);
	KTEST_EXPECT_EQ(tc, *child_pte & PG_ENTRY_ADDR_MASK, io_phys);
	KTEST_EXPECT_TRUE(tc, (*parent_pte & PG_IO) != 0);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_IO) != 0);
	KTEST_EXPECT_TRUE(tc, (*parent_pte & PG_WRITABLE) != 0);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_WRITABLE) != 0);
	KTEST_EXPECT_TRUE(tc, (*parent_pte & PG_COW) == 0);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_COW) == 0);

cleanup:
	if (child_live)
		destroy_forked_child(&child);
	destroy_test_proc(&parent);
}

static void
test_process_fork_child_survives_parent_exit_and_reuses_last_cow_ref(
    ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];
	static const char parent_msg[] = "parent-last-ref";
	static const char child_msg[] = "child-last-ref";
	uint32_t *child_pte = 0;
	uint32_t shared_phys = 0;
	int child_live = 0;

	if (init_fork_test_proc(&parent, parent_kstack_words, 64u) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (uaccess_copy_to_user(
	        &parent, TEST_BSS_ADDR, parent_msg, sizeof(parent_msg)) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	k_memset(&child, 0, sizeof(child));
	if (process_fork(&child, &parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}
	child_live = 1;

	if (paging_walk(child.pd_phys, TEST_BSS_ADDR, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		goto cleanup;
	}

	shared_phys = *child_pte & ~0xFFFu;
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_COW) != 0);
	KTEST_EXPECT_EQ(tc, pmm_refcount(shared_phys), 2u);

	destroy_test_proc(&parent);

	KTEST_EXPECT_EQ(tc, pmm_refcount(shared_phys), 1u);
	KTEST_EXPECT_EQ(tc,
	                uaccess_copy_to_user(
	                    &child, TEST_BSS_ADDR, child_msg, sizeof(child_msg)),
	                0u);

	if (paging_walk(child.pd_phys, TEST_BSS_ADDR, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		goto cleanup_child_only;
	}

	KTEST_EXPECT_EQ(tc, *child_pte & ~0xFFFu, shared_phys);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_COW) == 0);
	KTEST_EXPECT_TRUE(tc, (*child_pte & PG_WRITABLE) != 0);
	KTEST_EXPECT_EQ(tc, pmm_refcount(shared_phys), 1u);
	KTEST_EXPECT_EQ(
	    tc,
	    k_strcmp((char *)mapped_alias(&child, TEST_BSS_ADDR), child_msg),
	    0u);

cleanup_child_only:
	if (child_live)
		destroy_forked_child(&child);
	return;

cleanup:
	if (child_live)
		destroy_forked_child(&child);
	destroy_test_proc(&parent);
}

static void test_process_fork_child_stack_growth_is_private(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];
	static const char parent_msg[] = "parent-stack";
	static const char child_msg[] = "grown-stack";
	uint32_t *child_pte = 0;
	int child_live = 0;

	if (init_fork_test_proc(&parent, parent_kstack_words, 64u) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (uaccess_copy_to_user(
	        &parent, TEST_STACK_ADDR, parent_msg, sizeof(parent_msg)) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	k_memset(&child, 0, sizeof(child));
	if (process_fork(&child, &parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}
	child_live = 1;

	KTEST_EXPECT_EQ(tc,
	                uaccess_prepare(&parent, TEST_GROWN_STACK_ADDR, 1u, 1),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(tc,
	                paging_handle_fault(child.pd_phys,
	                                    TEST_GROWN_STACK_ADDR,
	                                    PF_ERR_WRITE | PF_ERR_USER,
	                                    TEST_GROWN_STACK_ADDR + 16u,
	                                    &child),
	                0u);
	KTEST_EXPECT_EQ(tc, child.stack_low_limit, TEST_GROWN_STACK_PAGE);
	KTEST_EXPECT_EQ(tc, parent.stack_low_limit, TEST_STACK_PAGE);
	KTEST_EXPECT_EQ(
	    tc, uaccess_prepare(&child, TEST_GROWN_STACK_ADDR, 1u, 1), 0u);
	KTEST_EXPECT_EQ(tc,
	                uaccess_prepare(&parent, TEST_GROWN_STACK_ADDR, 1u, 1),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(
	    tc,
	    uaccess_copy_to_user(
	        &child, TEST_GROWN_STACK_ADDR, child_msg, sizeof(child_msg)),
	    0u);

	if (paging_walk(child.pd_phys, TEST_GROWN_STACK_ADDR, &child_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		goto cleanup;
	}

	KTEST_EXPECT_TRUE(
	    tc, (*child_pte & (PG_PRESENT | PG_USER)) == (PG_PRESENT | PG_USER));
	KTEST_EXPECT_EQ(tc, pmm_refcount(*child_pte & ~0xFFFu), 1u);
	KTEST_EXPECT_EQ(
	    tc,
	    k_strcmp((char *)mapped_alias(&parent, TEST_STACK_ADDR), parent_msg),
	    0u);
	KTEST_EXPECT_EQ(
	    tc,
	    k_strcmp((char *)mapped_alias(&child, TEST_GROWN_STACK_ADDR),
	             child_msg),
	    0u);

cleanup:
	if (child_live)
		destroy_forked_child(&child);
	destroy_test_proc(&parent);
}

static void test_process_fork_child_gets_fresh_task_group_slot(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];

	sched_init();
	if (init_fork_test_proc(&parent, parent_kstack_words, 64u) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	parent.tid = 1;
	parent.pid = 1;
	parent.tgid = 1;
	parent.group = task_group_create(parent.tgid,
	                                 parent.tid,
	                                 0,
	                                 parent.pgid,
	                                 parent.sid,
	                                 parent.tty_id,
	                                 SIGCHLD);
	KTEST_ASSERT_NOT_NULL(tc, parent.group);
	task_group_add_task(parent.group);

	k_memset(&child, 0, sizeof(child));
	if (process_fork(&child, &parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		task_group_put(parent.group);
		destroy_test_proc(&parent);
		sched_init();
		return;
	}

	KTEST_EXPECT_EQ(tc, child.tgid, 0u);
	KTEST_EXPECT_NULL(tc, child.group);
	KTEST_EXPECT_EQ(tc, task_group_live_count(parent.group), 1u);

	destroy_forked_child(&child);
	task_group_put(parent.group);
	destroy_test_proc(&parent);
	sched_init();
}

/*
 * End-to-end regression for the abacc35 fork/execve interaction: after
 * fork() clears the child's tid/tgid/group and sched_add() assigns a fresh
 * task group, the execve guard at syscall.c's SYS_EXECVE dispatch
 *   (cur->group && task_group_live_count(cur->group) > 1)
 * must return false for the child. Before the fix the child inherited the
 * parent's group pointer, so at fork+sched_add time the parent's group
 * had live_tasks == 2 and execve was rejected. This test mirrors the shell
 * path: add the parent to the scheduler, fork, add the child, and verify
 * both sides independently pass the guard.
 */
static void
test_process_fork_then_sched_add_child_clears_exec_guard(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];
	process_t *parent_slot;
	process_t *child_slot;
	uint32_t parent_pd;
	uint32_t child_pd;

	sched_init();
	if (init_fork_test_proc(&parent, parent_kstack_words, 64u) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}
	/* Skip initial-frame construction; this test never context-switches. */
	parent.arch_state.context = 1;

	if (sched_add(&parent) < 1) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		sched_init();
		return;
	}
	parent_slot = sched_find_pid(parent.pid);
	KTEST_ASSERT_NOT_NULL(tc, parent_slot);
	KTEST_ASSERT_NOT_NULL(tc, parent_slot->group);
	KTEST_EXPECT_EQ(tc, task_group_live_count(parent_slot->group), 1u);

	k_memset(&child, 0, sizeof(child));
	if (process_fork(&child, parent_slot) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		sched_force_remove_task(parent.pid);
		destroy_test_proc(&parent);
		sched_init();
		return;
	}

	KTEST_EXPECT_NULL(tc, child.group);

	if (sched_add(&child) < 1) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_forked_child(&child);
		sched_force_remove_task(parent.pid);
		destroy_test_proc(&parent);
		sched_init();
		return;
	}
	child_slot = sched_find_pid(child.pid);
	KTEST_ASSERT_NOT_NULL(tc, child_slot);
	KTEST_ASSERT_NOT_NULL(tc, child_slot->group);

	/* The fix: child must land in its own task group, not the parent's. */
	KTEST_EXPECT_TRUE(tc, child_slot->group != parent_slot->group);
	KTEST_EXPECT_EQ(tc, task_group_live_count(child_slot->group), 1u);
	KTEST_EXPECT_EQ(tc, task_group_live_count(parent_slot->group), 1u);

	/* Simulate the SYS_EXECVE guard for both tasks — neither must trip it. */
	KTEST_EXPECT_TRUE(
	    tc,
	    !(child_slot->group && task_group_live_count(child_slot->group) > 1u));
	KTEST_EXPECT_TRUE(tc,
	                  !(parent_slot->group &&
	                    task_group_live_count(parent_slot->group) > 1u));

	/* sched_force_remove_task does not release pd_phys (there is no
     * address-space struct in this test harness), so capture and free it
     * manually to avoid leaking physical pages across test cases. */
	child_pd = child_slot->pd_phys;
	parent_pd = parent_slot->pd_phys;
	child_slot->pd_phys = 0;
	parent_slot->pd_phys = 0;

	sched_force_remove_task(child.pid);
	sched_force_remove_task(parent.pid);

	child.pd_phys = child_pd;
	parent.pd_phys = parent_pd;
	child.kstack_bottom = 0; /* already freed by sched_force_remove_task */
	process_release_user_space(&child);
	destroy_test_proc(&parent);
	sched_init();
}

static void
test_repeated_fork_exec_cleanup_preserves_parent_refs(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];
	static const char parent_bss[] = "parent-loop-bss";
	static const char parent_stack[] = "parent-loop-stack";
	static const char child_bss[] = "child-loop-bss";
	static const char child_stack[] = "child-loop-stack";
	uint32_t *parent_bss_pte = 0;
	uint32_t *parent_stack_pte = 0;
	uint32_t *child_bss_pte = 0;
	uint32_t *child_stack_pte = 0;
	uint32_t parent_bss_phys = 0;
	uint32_t parent_stack_phys = 0;

	if (init_fork_test_proc(&parent, parent_kstack_words, 64u) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (uaccess_copy_to_user(
	        &parent, TEST_BSS_ADDR, parent_bss, sizeof(parent_bss)) != 0 ||
	    uaccess_copy_to_user(
	        &parent, TEST_STACK_ADDR, parent_stack, sizeof(parent_stack)) !=
	        0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	if (paging_walk(parent.pd_phys, TEST_BSS_ADDR, &parent_bss_pte) != 0 ||
	    paging_walk(parent.pd_phys, TEST_STACK_ADDR, &parent_stack_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	parent_bss_phys = *parent_bss_pte & ~0xFFFu;
	parent_stack_phys = *parent_stack_pte & ~0xFFFu;

	for (uint32_t i = 0; i < 32u; i++) {
		uint32_t old_child_pd;
		uint32_t old_child_kstack;

		k_memset(&child, 0, sizeof(child));
		if (process_fork(&child, &parent) != 0) {
			KTEST_EXPECT_TRUE(tc, 0);
			break;
		}

		if (paging_walk(parent.pd_phys, TEST_BSS_ADDR, &parent_bss_pte) != 0 ||
		    paging_walk(parent.pd_phys, TEST_STACK_ADDR, &parent_stack_pte) !=
		        0 ||
		    paging_walk(child.pd_phys, TEST_BSS_ADDR, &child_bss_pte) != 0 ||
		    paging_walk(child.pd_phys, TEST_STACK_ADDR, &child_stack_pte) !=
		        0) {
			KTEST_EXPECT_TRUE(tc, 0);
			destroy_forked_child(&child);
			break;
		}

		KTEST_EXPECT_EQ(tc, *parent_bss_pte & ~0xFFFu, parent_bss_phys);
		KTEST_EXPECT_EQ(tc, *child_bss_pte & ~0xFFFu, parent_bss_phys);
		KTEST_EXPECT_TRUE(tc, (*parent_bss_pte & PG_COW) != 0);
		KTEST_EXPECT_TRUE(tc, (*child_bss_pte & PG_COW) != 0);
		KTEST_EXPECT_EQ(tc, pmm_refcount(parent_bss_phys), 2u);

		KTEST_EXPECT_EQ(tc, *parent_stack_pte & ~0xFFFu, parent_stack_phys);
		KTEST_EXPECT_EQ(tc, *child_stack_pte & ~0xFFFu, parent_stack_phys);
		KTEST_EXPECT_TRUE(tc, (*parent_stack_pte & PG_COW) != 0);
		KTEST_EXPECT_TRUE(tc, (*child_stack_pte & PG_COW) != 0);
		KTEST_EXPECT_EQ(tc, pmm_refcount(parent_stack_phys), 2u);

		KTEST_EXPECT_EQ(
		    tc,
		    uaccess_copy_to_user(
		        &child, TEST_BSS_ADDR, child_bss, sizeof(child_bss)),
		    0u);
		KTEST_EXPECT_EQ(
		    tc,
		    uaccess_copy_to_user(
		        &child, TEST_STACK_ADDR, child_stack, sizeof(child_stack)),
		    0u);

		old_child_pd = child.pd_phys;
		old_child_kstack = child.kstack_bottom;
		child.pd_phys = 0;
		child.kstack_bottom = 0;
		child.kstack_top = 0;
		child.arch_state.context = 0;
		process_exec_cleanup(old_child_pd, old_child_kstack);

		if (paging_walk(parent.pd_phys, TEST_BSS_ADDR, &parent_bss_pte) != 0 ||
		    paging_walk(parent.pd_phys, TEST_STACK_ADDR, &parent_stack_pte) !=
		        0) {
			KTEST_EXPECT_TRUE(tc, 0);
			break;
		}

		KTEST_EXPECT_EQ(tc, *parent_bss_pte & ~0xFFFu, parent_bss_phys);
		KTEST_EXPECT_EQ(tc, *parent_stack_pte & ~0xFFFu, parent_stack_phys);
		KTEST_EXPECT_EQ(tc, pmm_refcount(parent_bss_phys), 1u);
		KTEST_EXPECT_EQ(tc, pmm_refcount(parent_stack_phys), 1u);
		KTEST_EXPECT_EQ(
		    tc,
		    k_strcmp((char *)mapped_alias(&parent, TEST_BSS_ADDR), parent_bss),
		    0u);
		KTEST_EXPECT_EQ(tc,
		                k_strcmp((char *)mapped_alias(&parent, TEST_STACK_ADDR),
		                         parent_stack),
		                0u);
	}

	destroy_test_proc(&parent);
}

static void
test_process_fork_bumps_pipe_refs_once_with_resource_table(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];
	int pipe_idx;
	pipe_buf_t *pb;

	if (init_fork_test_proc(&parent, parent_kstack_words, 64u) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}
	if (proc_resource_init_fresh(&parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	pipe_idx = pipe_alloc();
	if (pipe_idx < 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		proc_resource_put_all(&parent);
		destroy_test_proc(&parent);
		return;
	}
	pb = pipe_get(pipe_idx);
	KTEST_ASSERT_NOT_NULL(tc, pb);

	parent.open_files[3].type = FD_TYPE_PIPE_READ;
	parent.open_files[3].u.pipe.pipe_idx = (uint32_t)pipe_idx;
	parent.open_files[4].type = FD_TYPE_PIPE_WRITE;
	parent.open_files[4].writable = 1;
	parent.open_files[4].u.pipe.pipe_idx = (uint32_t)pipe_idx;
	proc_resource_mirror_from_process(&parent);

	KTEST_EXPECT_EQ(tc, pb->read_open, 1u);
	KTEST_EXPECT_EQ(tc, pb->write_open, 1u);

	k_memset(&child, 0, sizeof(child));
	if (process_fork(&child, &parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		proc_resource_put_all(&parent);
		destroy_test_proc(&parent);
		return;
	}

	KTEST_EXPECT_EQ(tc, pb->read_open, 2u);
	KTEST_EXPECT_EQ(tc, pb->write_open, 2u);

	proc_resource_put_all(&child);
	destroy_forked_child(&child);
	proc_resource_put_all(&parent);
	destroy_test_proc(&parent);
}

static void test_copy_to_user_handles_three_way_cow_sharing(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child1;
	static process_t child2;
	uint32_t *parent_pte = 0;
	uint32_t *child1_pte = 0;
	uint32_t *child2_pte = 0;
	uint32_t shared_phys;
	static const char parent_msg[] = "parent-3way";
	static const char child1_msg[] = "child1-3way";
	static const char child2_msg[] = "child2-3way";

	if (init_test_proc(&parent) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (!map_user_page(&parent, 0x450000u, 0x00)) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	if (uaccess_copy_to_user(
	        &parent, 0x450000u, parent_msg, sizeof(parent_msg)) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	k_memset(&child1, 0, sizeof(child1));
	child1.pd_phys = paging_clone_user_space(parent.pd_phys);
	child1.vma_count = parent.vma_count;
	k_memcpy(child1.vmas, parent.vmas, sizeof(parent.vmas));

	k_memset(&child2, 0, sizeof(child2));
	child2.pd_phys = paging_clone_user_space(parent.pd_phys);
	child2.vma_count = parent.vma_count;
	k_memcpy(child2.vmas, parent.vmas, sizeof(parent.vmas));

	if (!child1.pd_phys || !child2.pd_phys) {
		KTEST_EXPECT_NOT_NULL(tc, child1.pd_phys);
		KTEST_EXPECT_NOT_NULL(tc, child2.pd_phys);
		destroy_test_proc(&child2);
		destroy_test_proc(&child1);
		destroy_test_proc(&parent);
		return;
	}

	if (paging_walk(parent.pd_phys, 0x450000u, &parent_pte) != 0 ||
	    paging_walk(child1.pd_phys, 0x450000u, &child1_pte) != 0 ||
	    paging_walk(child2.pd_phys, 0x450000u, &child2_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&child2);
		destroy_test_proc(&child1);
		destroy_test_proc(&parent);
		return;
	}

	shared_phys = *parent_pte & ~0xFFFu;
	KTEST_EXPECT_EQ(tc, shared_phys, *child1_pte & ~0xFFFu);
	KTEST_EXPECT_EQ(tc, shared_phys, *child2_pte & ~0xFFFu);
	KTEST_EXPECT_EQ(tc, pmm_refcount(shared_phys), 3u);

	KTEST_EXPECT_EQ(tc,
	                uaccess_copy_to_user(
	                    &child2, 0x450000u, child2_msg, sizeof(child2_msg)),
	                0u);

	if (paging_walk(parent.pd_phys, 0x450000u, &parent_pte) != 0 ||
	    paging_walk(child1.pd_phys, 0x450000u, &child1_pte) != 0 ||
	    paging_walk(child2.pd_phys, 0x450000u, &child2_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&child2);
		destroy_test_proc(&child1);
		destroy_test_proc(&parent);
		return;
	}

	KTEST_EXPECT_EQ(tc, *parent_pte & ~0xFFFu, shared_phys);
	KTEST_EXPECT_EQ(tc, *child1_pte & ~0xFFFu, shared_phys);
	KTEST_EXPECT_NE(tc, *child2_pte & ~0xFFFu, shared_phys);
	KTEST_EXPECT_EQ(tc, pmm_refcount(shared_phys), 2u);

	KTEST_EXPECT_EQ(tc,
	                uaccess_copy_to_user(
	                    &child1, 0x450000u, child1_msg, sizeof(child1_msg)),
	                0u);

	if (paging_walk(parent.pd_phys, 0x450000u, &parent_pte) != 0 ||
	    paging_walk(child1.pd_phys, 0x450000u, &child1_pte) != 0 ||
	    paging_walk(child2.pd_phys, 0x450000u, &child2_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&child2);
		destroy_test_proc(&child1);
		destroy_test_proc(&parent);
		return;
	}

	KTEST_EXPECT_NE(tc, *parent_pte & ~0xFFFu, *child1_pte & ~0xFFFu);
	KTEST_EXPECT_NE(tc, *parent_pte & ~0xFFFu, *child2_pte & ~0xFFFu);
	KTEST_EXPECT_NE(tc, *child1_pte & ~0xFFFu, *child2_pte & ~0xFFFu);
	KTEST_EXPECT_EQ(tc, pmm_refcount(*parent_pte & ~0xFFFu), 1u);
	KTEST_EXPECT_EQ(
	    tc, k_strcmp((char *)mapped_alias(&parent, 0x450000u), parent_msg), 0u);
	KTEST_EXPECT_EQ(
	    tc, k_strcmp((char *)mapped_alias(&child1, 0x450000u), child1_msg), 0u);
	KTEST_EXPECT_EQ(
	    tc, k_strcmp((char *)mapped_alias(&child2, 0x450000u), child2_msg), 0u);

	destroy_test_proc(&child2);
	destroy_test_proc(&child1);
	destroy_test_proc(&parent);
}

static void
test_process_fork_rolls_back_when_kstack_alloc_fails(ktest_case_t *tc)
{
	static process_t parent;
	static process_t child;
	static uint32_t parent_kstack_words[64];
	static void *heap_ptrs[128];
	static const char parent_bss[] = "parent-fork-bss";
	static const char parent_stack[] = "parent-fork-stack";
	uint32_t *parent_bss_pte = 0;
	uint32_t *parent_stack_pte = 0;
	uint32_t parent_bss_phys = 0;
	uint32_t parent_stack_phys = 0;
	uint32_t free_pages_before = 0;
	unsigned heap_count = 0;

	if (init_fork_test_proc(&parent, parent_kstack_words, 64u) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		return;
	}

	if (uaccess_copy_to_user(
	        &parent, TEST_BSS_ADDR, parent_bss, sizeof(parent_bss)) != 0 ||
	    uaccess_copy_to_user(
	        &parent, TEST_STACK_ADDR, parent_stack, sizeof(parent_stack)) !=
	        0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	if (paging_walk(parent.pd_phys, TEST_BSS_ADDR, &parent_bss_pte) != 0 ||
	    paging_walk(parent.pd_phys, TEST_STACK_ADDR, &parent_stack_pte) != 0) {
		KTEST_EXPECT_TRUE(tc, 0);
		destroy_test_proc(&parent);
		return;
	}

	parent_bss_phys = *parent_bss_pte & ~0xFFFu;
	parent_stack_phys = *parent_stack_pte & ~0xFFFu;
	free_pages_before = pmm_free_page_count();

	heap_count = exhaust_kernel_heap(heap_ptrs, 128u, 4096u);
	KTEST_EXPECT_TRUE(tc, heap_count > 0);

	k_memset(&child, 0, sizeof(child));
	KTEST_EXPECT_EQ(tc, process_fork(&child, &parent), (uint32_t)-1);
	KTEST_EXPECT_EQ(tc, child.pd_phys, 0u);
	KTEST_EXPECT_EQ(tc, child.kstack_bottom, 0u);
	KTEST_EXPECT_EQ(tc, child.kstack_top, 0u);
	KTEST_EXPECT_EQ(tc, child.arch_state.context, 0u);

	release_kernel_heap_allocs(heap_ptrs, heap_count);

	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), free_pages_before);
	KTEST_EXPECT_EQ(tc, pmm_refcount(parent_bss_phys), 1u);
	KTEST_EXPECT_EQ(tc, pmm_refcount(parent_stack_phys), 1u);
	KTEST_EXPECT_EQ(
	    tc,
	    k_strcmp((char *)mapped_alias(&parent, TEST_BSS_ADDR), parent_bss),
	    0u);
	KTEST_EXPECT_EQ(
	    tc,
	    k_strcmp((char *)mapped_alias(&parent, TEST_STACK_ADDR), parent_stack),
	    0u);

	destroy_test_proc(&child);
	destroy_test_proc(&parent);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_copy_from_user_reads_mapped_bytes),
    KTEST_CASE(test_copy_to_user_spans_pages),
    KTEST_CASE(test_copy_string_from_user_spans_pages),
    KTEST_CASE(test_prepare_rejects_kernel_and_unmapped_ranges),
    KTEST_CASE(test_copy_to_user_breaks_cow_clone),
    KTEST_CASE(test_release_user_space_drops_only_child_cow_reference),
    KTEST_CASE(test_process_fork_stack_pages_break_cow_on_write_fault),
    KTEST_CASE(test_process_fork_child_writes_image_data_before_exec),
    KTEST_CASE(test_process_fork_preserves_user_io_mappings),
    KTEST_CASE(
        test_process_fork_child_survives_parent_exit_and_reuses_last_cow_ref),
    KTEST_CASE(test_process_fork_child_stack_growth_is_private),
    KTEST_CASE(test_process_fork_child_gets_fresh_task_group_slot),
    KTEST_CASE(test_process_fork_then_sched_add_child_clears_exec_guard),
    KTEST_CASE(test_repeated_fork_exec_cleanup_preserves_parent_refs),
    KTEST_CASE(test_process_fork_bumps_pipe_refs_once_with_resource_table),
    KTEST_CASE(test_copy_to_user_handles_three_way_cow_sharing),
    KTEST_CASE(test_process_fork_rolls_back_when_kstack_alloc_fails),
};

static ktest_suite_t suite = KTEST_SUITE("uaccess", cases);

ktest_suite_t *ktest_suite_uaccess(void)
{
	return &suite;
}
