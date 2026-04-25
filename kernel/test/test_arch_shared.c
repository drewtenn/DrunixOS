/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_arch_shared.c - architecture-neutral behavior tests using arch APIs.
 */

#include "ktest.h"
#include "arch.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "pmm.h"
#include "process.h"
#include "resources.h"
#include "sched.h"
#include "syscall/syscall_internal.h"
#include "syscall/syscall_linux.h"
#include "uaccess.h"
#include "vfs.h"
#include "vma.h"

static void shared_init_vma_proc(process_t *proc)
{
	k_memset(proc, 0, sizeof(*proc));
	proc->heap_start = 0x00410000u;
	proc->brk = 0x00418000u;
	proc->stack_low_limit =
	    USER_STACK_TOP - (uint32_t)USER_STACK_PAGES * PAGE_SIZE;
	vma_init(proc);
	(void)vma_add(proc,
	              proc->heap_start,
	              proc->brk,
	              VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                  VMA_FLAG_PRIVATE,
	              VMA_KIND_HEAP);
	(void)vma_add(proc,
	              USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * PAGE_SIZE,
	              USER_STACK_TOP,
	              VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                  VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
	              VMA_KIND_STACK);
}

static int shared_init_user_proc(process_t *proc)
{
	shared_init_vma_proc(proc);
	proc->pd_phys = (uint32_t)arch_aspace_create();
	return proc->pd_phys ? 0 : -1;
}

static void shared_destroy_user_proc(process_t *proc)
{
	process_release_user_space(proc);
}

static uint32_t
shared_map_user_page(process_t *proc, uint32_t virt, uint8_t fill)
{
	uint32_t page = virt & ~0xFFFu;
	uint32_t phys = pmm_alloc_page();
	void *mapped;

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

	mapped = arch_page_temp_map(phys);
	if (!mapped) {
		pmm_free_page(phys);
		return 0;
	}
	k_memset(mapped, fill, PAGE_SIZE);
	arch_page_temp_unmap(mapped);

	if (arch_mm_map((arch_aspace_t)proc->pd_phys,
	                page,
	                phys,
	                ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ | ARCH_MM_MAP_WRITE |
	                    ARCH_MM_MAP_USER) != 0) {
		pmm_free_page(phys);
		return 0;
	}

	return phys;
}

static uint8_t shared_user_byte(process_t *proc, uint32_t virt)
{
	arch_mm_mapping_t mapping;
	uint8_t value = 0;
	void *mapped;

	if (arch_mm_query((arch_aspace_t)proc->pd_phys, virt & ~0xFFFu, &mapping) !=
	    0)
		return 0;
	mapped = arch_page_temp_map(mapping.phys_addr);
	if (!mapped)
		return 0;
	value = ((uint8_t *)mapped)[virt & 0xFFFu];
	arch_page_temp_unmap(mapped);
	return value;
}

static vm_area_t *
shared_find_vma_range(process_t *proc, uint32_t start, uint32_t end)
{
	vm_area_t *vma = vma_find(proc, start);

	if (!vma || vma->start != start || vma->end != end)
		return 0;
	return vma;
}

static process_t *shared_start_syscall_process(process_t *proc)
{
	sched_init();
	shared_init_vma_proc(proc);
	proc->pd_phys = (uint32_t)arch_aspace_create();
	if (!proc->pd_phys)
		return 0;

	proc->arch_state.context = 1u;
	proc->tty_id = 0;
	for (int fd = 0; fd < 3; fd++) {
		proc->open_files[fd].type = FD_TYPE_TTY;
		proc->open_files[fd].writable = 1;
		proc->open_files[fd].access_mode = LINUX_O_RDWR;
		proc->open_files[fd].u.tty.tty_idx = 0;
	}
	if (proc_resource_init_fresh(proc) != 0) {
		shared_destroy_user_proc(proc);
		return 0;
	}
	proc_resource_mirror_from_process(proc);

	if (sched_add(proc) < 1) {
		proc_resource_put_all(proc);
		return 0;
	}
	return sched_bootstrap();
}

static void shared_stop_syscall_process(process_t *proc)
{
	if (proc) {
		if (proc->as)
			proc_resource_put_all(proc);
		else
			shared_destroy_user_proc(proc);
	}
	sched_init();
}

