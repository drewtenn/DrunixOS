/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_arch_arm64.c - ARM64-specific kernel behavior paired with x86 KTESTs.
 */

#include "ktest.h"
#include "arch.h"
#include "kstring.h"
#include "pmm.h"
#include "process.h"
#include "resources.h"
#include "uaccess.h"
#include "vma.h"

static void test_arm64_pmm_alloc_free_reuses_pages(ktest_case_t *tc)
{
	uint32_t before = pmm_free_page_count();
	uint32_t page = pmm_alloc_page();
	uint32_t reused;

	KTEST_ASSERT_NOT_NULL(tc, page);
	KTEST_EXPECT_EQ(tc, page % PAGE_SIZE, 0u);
	KTEST_EXPECT_GE(tc, page, 0x00080000u);
	KTEST_EXPECT_EQ(tc, pmm_refcount(page), 1u);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before - 1u);

	pmm_free_page(page);
	KTEST_EXPECT_EQ(tc, pmm_refcount(page), 0u);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before);

	reused = pmm_alloc_page();
	KTEST_EXPECT_EQ(tc, reused, page);
	if (reused)
		pmm_free_page(reused);
}

static void test_arm64_pmm_multiple_allocations_are_distinct(ktest_case_t *tc)
{
	uint32_t pages[4] = {0, 0, 0, 0};
	uint32_t before = pmm_free_page_count();

	for (int i = 0; i < 4; i++) {
		pages[i] = pmm_alloc_page();
		KTEST_EXPECT_NOT_NULL(tc, pages[i]);
		if (!pages[i])
			goto out;
		KTEST_EXPECT_EQ(tc, pages[i] % PAGE_SIZE, 0u);
		for (int j = 0; j < i; j++)
			KTEST_EXPECT_NE(tc, pages[i], pages[j]);
	}
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before - 4u);

out:
	for (int i = 0; i < 4; i++)
		if (pages[i])
			pmm_free_page(pages[i]);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before);
}

static void test_arm64_pmm_refcount_tracks_shared_page(ktest_case_t *tc)
{
	uint32_t before = pmm_free_page_count();
	uint32_t page = pmm_alloc_page();

	KTEST_ASSERT_NOT_NULL(tc, page);
	KTEST_EXPECT_EQ(tc, pmm_refcount(page), 1u);

	pmm_incref(page);
	pmm_incref(page);
	KTEST_EXPECT_EQ(tc, pmm_refcount(page), 3u);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before - 1u);

	pmm_decref(page);
	KTEST_EXPECT_EQ(tc, pmm_refcount(page), 2u);
	pmm_decref(page);
	pmm_decref(page);
	KTEST_EXPECT_EQ(tc, pmm_refcount(page), 0u);
	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), before);
}

static void test_arm64_vma_add_sorts_and_finds_regions(ktest_case_t *tc)
{
	static process_t proc;
	uint32_t flags =
	    VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON | VMA_FLAG_PRIVATE;

	k_memset(&proc, 0, sizeof(proc));
	vma_init(&proc);

	KTEST_ASSERT_EQ(
	    tc,
	    vma_add(&proc, 0x00408000u, 0x00409000u, flags, VMA_KIND_GENERIC),
	    0);
	KTEST_ASSERT_EQ(
	    tc,
	    vma_add(&proc, 0x00402000u, 0x00403000u, flags, VMA_KIND_GENERIC),
	    0);
	KTEST_ASSERT_EQ(
	    tc,
	    vma_add(&proc, 0x00405000u, 0x00406000u, flags, VMA_KIND_GENERIC),
	    0);

	KTEST_EXPECT_EQ(tc, proc.vma_count, 3u);
	KTEST_EXPECT_EQ(tc, proc.vmas[0].start, 0x00402000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[1].start, 0x00405000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[2].start, 0x00408000u);
	KTEST_ASSERT_NOT_NULL(tc, vma_find(&proc, 0x00402000u));
	KTEST_ASSERT_NOT_NULL(tc, vma_find(&proc, 0x00405000u));
	KTEST_ASSERT_NOT_NULL(tc, vma_find(&proc, 0x00408000u));
	KTEST_EXPECT_NULL(tc, vma_find(&proc, 0x00403000u));
}

