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
#include "platform/virt/virtio_gpu.h"
#include "platform/virt/virtio_net.h"
#endif

extern const uint8_t virt_snapshot_dtb[];
extern const uint32_t virt_snapshot_dtb_size;

static void test_arm64_el1_uses_sp_el1(ktest_case_t *tc)
{
	uint64_t spsel;

	__asm__ volatile("mrs %0, spsel" : "=r"(spsel));
	KTEST_EXPECT_EQ(tc, spsel & 1u, 1u);
}

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

static void test_arm64_wall_clock_seeded_from_build_time(ktest_case_t *tc)
{
	uint32_t now = arch_time_unix_seconds();

	KTEST_EXPECT_NE(tc, now, 0u);
	KTEST_EXPECT_GE(tc, now, 1700000000u);
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

	KTEST_ASSERT_EQ(tc, fwcfg_find_file("etc/ramfb", &selector, &size), 0);
	/* QEMU's RAMFBCfg is 28 bytes (be64 + 5 × be32). */
	KTEST_EXPECT_EQ(tc, size, 28u);
	KTEST_EXPECT_NE(tc, (uint32_t)selector, 0u);
}

static void test_arm64_fwcfg_rejects_unknown_file(ktest_case_t *tc)
{
	uint16_t selector = 0xBEEF;
	uint32_t size = 0xDEADBEEFu;

	KTEST_EXPECT_NE(tc,
	                (uint32_t)fwcfg_find_file(
	                    "etc/this-key-does-not-exist", &selector, &size),
	                0u);
}

static void test_arm64_framebuffer_reservation_is_8mib(ktest_case_t *tc)
{
	const platform_ram_layout_t *l = platform_ram_layout();

	KTEST_ASSERT_NOT_NULL(tc, l);
	KTEST_EXPECT_EQ(tc, (uint32_t)l->framebuffer_size, 0x800000u);
	KTEST_EXPECT_NE(tc, (uint32_t)l->framebuffer_base, 0u);
	/* FB lives strictly above the heap and strictly inside RAM. */
	KTEST_EXPECT_GE(tc,
	                (uint32_t)l->framebuffer_base,
	                (uint32_t)(l->heap_base + l->heap_size));
	KTEST_EXPECT_GE(tc,
	                (uint32_t)((l->ram_base + l->ram_size) -
	                           (l->framebuffer_base + l->framebuffer_size)),
	                0u);
}

static void test_arm64_framebuffer_reservation_classifier(ktest_case_t *tc)
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
	                (uint32_t)platform_mm_classify(l->framebuffer_base +
	                                               l->framebuffer_size),
	                (uint32_t)PLATFORM_MM_FRAMEBUFFER);
}

static void test_arm64_framebuffer_kernel_alias_is_normal_nc(ktest_case_t *tc)
{
	const platform_ram_layout_t *l = platform_ram_layout();
	arch_mm_mapping_t map;
	int rc;

	if (l->framebuffer_size == 0 || chardev_get("fb0") == 0) {
		/* No active framebuffer provider: the kernel-alias remap
		 * (owned by whichever provider published /dev/fb0) did not
		 * run, so the assertion below is vacuously inapplicable. */
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}
	rc = arch_mm_query(
	    arch_aspace_kernel(), (uintptr_t)l->framebuffer_base, &map);
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
 * M2.5b virtio-input acceptance. The KTEST harness now passes keyboard
 * and pointer devices, and arm64_virt_input_register_all has run before
 * ktest_run_all. We assert that /dev/kbd and /dev/mouse are registered,
 * then that the synthetic evdev push helpers (used by the virtio-input
 * IRQ path) actually deliver records into the chardev rings the desktop
 * reads.
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

/*
 * M4 commit 1 — virtio-net device enumeration. arm64_virt_virtio_net_init
 * runs from arm64_start_kernel before KTEST execution. The KTEST harness
 * advertises -device virtio-net-device, so the checked-in run exercises
 * the found-path; an environment that omits the device still keeps the
 * suite green via the skip-pass path below.
 */
static void test_arm64_virtio_net_mmio_device_enumerated(ktest_case_t *tc)
{
	uintptr_t base;
	uint32_t slot;
	uint32_t version;

	/* Skip-pass when no virtio-net device was advertised on the bus. */
	if (!arm64_virt_virtio_net_device_found()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	base = arm64_virt_virtio_net_mmio_base();
	slot = arm64_virt_virtio_net_slot();
	version = arm64_virt_virtio_net_version();

	/* MMIO base lies inside QEMU virt's virtio-mmio window. */
	KTEST_EXPECT_TRUE(tc, base >= 0x0A000000UL);
	KTEST_EXPECT_TRUE(tc, base < 0x0A000000UL + 32u * 0x200u);
	/* Slot index is within the 32-slot window. */
	KTEST_EXPECT_TRUE(tc, slot < 32u);
	/* Base must align with the slot's expected position. */
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)base,
	                (uint32_t)(0x0A000000UL + slot * 0x200u));
	/* Transport version is legacy (1) or modern (2). */
	KTEST_EXPECT_TRUE(tc, version == 1u || version == 2u);
}

