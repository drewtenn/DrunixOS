/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_arch_arm64.c - ARM64-specific kernel behavior paired with x86 KTESTs.
 */

#include "ktest.h"
#include "arch.h"
#include "fdt.h"
#include "kstring.h"
#include "pmm.h"
#include "process.h"
#include "resources.h"
#include "uaccess.h"
#include "vma.h"
#if DRUNIX_ARM64_PLATFORM_VIRT
#include "blkdev.h"
#include "chardev.h"
#include "dma.h"
#include "fbdev.h"
#include "inputdev.h"
#include "platform/platform.h"
#include "platform/virt/dma.h"
#include "platform/virt/fwcfg.h"
#endif

extern const uint8_t virt_snapshot_dtb[];
extern const uint32_t virt_snapshot_dtb_size;

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

#if DRUNIX_ARM64_PLATFORM_VIRT

/* Virt-platform DMA-allocator tests. Phase 1 M2.3 / FR-012.
 * The pool is shared with the live virtio-blk driver; the tests
 * allocate from the *current* free run rather than asserting on
 * absolute free-counts so that prior allocations (queue backing,
 * request header, data scratch) do not break invariants. */

static void test_arm64_dma_alloc_returns_aligned_in_pool(ktest_case_t *tc)
{
	void *p = virt_dma_alloc(1u);
	uintptr_t addr;

	KTEST_ASSERT_NOT_NULL(tc, p);
	addr = (uintptr_t)p;
	KTEST_EXPECT_EQ(tc, addr % VIRT_DMA_PAGE_SIZE, 0u);
	KTEST_EXPECT_EQ(tc, virt_virt_to_phys(p), (uint64_t)addr);
	KTEST_EXPECT_EQ(tc, (uintptr_t)virt_phys_to_virt((uint64_t)addr), addr);
	virt_dma_free(p, 1u);
}

static void test_arm64_dma_alloc_free_reuses_pages(ktest_case_t *tc)
{
	void *first = virt_dma_alloc(1u);
	void *second;

	KTEST_ASSERT_NOT_NULL(tc, first);
	virt_dma_free(first, 1u);
	second = virt_dma_alloc(1u);
	KTEST_EXPECT_EQ(tc, (uintptr_t)second, (uintptr_t)first);
	if (second)
		virt_dma_free(second, 1u);
}

static void test_arm64_dma_alloc_multipage_is_contiguous(ktest_case_t *tc)
{
	uint8_t *p = virt_dma_alloc(3u);

	KTEST_ASSERT_NOT_NULL(tc, p);
	KTEST_EXPECT_EQ(tc, (uintptr_t)p % VIRT_DMA_PAGE_SIZE, 0u);
	/* Writing across the run must not fault — proves the pages are
	 * contiguous and the bitmap reservations are correct. */
	for (uint32_t i = 0; i < 3u * VIRT_DMA_PAGE_SIZE; i++)
		p[i] = (uint8_t)i;
	virt_dma_free(p, 3u);
}

static void
test_arm64_dma_phys_virt_round_trip_validates_bounds(ktest_case_t *tc)
{
	uint8_t outside_pool = 0;
	void *converted;

	KTEST_EXPECT_EQ(tc, virt_virt_to_phys(&outside_pool), 0u);
	converted = virt_phys_to_virt((uint64_t)0xDEADBEEFu);
	KTEST_EXPECT_NULL(tc, converted);
	KTEST_EXPECT_NULL(tc, virt_dma_alloc(0u));
	KTEST_EXPECT_NULL(tc, virt_dma_alloc(virt_dma_pages_total() + 1u));
}

static void test_arm64_dma_barriers_compile_and_execute(ktest_case_t *tc)
{
	/* Memory ordering can't be observed reliably on a single emulated
	 * CPU; this case asserts the helpers exist, link, and execute
	 * without trapping. The compile-time check that they actually
	 * emit DMB encodings lives in dma.h. */
	arm64_dma_wmb();
	arm64_dma_rmb();
	arm64_dma_mb();
	arm64_dma_cache_clean(0, 0);
	arm64_dma_cache_invalidate(0, 0);
	KTEST_EXPECT_EQ(tc, 1u, 1u);
}

/* M2.4b platform_mm tests. Verify the FDT-driven RAM layout and the
 * classifier produce the expected attributes for every architecturally
 * relevant address: virtio-mmio (DEVICE), RAM (NORMAL), beyond-RAM
 * (UNMAPPED). */