static void test_arm64_vma_add_rejects_overlapping_regions(ktest_case_t *tc)
{
	static process_t proc;
	uint32_t flags =
	    VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON | VMA_FLAG_PRIVATE;

	k_memset(&proc, 0, sizeof(proc));
	vma_init(&proc);

	KTEST_EXPECT_EQ(
	    tc,
	    vma_add(&proc, 0x00403000u, 0x00404000u, flags, VMA_KIND_GENERIC),
	    0);
	KTEST_EXPECT_EQ(
	    tc,
	    vma_add(&proc, 0x00401000u, 0x00402000u, flags, VMA_KIND_GENERIC),
	    0);
	KTEST_EXPECT_EQ(
	    tc,
	    vma_add(&proc, 0x00401800u, 0x00402800u, flags, VMA_KIND_GENERIC),
	    -1);

	KTEST_EXPECT_EQ(tc, proc.vma_count, 2u);
	KTEST_ASSERT_NOT_NULL(tc, vma_find(&proc, 0x00401000u));
	KTEST_ASSERT_NOT_NULL(tc, vma_find(&proc, 0x00403000u));
	KTEST_EXPECT_EQ(tc, proc.vmas[0].start, 0x00401000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[1].start, 0x00403000u);
}

static void
test_arm64_vma_unmap_and_protect_split_generic_mapping(ktest_case_t *tc)
{
	static process_t proc;
	uint32_t flags =
	    VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON | VMA_FLAG_PRIVATE;

	k_memset(&proc, 0, sizeof(proc));
	vma_init(&proc);

	KTEST_ASSERT_EQ(
	    tc,
	    vma_add(&proc, 0x00800000u, 0x00803000u, flags, VMA_KIND_GENERIC),
	    0);
	KTEST_ASSERT_EQ(tc, vma_unmap_range(&proc, 0x00801000u, 0x00802000u), 0);
	KTEST_EXPECT_EQ(tc, proc.vma_count, 2u);
	KTEST_EXPECT_EQ(tc, proc.vmas[0].start, 0x00800000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[0].end, 0x00801000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[1].start, 0x00802000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[1].end, 0x00803000u);

	KTEST_ASSERT_EQ(
	    tc,
	    vma_add(&proc, 0x00900000u, 0x00903000u, flags, VMA_KIND_GENERIC),
	    0);
	KTEST_ASSERT_EQ(
	    tc,
	    vma_protect_range(&proc,
	                      0x00901000u,
	                      0x00902000u,
	                      VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE),
	    0);
	KTEST_EXPECT_EQ(tc, proc.vma_count, 5u);
	KTEST_EXPECT_EQ(tc, proc.vmas[3].start, 0x00901000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[3].end, 0x00902000u);
	KTEST_EXPECT_EQ(tc,
	                proc.vmas[3].flags,
	                VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE);
}

static void test_arm64_uaccess_copies_mapped_user_bytes(ktest_case_t *tc)
{
	static process_t proc;
	const char *message = "arm64-user";
	const char *replacement = "copy-ok";
	char dst[16];
	uint32_t phys = 0;
	void *page = 0;
	int mapped = 0;

	k_memset(&proc, 0, sizeof(proc));
	proc.pd_phys = (uint32_t)arch_aspace_create();
	KTEST_EXPECT_NOT_NULL(tc, proc.pd_phys);
	if (!proc.pd_phys)
		return;
	vma_init(&proc);
	KTEST_EXPECT_EQ(tc,
	                vma_add(&proc,
	                        0x00400000u,
	                        0x00401000u,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE,
	                        VMA_KIND_GENERIC),
	                0);
	if (tc->failed)
		goto out;

	phys = pmm_alloc_page();
	KTEST_EXPECT_NOT_NULL(tc, phys);
	if (!phys)
		goto out;
	page = arch_page_temp_map(phys);
	KTEST_EXPECT_NOT_NULL(tc, page);
	if (!page)
		goto out;
	k_memset(page, 0, PAGE_SIZE);
	k_memcpy(page, message, 11u);
	arch_page_temp_unmap(page);
	page = 0;

	KTEST_EXPECT_EQ(tc,
	                arch_mm_map((arch_aspace_t)proc.pd_phys,
	                            0x00400000u,
	                            phys,
	                            ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
	                                ARCH_MM_MAP_WRITE | ARCH_MM_MAP_USER),
	                0);
	if (tc->failed)
		goto out;
	mapped = 1;

	k_memset(dst, 0, sizeof(dst));
	KTEST_EXPECT_EQ(
	    tc, uaccess_copy_from_user(&proc, dst, 0x00400000u, 11u), 0);
	KTEST_EXPECT_EQ(tc, (uint32_t)k_strcmp(dst, message), 0u);

	k_memset(dst, 0, sizeof(dst));
	KTEST_EXPECT_EQ(
	    tc,
	    uaccess_copy_string_from_user(&proc, dst, sizeof(dst), 0x00400000u),
	    0);
	KTEST_EXPECT_EQ(tc, (uint32_t)k_strcmp(dst, message), 0u);

	KTEST_EXPECT_EQ(
	    tc, uaccess_copy_to_user(&proc, 0x00400000u, replacement, 8u), 0);
	page = arch_page_temp_map(phys);
	KTEST_EXPECT_NOT_NULL(tc, page);
	if (page)
		KTEST_EXPECT_EQ(
		    tc, (uint32_t)k_strcmp((const char *)page, replacement), 0u);

out:
	if (page)
		arch_page_temp_unmap(page);
	if (mapped)
		(void)arch_mm_unmap((arch_aspace_t)proc.pd_phys, 0x00400000u);
	else if (phys)
		pmm_free_page(phys);
	if (proc.pd_phys)
		arch_aspace_destroy((arch_aspace_t)proc.pd_phys);
}