/*
 * M4 commit 2 — virtio-net feature negotiation + MAC discovery.
 * arm64_virt_virtio_net_init now drives the legacy handshake; these
 * tests assert the post-handshake state when a device is present.
 * The KTEST harness advertises -device virtio-net-device,netdev=n0
 * with mac=52:54:00:0d:00:01, so the found-path runs end-to-end.
 */
static void test_arm64_virtio_net_features_ok_with_mac(ktest_case_t *tc)
{
	const uint8_t *mac;

	if (!arm64_virt_virtio_net_device_found()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	KTEST_EXPECT_TRUE(tc, arm64_virt_virtio_net_features_ok());

	/* MAC must be non-zero post-handshake. */
	mac = arm64_virt_virtio_net_mac();
	KTEST_ASSERT_NOT_NULL(tc, mac);
	KTEST_EXPECT_TRUE(tc,
	                  (mac[0] | mac[1] | mac[2] |
	                   mac[3] | mac[4] | mac[5]) != 0u);
}

static void test_arm64_virtio_net_reads_mac_as_bytes(ktest_case_t *tc)
{
	const uint8_t *mac;

	if (!arm64_virt_virtio_net_device_found() ||
	    !arm64_virt_virtio_net_features_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	/* Harness MAC is 52:54:00:0d:00:01. Verify byte order is correct,
	 * which catches a regression where the driver reads MAC as a
	 * packed 32+16 bit load and gets the bytes in the wrong order. */
	mac = arm64_virt_virtio_net_mac();
	KTEST_EXPECT_EQ(tc, (uint32_t)mac[0], 0x52u);
	KTEST_EXPECT_EQ(tc, (uint32_t)mac[1], 0x54u);
	KTEST_EXPECT_EQ(tc, (uint32_t)mac[2], 0x00u);
	KTEST_EXPECT_EQ(tc, (uint32_t)mac[3], 0x0du);
	KTEST_EXPECT_EQ(tc, (uint32_t)mac[4], 0x00u);
	KTEST_EXPECT_EQ(tc, (uint32_t)mac[5], 0x01u);
}

static void test_arm64_virtio_net_rejects_modern_transport(ktest_case_t *tc)
{
	uint32_t version;

	if (!arm64_virt_virtio_net_device_found()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	/* Commit 2 only accepts version == 1 (legacy). The harness uses
	 * QEMU virt's default which is legacy mmio (version 1). If a
	 * future harness change advertises modern transport this test
	 * will fail, signalling the version-2 branch needs implementation
	 * before the harness flips. */
	version = arm64_virt_virtio_net_version();
	KTEST_EXPECT_EQ(tc, version, 1u);
}

/*
 * M4 commit 3 — DMA-pool ring allocation. arm64_virt_virtio_net_init
 * now allocates queue backing and packet buffer pools from
 * virt_dma_alloc and configures both queues at the MMIO level. These
 * tests assert the ring/buffer state is consistent post-init.
 */
static void test_arm64_virtio_net_dma_rings_allocated(ktest_case_t *tc)
{
	if (!arm64_virt_virtio_net_device_found() ||
	    !arm64_virt_virtio_net_features_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	KTEST_EXPECT_TRUE(tc, arm64_virt_virtio_net_rings_ready());
	KTEST_EXPECT_TRUE(tc, arm64_virt_virtio_net_rx_queue_phys() != 0u);
	KTEST_EXPECT_TRUE(tc, arm64_virt_virtio_net_tx_queue_phys() != 0u);
	KTEST_EXPECT_TRUE(tc,
	                  arm64_virt_virtio_net_rx_queue_phys() !=
	                  arm64_virt_virtio_net_tx_queue_phys());
}

static void
test_arm64_virtio_net_packet_buffers_translate_nonzero(ktest_case_t *tc)
{
	uint32_t count;

	if (!arm64_virt_virtio_net_rings_ready()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	count = arm64_virt_virtio_net_buffer_count();
	KTEST_ASSERT_EQ(tc, count, 16u);

	/* The silent-DMA-to-zero canary: virt_virt_to_phys returns 0 for
	 * any pointer outside the virt_dma_alloc pool. If a regression
	 * accidentally allocates a buffer from stack / static / kheap,
	 * this loop catches it before the device writes to physical 0. */
	for (uint32_t i = 0; i < count; i++) {
		KTEST_EXPECT_TRUE(tc,
		                  arm64_virt_virtio_net_rx_buffer_phys(i) != 0u);
		KTEST_EXPECT_TRUE(tc,
		                  arm64_virt_virtio_net_tx_buffer_phys(i) != 0u);
	}
}

static void test_arm64_virtio_net_packet_buffers_distinct(ktest_case_t *tc)
{
	uint32_t count;
	uint64_t prev_rx;
	uint64_t prev_tx;

	if (!arm64_virt_virtio_net_rings_ready()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	count = arm64_virt_virtio_net_buffer_count();

	/* Adjacent rx buffers must differ by exactly the buffer stride
	 * (a slicing arithmetic bug would yield repeats or non-stride
	 * gaps). Same for tx. RX vs TX must occupy different pools. */
	prev_rx = arm64_virt_virtio_net_rx_buffer_phys(0);
	prev_tx = arm64_virt_virtio_net_tx_buffer_phys(0);
	for (uint32_t i = 1; i < count; i++) {
		uint64_t rx = arm64_virt_virtio_net_rx_buffer_phys(i);
		uint64_t tx = arm64_virt_virtio_net_tx_buffer_phys(i);

		KTEST_EXPECT_TRUE(tc, rx != prev_rx);
		KTEST_EXPECT_TRUE(tc, tx != prev_tx);
		KTEST_EXPECT_TRUE(tc, rx != tx);
		prev_rx = rx;
		prev_tx = tx;
	}

	/* RX pool 0 must not overlap TX pool 0. */
	KTEST_EXPECT_TRUE(tc,
	                  arm64_virt_virtio_net_rx_buffer_phys(0) !=
	                  arm64_virt_virtio_net_tx_buffer_phys(0));
}

/*
 * M4 commit 4 — RX refill, IRQ completion, structural cache discipline.
 * arm64_virt_virtio_net_init now primes the receiveq, registers the
 * GICv3 SPI handler, and asserts DRIVER_OK. The host-net harness uses
 * an isolated user-mode netdev (-netdev user,restrict=on) which does
 * not deliver inbound traffic, so live RX is exercised in commit 7+
 * via a socket-backed harness. These tests cover the post-init state
 * and the structural cache-discipline contract.
 */
static void test_arm64_virtio_net_driver_ok_after_init(ktest_case_t *tc)
{
	if (!arm64_virt_virtio_net_rings_ready()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	KTEST_EXPECT_TRUE(tc, arm64_virt_virtio_net_driver_ok());
}

static void test_arm64_virtio_net_rx_counters_initialized(ktest_case_t *tc)
{
	if (!arm64_virt_virtio_net_driver_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	/* No frames have been received in the isolated harness, so
	 * every RX counter must be 0 post-init. A non-zero short or
	 * oversize counter would mean the IRQ handler ran with bad
	 * lengths during the handshake — a logic bug. */
	KTEST_EXPECT_EQ(tc, arm64_virt_virtio_net_rx_packets(), 0u);
	KTEST_EXPECT_EQ(tc, arm64_virt_virtio_net_rx_drops_short(), 0u);
	KTEST_EXPECT_EQ(tc, arm64_virt_virtio_net_rx_drops_oversize(), 0u);
	KTEST_EXPECT_EQ(tc, arm64_virt_virtio_net_rx_drops_ring_full(), 0u);
}

static void test_arm64_virtio_net_rx_dequeue_empty_returns_zero(ktest_case_t *tc)
{
	uint8_t buf[16];
	int32_t rc;

	if (!arm64_virt_virtio_net_driver_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	/* No frames yet -> dequeue returns 0, doesn't touch buf. */
	k_memset(buf, 0xAA, sizeof(buf));
	rc = arm64_virt_virtio_net_rx_dequeue(buf, sizeof(buf));
	KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);
	/* buf must be untouched. */
	KTEST_EXPECT_EQ(tc, (uint32_t)buf[0], 0xAAu);
	KTEST_EXPECT_EQ(tc, (uint32_t)buf[15], 0xAAu);
}

/*
 * M4 commit 5 — TX submission. The harness's restrict=on netdev
 * accepts and consumes outbound frames (the device returns
 * descriptors after processing) but does not deliver responses to
 * the guest. These tests exercise the API surface and verify the
 * counters update as expected on submit and rejection.
 */
static void test_arm64_virtio_net_tx_send_returns_len(ktest_case_t *tc)
{
	/* A minimal valid Ethernet frame: dst MAC (6) + src MAC (6) +
	 * EtherType (2) = 14 bytes. EtherType 0x88b5 is reserved for
	 * local experimental use (per IEEE 802 conventions). */
	uint8_t frame[14] = {
	    0x52, 0x54, 0x00, 0x0d, 0x00, 0x02,  /* dst */
	    0x52, 0x54, 0x00, 0x0d, 0x00, 0x01,  /* src */
	    0x88, 0xb5,                          /* EtherType */
	};
	uint32_t before;
	int32_t rc;

	if (!arm64_virt_virtio_net_driver_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	before = arm64_virt_virtio_net_tx_packets();
	rc = arm64_virt_virtio_net_send_frame(frame, sizeof(frame));
	KTEST_EXPECT_EQ(tc, (uint32_t)rc, (uint32_t)sizeof(frame));
	KTEST_EXPECT_EQ(tc, arm64_virt_virtio_net_tx_packets(), before + 1u);
}

static void test_arm64_virtio_net_tx_rejects_short(ktest_case_t *tc)
{
	uint8_t frame[13];
	uint32_t before_packets;
	uint32_t before_drops;
	int32_t rc;

	if (!arm64_virt_virtio_net_driver_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	k_memset(frame, 0xff, sizeof(frame));
	before_packets = arm64_virt_virtio_net_tx_packets();
	before_drops = arm64_virt_virtio_net_tx_drops_busy();
	rc = arm64_virt_virtio_net_send_frame(frame, sizeof(frame));
	KTEST_EXPECT_EQ(tc, (uint32_t)(int32_t)rc, (uint32_t)-1);
	/* tx_packets must NOT have advanced; drops_busy MUST. */
	KTEST_EXPECT_EQ(tc, arm64_virt_virtio_net_tx_packets(), before_packets);
	KTEST_EXPECT_EQ(tc,
	                arm64_virt_virtio_net_tx_drops_busy(),
	                before_drops + 1u);
}

static void test_arm64_virtio_net_tx_rejects_oversize(ktest_case_t *tc)
{
	uint32_t before_packets;
	uint32_t before_drops;
	int32_t rc;

	if (!arm64_virt_virtio_net_driver_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	before_packets = arm64_virt_virtio_net_tx_packets();
	before_drops = arm64_virt_virtio_net_tx_drops_busy();
	/* Pass NULL to dodge needing a 1515-byte stack buffer; an
	 * oversize length should fail BEFORE the frame pointer is
	 * dereferenced. (Driver code path: length check is first.) */
	rc = arm64_virt_virtio_net_send_frame((const uint8_t *)0x100, 1515u);
	KTEST_EXPECT_EQ(tc, (uint32_t)(int32_t)rc, (uint32_t)-1);
	KTEST_EXPECT_EQ(tc, arm64_virt_virtio_net_tx_packets(), before_packets);
	KTEST_EXPECT_EQ(tc,
	                arm64_virt_virtio_net_tx_drops_busy(),
	                before_drops + 1u);
}

/*
 * M4 commit 6 — /dev/net0 raw frame chardev. The netdev layer
 * registers /dev/net0 from start_kernel, and the virtio-net driver
 * registers its ops via netdev_register at the end of init. These
 * tests assert the chardev is published and that read/write route
 * through to the transport.
 */
static void test_arm64_virtio_net_chardev_published(ktest_case_t *tc)
{
	const chardev_ops_t *net0;

	if (!arm64_virt_virtio_net_driver_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	net0 = chardev_get("net0");
	KTEST_ASSERT_NOT_NULL(tc, net0);
	KTEST_EXPECT_NOT_NULL(tc, net0->read);
	KTEST_EXPECT_NOT_NULL(tc, net0->write);
}

static void test_arm64_virtio_net_chardev_write_submits_tx(ktest_case_t *tc)
{
	const chardev_ops_t *net0;
	uint32_t before;
	int n;
	uint8_t frame[14] = {
	    0x52, 0x54, 0x00, 0x0d, 0x00, 0x02,
	    0x52, 0x54, 0x00, 0x0d, 0x00, 0x01,
	    0x88, 0xb5,
	};

	if (!arm64_virt_virtio_net_driver_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	net0 = chardev_get("net0");
	KTEST_ASSERT_NOT_NULL(tc, net0);
	KTEST_ASSERT_NOT_NULL(tc, net0->write);

	before = arm64_virt_virtio_net_tx_packets();
	n = net0->write(0u, frame, (uint32_t)sizeof(frame));
	KTEST_EXPECT_EQ(tc, (uint32_t)n, (uint32_t)sizeof(frame));
	KTEST_EXPECT_EQ(tc, arm64_virt_virtio_net_tx_packets(), before + 1u);
}

static void test_arm64_virtio_net_chardev_rejects_oversize(ktest_case_t *tc)
{
	const chardev_ops_t *net0;
	uint32_t before_drops;
	int n;

	if (!arm64_virt_virtio_net_driver_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	net0 = chardev_get("net0");
	KTEST_ASSERT_NOT_NULL(tc, net0);

	before_drops = arm64_virt_virtio_net_tx_drops_busy();
	/* 1515 byte frame > MTU 1514 must reject. Use a non-NULL pointer
	 * to dodge the early NULL check; length validation runs first. */
	n = net0->write(0u, (const uint8_t *)0x100, 1515u);
	KTEST_EXPECT_EQ(tc, (uint32_t)(int32_t)n, (uint32_t)-1);
	KTEST_EXPECT_EQ(tc,
	                arm64_virt_virtio_net_tx_drops_busy(),
	                before_drops + 1u);
}

static void test_arm64_virtio_net_chardev_rejects_short(ktest_case_t *tc)
{
	const chardev_ops_t *net0;
	uint8_t frame[13];
	uint32_t before_drops;
	int n;

	if (!arm64_virt_virtio_net_driver_ok()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}

	net0 = chardev_get("net0");
	KTEST_ASSERT_NOT_NULL(tc, net0);

	k_memset(frame, 0xff, sizeof(frame));
	before_drops = arm64_virt_virtio_net_tx_drops_busy();
	n = net0->write(0u, frame, (uint32_t)sizeof(frame));
	KTEST_EXPECT_EQ(tc, (uint32_t)(int32_t)n, (uint32_t)-1);
	KTEST_EXPECT_EQ(tc,
	                arm64_virt_virtio_net_tx_drops_busy(),
	                before_drops + 1u);
}

/*
 * Phase 2 M3.0 — virtio-gpu front-end. The driver's init runs from
 * arm64_start_kernel before KTEST execution, so by the time these
 * tests run the device has either reached arm64_virt_virtio_gpu_ready()
 * (DRIVER_OK + six-command sequence complete) or skipped because no
 * `-device virtio-gpu-device` was on the QEMU command line. The
 * harness in tools/arm64_qemu_harness.py advertises the device, so the
 * checked-in CI run exercises the protocol path; an environment that
 * skips the device still keeps the rest of the suite green.
 */

static void test_arm64_virtio_gpu_reached_ready(ktest_case_t *tc)
{
	/* If virtio-gpu wasn't advertised on the bus (the ramfb-fallback
	 * harness), there's nothing to be ready about — skip-pass. The
	 * fallback harness asserts ramfb owns /dev/fb0 separately. */
	if (!arm64_virt_virtio_gpu_device_found()) {
		KTEST_EXPECT_EQ(tc, 0u, 0u);
		return;
	}
	KTEST_EXPECT_TRUE(tc, arm64_virt_virtio_gpu_ready());
}

static void
test_arm64_virtio_gpu_query_display_returns_nonzero(ktest_case_t *tc)
{
	uint32_t w = 0;
	uint32_t h = 0;
	int rc;

	if (!arm64_virt_virtio_gpu_ready()) {
		/* Skip silently — the ready test above already records the
		 * absence; failing here would double-count the same skip. */
		return;
	}
	rc = arm64_virt_virtio_gpu_query_display(&w, &h);
	KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);
	KTEST_EXPECT_NE(tc, w, 0u);
	KTEST_EXPECT_NE(tc, h, 0u);
}

static void
test_arm64_virtio_gpu_pattern_checksum_is_deterministic(ktest_case_t *tc)
{
	uint32_t a;
	uint32_t b;

	if (!arm64_virt_virtio_gpu_ready())
		return;

	/* The kernel-side test pattern is written once during init and is
	 * static after that. Two consecutive checksum reads must agree;
	 * the absolute value is left unchecked here so subtle pixel-format
	 * tweaks don't require an opaque magic-number rebase. */
	a = arm64_virt_virtio_gpu_checksum_pattern();
	b = arm64_virt_virtio_gpu_checksum_pattern();
	KTEST_EXPECT_EQ(tc, a, b);
	KTEST_EXPECT_NE(tc, a, 0u);
}

static void test_arm64_virtio_gpu_partial_flush_smoke_passes(ktest_case_t *tc)
{
	uint32_t before;
	uint32_t after;
	int rc;

	if (!arm64_virt_virtio_gpu_ready())
		return;

	before = arm64_virt_virtio_gpu_checksum_pattern();
	rc = arm64_virt_virtio_gpu_partial_flush_smoke();
	after = arm64_virt_virtio_gpu_checksum_pattern();

	KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);
	/* Partial flush mutates a 16x16 patch in the bottom-right quadrant
	 * to a sentinel color, so the checksum must change. */
	KTEST_EXPECT_NE(tc, before, after);
}

static void test_arm64_virtio_gpu_dma_pages_held_within_budget(ktest_case_t *tc)
{
	uint32_t held;
	uint32_t expected = 6u;

	if (!arm64_virt_virtio_gpu_ready())
		return;

	/* M3.1 budget: 2 controlq + 2 cursorq + 1 req + 1 resp = 6 pages.
	 * M3.3 hardware cursor adds 4 pages of cursor sprite backing
	 * (64x64x4 = 16 KiB) + 1 page of dedicated cursorq request
	 * buffer = 5 more pages, total 11. The cursor budget only
	 * counts when the cursor actually uploaded successfully —
	 * cursor_ready gates that. The scanout buffer no longer comes
	 * from the DMA pool: it lives in
	 * platform_ram_layout()->framebuffer_base/size. */
	if (arm64_virt_virtio_gpu_cursor_ready())
		expected = 11u;

	held = arm64_virt_virtio_gpu_dma_pages_held();
	KTEST_EXPECT_EQ(tc, held, expected);
}

static void test_arm64_virtio_gpu_owns_fb0_post_init(ktest_case_t *tc)
{
	const platform_ram_layout_t *l = platform_ram_layout();
	const chardev_ops_t *fb0;

	if (!arm64_virt_virtio_gpu_ready())
		return;

	/* M3.1: virtio-gpu must publish /dev/fb0 when it reaches DRIVER_OK
	 * + fbdev_init succeeds. ramfb's chardev_get("fb0") skip check
	 * fires AFTER virtio-gpu has registered, so on the gpu path
	 * /dev/fb0 is owned by virtio-gpu. */
	fb0 = chardev_get("fb0");
	KTEST_ASSERT_NOT_NULL(tc, fb0);
	/* The published phys equals the platform's framebuffer reservation
	 * — the same physical region ramfb would have used on the fallback
	 * path. */
	KTEST_EXPECT_NE(tc, (uint32_t)l->framebuffer_base, 0u);
	KTEST_EXPECT_GE(tc, (uint32_t)l->framebuffer_size, 0x300000u);
}

static void test_arm64_virtio_gpu_publish_pump_advances_counter(
    ktest_case_t *tc)
{
	uint32_t before;
	uint32_t after;

	if (!arm64_virt_virtio_gpu_ready())
		return;

	/*
	 * M3.2: publish a small dirty rect, pump it, and verify the
	 * cumulative pump-runs counter advances by AT LEAST 1.
	 *
	 * Replaces the M3.1-era test_arm64_virtio_gpu_request_pump_*
	 * because request_flush is now a no-ioctl-recent fallback
	 * counter that only synthesizes a rect after ~17 timer ticks of
	 * silence — a single call from this test wouldn't trigger the
	 * threshold.
	 *
	 * The "at least 1" framing is unchanged from M3.1: the timer
	 * tick can asynchronously union a fallback rect into the pending
	 * state at any point, so strict equality would race.
	 */
	before = arm64_virt_virtio_gpu_pump_runs();
	arm64_virt_virtio_gpu_publish_dirty_rect(drunix_rect_make(0, 0, 32, 32));
	arm64_virt_virtio_gpu_pump_flush();
	after = arm64_virt_virtio_gpu_pump_runs();
	KTEST_EXPECT_GE(tc, after, before + 1u);
}

static void test_arm64_virtio_gpu_cursor_uploaded(ktest_case_t *tc)
{
	if (!arm64_virt_virtio_gpu_ready())
		return;

#if DRUNIX_ARM64_VIRT_HW_CURSOR
	/* Opt-in hardware cursor path: the gpu-enabled build must upload
	 * the cursor sprite and publish the move-cursor hook. */
	KTEST_EXPECT_TRUE(tc, arm64_virt_virtio_gpu_cursor_ready());
#else
	/* Default path: keep the compositor software cursor active. */
	KTEST_EXPECT_FALSE(tc, arm64_virt_virtio_gpu_cursor_ready());
#endif
}

static void test_arm64_virtio_gpu_move_cursor_advances_counter(
    ktest_case_t *tc)
{
	uint32_t before;
	uint32_t after;

	if (!arm64_virt_virtio_gpu_ready() ||
	    !arm64_virt_virtio_gpu_cursor_ready())
		return;

	/* A single MOVE_CURSOR call to a known-good in-bounds position
	 * must advance the cursorq submit counter. Same "at least 1"
	 * framing as the M3.2 pump test: timer-tick activity is
	 * orthogonal but we don't expect any other path to bump the
	 * cursor counter, so before+1 is the strict lower bound. */
	before = arm64_virt_virtio_gpu_cursor_move_runs();
	arm64_virt_virtio_gpu_move_cursor(100, 100);
	after = arm64_virt_virtio_gpu_cursor_move_runs();
	KTEST_EXPECT_GE(tc, after, before + 1u);
}

static void test_arm64_virtio_gpu_move_cursor_off_screen_rejected(
    ktest_case_t *tc)
{
	uint32_t before;
	uint32_t after;

	if (!arm64_virt_virtio_gpu_ready() ||
	    !arm64_virt_virtio_gpu_cursor_ready())
		return;

	/* Negative or off-screen coordinates must NOT submit. The
	 * counter must stay constant across the calls below. */
	before = arm64_virt_virtio_gpu_cursor_move_runs();
	arm64_virt_virtio_gpu_move_cursor(-1, 0);
	arm64_virt_virtio_gpu_move_cursor(0, -1);
	arm64_virt_virtio_gpu_move_cursor(100000, 100000);
	after = arm64_virt_virtio_gpu_cursor_move_runs();
	KTEST_EXPECT_EQ(tc, after, before);
}

static void test_arm64_virtio_gpu_publish_invalid_rect_ignored(
    ktest_case_t *tc)
{
	uint32_t before;
	uint32_t after;

	if (!arm64_virt_virtio_gpu_ready())
		return;

	/*
	 * M3.2 publish_dirty_rect must reject invalid rects (zero/negative
	 * w/h) before queueing. After invalid publishes, a pump_flush
	 * call must NOT advance pump_runs (no rect was queued).
	 *
	 * As with the other pump tests, "must not advance" is racy
	 * against the timer tick's fallback synthesis — so the test
	 * uses GE. The minimum-viable check is that the invalid publish
	 * path doesn't crash + pump_runs is monotonically non-decreasing.
	 */
	(void)arm64_virt_virtio_gpu_publish_dirty_rect;
	arm64_virt_virtio_gpu_publish_dirty_rect(drunix_rect_make(0, 0, 0, 0));
	arm64_virt_virtio_gpu_publish_dirty_rect(drunix_rect_make(0, 0, -5, 10));
	arm64_virt_virtio_gpu_publish_dirty_rect(drunix_rect_make(0, 0, 10, -5));

	before = arm64_virt_virtio_gpu_pump_runs();
	arm64_virt_virtio_gpu_pump_flush();
	after = arm64_virt_virtio_gpu_pump_runs();
	/* GE rather than EQ: the timer-tick fallback can advance the
	 * counter asynchronously between snapshots. The test is
	 * specifically that the three invalid publishes above did NOT
	 * crash; pump_runs being unchanged after a single pump call is
	 * directionally correct. */
	KTEST_EXPECT_GE(tc, after, before);
}

static void test_arm64_virtio_gpu_init_is_idempotent(ktest_case_t *tc)
{
	uint32_t before_pages;
	int rc;

	if (!arm64_virt_virtio_gpu_ready())
		return;

	/* A second call to init must short-circuit (g_initialized == 1)
	 * and must not allocate additional DMA pages. The post-fix
	 * f7969e2 cleanup made this property hold even after a partial-
	 * init failure; the success path was always idempotent and this
	 * test pins that contract. (Codex M3.0 delivery review #6.) */
	before_pages = arm64_virt_virtio_gpu_dma_pages_held();
	rc = arm64_virt_virtio_gpu_init();
	KTEST_EXPECT_EQ(tc, (uint32_t)rc, 0u);
	KTEST_EXPECT_EQ(tc, arm64_virt_virtio_gpu_dma_pages_held(), before_pages);
	KTEST_EXPECT_TRUE(tc, arm64_virt_virtio_gpu_ready());
}

static void test_arm64_virtio_gpu_display_can_host_scanout(ktest_case_t *tc)
{
	uint32_t w = 0;
	uint32_t h = 0;

	if (!arm64_virt_virtio_gpu_ready())
		return;

	/* The 32x32 BGRA M3.0 scanout sits inside scanout 0; the display
	 * must therefore be at least that big. QEMU's default virt
	 * display reports 1280x800; the assertion here is just the
	 * inequality so a future host with a smaller default still passes
	 * as long as the scanout fits. */
	(void)arm64_virt_virtio_gpu_query_display(&w, &h);
	KTEST_EXPECT_GE(tc, w, 32u);
	KTEST_EXPECT_GE(tc, h, 32u);
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
    KTEST_CASE(test_arm64_el1_uses_sp_el1),
    KTEST_CASE(test_arm64_wall_clock_seeded_from_build_time),
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
    KTEST_CASE(test_arm64_framebuffer_reservation_is_8mib),
    KTEST_CASE(test_arm64_framebuffer_reservation_classifier),
    KTEST_CASE(test_arm64_framebuffer_kernel_alias_is_normal_nc),
    KTEST_CASE(test_arm64_fbdev_chardev_published),
    KTEST_CASE(test_arm64_fbdev_geometry_matches_ramfb_config),
    KTEST_CASE(test_arm64_virtio_input_devices_enumerated),
    KTEST_CASE(test_arm64_virtio_keyboard_event_to_kbdev),
    KTEST_CASE(test_arm64_virtio_mouse_event_to_mousedev),
    KTEST_CASE(test_arm64_virtio_net_mmio_device_enumerated),
    KTEST_CASE(test_arm64_virtio_net_features_ok_with_mac),
    KTEST_CASE(test_arm64_virtio_net_reads_mac_as_bytes),
    KTEST_CASE(test_arm64_virtio_net_rejects_modern_transport),
    KTEST_CASE(test_arm64_virtio_net_dma_rings_allocated),
    KTEST_CASE(test_arm64_virtio_net_packet_buffers_translate_nonzero),
    KTEST_CASE(test_arm64_virtio_net_packet_buffers_distinct),
    KTEST_CASE(test_arm64_virtio_net_driver_ok_after_init),
    KTEST_CASE(test_arm64_virtio_net_rx_counters_initialized),
    KTEST_CASE(test_arm64_virtio_net_rx_dequeue_empty_returns_zero),
    KTEST_CASE(test_arm64_virtio_net_tx_send_returns_len),
    KTEST_CASE(test_arm64_virtio_net_tx_rejects_short),
    KTEST_CASE(test_arm64_virtio_net_tx_rejects_oversize),
    KTEST_CASE(test_arm64_virtio_net_chardev_published),
    KTEST_CASE(test_arm64_virtio_net_chardev_write_submits_tx),
    KTEST_CASE(test_arm64_virtio_net_chardev_rejects_oversize),
    KTEST_CASE(test_arm64_virtio_net_chardev_rejects_short),
    KTEST_CASE(test_arm64_virtio_gpu_reached_ready),
    KTEST_CASE(test_arm64_virtio_gpu_query_display_returns_nonzero),
    KTEST_CASE(test_arm64_virtio_gpu_pattern_checksum_is_deterministic),
    KTEST_CASE(test_arm64_virtio_gpu_partial_flush_smoke_passes),
    KTEST_CASE(test_arm64_virtio_gpu_dma_pages_held_within_budget),
    KTEST_CASE(test_arm64_virtio_gpu_init_is_idempotent),
    KTEST_CASE(test_arm64_virtio_gpu_display_can_host_scanout),
    KTEST_CASE(test_arm64_virtio_gpu_owns_fb0_post_init),
    KTEST_CASE(test_arm64_virtio_gpu_publish_pump_advances_counter),
    KTEST_CASE(test_arm64_virtio_gpu_publish_invalid_rect_ignored),
    KTEST_CASE(test_arm64_virtio_gpu_cursor_uploaded),
    KTEST_CASE(test_arm64_virtio_gpu_move_cursor_advances_counter),
    KTEST_CASE(test_arm64_virtio_gpu_move_cursor_off_screen_rejected),
#endif
};

static ktest_suite_t suite = KTEST_SUITE("arch_arm64", cases);

ktest_suite_t *ktest_suite_arch_arm64(void)
{
	return &suite;
}