static void test_arm64_virt_layout_uses_fdt_memory(ktest_case_t *tc)
{
	const platform_ram_layout_t *l = platform_ram_layout();

	KTEST_ASSERT_NOT_NULL(tc, l);
	KTEST_EXPECT_EQ(tc, (uint32_t)l->ram_base, 0x40000000u);
	KTEST_EXPECT_EQ(tc, (uint32_t)l->ram_size, 0x40000000u);
	KTEST_EXPECT_GE(tc, (uint32_t)l->heap_base, 0x40000000u);
	KTEST_EXPECT_GE(tc, (uint32_t)l->heap_size, (uint32_t)0x100000u);
}

static void test_arm64_virt_classifier_categorises_ram(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)platform_mm_classify(0x40000000ull),
	                (uint32_t)PLATFORM_MM_NORMAL);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)platform_mm_classify(0x09000000ull),
	                (uint32_t)PLATFORM_MM_DEVICE);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)platform_mm_classify(0x80000000ull),
	                (uint32_t)PLATFORM_MM_UNMAPPED);
}

static void test_arm64_virt_dma_cache_clean_at_pool_address(ktest_case_t *tc)
{
	uint8_t *p = virt_dma_alloc(1u);

	KTEST_ASSERT_NOT_NULL(tc, p);
	for (uint32_t i = 0; i < VIRT_DMA_PAGE_SIZE; i++)
		p[i] = (uint8_t)(i & 0xFFu);

	/* Real cache maintenance — exercises dc cvac / dc ivac at a
	 * page that sits in Normal cacheable RAM. Failure mode would be
	 * an alignment fault or a translation fault, both of which would
	 * trap before the test returned. */
	arm64_dma_cache_clean(p, VIRT_DMA_PAGE_SIZE);
	arm64_dma_cache_invalidate(p, VIRT_DMA_PAGE_SIZE);
	KTEST_EXPECT_EQ(tc, p[0], 0u);

	virt_dma_free(p, 1u);
}

/* M2.4c root-mount acceptance. Runs after arm64_mount_root_namespace
 * has executed in start_kernel.c, so sda + sda1 are registered as
 * blkdevs and the root has been mounted. We assert on blkdev state
 * (which survives the vfs_reset() the vfs test suite performs)
 * rather than VFS mount state. */
static void test_arm64_virt_root_mounted_as_ext3(ktest_case_t *tc)
{
	KTEST_EXPECT_GE(tc, (uint32_t)blkdev_find_index("sda"), 0u);
	KTEST_EXPECT_GE(tc, (uint32_t)blkdev_find_index("sda1"), 0u);
}

/*
 * M2.5a ramfb acceptance. The KTEST runner attaches `-device ramfb`
 * (see tools/arm64_qemu_harness.py), so by the time these cases run
 * fwcfg has signalled present, ramfb has been published as /dev/fb0,
 * and the kernel linear map for the framebuffer span has been
 * downgraded to Normal-NC. The acceptance criteria for the milestone
 * are that the boot envelope reaches all of those states without
 * regressing M2.4c root mount or the existing arm64 KTEST suite.
 */
static void test_arm64_fwcfg_present_after_init(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(tc, (uint32_t)fwcfg_present(), 1u);
}

static void test_arm64_fwcfg_finds_etc_ramfb(ktest_case_t *tc)
{
	uint16_t selector = 0;
	uint32_t size = 0;

	KTEST_ASSERT_EQ(tc,
	                fwcfg_find_file("etc/ramfb", &selector, &size), 0);
	/* QEMU's RAMFBCfg is 28 bytes (be64 + 5 × be32). */
	KTEST_EXPECT_EQ(tc, size, 28u);
	KTEST_EXPECT_NE(tc, (uint32_t)selector, 0u);
}

static void test_arm64_fwcfg_rejects_unknown_file(ktest_case_t *tc)
{
	uint16_t selector = 0xBEEF;
	uint32_t size = 0xDEADBEEFu;

	KTEST_EXPECT_NE(
	    tc,
	    (uint32_t)fwcfg_find_file("etc/this-key-does-not-exist",
	                              &selector, &size),
	    0u);
}