static int shared_cwd_stat(void *ctx, const char *path, vfs_stat_t *st)
{
	(void)ctx;

	if (!path || k_strcmp(path, "bin") != 0)
		return -1;

	st->type = VFS_STAT_TYPE_DIR;
	st->size = 0;
	st->link_count = 1;
	st->mtime = 0;
	return 0;
}

static const fs_ops_t shared_cwd_ops = {
    .stat = shared_cwd_stat,
};

static void
test_shared_vma_map_anonymous_places_regions_below_stack(ktest_case_t *tc)
{
	static process_t proc;
	uint32_t addr = 0;
	uint32_t stack_base =
	    USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * PAGE_SIZE;

	shared_init_vma_proc(&proc);

	KTEST_ASSERT_EQ(tc,
	                vma_map_anonymous(&proc,
	                                  0,
	                                  0x2000u,
	                                  VMA_FLAG_READ | VMA_FLAG_WRITE |
	                                      VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
	                                  &addr),
	                0);
	KTEST_EXPECT_EQ(tc, addr, stack_base - 0x2000u);
	KTEST_EXPECT_EQ(tc, proc.vma_count, 3u);
	KTEST_EXPECT_EQ(tc, proc.vmas[1].start, stack_base - 0x2000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[1].end, stack_base);
	KTEST_EXPECT_EQ(tc, proc.vmas[1].kind, VMA_KIND_GENERIC);
}

static void test_shared_vma_unmap_range_rejects_heap_or_stack(ktest_case_t *tc)
{
	static process_t proc;

	shared_init_vma_proc(&proc);
	KTEST_EXPECT_EQ(
	    tc, vma_unmap_range(&proc, 0x00410000u, 0x00411000u), (uint32_t)-1);
	KTEST_EXPECT_EQ(tc, proc.vma_count, 2u);
}

static void test_shared_vma_protect_range_splits_and_requires_full_coverage(
    ktest_case_t *tc)
{
	static process_t proc;

	shared_init_vma_proc(&proc);
	KTEST_ASSERT_EQ(tc,
	                vma_add(&proc,
	                        0x80000000u,
	                        0x80003000u,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE,
	                        VMA_KIND_GENERIC),
	                0);

	KTEST_ASSERT_EQ(
	    tc,
	    vma_protect_range(&proc,
	                      0x80001000u,
	                      0x80002000u,
	                      VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE),
	    0);
	KTEST_EXPECT_EQ(tc, proc.vma_count, 5u);
	vm_area_t *protected_vma =
	    shared_find_vma_range(&proc, 0x80001000u, 0x80002000u);
	KTEST_ASSERT_NOT_NULL(tc, protected_vma);
	KTEST_EXPECT_EQ(tc,
	                protected_vma->flags,
	                VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE);
	KTEST_EXPECT_EQ(
	    tc,
	    vma_protect_range(&proc,
	                      0x70000000u,
	                      0x70001000u,
	                      VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE),
	    (uint32_t)-1);
}

static void
test_shared_proc_resource_put_exec_owner_releases_solo_owner(ktest_case_t *tc)
{
	static process_t proc;

	k_memset(&proc, 0, sizeof(proc));
	KTEST_ASSERT_EQ(tc, (uint32_t)proc_resource_init_fresh(&proc), 0u);
	KTEST_ASSERT_NOT_NULL(tc, proc.as);
	KTEST_ASSERT_NOT_NULL(tc, proc.files);
	KTEST_ASSERT_NOT_NULL(tc, proc.fs_state);
	KTEST_ASSERT_NOT_NULL(tc, proc.sig_actions);
	KTEST_EXPECT_EQ(tc, proc.as->refs, 1u);
	KTEST_EXPECT_EQ(tc, proc.files->refs, 1u);
	KTEST_EXPECT_EQ(tc, proc.fs_state->refs, 1u);
	KTEST_EXPECT_EQ(tc, proc.sig_actions->refs, 1u);

	proc_resource_put_exec_owner(&proc);

	KTEST_EXPECT_NULL(tc, proc.as);
	KTEST_EXPECT_NULL(tc, proc.files);
	KTEST_EXPECT_NULL(tc, proc.fs_state);
	KTEST_EXPECT_NULL(tc, proc.sig_actions);
}

