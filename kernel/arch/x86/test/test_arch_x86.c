/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_arch_x86.c - x86-specific kernel behavior paired with shared KTESTs.
 */

#include "ktest.h"
#include "arch.h"
#include "arch_layout.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"

static void init_dummy_proc(process_t *proc)
{
	k_memset(proc, 0, sizeof(*proc));
	proc->arch_state.context = 1;
}

static void
test_sched_record_user_fault_preserves_full_fault_addr(ktest_case_t *tc)
{
	static process_t proc;
	arch_trap_frame_t frame;
	process_t *running;
	uint64_t fault_addr = 0x1234567887654321ull;

	sched_init();
	init_dummy_proc(&proc);
	KTEST_ASSERT_TRUE(tc, sched_add(&proc) >= 1);

	running = sched_bootstrap();
	KTEST_ASSERT_NOT_NULL(tc, running);

	k_memset(&frame, 0, sizeof(frame));
	frame.eip = 0x00401234u;
	frame.vector = 14u;
	frame.error_code = 0x5u;

	sched_record_user_fault(&frame, fault_addr, SIGSEGV);

	KTEST_EXPECT_EQ(tc, running->crash.valid, 1u);
	KTEST_EXPECT_EQ(tc, running->crash.signum, (uint32_t)SIGSEGV);
	KTEST_EXPECT_TRUE(tc, running->crash.fault_addr == fault_addr);
	KTEST_EXPECT_EQ(tc, running->crash.frame.eip, frame.eip);
	KTEST_EXPECT_EQ(tc, running->crash.frame.vector, frame.vector);
}

static void test_user_mapping_rejects_direct_map_addresses(ktest_case_t *tc)
{
	arch_aspace_t aspace;
	uint32_t phys;

	aspace = arch_aspace_create();
	KTEST_ASSERT_NE(tc, (uint32_t)aspace, 0u);

	phys = pmm_alloc_page();
	KTEST_ASSERT_NE(tc, phys, 0u);
	KTEST_EXPECT_EQ(tc,
	                arch_mm_map(aspace,
	                            0x01000000u,
	                            phys,
	                            ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
	                                ARCH_MM_MAP_WRITE | ARCH_MM_MAP_USER),
	                (uint32_t)-1);
	pmm_free_page(phys);

	phys = pmm_alloc_page();
	KTEST_ASSERT_NE(tc, phys, 0u);
	KTEST_EXPECT_EQ(tc,
	                arch_mm_map(aspace,
	                            (uint32_t)ARCH_USER_IMAGE_BASE,
	                            phys,
	                            ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
	                                ARCH_MM_MAP_WRITE | ARCH_MM_MAP_USER),
	                0u);

	arch_aspace_destroy(aspace);
}

static void test_paging_rejects_direct_user_page_mapping(ktest_case_t *tc)
{
	uint32_t pd_phys;
	uint32_t phys;

	pd_phys = paging_create_user_space();
	KTEST_ASSERT_NE(tc, pd_phys, 0u);

	phys = pmm_alloc_page();
	KTEST_ASSERT_NE(tc, phys, 0u);
	KTEST_EXPECT_EQ(
	    tc,
	    paging_map_page(
	        pd_phys, 0x01000000u, phys, PG_PRESENT | PG_WRITABLE | PG_USER),
	    (uint32_t)-1);
	pmm_free_page(phys);

	paging_destroy_user_space(pd_phys);
}

static void test_user_space_inherits_higher_half_kernel_pdes(ktest_case_t *tc)
{
	uint32_t pd_phys;
	uint32_t *pd;
	uint32_t first_kernel_pde = (uint32_t)ARCH_KERNEL_VIRT_BASE >> 22;
	uint32_t kernel_pde_count =
	    (uint32_t)ARCH_KERNEL_DIRECT_PHYS_MAX / 0x400000u;
	uint32_t phys;
	int map_rc;

	pd_phys = paging_create_user_space();
	KTEST_ASSERT_NE(tc, pd_phys, 0u);

	pd = (uint32_t *)paging_temp_map(pd_phys);
	KTEST_ASSERT_NOT_NULL(tc, pd);

	for (uint32_t i = 0; i < kernel_pde_count; i++) {
		uint32_t pde = pd[first_kernel_pde + i];

		KTEST_EXPECT_TRUE(tc, (pde & PG_PRESENT) != 0);
		KTEST_EXPECT_TRUE(tc, (pde & PG_USER) == 0);
	}

	phys = pmm_alloc_page();
	KTEST_ASSERT_NE(tc, phys, 0u);
	map_rc = paging_map_page(pd_phys,
	                         (uint32_t)ARCH_USER_IMAGE_BASE,
	                         phys,
	                         PG_PRESENT | PG_WRITABLE | PG_USER);
	KTEST_EXPECT_EQ(tc, map_rc, 0u);
	if (map_rc == 0) {
		KTEST_EXPECT_TRUE(
		    tc, (pd[(uint32_t)ARCH_USER_IMAGE_BASE >> 22] & PG_USER) != 0);
	} else {
		pmm_free_page(phys);
	}

	paging_temp_unmap(pd);
	paging_destroy_user_space(pd_phys);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_sched_record_user_fault_preserves_full_fault_addr),
    KTEST_CASE(test_user_mapping_rejects_direct_map_addresses),
    KTEST_CASE(test_paging_rejects_direct_user_page_mapping),
    KTEST_CASE(test_user_space_inherits_higher_half_kernel_pdes),
};

static ktest_suite_t suite = KTEST_SUITE("arch-x86", cases);

ktest_suite_t *ktest_suite_arch_x86(void)
{
	return &suite;
}