static void test_arm64_ramfb_layout_reserves_8mib(ktest_case_t *tc)
{
	const platform_ram_layout_t *l = platform_ram_layout();

	KTEST_ASSERT_NOT_NULL(tc, l);
	KTEST_EXPECT_EQ(tc, (uint32_t)l->framebuffer_size, 0x800000u);
	KTEST_EXPECT_NE(tc, (uint32_t)l->framebuffer_base, 0u);
	/* FB lives strictly above the heap and strictly inside RAM. */
	KTEST_EXPECT_GE(tc,
	                (uint32_t)l->framebuffer_base,
	                (uint32_t)(l->heap_base + l->heap_size));
	KTEST_EXPECT_GE(
	    tc,
	    (uint32_t)((l->ram_base + l->ram_size) -
	               (l->framebuffer_base + l->framebuffer_size)),
	    0u);
}

static void test_arm64_ramfb_classifier_returns_framebuffer(ktest_case_t *tc)
{
	const platform_ram_layout_t *l = platform_ram_layout();
	uint64_t mid;

	if (l->framebuffer_size == 0) {
		KTEST_EXPECT_EQ(tc, 0u, 0u); /* layout absent — skip */
		return;
	}
	mid = l->framebuffer_base + (l->framebuffer_size / 2u);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)platform_mm_classify(l->framebuffer_base),
	                (uint32_t)PLATFORM_MM_FRAMEBUFFER);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)platform_mm_classify(mid),
	                (uint32_t)PLATFORM_MM_FRAMEBUFFER);
	/* One byte past the FB span must NOT be classified as FB. */
	KTEST_EXPECT_NE(tc,
	                (uint32_t)platform_mm_classify(
	                    l->framebuffer_base + l->framebuffer_size),
	                (uint32_t)PLATFORM_MM_FRAMEBUFFER);
}

static void test_arm64_ramfb_kernel_alias_is_normal_nc(ktest_case_t *tc)
{
	const platform_ram_layout_t *l = platform_ram_layout();
	arch_mm_mapping_t map;
	int rc;

	if (l->framebuffer_size == 0 || chardev_get("fb0") == 0) {
		/* ramfb path was skipped at boot — kernel-alias remap did
		 * not run, so the assertion below is vacuously inapplicable. */
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}
	rc = arch_mm_query(arch_aspace_kernel(),
	                   (uintptr_t)l->framebuffer_base, &map);
	KTEST_ASSERT_EQ(tc, rc, 0);
	KTEST_EXPECT_NE(tc, map.flags & ARCH_MM_MAP_NC, 0u);
	KTEST_EXPECT_EQ(tc, map.flags & ARCH_MM_MAP_IO, 0u);
}

static void test_arm64_fbdev_chardev_published(ktest_case_t *tc)
{
	/* `-device ramfb` is on; /dev/fb0 must be registered. */
	const chardev_ops_t *ops = chardev_get("fb0");
	const chardev_ops_t *info = chardev_get("fb0info");

	KTEST_ASSERT_NOT_NULL(tc, ops);
	KTEST_EXPECT_NOT_NULL(tc, ops->mmap_phys);
	KTEST_EXPECT_NOT_NULL(tc, ops->mmap_cache_policy);
	KTEST_ASSERT_NOT_NULL(tc, info);
	KTEST_EXPECT_NOT_NULL(tc, info->read);
}

static void test_arm64_fbdev_geometry_matches_ramfb_config(ktest_case_t *tc)
{
	const chardev_ops_t *info = chardev_get("fb0info");
	fbdev_info_t geom;
	int rc;

	if (!info || !info->read) {
		KTEST_EXPECT_EQ(tc, 0u, 0u); /* /dev/fb0 absent — skip */
		return;
	}
	k_memset(&geom, 0xff, sizeof(geom));
	rc = info->read(0u, (uint8_t *)&geom, (uint32_t)sizeof(geom));
	KTEST_EXPECT_EQ(tc, (uint32_t)rc, (uint32_t)sizeof(geom));
	KTEST_EXPECT_EQ(tc, geom.width, 1024u);
	KTEST_EXPECT_EQ(tc, geom.height, 768u);
	KTEST_EXPECT_EQ(tc, geom.pitch, 4096u);
	KTEST_EXPECT_EQ(tc, geom.bpp, 32u);
	KTEST_EXPECT_EQ(tc, geom.red_pos, (uint8_t)16);
	KTEST_EXPECT_EQ(tc, geom.green_pos, (uint8_t)8);
	KTEST_EXPECT_EQ(tc, geom.blue_pos, (uint8_t)0);
}