static void test_shared_repeated_exec_owner_put_preserves_heap(ktest_case_t *tc)
{
	static process_t proc;
	uint32_t free_before = kheap_free_bytes();

	for (uint32_t i = 0; i < 64u; i++) {
		k_memset(&proc, 0, sizeof(proc));
		KTEST_ASSERT_EQ(tc, (uint32_t)proc_resource_init_fresh(&proc), 0u);
		proc_resource_put_exec_owner(&proc);
		KTEST_EXPECT_NULL(tc, proc.as);
		KTEST_EXPECT_NULL(tc, proc.files);
		KTEST_EXPECT_NULL(tc, proc.fs_state);
		KTEST_EXPECT_NULL(tc, proc.sig_actions);
	}

	KTEST_EXPECT_GE(tc, kheap_free_bytes(), free_before);
}

static void test_shared_copy_to_user_spans_pages(ktest_case_t *tc)
{
	static process_t proc;
	uint8_t src[200];
	uint32_t start = 0x401000u + PAGE_SIZE - 50u;

	KTEST_ASSERT_EQ(tc, shared_init_user_proc(&proc), 0);
	KTEST_ASSERT_NOT_NULL(tc, shared_map_user_page(&proc, 0x401000u, 0xAA));
	KTEST_ASSERT_NOT_NULL(tc, shared_map_user_page(&proc, 0x402000u, 0xBB));

	for (uint32_t i = 0; i < sizeof(src); i++)
		src[i] = (uint8_t)i;

	KTEST_EXPECT_EQ(
	    tc, uaccess_copy_to_user(&proc, start, src, sizeof(src)), 0);
	KTEST_EXPECT_EQ(tc, shared_user_byte(&proc, start), 0u);
	KTEST_EXPECT_EQ(tc, shared_user_byte(&proc, start + 49u), 49u);
	KTEST_EXPECT_EQ(tc, shared_user_byte(&proc, 0x402000u), 50u);
	KTEST_EXPECT_EQ(tc, shared_user_byte(&proc, 0x402000u + 149u), 199u);

	shared_destroy_user_proc(&proc);
}

static void test_shared_copy_string_from_user_spans_pages(ktest_case_t *tc)
{
	static process_t proc;
	static const char msg[] = "cross-page";
	char buf[32];
	uint32_t start = 0x410000u + PAGE_SIZE - 6u;

	KTEST_ASSERT_EQ(tc, shared_init_user_proc(&proc), 0);
	KTEST_ASSERT_NOT_NULL(tc, shared_map_user_page(&proc, 0x410000u, 0));
	KTEST_ASSERT_NOT_NULL(tc, shared_map_user_page(&proc, 0x411000u, 0));
	KTEST_ASSERT_EQ(
	    tc, uaccess_copy_to_user(&proc, start, msg, sizeof(msg)), 0);

	KTEST_EXPECT_EQ(
	    tc, uaccess_copy_string_from_user(&proc, buf, sizeof(buf), start), 0);
	KTEST_EXPECT_EQ(tc, (uint32_t)k_strcmp(buf, msg), 0u);

	shared_destroy_user_proc(&proc);
}