static int arm64_test_resource_init(ktest_case_t *tc, process_t *proc)
{
	k_memset(proc, 0, sizeof(*proc));
	proc->pd_phys = 1u;

	KTEST_EXPECT_EQ(tc, proc_resource_init_fresh(proc), 0);
	if (tc->failed)
		return -1;
	KTEST_EXPECT_NOT_NULL(tc, proc->as);
	KTEST_EXPECT_NOT_NULL(tc, proc->files);
	KTEST_EXPECT_NOT_NULL(tc, proc->fs_state);
	KTEST_EXPECT_NOT_NULL(tc, proc->sig_actions);
	return tc->failed ? -1 : 0;
}

static void
test_arm64_process_resources_start_with_single_refs(ktest_case_t *tc)
{
	static process_t proc;

	if (arm64_test_resource_init(tc, &proc) != 0)
		goto out;

	KTEST_EXPECT_EQ(tc, proc.as->refs, 1u);
	KTEST_EXPECT_EQ(tc, proc.files->refs, 1u);
	KTEST_EXPECT_EQ(tc, proc.fs_state->refs, 1u);
	KTEST_EXPECT_EQ(tc, proc.sig_actions->refs, 1u);

out:
	proc_resource_put_all(&proc);
	KTEST_EXPECT_NULL(tc, proc.as);
	KTEST_EXPECT_NULL(tc, proc.files);
	KTEST_EXPECT_NULL(tc, proc.fs_state);
	KTEST_EXPECT_NULL(tc, proc.sig_actions);
}

static void test_arm64_process_resource_get_put_tracks_refs(ktest_case_t *tc)
{
	static process_t proc;

	if (arm64_test_resource_init(tc, &proc) != 0)
		goto out;
	proc_resource_get_all(&proc);
	KTEST_EXPECT_EQ(tc, proc.as->refs, 2u);
	KTEST_EXPECT_EQ(tc, proc.files->refs, 2u);
	KTEST_EXPECT_EQ(tc, proc.fs_state->refs, 2u);
	KTEST_EXPECT_EQ(tc, proc.sig_actions->refs, 2u);

	proc_resource_put_all(&proc);
	KTEST_EXPECT_EQ(tc, proc.as->refs, 1u);
	KTEST_EXPECT_EQ(tc, proc.files->refs, 1u);
	KTEST_EXPECT_EQ(tc, proc.fs_state->refs, 1u);
	KTEST_EXPECT_EQ(tc, proc.sig_actions->refs, 1u);

out:
	proc_resource_put_all(&proc);
	KTEST_EXPECT_NULL(tc, proc.as);
	KTEST_EXPECT_NULL(tc, proc.files);
	KTEST_EXPECT_NULL(tc, proc.fs_state);
	KTEST_EXPECT_NULL(tc, proc.sig_actions);
}

static void test_arm64_pmm_refcount_saturates_at_255(ktest_case_t *tc)
{
	uint32_t page = 0x00000000u;

	KTEST_EXPECT_EQ(tc, pmm_refcount(page), 255u);
	pmm_incref(page);
	pmm_incref(page);
	KTEST_EXPECT_EQ(tc, pmm_refcount(page), 255u);
	pmm_decref(page);
	KTEST_EXPECT_EQ(tc, pmm_refcount(page), 255u);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_arm64_pmm_alloc_free_reuses_pages),
    KTEST_CASE(test_arm64_pmm_multiple_allocations_are_distinct),
    KTEST_CASE(test_arm64_pmm_refcount_tracks_shared_page),
    KTEST_CASE(test_arm64_pmm_refcount_saturates_at_255),
    KTEST_CASE(test_arm64_vma_add_sorts_and_finds_regions),
    KTEST_CASE(test_arm64_vma_add_rejects_overlapping_regions),
    KTEST_CASE(test_arm64_vma_unmap_and_protect_split_generic_mapping),
    KTEST_CASE(test_arm64_uaccess_copies_mapped_user_bytes),
    KTEST_CASE(test_arm64_process_resources_start_with_single_refs),
    KTEST_CASE(test_arm64_process_resource_get_put_tracks_refs),
};

static ktest_suite_t suite = KTEST_SUITE("arch_arm64", cases);

ktest_suite_t *ktest_suite_arch_arm64(void)
{
	return &suite;
}