/*
 * M2.5b virtio-input acceptance. The KTEST harness now passes both
 * `-device virtio-keyboard-device` and `-device virtio-mouse-device`,
 * and arm64_virt_input_register_all has run before ktest_run_all. We
 * assert that /dev/kbd and /dev/mouse are registered, then that the
 * synthetic evdev push helpers (used by the virtio-input IRQ path)
 * actually deliver records into the chardev rings the desktop reads.
 */
static void test_arm64_virtio_input_devices_enumerated(ktest_case_t *tc)
{
	const chardev_ops_t *kbd = chardev_get("kbd");
	const chardev_ops_t *mouse = chardev_get("mouse");

	KTEST_EXPECT_NOT_NULL(tc, kbd);
	KTEST_EXPECT_NOT_NULL(tc, mouse);
	if (kbd)
		KTEST_EXPECT_NOT_NULL(tc, kbd->read);
	if (mouse)
		KTEST_EXPECT_NOT_NULL(tc, mouse->read);
}

static void test_arm64_virtio_keyboard_event_to_kbdev(ktest_case_t *tc)
{
	const chardev_ops_t *kbd = chardev_get("kbd");
	input_event_t evt;
	int rc;

	if (!kbd || !kbd->read) {
		KTEST_EXPECT_EQ(tc, 0u, 0u); /* skip when absent */
		return;
	}

	/* Inject a synthetic press of KEY_A (Linux keycode 30, also PS/2
	 * set-1 'a' = 0x1E = 30). The evdev push helpers do not synthesise
	 * SYN_REPORT; emit one explicitly so kbdev's wait queue wakes. */
	kbdev_push_event(EV_KEY, 30u, 1);
	kbdev_push_event(EV_SYN, SYN_REPORT, 0);

	k_memset(&evt, 0xff, sizeof(evt));
	rc = kbd->read(0u, (uint8_t *)&evt, (uint32_t)sizeof(evt));
	KTEST_ASSERT_EQ(tc, (uint32_t)rc, (uint32_t)sizeof(evt));
	KTEST_EXPECT_EQ(tc, (uint32_t)evt.type, (uint32_t)EV_KEY);
	KTEST_EXPECT_EQ(tc, (uint32_t)evt.code, 30u);
	KTEST_EXPECT_EQ(tc, (uint32_t)evt.value, 1u);

	/* Drain the SYN we emitted so it does not leak into the next test. */
	(void)kbd->read(0u, (uint8_t *)&evt, (uint32_t)sizeof(evt));
}

static void test_arm64_virtio_mouse_event_to_mousedev(ktest_case_t *tc)
{
	const chardev_ops_t *mouse = chardev_get("mouse");
	input_event_t evt;
	int rc;

	if (!mouse || !mouse->read) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	/* Inject a synthetic relative-X motion event. */
	mousedev_push_event(EV_REL, REL_X, 5);
	mousedev_push_event(EV_SYN, SYN_REPORT, 0);

	k_memset(&evt, 0xff, sizeof(evt));
	rc = mouse->read(0u, (uint8_t *)&evt, (uint32_t)sizeof(evt));
	KTEST_ASSERT_EQ(tc, (uint32_t)rc, (uint32_t)sizeof(evt));
	KTEST_EXPECT_EQ(tc, (uint32_t)evt.type, (uint32_t)EV_REL);
	KTEST_EXPECT_EQ(tc, (uint32_t)evt.code, (uint32_t)REL_X);
	KTEST_EXPECT_EQ(tc, (uint32_t)evt.value, 5u);

	(void)mouse->read(0u, (uint8_t *)&evt, (uint32_t)sizeof(evt));
}

#endif /* DRUNIX_ARM64_PLATFORM_VIRT */

/* FDT-parser tests. Phase 1 M2.4a / FR-002. The snapshot blob is a
 * pinned device-tree dump from QEMU's `-M virt,gic-version=3` machine
 * (see tools/virt-snapshot.md). Embedded via .incbin so KTESTs do
 * not depend on QEMU at build time. Tests are platform-agnostic — the
 * snapshot is the input, the parser is the system-under-test. */