static void
test_shared_prepare_rejects_kernel_and_unmapped_ranges(ktest_case_t *tc)
{
	static process_t proc;

	KTEST_ASSERT_EQ(tc, shared_init_user_proc(&proc), 0);

	KTEST_EXPECT_EQ(tc, uaccess_prepare(&proc, 0x1000u, 1u, 0), (uint32_t)-1);
	KTEST_ASSERT_NOT_NULL(tc, shared_map_user_page(&proc, 0x420000u, 0));
	KTEST_EXPECT_EQ(tc,
	                uaccess_prepare(&proc, 0x420000u + PAGE_SIZE - 2u, 4u, 0),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(
	    tc, uaccess_prepare(&proc, USER_STACK_TOP - 2u, 4u, 0), (uint32_t)-1);

	shared_destroy_user_proc(&proc);
}

static void test_shared_syscall_fstat_reads_resource_fd_table(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = shared_start_syscall_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	cur->files->open_files[1].type = FD_TYPE_NONE;
	KTEST_EXPECT_EQ(tc, syscall_case_fstat64(1u, 0x00800400u), (uint32_t)-1);

	shared_stop_syscall_process(cur);
}

static void test_shared_syscall_getcwd_reads_resource_fs_state(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = shared_start_syscall_process(&proc);
	uint8_t got[8];
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_NOT_NULL(tc, shared_map_user_page(cur, 0x00800000u, 0));

	k_strncpy(cur->fs_state->cwd, "home", sizeof(cur->fs_state->cwd) - 1u);

	KTEST_EXPECT_EQ(tc, syscall_case_getcwd(0x00800000u, 64u), 6u);
	KTEST_EXPECT_EQ(tc, uaccess_copy_from_user(cur, got, 0x00800000u, 6u), 0);
	KTEST_EXPECT_EQ(tc, got[0], (uint8_t)'/');
	KTEST_EXPECT_EQ(tc, got[1], (uint8_t)'h');
	KTEST_EXPECT_EQ(tc, got[2], (uint8_t)'o');
	KTEST_EXPECT_EQ(tc, got[3], (uint8_t)'m');
	KTEST_EXPECT_EQ(tc, got[4], (uint8_t)'e');
	KTEST_EXPECT_EQ(tc, got[5], 0u);

	shared_stop_syscall_process(cur);
}

static void
test_shared_syscall_chdir_updates_resource_fs_state(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur;
	static const char path[] = "bin";
	char got[8];

	vfs_reset();
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_register("cwdtest", &shared_cwd_ops), 0u);
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_mount("/", "cwdtest"), 0u);

	cur = shared_start_syscall_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_NOT_NULL(tc, shared_map_user_page(cur, 0x00800000u, 0));
	KTEST_ASSERT_EQ(
	    tc, uaccess_copy_to_user(cur, 0x00800000u, path, sizeof(path)), 0);

	KTEST_ASSERT_EQ(tc, syscall_case_chdir(0x00800000u), 0u);
	KTEST_EXPECT_TRUE(tc, k_strcmp(cur->fs_state->cwd, "bin") == 0);
	KTEST_EXPECT_EQ(tc, syscall_case_getcwd(0x00800100u, 64u), 5u);
	KTEST_EXPECT_EQ(tc, uaccess_copy_from_user(cur, got, 0x00800100u, 5u), 0);
	KTEST_EXPECT_TRUE(tc, k_strcmp(got, "/bin") == 0);

	shared_stop_syscall_process(cur);
	vfs_reset();
}

static void
test_shared_syscall_brk_reads_resource_address_space(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = shared_start_syscall_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	cur->as->brk = cur->as->heap_start + PAGE_SIZE;

	KTEST_EXPECT_EQ(tc, syscall_case_brk(0), cur->as->brk);

	shared_stop_syscall_process(cur);
}

static void test_shared_rt_sigaction_reads_resource_handlers(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = shared_start_syscall_process(&proc);
	uint8_t got[4];
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_NOT_NULL(tc, shared_map_user_page(cur, 0x00800000u, 0));

	cur->sig_actions->handlers[SIGTERM] = SIG_IGN;

	KTEST_EXPECT_EQ(
	    tc, syscall_case_rt_sigaction(SIGTERM, 0, 0x00800000u, 8u), 0u);
	KTEST_EXPECT_EQ(
	    tc, uaccess_copy_from_user(cur, got, 0x00800000u, sizeof(got)), 0);
	KTEST_EXPECT_EQ(tc, got[0], (uint8_t)SIG_IGN);

	shared_stop_syscall_process(cur);
}

static void test_shared_clone_rejects_sighand_without_vm(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = shared_start_syscall_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_case_clone(
	        CLONE_SIGHAND | SIGCHLD, USER_STACK_TOP - PAGE_SIZE, 0, 0, 0),
	    (uint32_t)-LINUX_EINVAL);

	shared_stop_syscall_process(cur);
}

static void test_shared_clone_rejects_thread_without_sighand(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = shared_start_syscall_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	KTEST_EXPECT_EQ(tc,
	                syscall_case_clone(CLONE_THREAD | CLONE_VM | SIGCHLD,
	                                   USER_STACK_TOP - PAGE_SIZE,
	                                   0,
	                                   0,
	                                   0),
	                (uint32_t)-LINUX_EINVAL);

	shared_stop_syscall_process(cur);
}

static void
test_shared_clone_thread_shares_group_and_selected_resources(ktest_case_t *tc)
{
	static process_t seed;
	process_t *parent = shared_start_syscall_process(&seed);
	uint32_t flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
	                 CLONE_THREAD | SIGCHLD;
	uint32_t tid;
	process_t *child;
	KTEST_ASSERT_NOT_NULL(tc, parent);

	tid = syscall_case_clone(flags, USER_STACK_TOP - PAGE_SIZE, 0, 0, 0);
	KTEST_ASSERT_NE(tc, tid, (uint32_t)-1);

	child = sched_find_pid(tid);
	KTEST_ASSERT_NOT_NULL(tc, child);
	KTEST_EXPECT_EQ(tc, child->tgid, parent->tgid);
	KTEST_EXPECT_EQ(tc, child->group, parent->group);
	KTEST_EXPECT_EQ(tc, child->as, parent->as);
	KTEST_EXPECT_EQ(tc, child->files, parent->files);
	KTEST_EXPECT_EQ(tc, child->fs_state, parent->fs_state);
	KTEST_EXPECT_EQ(tc, child->sig_actions, parent->sig_actions);
	KTEST_EXPECT_EQ(tc, parent->as->refs, 2u);

	KTEST_EXPECT_EQ(tc, (uint32_t)sched_force_remove_task(tid), 0u);
	shared_stop_syscall_process(parent);
}

static void test_shared_clone_process_without_vm_gets_distinct_group_and_as(
    ktest_case_t *tc)
{
	static process_t seed;
	process_t *parent = shared_start_syscall_process(&seed);
	uint32_t tid;
	process_t *child;
	KTEST_ASSERT_NOT_NULL(tc, parent);

	tid = syscall_case_clone(SIGCHLD, USER_STACK_TOP - PAGE_SIZE, 0, 0, 0);
	KTEST_ASSERT_NE(tc, tid, (uint32_t)-1);

	child = sched_find_pid(tid);
	KTEST_ASSERT_NOT_NULL(tc, child);
	KTEST_EXPECT_EQ(tc, child->tgid, child->tid);
	KTEST_EXPECT_NE(tc, child->group, parent->group);
	KTEST_EXPECT_NE(tc, child->as, parent->as);

	KTEST_EXPECT_EQ(tc, (uint32_t)sched_force_remove_task(tid), 0u);
	shared_stop_syscall_process(parent);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_shared_vma_map_anonymous_places_regions_below_stack),
    KTEST_CASE(test_shared_vma_unmap_range_rejects_heap_or_stack),
    KTEST_CASE(test_shared_vma_protect_range_splits_and_requires_full_coverage),
    KTEST_CASE(test_shared_proc_resource_put_exec_owner_releases_solo_owner),
    KTEST_CASE(test_shared_repeated_exec_owner_put_preserves_heap),
    KTEST_CASE(test_shared_copy_to_user_spans_pages),
    KTEST_CASE(test_shared_copy_string_from_user_spans_pages),
    KTEST_CASE(test_shared_prepare_rejects_kernel_and_unmapped_ranges),
    KTEST_CASE(test_shared_syscall_fstat_reads_resource_fd_table),
    KTEST_CASE(test_shared_syscall_getcwd_reads_resource_fs_state),
    KTEST_CASE(test_shared_syscall_chdir_updates_resource_fs_state),
    KTEST_CASE(test_shared_syscall_brk_reads_resource_address_space),
    KTEST_CASE(test_shared_rt_sigaction_reads_resource_handlers),
    KTEST_CASE(test_shared_clone_rejects_sighand_without_vm),
    KTEST_CASE(test_shared_clone_rejects_thread_without_sighand),
    KTEST_CASE(test_shared_clone_thread_shares_group_and_selected_resources),
    KTEST_CASE(test_shared_clone_process_without_vm_gets_distinct_group_and_as),
};

static ktest_suite_t suite = KTEST_SUITE("arch_shared", cases);

ktest_suite_t *ktest_suite_arch_shared(void)
{
	return &suite;
}