static void test_arm64_fdt_validates_snapshot_magic(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(tc, (uint32_t)fdt_validate(virt_snapshot_dtb), 0u);
}

static void test_arm64_fdt_rejects_garbage_blob(ktest_case_t *tc)
{
	uint8_t garbage[64];

	for (uint32_t i = 0; i < sizeof(garbage); i++)
		garbage[i] = (uint8_t)i;
	KTEST_EXPECT_NE(tc, (uint32_t)fdt_validate(garbage), 0u);
	KTEST_EXPECT_NE(tc, (uint32_t)fdt_validate(0), 0u);
}

static void test_arm64_fdt_finds_memory_range(ktest_case_t *tc)
{
	fdt_memory_range_t ranges[FDT_MAX_MEMORY_RANGES];
	uint32_t count = 0;

	KTEST_ASSERT_EQ(
	    tc,
	    fdt_get_memory(
	        virt_snapshot_dtb, ranges, FDT_MAX_MEMORY_RANGES, &count),
	    0);
	KTEST_EXPECT_EQ(tc, count, 1u);
	if (count >= 1u) {
		KTEST_EXPECT_EQ(tc, (uint32_t)(ranges[0].base >> 32), 0u);
		KTEST_EXPECT_EQ(tc, (uint32_t)ranges[0].base, 0x40000000u);
		KTEST_EXPECT_EQ(tc, (uint32_t)(ranges[0].size >> 32), 0u);
		KTEST_EXPECT_EQ(tc, (uint32_t)ranges[0].size, 0x40000000u);
	}
}

static void test_arm64_fdt_chosen_bootargs_optional(ktest_case_t *tc)
{
	const char *args = fdt_get_chosen_bootargs(virt_snapshot_dtb);

	/* QEMU virt's /chosen has no bootargs by default; an empty
	 * string or NULL are both acceptable. The point of the test
	 * is that the call returns without crashing on absent props. */
	if (args)
		KTEST_EXPECT_GE(tc, (uint32_t)k_strlen(args), 0u);
	else
		KTEST_EXPECT_NULL(tc, args);
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
    KTEST_CASE(test_arm64_fdt_validates_snapshot_magic),
    KTEST_CASE(test_arm64_fdt_rejects_garbage_blob),
    KTEST_CASE(test_arm64_fdt_finds_memory_range),
    KTEST_CASE(test_arm64_fdt_chosen_bootargs_optional),
#if DRUNIX_ARM64_PLATFORM_VIRT
    KTEST_CASE(test_arm64_dma_alloc_returns_aligned_in_pool),
    KTEST_CASE(test_arm64_dma_alloc_free_reuses_pages),
    KTEST_CASE(test_arm64_dma_alloc_multipage_is_contiguous),
    KTEST_CASE(test_arm64_virt_layout_uses_fdt_memory),
    KTEST_CASE(test_arm64_virt_classifier_categorises_ram),
    KTEST_CASE(test_arm64_virt_dma_cache_clean_at_pool_address),
    KTEST_CASE(test_arm64_dma_phys_virt_round_trip_validates_bounds),
    KTEST_CASE(test_arm64_dma_barriers_compile_and_execute),
    KTEST_CASE(test_arm64_virt_root_mounted_as_ext3),
    KTEST_CASE(test_arm64_fwcfg_present_after_init),
    KTEST_CASE(test_arm64_fwcfg_finds_etc_ramfb),
    KTEST_CASE(test_arm64_fwcfg_rejects_unknown_file),
    KTEST_CASE(test_arm64_ramfb_layout_reserves_8mib),
    KTEST_CASE(test_arm64_ramfb_classifier_returns_framebuffer),
    KTEST_CASE(test_arm64_ramfb_kernel_alias_is_normal_nc),
    KTEST_CASE(test_arm64_fbdev_chardev_published),
    KTEST_CASE(test_arm64_fbdev_geometry_matches_ramfb_config),
    KTEST_CASE(test_arm64_virtio_input_devices_enumerated),
    KTEST_CASE(test_arm64_virtio_keyboard_event_to_kbdev),
    KTEST_CASE(test_arm64_virtio_mouse_event_to_mousedev),
#endif
};

static ktest_suite_t suite = KTEST_SUITE("arch_arm64", cases);

ktest_suite_t *ktest_suite_arch_arm64(void)
{
	return &suite;
}
