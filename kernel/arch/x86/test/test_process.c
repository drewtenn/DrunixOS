/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * test_process.c — unit tests for synthetic process launch frames.
 */

#include "ktest.h"
#include "blkdev.h"
#include "core.h"
#include "elf.h"
#include "fs.h"
#include "kheap.h"
#include "process.h"
#include "resources.h"
#include "sched.h"
#include "gdt.h"
#include "kprintf.h"
#include "kstring.h"
#include "mem_forensics.h"
#include "paging.h"
#include "pmm.h"
#include "syscall.h"
#include "tty.h"
#include "vfs.h"
#include "vma.h"

extern void process_initial_launch(void);
extern void process_exec_launch(void);

#define TEST_CORE_DUMP_PID 77u
#define TEST_CORE_NOTE_ALIGN 4u
#define TEST_NT_PRPSINFO 3u
#define TEST_NT_DRUNIX_VMSTAT 0x4458564du
#define TEST_NT_DRUNIX_FAULT 0x44584654u
#define TEST_NT_DRUNIX_MAPS 0x44584d50u
#define TEST_LINUX_MAP_FIXED 0x10u
#define TEST_LINUX_AT_NO_AUTOMOUNT 0x0800u
#define TEST_LINUX_AT_EMPTY_PATH 0x1000u
#define TEST_LINUX_STATX_BASIC_STATS 0x7ffu
#define TEST_LINUX_F_DUPFD 0u
#define TEST_LINUX_F_GETFD 1u
#define TEST_LINUX_F_SETFD 2u
#define TEST_LINUX_F_GETFL 3u
#define TEST_LINUX_TCGETS 0x5401u
#define TEST_LINUX_TCSETS 0x5402u
#define TEST_LINUX_TIOCGWINSZ 0x5413u
#define TEST_LINUX_FIONREAD 0x541Bu
#define TEST_LINUX_O_WRONLY 01u
#define TEST_LINUX_O_RDWR 02u
#define TEST_LINUX_O_CREAT 0100u
#define TEST_LINUX_O_APPEND 02000u
#define TEST_LINUX_POLLIN 0x0001u
#define TEST_LINUX_EBADF 9u
#define TEST_LINUX_EINVAL 22u
#define TEST_CLONE_VM 0x00000100u
#define TEST_CLONE_FS 0x00000200u
#define TEST_CLONE_FILES 0x00000400u
#define TEST_CLONE_SIGHAND 0x00000800u
#define TEST_CLONE_PARENT_SETTID 0x00100000u
#define TEST_CLONE_THREAD 0x00010000u
#define TEST_CLONE_SETTLS 0x00080000u
#define TEST_CLONE_CHILD_CLEARTID 0x00200000u
#define TEST_CLONE_CHILD_SETTID 0x01000000u

static uint32_t test_align_up(uint32_t val, uint32_t align)
{
	return (val + align - 1u) & ~(align - 1u);
}

static uint32_t test_read_u32_le(const uint8_t *buf, uint32_t off)
{
	return (uint32_t)buf[off + 0] | ((uint32_t)buf[off + 1] << 8) |
	       ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

static uint64_t test_read_u64_le(const uint8_t *buf, uint32_t off)
{
	return (uint64_t)test_read_u32_le(buf, off) |
	       ((uint64_t)test_read_u32_le(buf, off + 4u) << 32);
}

static void init_frame_proc(process_t *proc,
                            uint32_t *kstack_words,
                            uint32_t entry,
                            uint32_t user_stack)
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
	proc->heap_start = 0x10010000u;
	proc->brk = 0x10018000u;
	proc->stack_low_limit =
	    USER_STACK_TOP - (uint32_t)USER_STACK_PAGES * 0x1000u;
	vma_init(proc);
	vma_add(proc,
	        proc->heap_start,
	        proc->brk,
	        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
	        VMA_KIND_HEAP);
	vma_add(proc,
	        USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * 0x1000u,
	        USER_STACK_TOP,
	        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON | VMA_FLAG_PRIVATE |
	            VMA_FLAG_GROWSDOWN,
	        VMA_KIND_STACK);
}

static void init_fresh_process_layout_proc(process_t *proc)
{
	k_memset(proc, 0, sizeof(*proc));
	proc->image_start = 0x10000000u;
	proc->image_end = 0x10010000u;
	proc->heap_start = proc->image_end;
	proc->brk = proc->heap_start;
	proc->stack_low_limit =
	    USER_STACK_TOP - (uint32_t)USER_STACK_PAGES * 0x1000u;
	vma_init(proc);
	vma_add(proc,
	        proc->image_start,
	        proc->image_end,
	        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC | VMA_FLAG_PRIVATE,
	        VMA_KIND_IMAGE);
	vma_add(proc,
	        proc->heap_start,
	        proc->brk,
	        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
	        VMA_KIND_HEAP);
	vma_add(proc,
	        USER_STACK_TOP - (uint32_t)USER_STACK_MAX_PAGES * 0x1000u,
	        USER_STACK_TOP,
	        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON | VMA_FLAG_PRIVATE |
	            VMA_FLAG_GROWSDOWN,
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

	if (!vma_find(proc, page) && vma_add(proc,
	                                     page,
	                                     page + PAGE_SIZE,
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

static int cwdtest_stat(void *ctx, const char *path, vfs_stat_t *st)
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

static const fs_ops_t cwdtest_ops = {
    .stat = cwdtest_stat,
};

static process_t *start_syscall_test_process(process_t *proc)
{
	sched_init();
	init_fresh_process_layout_proc(proc);
	proc->pd_phys = paging_create_user_space();
	if (!proc->pd_phys)
		return 0;

	proc->arch_state.context =
	    1; /* syscall tests do not context-switch this task */
	proc->tty_id = 0;
	proc->open_files[0].type = FD_TYPE_TTY;
	proc->open_files[0].writable = 1;
	proc->open_files[0].access_mode = TEST_LINUX_O_RDWR;
	proc->open_files[0].u.tty.tty_idx = 0;
	proc->open_files[1].type = FD_TYPE_TTY;
	proc->open_files[1].writable = 1;
	proc->open_files[1].access_mode = TEST_LINUX_O_RDWR;
	proc->open_files[1].u.tty.tty_idx = 0;
	proc->open_files[2].type = FD_TYPE_TTY;
	proc->open_files[2].writable = 1;
	proc->open_files[2].access_mode = TEST_LINUX_O_RDWR;
	proc->open_files[2].u.tty.tty_idx = 0;
	if (proc_resource_init_fresh(proc) != 0) {
		process_release_user_space(proc);
		return 0;
	}
	proc_resource_mirror_from_process(proc);

	if (sched_add(proc) < 1) {
		proc_resource_put_all(proc);
		process_release_user_space(proc);
		return 0;
	}
	return sched_bootstrap();
}

static void stop_syscall_test_process(process_t *proc)
{
	if (proc) {
		if (proc->as)
			proc_resource_put_all(proc);
		else
			process_release_user_space(proc);
	}
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
	proc->crash.fault_addr = 0xDEADBEEFu;
	proc->crash.frame.vector = 14u;
	proc->crash.frame.error_code = 0x6u;
	proc->crash.frame.eip = 0x10002A16u;
}

static void test_process_build_initial_frame_layout(ktest_case_t *tc)
{
	static process_t proc;
	static uint32_t kstack_words[32];

	init_frame_proc(&proc, kstack_words, 0x10001000u, 0xBFFFE000u);
	process_build_initial_frame(&proc);

	uint32_t *frame = (uint32_t *)proc.arch_state.context;
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
	KTEST_EXPECT_EQ(tc, frame[19], 0x10001000u);
	KTEST_EXPECT_EQ(tc, frame[20], GDT_USER_CS);
	KTEST_EXPECT_EQ(tc, frame[21], 0x202u);
	KTEST_EXPECT_EQ(tc, frame[22], 0xBFFFE000u);
	KTEST_EXPECT_EQ(tc, frame[23], GDT_USER_DS);
}

static void test_process_build_exec_frame_layout(ktest_case_t *tc)
{
	static process_t proc;
	static uint32_t kstack_words[32];

	init_frame_proc(&proc, kstack_words, 0x10002000u, 0xBFFFD000u);
	process_build_exec_frame(&proc, 0x00123000u, 0x00ABC000u);

	uint32_t *frame = (uint32_t *)proc.arch_state.context;
	KTEST_ASSERT_NOT_NULL(tc, frame);

	KTEST_EXPECT_EQ(tc, frame[0], 0u);
	KTEST_EXPECT_EQ(tc, frame[1], 0u);
	KTEST_EXPECT_EQ(tc, frame[2], 0u);
	KTEST_EXPECT_EQ(tc, frame[3], 0u);
	KTEST_EXPECT_EQ(tc, frame[4], (uint32_t)process_exec_launch);
	KTEST_EXPECT_EQ(tc, frame[5], 0x00123000u);
	KTEST_EXPECT_EQ(tc, frame[6], 0x00ABC000u);
	KTEST_EXPECT_EQ(tc, frame[7], GDT_USER_DS);
	KTEST_EXPECT_EQ(tc, frame[21], 0x10002000u);
	KTEST_EXPECT_EQ(tc, frame[22], GDT_USER_CS);
	KTEST_EXPECT_EQ(tc, frame[23], 0x202u);
	KTEST_EXPECT_EQ(tc, frame[24], 0xBFFFD000u);
	KTEST_EXPECT_EQ(tc, frame[25], GDT_USER_DS);
}

static void test_elf_machine_validation_is_arch_owned(ktest_case_t *tc)
{
	KTEST_EXPECT_EQ(
	    tc, (uint32_t)arch_elf_machine_supported(ELF_CLASS_32, EM_386), 1u);
	KTEST_EXPECT_EQ(
	    tc, (uint32_t)arch_elf_machine_supported(ELF_CLASS_64, EM_AARCH64), 0u);
}

static Elf32_Phdr test_elf_load_phdr(uint32_t vaddr, uint32_t memsz)
{
	Elf32_Phdr phdr;

	k_memset(&phdr, 0, sizeof(phdr));
	phdr.p_type = PT_LOAD;
	phdr.p_vaddr = vaddr;
	phdr.p_memsz = memsz;
	phdr.p_flags = PF_R | PF_X;
	phdr.p_align = PAGE_SIZE;
	return phdr;
}

static int test_write_elf_image(const char *name,
                                const Elf32_Phdr *phdrs,
                                uint16_t phnum,
                                vfs_file_ref_t *ref_out)
{
	static uint8_t image[512];
	Elf32_Ehdr ehdr;
	uint32_t image_len;
	uint32_t size = 0;
	int ino;

	if (!name || !phdrs || phnum == 0 || !ref_out)
		return -1;
	image_len = (uint32_t)sizeof(ehdr) + (uint32_t)phnum * sizeof(phdrs[0]);
	if (image_len > sizeof(image))
		return -1;

	k_memset(image, 0, sizeof(image));
	k_memset(&ehdr, 0, sizeof(ehdr));
	ehdr.e_ident[0] = 0x7Fu;
	ehdr.e_ident[1] = 'E';
	ehdr.e_ident[2] = 'L';
	ehdr.e_ident[3] = 'F';
	ehdr.e_ident[EI_CLASS] = ELF_CLASS_32;
	ehdr.e_type = ET_EXEC;
	ehdr.e_machine = EM_386;
	ehdr.e_entry = phdrs[0].p_vaddr;
	ehdr.e_phoff = sizeof(ehdr);
	ehdr.e_ehsize = sizeof(ehdr);
	ehdr.e_phentsize = sizeof(phdrs[0]);
	ehdr.e_phnum = phnum;

	k_memcpy(image, &ehdr, sizeof(ehdr));
	k_memcpy(image + sizeof(ehdr), phdrs, (uint32_t)phnum * sizeof(phdrs[0]));

	vfs_reset();
	dufs_register();
	if (vfs_mount("/", "dufs") != 0)
		return -1;
	(void)fs_unlink(name);
	ino = vfs_create(name);
	if (ino <= 0)
		return -1;
	if (vfs_open_file(name, ref_out, &size) != 0)
		return -1;
	if (vfs_write(*ref_out, 0, image, image_len) != (int)image_len)
		return -1;
	return 0;
}

static int test_load_elf_with_phdrs(const char *name,
                                    const Elf32_Phdr *phdrs,
                                    uint16_t phnum)
{
	vfs_file_ref_t ref;
	arch_aspace_t aspace;
	uintptr_t entry = 0;
	uintptr_t image_start = 0;
	uintptr_t heap_start = 0;
	int rc;

	if (test_write_elf_image(name, phdrs, phnum, &ref) != 0)
		return -100;

	aspace = arch_aspace_create();
	if (!aspace)
		return -101;

	rc = arch_elf_load_user_image(
	    ref, aspace, &entry, &image_start, &heap_start);
	arch_aspace_destroy(aspace);
	return rc;
}

static void test_elf_loader_rejects_direct_map_pt_load(ktest_case_t *tc)
{
	Elf32_Phdr phdr = test_elf_load_phdr(0x01000000u, PAGE_SIZE);

	KTEST_EXPECT_NE(
	    tc, (uint32_t)test_load_elf_with_phdrs("_ktelf_dm_", &phdr, 1), 0u);
}

static void test_elf_loader_rejects_overlapping_pt_loads(ktest_case_t *tc)
{
	Elf32_Phdr phdrs[2];

	phdrs[0] = test_elf_load_phdr(0x10000000u, 0x3000u);
	phdrs[1] = test_elf_load_phdr(0x10002000u, PAGE_SIZE);

	KTEST_EXPECT_NE(
	    tc, (uint32_t)test_load_elf_with_phdrs("_ktelf_ov_", phdrs, 2), 0u);
}

static void test_elf_loader_rejects_stack_pt_load(ktest_case_t *tc)
{
	Elf32_Phdr phdr = test_elf_load_phdr(USER_STACK_TOP - PAGE_SIZE, PAGE_SIZE);

	KTEST_EXPECT_NE(
	    tc, (uint32_t)test_load_elf_with_phdrs("_ktelf_st_", &phdr, 1), 0u);
}

static void test_elf_loader_rejects_kernel_pt_load(ktest_case_t *tc)
{
	Elf32_Phdr phdr = test_elf_load_phdr(USER_STACK_TOP, PAGE_SIZE);

	KTEST_EXPECT_NE(
	    tc, (uint32_t)test_load_elf_with_phdrs("_ktelf_kern_", &phdr, 1), 0u);
}

static void test_elf_loader_rejects_overflowing_pt_load(ktest_case_t *tc)
{
	Elf32_Phdr phdr = test_elf_load_phdr(0xFFFFF000u, 0x2000u);

	KTEST_EXPECT_NE(
	    tc, (uint32_t)test_load_elf_with_phdrs("_ktelf_of_", &phdr, 1), 0u);
}

static void
test_sched_add_builds_initial_frame_for_never_run_process(ktest_case_t *tc)
{
	static process_t proc;
	static uint32_t kstack_words[32];

	sched_init();
	init_frame_proc(&proc, kstack_words, 0x10003000u, 0xBFFFC000u);
	proc.arch_state.context = 0;

	int pid = sched_add(&proc);
	KTEST_ASSERT_TRUE(tc, pid >= 1);

	process_t *slot = sched_find_pid((uint32_t)pid);
	KTEST_ASSERT_NOT_NULL(tc, slot);
	KTEST_EXPECT_EQ(tc, slot->state, PROC_READY);
	KTEST_EXPECT_NE(tc, slot->arch_state.context, 0u);

	uint32_t *frame = (uint32_t *)slot->arch_state.context;
	KTEST_ASSERT_NOT_NULL(tc, frame);
	KTEST_EXPECT_EQ(tc, frame[4], (uint32_t)process_initial_launch);
	KTEST_EXPECT_EQ(tc, frame[19], 0x10003000u);
	KTEST_EXPECT_EQ(tc, frame[22], 0xBFFFC000u);
}

static void test_process_builds_linux_i386_initial_stack_shape(ktest_case_t *tc)
{
	static process_t proc;
	const char *argv[] = {"linuxprobe", 0};
	const char *envp[] = {"DRUNIX=1", 0};
	uintptr_t esp = USER_STACK_TOP;
	uint32_t *stack;
	const char *arg0;
	const char *env0;

	k_memset(&proc, 0, sizeof(proc));
	vma_init(&proc);
	proc.pd_phys = paging_create_user_space();
	KTEST_ASSERT_NE(tc, proc.pd_phys, 0u);
	KTEST_ASSERT_EQ(tc,
	                map_test_page(&proc,
	                              USER_STACK_TOP - PAGE_SIZE,
	                              PG_PRESENT | PG_WRITABLE | PG_USER),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)arch_process_build_user_stack(
	                    (arch_aspace_t)proc.pd_phys, argv, 1, envp, 1, &esp),
	                0u);
	stack = (uint32_t *)mapped_alias(&proc, (uint32_t)esp);
	KTEST_ASSERT_NOT_NULL(tc, stack);

	KTEST_EXPECT_EQ(tc, (uint32_t)esp & 3u, 0u);
	KTEST_EXPECT_EQ(tc, stack[0], 1u);
	KTEST_EXPECT_EQ(tc, stack[2], 0u);
	KTEST_EXPECT_EQ(tc, stack[4], 0u);
	KTEST_EXPECT_EQ(tc, stack[5], 6u); /* AT_PAGESZ */
	KTEST_EXPECT_EQ(tc, stack[6], PAGE_SIZE);
	KTEST_EXPECT_EQ(tc, stack[7], 0u); /* AT_NULL */
	KTEST_EXPECT_EQ(tc, stack[8], 0u);
	KTEST_EXPECT_TRUE(tc, stack[1] > (uint32_t)esp);
	KTEST_EXPECT_TRUE(tc, stack[3] > stack[1]);
	KTEST_EXPECT_TRUE(tc, stack[3] < USER_STACK_TOP);

	arg0 = (const char *)mapped_alias(&proc, stack[1]);
	env0 = (const char *)mapped_alias(&proc, stack[3]);
	KTEST_ASSERT_NOT_NULL(tc, arg0);
	KTEST_ASSERT_NOT_NULL(tc, env0);
	KTEST_EXPECT_TRUE(tc, k_strcmp(arg0, "linuxprobe") == 0);
	KTEST_EXPECT_TRUE(tc, k_strcmp(env0, "DRUNIX=1") == 0);

	process_release_user_space(&proc);
}

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
	static process_t proc;
	process_t *cur;
	uint32_t old_brk;
	uint32_t ret;

	sched_init();
	init_vma_proc(&proc);
	proc.as = 0;
	proc.arch_state.context = 1;

	KTEST_ASSERT_NE(tc, sched_add(&proc), -1);
	cur = sched_bootstrap();
	KTEST_ASSERT_NOT_NULL(tc, cur);
	old_brk = cur->brk;

	ret =
	    syscall_handler(SYS_BRK, USER_STACK_BASE + PAGE_SIZE, 0, 0, 0, 0, 0);
	KTEST_EXPECT_EQ(tc, ret, old_brk);
	KTEST_EXPECT_EQ(tc, cur->brk, old_brk);

	sched_init();
}

static void test_vma_add_keeps_regions_sorted_and_findable(ktest_case_t *tc)
{
	static process_t proc;

	k_memset(&proc, 0, sizeof(proc));
	vma_init(&proc);

	KTEST_ASSERT_EQ(tc,
	                vma_add(&proc,
	                        0xBFC00000u,
	                        USER_STACK_TOP,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE | VMA_FLAG_GROWSDOWN,
	                        VMA_KIND_STACK),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                vma_add(&proc,
	                        0x10010000u,
	                        0x10018000u,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE,
	                        VMA_KIND_HEAP),
	                0u);

	KTEST_EXPECT_EQ(tc, proc.vma_count, 2u);
	KTEST_EXPECT_EQ(tc, proc.vmas[0].kind, VMA_KIND_HEAP);
	KTEST_EXPECT_EQ(tc, proc.vmas[0].start, 0x10010000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[1].kind, VMA_KIND_STACK);
	KTEST_EXPECT_EQ(tc, proc.vmas[1].start, 0xBFC00000u);

	vm_area_t *heap = vma_find(&proc, 0x10017FFFu);
	vm_area_t *stack = vma_find(&proc, 0xBFFFF000u);

	KTEST_ASSERT_NOT_NULL(tc, heap);
	KTEST_ASSERT_NOT_NULL(tc, stack);
	KTEST_EXPECT_EQ(tc, heap->kind, VMA_KIND_HEAP);
	KTEST_EXPECT_EQ(tc, stack->kind, VMA_KIND_STACK);
	KTEST_EXPECT_NULL(tc, vma_find(&proc, 0x10100000u));
}

static void test_vma_add_rejects_overlapping_regions(ktest_case_t *tc)
{
	static process_t proc;

	k_memset(&proc, 0, sizeof(proc));
	vma_init(&proc);

	KTEST_ASSERT_EQ(tc,
	                vma_add(&proc,
	                        0x10010000u,
	                        0x10018000u,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE,
	                        VMA_KIND_HEAP),
	                0u);
	KTEST_EXPECT_EQ(tc,
	                vma_add(&proc,
	                        0x10017000u,
	                        0x1001A000u,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE,
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
	                vma_map_anonymous(&proc,
	                                  0,
	                                  0x2000u,
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
	                vma_add(&proc,
	                        0x80000000u,
	                        0x80003000u,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE,
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
	KTEST_EXPECT_EQ(
	    tc, vma_unmap_range(&proc, 0x10010000u, 0x10011000u), (uint32_t)-1);
	KTEST_EXPECT_EQ(tc, proc.vma_count, 2u);
}

static void
test_vma_protect_range_splits_and_requires_full_coverage(ktest_case_t *tc)
{
	static process_t proc;

	init_vma_proc(&proc);
	KTEST_ASSERT_EQ(tc,
	                vma_add(&proc,
	                        0x80000000u,
	                        0x80003000u,
	                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
	                            VMA_FLAG_PRIVATE,
	                        VMA_KIND_GENERIC),
	                0u);

	KTEST_ASSERT_EQ(
	    tc,
	    vma_protect_range(&proc,
	                      0x80001000u,
	                      0x80002000u,
	                      VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE),
	    0u);
	KTEST_EXPECT_EQ(tc, proc.vma_count, 5u);
	KTEST_EXPECT_EQ(tc, proc.vmas[2].start, 0x80001000u);
	KTEST_EXPECT_EQ(tc, proc.vmas[2].end, 0x80002000u);
	KTEST_EXPECT_EQ(tc,
	                proc.vmas[2].flags,
	                VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE);
	KTEST_EXPECT_EQ(
	    tc,
	    vma_protect_range(&proc,
	                      0x80003000u,
	                      0x80005000u,
	                      VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE),
	    (uint32_t)-1);
}

static void test_mem_forensics_collects_basic_region_totals(ktest_case_t *tc)
{
	static process_t proc;
	mem_forensics_report_t report;
	uint32_t stack_reserved = (uint32_t)USER_STACK_MAX_PAGES * 0x1000u;

	init_vma_proc(&proc);
	proc.image_start = 0x10000000u;
	proc.image_end = 0x10010000u;
	k_strncpy(proc.name, "shell", sizeof(proc.name) - 1);

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_EQ(tc, report.region_count, 3u);
	KTEST_EXPECT_EQ(tc, report.image_reserved_bytes, 0x00010000u);
	KTEST_EXPECT_EQ(tc, report.heap_reserved_bytes, 0x00008000u);
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
	vfs_file_ref_t ref;
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
	                (uint32_t)map_test_page(&proc,
	                                        proc.image_start,
	                                        PG_PRESENT | PG_USER | PG_WRITABLE),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)map_test_page(&proc,
	                                        USER_STACK_TOP - 0x1000u,
	                                        PG_PRESENT | PG_USER | PG_WRITABLE),
	                0u);

	KTEST_ASSERT_EQ(
	    tc,
	    (uint32_t)mem_forensics_render_vmstat(&proc,
	                                          expected_vmstat,
	                                          sizeof(expected_vmstat),
	                                          &expected_vmstat_len),
	    0u);
	KTEST_ASSERT_EQ(
	    tc,
	    (uint32_t)mem_forensics_render_fault(
	        &proc, expected_fault, sizeof(expected_fault), &expected_fault_len),
	    0u);
	KTEST_ASSERT_EQ(
	    tc,
	    (uint32_t)mem_forensics_render_maps(
	        &proc, expected_maps, sizeof(expected_maps), &expected_maps_len),
	    0u);

	KTEST_ASSERT_EQ(tc, (uint32_t)core_dump_process(&proc, SIGSEGV), 0u);
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_open_file("core.77", &ref, &size), 0u);

	n = vfs_read(ref, 0u, (uint8_t *)&ehdr, (uint32_t)sizeof(ehdr));
	KTEST_ASSERT_EQ(tc, (uint32_t)n, (uint32_t)sizeof(ehdr));
	KTEST_EXPECT_EQ(tc, ehdr.e_type, ET_CORE);

	n = vfs_read(ref, ehdr.e_phoff, (uint8_t *)&phdr, (uint32_t)sizeof(phdr));
	KTEST_ASSERT_EQ(tc, (uint32_t)n, (uint32_t)sizeof(phdr));
	KTEST_EXPECT_EQ(tc, phdr.p_type, PT_NOTE);
	KTEST_ASSERT_TRUE(tc, phdr.p_filesz < sizeof(note_buf));

	n = vfs_read(ref, phdr.p_offset, note_buf, phdr.p_filesz);
	KTEST_ASSERT_EQ(tc, (uint32_t)n, phdr.p_filesz);

	k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
	KTEST_EXPECT_EQ(tc, nhdr.n_type, NT_PRSTATUS);
	KTEST_EXPECT_EQ(tc, nhdr.n_namesz, 5u);
	KTEST_EXPECT_TRUE(
	    tc,
	    k_strcmp((const char *)(note_buf + off + sizeof(nhdr)), "CORE") == 0);
	off += (uint32_t)sizeof(nhdr) +
	       test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
	       test_align_up(nhdr.n_descsz, TEST_CORE_NOTE_ALIGN);

	k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
	KTEST_EXPECT_EQ(tc, nhdr.n_type, TEST_NT_PRPSINFO);
	KTEST_EXPECT_EQ(tc, nhdr.n_namesz, 5u);
	KTEST_EXPECT_TRUE(
	    tc,
	    k_strcmp((const char *)(note_buf + off + sizeof(nhdr)), "CORE") == 0);
	off += (uint32_t)sizeof(nhdr) +
	       test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
	       test_align_up(nhdr.n_descsz, TEST_CORE_NOTE_ALIGN);

	k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
	KTEST_EXPECT_EQ(tc, nhdr.n_type, TEST_NT_DRUNIX_VMSTAT);
	KTEST_EXPECT_EQ(tc, nhdr.n_descsz, expected_vmstat_len);
	KTEST_EXPECT_TRUE(
	    tc,
	    k_strcmp((const char *)(note_buf + off + sizeof(nhdr)), "DRUNIX") == 0);
	KTEST_EXPECT_TRUE(
	    tc,
	    k_memcmp(note_buf + off + (uint32_t)sizeof(nhdr) +
	                 test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN),
	             expected_vmstat,
	             expected_vmstat_len) == 0);
	off += (uint32_t)sizeof(nhdr) +
	       test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
	       test_align_up(nhdr.n_descsz, TEST_CORE_NOTE_ALIGN);

	k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
	KTEST_EXPECT_EQ(tc, nhdr.n_type, TEST_NT_DRUNIX_FAULT);
	KTEST_EXPECT_EQ(tc, nhdr.n_descsz, expected_fault_len);
	KTEST_EXPECT_TRUE(
	    tc,
	    k_strcmp((const char *)(note_buf + off + sizeof(nhdr)), "DRUNIX") == 0);
	KTEST_EXPECT_TRUE(
	    tc,
	    k_memcmp(note_buf + off + (uint32_t)sizeof(nhdr) +
	                 test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN),
	             expected_fault,
	             expected_fault_len) == 0);
	off += (uint32_t)sizeof(nhdr) +
	       test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
	       test_align_up(nhdr.n_descsz, TEST_CORE_NOTE_ALIGN);

	k_memcpy(&nhdr, note_buf + off, sizeof(nhdr));
	KTEST_EXPECT_EQ(tc, nhdr.n_type, TEST_NT_DRUNIX_MAPS);
	KTEST_EXPECT_EQ(tc, nhdr.n_descsz, expected_maps_len);
	KTEST_EXPECT_TRUE(
	    tc,
	    k_strcmp((const char *)(note_buf + off + sizeof(nhdr)), "DRUNIX") == 0);
	KTEST_EXPECT_TRUE(
	    tc,
	    k_memcmp(note_buf + off + (uint32_t)sizeof(nhdr) +
	                 test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN),
	             expected_maps,
	             expected_maps_len) == 0);
	off += (uint32_t)sizeof(nhdr) +
	       test_align_up(nhdr.n_namesz, TEST_CORE_NOTE_ALIGN) +
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
	uint32_t stack_reserved = (uint32_t)USER_STACK_MAX_PAGES * 0x1000u;

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
	KTEST_EXPECT_EQ(
	    tc, report.total_reserved_bytes, 0x00010000u + stack_reserved);
}

static void
test_mem_forensics_collects_full_vma_table_with_fallback_image(ktest_case_t *tc)
{
	static process_t proc;
	mem_forensics_report_t report;
	uint32_t expected_total = 0x00010000u;

	k_memset(&proc, 0, sizeof(proc));
	proc.image_start = 0x10000000u;
	proc.image_end = 0x10010000u;
	proc.brk = 0x10050000u;
	vma_init(&proc);

	for (uint32_t i = 0; i < PROCESS_MAX_VMAS; i++) {
		uint32_t start = 0x10200000u + i * 0x2000u;
		KTEST_ASSERT_EQ(tc,
		                vma_add(&proc,
		                        start,
		                        start + 0x1000u,
		                        VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_ANON |
		                            VMA_FLAG_PRIVATE,
		                        VMA_KIND_GENERIC),
		                0u);
		expected_total += 0x1000u;
	}

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_EQ(tc, report.region_count, PROCESS_MAX_VMAS + 1u);
	KTEST_EXPECT_EQ(tc, report.regions[0].kind, MEM_FORENSICS_REGION_IMAGE);
	KTEST_EXPECT_EQ(tc, report.image_reserved_bytes, 0x00010000u);
	KTEST_EXPECT_EQ(
	    tc, report.mmap_reserved_bytes, (uint32_t)PROCESS_MAX_VMAS * 0x1000u);
	KTEST_EXPECT_EQ(tc, report.total_reserved_bytes, expected_total);
}

static void test_mem_forensics_classifies_unmapped_fault(ktest_case_t *tc)
{
	static process_t proc;
	mem_forensics_report_t report;

	init_vma_proc(&proc);
	proc.crash.valid = 1;
	proc.crash.signum = SIGSEGV;
	proc.crash.fault_addr = 0xDEADBEEFu;
	proc.crash.frame.vector = 14u;
	proc.crash.frame.error_code = 0x6u;

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_EQ(tc, report.fault.valid, 1u);
	KTEST_EXPECT_EQ(
	    tc, report.fault.classification, MEM_FORENSICS_FAULT_UNMAPPED);
}

static void test_mem_forensics_classifies_heap_lazy_miss(ktest_case_t *tc)
{
	static process_t proc;
	mem_forensics_report_t report;
	uint32_t *pte = 0;

	init_vma_proc(&proc);
	proc.pd_phys = paging_create_user_space();
	KTEST_ASSERT_NE(tc, proc.pd_phys, 0u);
	KTEST_EXPECT_NE(tc, paging_walk(proc.pd_phys, proc.heap_start, &pte), 0u);

	proc.crash.valid = 1;
	proc.crash.signum = SIGSEGV;
	proc.crash.fault_addr = proc.heap_start;
	proc.crash.frame.vector = 14u;
	proc.crash.frame.error_code = 0x7u;

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_EQ(
	    tc, report.fault.classification, MEM_FORENSICS_FAULT_LAZY_MISS);

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
	KTEST_ASSERT_EQ(
	    tc,
	    paging_map_page(
	        proc.pd_phys, proc.heap_start, phys, PG_PRESENT | PG_USER),
	    0u);
	KTEST_ASSERT_EQ(tc, paging_walk(proc.pd_phys, proc.heap_start, &pte), 0u);
	*pte |= PG_COW;

	proc.crash.valid = 1;
	proc.crash.signum = SIGSEGV;
	proc.crash.fault_addr = proc.heap_start;
	proc.crash.frame.vector = 14u;
	proc.crash.frame.error_code = 0x7u;

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_EQ(
	    tc, report.fault.classification, MEM_FORENSICS_FAULT_COW_WRITE);

	process_release_user_space(&proc);
}

static void test_mem_forensics_classifies_protection_fault(ktest_case_t *tc)
{
	static process_t proc;
	mem_forensics_report_t report;

	k_memset(&proc, 0, sizeof(proc));
	vma_init(&proc);
	KTEST_ASSERT_EQ(tc,
	                vma_add(&proc,
	                        0x80000000u,
	                        0x80001000u,
	                        VMA_FLAG_READ | VMA_FLAG_ANON | VMA_FLAG_PRIVATE,
	                        VMA_KIND_GENERIC),
	                0u);

	proc.crash.valid = 1;
	proc.crash.signum = SIGSEGV;
	proc.crash.fault_addr = 0x80000000u;
	proc.crash.frame.vector = 14u;
	proc.crash.frame.error_code = 0x7u;

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_EQ(
	    tc, report.fault.classification, MEM_FORENSICS_FAULT_PROTECTION);
}

static void test_mem_forensics_classifies_unknown_fault_vector(ktest_case_t *tc)
{
	static process_t proc;
	mem_forensics_report_t report;

	init_vma_proc(&proc);
	proc.crash.valid = 1;
	proc.crash.signum = SIGSEGV;
	proc.crash.fault_addr = proc.heap_start;
	proc.crash.frame.vector = 13u;
	proc.crash.frame.error_code = 0u;

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_EQ(
	    tc, report.fault.classification, MEM_FORENSICS_FAULT_UNKNOWN);
}

static void
test_mem_forensics_preserves_high_fault_addr_as_unknown(ktest_case_t *tc)
{
	static process_t proc;
	mem_forensics_report_t report;
	char buf[256];
	uint32_t size = 0u;

	init_vma_proc(&proc);
	proc.crash.valid = 1;
	proc.crash.signum = SIGSEGV;
	proc.crash.fault_addr = 0x1234567887654321ull;
	proc.crash.frame.vector = 14u;
	proc.crash.frame.error_code = 0x6u;

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_TRUE(tc, report.fault.cr2 == proc.crash.fault_addr);
	KTEST_EXPECT_EQ(
	    tc, report.fault.classification, MEM_FORENSICS_FAULT_UNKNOWN);
	KTEST_EXPECT_EQ(tc, report.fault.in_region, 0u);

	KTEST_ASSERT_EQ(
	    tc,
	    (uint32_t)mem_forensics_render_fault(&proc, buf, sizeof(buf), &size),
	    0u);
	if (size >= sizeof(buf))
		size = sizeof(buf) - 1u;
	buf[size] = '\0';
	KTEST_EXPECT_TRUE(tc, k_strstr(buf, "CR2:\t0x1234567887654321") != 0);
}

static void test_mem_forensics_classifies_stack_limit_fault(ktest_case_t *tc)
{
	static process_t proc;
	mem_forensics_report_t report;

	init_vma_proc(&proc);
	proc.crash.valid = 1;
	proc.crash.signum = SIGSEGV;
	proc.crash.fault_addr = proc.stack_low_limit - PAGE_SIZE;
	proc.crash.frame.vector = 14u;
	proc.crash.frame.error_code = 0x6u;
	proc.crash.frame.user_esp = proc.stack_low_limit;

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_EQ(
	    tc, report.fault.classification, MEM_FORENSICS_FAULT_STACK_LIMIT);
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
	                paging_map_page(proc.pd_phys,
	                                proc.heap_start,
	                                phys,
	                                PG_PRESENT | PG_USER | PG_WRITABLE),
	                0u);

	KTEST_ASSERT_EQ(tc, mem_forensics_collect(&proc, &report), 0u);
	KTEST_EXPECT_EQ(tc, report.heap_mapped_bytes, 0x1000u);

	process_release_user_space(&proc);
}

static void test_process_resources_start_with_single_refs(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = start_syscall_test_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	KTEST_ASSERT_NOT_NULL(tc, cur->as);
	KTEST_ASSERT_NOT_NULL(tc, cur->files);
	KTEST_ASSERT_NOT_NULL(tc, cur->fs_state);
	KTEST_ASSERT_NOT_NULL(tc, cur->sig_actions);
	KTEST_EXPECT_EQ(tc, cur->as->refs, 1u);
	KTEST_EXPECT_EQ(tc, cur->files->refs, 1u);
	KTEST_EXPECT_EQ(tc, cur->fs_state->refs, 1u);
	KTEST_EXPECT_EQ(tc, cur->sig_actions->refs, 1u);

	stop_syscall_test_process(cur);
}

static void test_process_resource_get_put_tracks_refs(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = start_syscall_test_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	proc_resource_get_all(cur);
	KTEST_EXPECT_EQ(tc, cur->as->refs, 2u);
	KTEST_EXPECT_EQ(tc, cur->files->refs, 2u);
	KTEST_EXPECT_EQ(tc, cur->fs_state->refs, 2u);
	KTEST_EXPECT_EQ(tc, cur->sig_actions->refs, 2u);

	proc_resource_put_all(cur);
	KTEST_EXPECT_EQ(tc, cur->as->refs, 1u);
	KTEST_EXPECT_EQ(tc, cur->files->refs, 1u);
	KTEST_EXPECT_EQ(tc, cur->fs_state->refs, 1u);
	KTEST_EXPECT_EQ(tc, cur->sig_actions->refs, 1u);

	stop_syscall_test_process(cur);
}

static void
test_proc_resource_put_exec_owner_releases_solo_owner(ktest_case_t *tc)
{
	/*
     * Single-owner execve path: every resource struct has refs == 1, so
     * put_exec_owner() must drop them all to zero, kfree the structs, and
     * NULL the pointers. If a future refactor reintroduces the pre-abacc35
     * exec leak (silently overwriting the fields without dropping refs),
     * this test fails immediately.
     */
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

static void test_repeated_exec_owner_put_preserves_heap(ktest_case_t *tc)
{
	/*
     * Regression guard for the execve refcount leak: repeat the
     * init_fresh -> put_exec_owner cycle many times and verify the kernel
     * heap returns to at least its starting free-byte count. Before the
     * fix, each execve silently leaked four proc_*_t structs, so this loop
     * would bleed ~128 bytes per iteration and the final kheap_free_bytes()
     * would be lower than free_before.
     */
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

static void test_syscall_fstat_reads_resource_fd_table(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = start_syscall_test_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	cur->files->open_files[1].type = FD_TYPE_NONE;

	KTEST_EXPECT_EQ(tc,
	                syscall_handler(SYS_FSTAT64, 1, 0x10100400u, 0, 0, 0, 0),
	                (uint32_t)-1);

	stop_syscall_test_process(cur);
}

static void test_syscall_getcwd_reads_resource_fs_state(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = start_syscall_test_process(&proc);
	uint8_t *page;
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);

	k_strncpy(cur->fs_state->cwd, "home", sizeof(cur->fs_state->cwd) - 1u);

	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_GETCWD, 0x10100000u, 64, 0, 0, 0, 0), 6u);
	KTEST_EXPECT_EQ(tc, page[0], (uint8_t)'/');
	KTEST_EXPECT_EQ(tc, page[1], (uint8_t)'h');
	KTEST_EXPECT_EQ(tc, page[2], (uint8_t)'o');
	KTEST_EXPECT_EQ(tc, page[3], (uint8_t)'m');
	KTEST_EXPECT_EQ(tc, page[4], (uint8_t)'e');
	KTEST_EXPECT_EQ(tc, page[5], 0u);

	stop_syscall_test_process(cur);
}

static void test_syscall_chdir_updates_resource_fs_state(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur;
	uint8_t *page;

	vfs_reset();
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_register("cwdtest", &cwdtest_ops), 0u);
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_mount("/", "cwdtest"), 0u);

	cur = start_syscall_test_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);

	k_strcpy((char *)page, "bin");
	KTEST_ASSERT_EQ(
	    tc, syscall_handler(SYS_CHDIR, 0x10100000u, 0, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_TRUE(tc, k_strcmp(cur->fs_state->cwd, "bin") == 0);
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_GETCWD, 0x10100100u, 64, 0, 0, 0, 0), 5u);
	KTEST_EXPECT_TRUE(tc, k_strcmp((const char *)(page + 0x100), "/bin") == 0);

	stop_syscall_test_process(cur);
	vfs_reset();
}

static void test_syscall_brk_reads_resource_address_space(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = start_syscall_test_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	cur->as->brk = cur->as->heap_start + PAGE_SIZE;

	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_BRK, 0, 0, 0, 0, 0, 0), cur->as->brk);

	stop_syscall_test_process(cur);
}

static void test_rt_sigaction_reads_resource_handlers(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = start_syscall_test_process(&proc);
	uint8_t *page;
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);

	cur->sig_actions->handlers[SIGTERM] = SIG_IGN;

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_RT_SIGACTION, SIGTERM, 0, 0x10100000u, 8, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(tc, page[0], (uint8_t)SIG_IGN);

	stop_syscall_test_process(cur);
}

static void test_clone_rejects_sighand_without_vm(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = start_syscall_test_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	KTEST_EXPECT_EQ(tc,
	                syscall_handler(SYS_CLONE,
	                                TEST_CLONE_SIGHAND | SIGCHLD,
	                                USER_STACK_TOP - 0x1000u,
	                                0,
	                                0,
	                                0,
	                                0),
	                (uint32_t)-TEST_LINUX_EINVAL);

	stop_syscall_test_process(cur);
}

static void test_clone_rejects_thread_without_sighand(ktest_case_t *tc)
{
	static process_t proc;
	process_t *cur = start_syscall_test_process(&proc);
	KTEST_ASSERT_NOT_NULL(tc, cur);

	KTEST_EXPECT_EQ(tc,
	                syscall_handler(SYS_CLONE,
	                                TEST_CLONE_THREAD | TEST_CLONE_VM | SIGCHLD,
	                                USER_STACK_TOP - 0x1000u,
	                                0,
	                                0,
	                                0,
	                                0),
	                (uint32_t)-TEST_LINUX_EINVAL);

	stop_syscall_test_process(cur);
}

static void
test_clone_thread_shares_group_and_selected_resources(ktest_case_t *tc)
{
	static process_t seed;
	process_t *parent = start_syscall_test_process(&seed);
	uint32_t flags = TEST_CLONE_VM | TEST_CLONE_FS | TEST_CLONE_FILES |
	                 TEST_CLONE_SIGHAND | TEST_CLONE_THREAD | SIGCHLD;
	KTEST_ASSERT_NOT_NULL(tc, parent);

	uint32_t tid =
	    syscall_handler(SYS_CLONE, flags, USER_STACK_TOP - 0x1000u, 0, 0, 0, 0);
	KTEST_ASSERT_NE(tc, tid, (uint32_t)-1);

	process_t *child = sched_find_pid(tid);
	KTEST_ASSERT_NOT_NULL(tc, child);
	KTEST_EXPECT_EQ(tc, child->tgid, parent->tgid);
	KTEST_EXPECT_EQ(tc, child->group, parent->group);
	KTEST_EXPECT_EQ(tc, child->as, parent->as);
	KTEST_EXPECT_EQ(tc, child->files, parent->files);
	KTEST_EXPECT_EQ(tc, child->fs_state, parent->fs_state);
	KTEST_EXPECT_EQ(tc, child->sig_actions, parent->sig_actions);
	KTEST_EXPECT_EQ(tc, parent->as->refs, 2u);

	sched_init();
}

static void
test_clone_process_without_vm_gets_distinct_group_and_as(ktest_case_t *tc)
{
	static process_t seed;
	process_t *parent = start_syscall_test_process(&seed);
	KTEST_ASSERT_NOT_NULL(tc, parent);

	uint32_t tid = syscall_handler(
	    SYS_CLONE, SIGCHLD, USER_STACK_TOP - 0x1000u, 0, 0, 0, 0);
	KTEST_ASSERT_NE(tc, tid, (uint32_t)-1);

	process_t *child = sched_find_pid(tid);
	KTEST_ASSERT_NOT_NULL(tc, child);
	KTEST_EXPECT_EQ(tc, child->tgid, child->tid);
	KTEST_EXPECT_NE(tc, child->group, parent->group);
	KTEST_EXPECT_NE(tc, child->as, parent->as);

	sched_init();
}

static void test_linux_syscalls_fill_uname_time_and_fstat64(ktest_case_t *tc)
{
	static process_t seed;
	process_t *cur = start_syscall_test_process(&seed);
	uint8_t *page;

	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);

	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_UNAME, 0x10100000u, 0, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, page[0], (uint8_t)'D');
	KTEST_EXPECT_EQ(tc, page[1], (uint8_t)'r');
	KTEST_EXPECT_EQ(tc, page[260], (uint8_t)'i');
	KTEST_EXPECT_EQ(tc, page[261], (uint8_t)'4');

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_GETTIMEOFDAY, 0x10100200u, 0x10100210u, 0, 0, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(tc, page[0x210], 0u);
	KTEST_EXPECT_EQ(tc, page[0x211], 0u);

	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_FSTAT64, 1, 0x10100400u, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, page[0x410], 0x80u);
	KTEST_EXPECT_EQ(tc, page[0x411], 0x21u);

	page[0x300] = '\0';
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_STATX,
	                    1,
	                    0x10100300u,
	                    TEST_LINUX_AT_EMPTY_PATH | TEST_LINUX_AT_NO_AUTOMOUNT,
	                    TEST_LINUX_STATX_BASIC_STATS,
	                    0x10100500u,
	                    0),
	    0u);
	KTEST_EXPECT_EQ(tc, page[0x500], 0xffu);
	KTEST_EXPECT_EQ(tc, page[0x501], 0x07u);
	KTEST_EXPECT_EQ(tc, page[0x51c], 0x80u);
	KTEST_EXPECT_EQ(tc, page[0x51d], 0x21u);

	stop_syscall_test_process(cur);
}

static void test_linux_syscalls_cover_blockdev_fd_path(ktest_case_t *tc)
{
	static process_t seed;
	process_t *cur;
	uint8_t *page;
	int32_t fd;
	int32_t fd_part;
	int32_t sys_fd;
	uint8_t *stat_base;
	uint8_t *fstat_base;
	uint8_t *poll_base;
	uint8_t *read_base;
	uint32_t stat_mode;
	uint32_t stat_ino;
	uint32_t stat_size;
	uint32_t fstat_mode;
	uint32_t fstat_ino;
	const blkdev_ops_t *sda_ops;
	uint8_t sector1[BLKDEV_SECTOR_SIZE];
	char expected_size[32];

	vfs_reset();
	dufs_register();
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_mount("/", "dufs"), 0u);
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_mount("/dev", "devfs"), 0u);
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_mount("/sys", "sysfs"), 0u);
	(void)vfs_unlink("_ktsyslink_");
	KTEST_ASSERT_EQ(
	    tc, (uint32_t)vfs_symlink("/sys/block/sda/size", "_ktsyslink_"), 0u);
	sda_ops = blkdev_get("sda");
	KTEST_ASSERT_NOT_NULL(tc, sda_ops);
	KTEST_ASSERT_EQ(tc, (uint32_t)sda_ops->read_sector(1u, sector1), 0u);

	cur = start_syscall_test_process(&seed);
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);

	k_strcpy((char *)page, "/dev/sda");
	stat_base = page + 0x100u;
	fstat_base = page + 0x180u;
	poll_base = page + 0x200u;
	read_base = page + 0x300u;

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_STAT64, 0x10100000u, 0x10100100u, 0, 0, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(tc, stat_base[16], 0x24u);
	KTEST_EXPECT_EQ(tc, stat_base[17], 0x61u);
	stat_mode = test_read_u32_le(stat_base, 16u);
	stat_size = test_read_u32_le(stat_base, 44u);
	KTEST_EXPECT_EQ(tc, stat_mode, 0x00006124u);
	KTEST_EXPECT_TRUE(tc, stat_size != 0u);
	stat_ino = test_read_u32_le(stat_base, 88u);
	KTEST_EXPECT_EQ(tc, test_read_u64_le(stat_base, 32u), 0x800u);
	KTEST_EXPECT_EQ(tc,
	                syscall_handler(SYS_STATX,
	                                0,
	                                0x10100000u,
	                                0,
	                                TEST_LINUX_STATX_BASIC_STATS,
	                                0x10100180u,
	                                0),
	                0u);
	KTEST_EXPECT_EQ(tc, test_read_u32_le(fstat_base, 0x80u), 0x8u);
	KTEST_EXPECT_EQ(tc, test_read_u32_le(fstat_base, 0x84u), 0u);

	fd = (int32_t)syscall_handler(SYS_OPEN, 0x10100000u, 0u, 0, 0, 0, 0);
	KTEST_ASSERT_TRUE(tc, fd >= 0);
	KTEST_EXPECT_EQ(tc,
	                cur->files->open_files[(uint32_t)fd].type,
	                (uint32_t)FD_TYPE_BLOCKDEV);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_FSTAT64, (uint32_t)fd, 0x10100180u, 0, 0, 0, 0),
	    0u);
	fstat_mode = test_read_u32_le(fstat_base, 16u);
	fstat_ino = test_read_u32_le(fstat_base, 88u);
	KTEST_EXPECT_EQ(tc, fstat_mode, stat_mode);
	KTEST_EXPECT_EQ(tc, fstat_ino, stat_ino);
	KTEST_EXPECT_EQ(tc,
	                test_read_u64_le(fstat_base, 32u),
	                test_read_u64_le(stat_base, 32u));

	k_strcpy((char *)page, "/dev/sda1");
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_STAT64, 0x10100000u, 0x10100100u, 0, 0, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(tc, test_read_u64_le(stat_base, 32u), 0x801u);
	fd_part = (int32_t)syscall_handler(SYS_OPEN, 0x10100000u, 0u, 0, 0, 0, 0);
	KTEST_ASSERT_TRUE(tc, fd_part >= 0);
	KTEST_EXPECT_EQ(tc,
	                cur->files->open_files[(uint32_t)fd_part].type,
	                (uint32_t)FD_TYPE_BLOCKDEV);
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(
	        SYS_FSTAT64, (uint32_t)fd_part, 0x10100180u, 0, 0, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(tc, test_read_u64_le(fstat_base, 32u), 0x801u);

	KTEST_EXPECT_EQ(tc,
	                syscall_handler(SYS_STATX,
	                                0,
	                                0x10100000u,
	                                0,
	                                TEST_LINUX_STATX_BASIC_STATS,
	                                0x10100180u,
	                                0),
	                0u);
	KTEST_EXPECT_EQ(tc, test_read_u32_le(fstat_base, 0x80u), 0x8u);
	KTEST_EXPECT_EQ(tc, test_read_u32_le(fstat_base, 0x84u), 0x1u);

	((uint32_t *)poll_base)[0] = (uint32_t)fd;
	((uint32_t *)poll_base)[1] = TEST_LINUX_POLLIN;
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_POLL, 0x10100200u, 1u, 0, 0, 0, 0), 1u);
	KTEST_EXPECT_EQ(
	    tc, ((uint32_t *)poll_base)[1] & 0xFFFFu, TEST_LINUX_POLLIN);
	KTEST_EXPECT_TRUE(tc, (((uint32_t *)poll_base)[1] >> 16) != 0u);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_READ, (uint32_t)fd, 0x10100300u, 513u, 0, 0, 0),
	    513u);
	KTEST_EXPECT_EQ(tc, read_base[510u], 0x55u);
	KTEST_EXPECT_EQ(tc, read_base[511u], 0xAAu);
	KTEST_EXPECT_EQ(tc, read_base[512u], sector1[0]);
	KTEST_EXPECT_EQ(
	    tc, cur->files->open_files[(uint32_t)fd].u.blockdev.offset, 513u);
	read_base[0] = 0xEEu;
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_READ, (uint32_t)fd, 0x10100300u, 1u, 0, 0, 0),
	    1u);
	KTEST_EXPECT_EQ(tc, read_base[0], sector1[1]);
	KTEST_EXPECT_EQ(
	    tc, cur->files->open_files[(uint32_t)fd].u.blockdev.offset, 514u);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_WRITE, (uint32_t)fd, 0x10100100u, 1u, 0, 0, 0),
	    (uint32_t)-1);
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0, 0), 0u);

	k_strcpy((char *)page, "/sys/block/sda/size");
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_STAT64, 0x10100000u, 0x10100100u, 0, 0, 0, 0),
	    0u);
	stat_mode = test_read_u32_le(stat_base, 16u);
	KTEST_EXPECT_EQ(tc, stat_mode, 0x00008124u);
	KTEST_ASSERT_TRUE(tc,
	                  k_snprintf(expected_size,
	                             sizeof(expected_size),
	                             "%u\n",
	                             (uint32_t)DRUNIX_DISK_SECTORS) > 0);
	KTEST_EXPECT_EQ(
	    tc, test_read_u32_le(stat_base, 44u), k_strlen(expected_size));
	k_strcpy((char *)page, "/_ktsyslink_");
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_LSTAT64, 0x10100000u, 0x10100100u, 0, 0, 0, 0),
	    0u);
	stat_mode = test_read_u32_le(stat_base, 16u);
	KTEST_EXPECT_EQ(tc, stat_mode, 0x0000A1FFu);
	k_strcpy((char *)page, "/sys/block/sda/size");
	sys_fd = (int32_t)syscall_handler(SYS_OPEN, 0x10100000u, 0u, 0, 0, 0, 0);
	KTEST_ASSERT_TRUE(tc, sys_fd >= 0);
	KTEST_EXPECT_EQ(tc,
	                cur->files->open_files[(uint32_t)sys_fd].type,
	                (uint32_t)FD_TYPE_SYSFILE);
	((uint32_t *)poll_base)[0] = (uint32_t)sys_fd;
	((uint32_t *)poll_base)[1] = TEST_LINUX_POLLIN;
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_POLL, 0x10100200u, 1u, 0, 0, 0, 0), 1u);
	KTEST_EXPECT_EQ(tc,
	                syscall_handler(SYS_IOCTL,
	                                (uint32_t)sys_fd,
	                                TEST_LINUX_FIONREAD,
	                                0x10100100u,
	                                0,
	                                0,
	                                0),
	                0u);
	KTEST_EXPECT_EQ(
	    tc, test_read_u32_le(stat_base, 0u), k_strlen(expected_size));
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_LSEEK, (uint32_t)sys_fd, 2u, 0, 0, 0, 0), 2u);
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_READ, (uint32_t)sys_fd, 0x10100300u, 5u, 0, 0, 0),
	    5u);
	read_base[5] = '\0';
	KTEST_EXPECT_TRUE(tc, k_strcmp((char *)read_base, expected_size + 2u) == 0);
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_LSEEK, (uint32_t)sys_fd, 0u, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc,
	                syscall_handler(SYS_READ,
	                                (uint32_t)sys_fd,
	                                0x10100300u,
	                                k_strlen(expected_size),
	                                0,
	                                0,
	                                0),
	                k_strlen(expected_size));
	read_base[k_strlen(expected_size)] = '\0';
	KTEST_EXPECT_TRUE(tc, k_strcmp((char *)read_base, expected_size) == 0);
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_WRITE, (uint32_t)sys_fd, 0x10100100u, 1u, 0, 0, 0),
	    (uint32_t)-TEST_LINUX_EBADF);
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_FSTAT64, (uint32_t)sys_fd, 0x10100180u, 0, 0, 0, 0),
	    0u);
	fstat_mode = test_read_u32_le(fstat_base, 16u);
	KTEST_EXPECT_EQ(tc, fstat_mode, 0x00008124u);
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_CLOSE, (uint32_t)sys_fd, 0, 0, 0, 0, 0), 0u);

	stop_syscall_test_process(cur);
	(void)vfs_unlink("_ktsyslink_");
	vfs_reset();
}

static void
test_linux_syscalls_support_busybox_identity_and_rt_sigmask(ktest_case_t *tc)
{
	static process_t seed;
	process_t *cur = start_syscall_test_process(&seed);
	uint8_t *page;

	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);

	KTEST_EXPECT_EQ(tc, syscall_handler(SYS_GETUID32, 0, 0, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, syscall_handler(SYS_GETGID32, 0, 0, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, syscall_handler(SYS_GETEUID32, 0, 0, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, syscall_handler(SYS_GETEGID32, 0, 0, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, syscall_handler(SYS_SETGID32, 0, 0, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, syscall_handler(SYS_SETUID32, 0, 0, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_GETPID, 0, 0, 0, 0, 0, 0), cur->tgid);
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_GETTID, 0, 0, 0, 0, 0, 0), cur->tid);

	page[0x100] = (uint8_t)(1u << 2); /* block signal 2 */
	page[0x101] = 0;
	page[0x102] = 0;
	page[0x103] = 0;
	page[0x104] = 0xAA;
	page[0x105] = 0xAA;
	page[0x106] = 0xAA;
	page[0x107] = 0xAA;
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(
	        SYS_RT_SIGPROCMASK, 0, 0x10100100u, 0x10100200u, 8, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(tc, cur->sig_blocked, 1u << 2);
	KTEST_EXPECT_EQ(tc, page[0x200], 0u);
	KTEST_EXPECT_EQ(tc, page[0x204], 0u);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(
	        SYS_RT_SIGPROCMASK, 1, 0x10100100u, 0x10100200u, 8, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(tc, cur->sig_blocked, 0u);
	KTEST_EXPECT_EQ(tc, page[0x200], 1u << 2);

	stop_syscall_test_process(cur);
}

static void test_linux_poll_and_select_wait_for_tty_input(ktest_case_t *tc)
{
	static process_t seed;
	process_t *cur;
	uint8_t *page;
	uint8_t *poll_base;
	uint32_t *readfds;
	tty_t *tty;

	tty_init();
	tty = tty_get(0);
	KTEST_ASSERT_NOT_NULL(tc, tty);
	tty->termios.c_lflag = 0;

	cur = start_syscall_test_process(&seed);
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);
	cur->files->open_files[0].type = FD_TYPE_TTY;
	cur->files->open_files[0].writable = 0;
	cur->files->open_files[0].u.tty.tty_idx = 0;

	poll_base = page + 0x100u;
	((uint32_t *)poll_base)[0] = 0u;
	((uint32_t *)poll_base)[1] = TEST_LINUX_POLLIN;
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_POLL, 0x10100100u, 1u, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, ((uint32_t *)poll_base)[1] >> 16, 0u);

	readfds = (uint32_t *)(page + 0x200u);
	*readfds = 1u;
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS__NEWSELECT, 1u, 0x10100200u, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, *readfds, 0u);

	tty_input_char(0, 'q');
	((uint32_t *)poll_base)[1] = TEST_LINUX_POLLIN;
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_POLL, 0x10100100u, 1u, 0, 0, 0, 0), 1u);
	KTEST_EXPECT_EQ(tc, ((uint32_t *)poll_base)[1] >> 16, TEST_LINUX_POLLIN);

	stop_syscall_test_process(cur);
}

static void
test_linux_termios_on_stdout_controls_foreground_tty(ktest_case_t *tc)
{
	static process_t seed;
	process_t *cur;
	uint8_t *page;
	uint32_t *termios;
	tty_t *tty;

	tty_init();
	tty = tty_get(0);
	KTEST_ASSERT_NOT_NULL(tc, tty);
	tty->termios.c_lflag = ICANON | ECHO | ISIG;

	cur = start_syscall_test_process(&seed);
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);
	KTEST_EXPECT_EQ(tc, cur->files->open_files[1].type, FD_TYPE_TTY);
	KTEST_EXPECT_EQ(tc, cur->files->open_files[1].u.tty.tty_idx, 0u);

	termios = (uint32_t *)(page + 0x100u);
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_IOCTL, 1u, TEST_LINUX_TCGETS, 0x10100100u, 0, 0, 0),
	    0u);
	KTEST_EXPECT_TRUE(tc, (termios[0] & 0000400u) != 0u);
	KTEST_EXPECT_TRUE(tc, (termios[3] & 0000002u) != 0u);
	KTEST_EXPECT_EQ(tc, page[0x100u + 17u + 6u], 1u);
	KTEST_EXPECT_EQ(tc, page[0x100u + 17u + 0u], 0x03u);

	termios[0] &= ~0000400u;
	termios[3] &= ~0000002u;
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_IOCTL, 1u, TEST_LINUX_TCSETS, 0x10100100u, 0, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(tc, tty->termios.c_iflag & ICRNL, 0u);
	KTEST_EXPECT_EQ(tc, tty->termios.c_lflag & ICANON, 0u);

	stop_syscall_test_process(cur);
}

static void test_linux_syscalls_support_busybox_stdio_helpers(ktest_case_t *tc)
{
	static process_t seed;
	process_t *cur = start_syscall_test_process(&seed);
	uint8_t *page;
	uint32_t *u32;

	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);
	u32 = (uint32_t *)page;

	u32[0] = 0x10100100u;
	u32[1] = 3u;
	u32[2] = 0x10100110u;
	u32[3] = 2u;
	page[0x100] = (uint8_t)'o';
	page[0x101] = (uint8_t)'k';
	page[0x102] = (uint8_t)'\n';
	page[0x110] = (uint8_t)'.';
	page[0x111] = (uint8_t)'\n';
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_WRITEV, 1, 0x10100000u, 2, 0, 0, 0), 5u);
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_WRITE, 1, 0x10100100u, 3, 0, 0, 0), 3u);

	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_GETCWD, 0x10100500u, 32, 0, 0, 0, 0), 2u);
	KTEST_EXPECT_EQ(tc, page[0x500], (uint8_t)'/');
	KTEST_EXPECT_EQ(tc, page[0x501], 0u);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(
	        SYS_IOCTL, 1, TEST_LINUX_TIOCGWINSZ, 0x10100200u, 0, 0, 0),
	    0u);
	KTEST_EXPECT_TRUE(tc, page[0x200] != 0u || page[0x201] != 0u);
	KTEST_EXPECT_TRUE(tc, page[0x202] != 0u || page[0x203] != 0u);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_FCNTL64, 1, TEST_LINUX_F_GETFD, 0, 0, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_FCNTL64, 1, TEST_LINUX_F_SETFD, 1, 0, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_FCNTL64, 1, TEST_LINUX_F_GETFL, 0, 0, 0, 0),
	    TEST_LINUX_O_RDWR);
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_FCNTL64, 1, TEST_LINUX_F_DUPFD, 4, 0, 0, 0),
	    4u);
	KTEST_EXPECT_EQ(tc, cur->files->open_files[4].type, FD_TYPE_TTY);

	cur->files->open_files[3].type = FD_TYPE_FILE;
	cur->files->open_files[3].writable = 0;
	cur->files->open_files[3].u.file.inode_num = 1u;
	cur->files->open_files[3].u.file.size = 100u;
	cur->files->open_files[3].u.file.offset = 10u;

	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS__LLSEEK, 3, 0, 5, 0x10100300u, 1, 0), 0u);
	KTEST_EXPECT_EQ(tc, u32[0x300 / 4], 15u);
	KTEST_EXPECT_EQ(tc, u32[0x304 / 4], 0u);
	KTEST_EXPECT_EQ(tc, cur->files->open_files[3].u.file.offset, 15u);

	u32[0x400 / 4] = 5u;
	u32[0x404 / 4] = 0u;
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_SENDFILE64, 1, 3, 0x10100400u, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(tc, u32[0x400 / 4], 5u);

	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_GETTID, 0, 0, 0, 0, 0, 0), cur->pid);

	stop_syscall_test_process(cur);
}

static void
test_linux_open_create_append_preserves_flags_and_data(ktest_case_t *tc)
{
	static const uint8_t original[] = {'a', 'b', 'c', 'd', 'e', 'f'};
	static const uint8_t appended[] = {'a', 'b', 'c', 'd', 'e', 'f', 'Z'};
	static process_t seed;
	process_t *cur;
	uint8_t *page;
	vfs_file_ref_t ref;
	uint8_t buf[sizeof(appended)];
	uint32_t size = 0;
	uint32_t fd;
	int ino;
	int n;

	vfs_reset();
	dufs_register();
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_mount("/", "dufs"), 0u);
	(void)fs_unlink("_ktopen_");

	ino = vfs_create("_ktopen_");
	KTEST_ASSERT_TRUE(tc, ino > 0);
	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_open_file("_ktopen_", &ref, &size), 0u);
	KTEST_ASSERT_EQ(
	    tc,
	    (uint32_t)vfs_write(ref, 0, original, (uint32_t)sizeof(original)),
	    (uint32_t)sizeof(original));

	cur = start_syscall_test_process(&seed);
	KTEST_ASSERT_NOT_NULL(tc, cur);
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10100000u), 0u);
	page = mapped_alias(cur, 0x10100000u);
	KTEST_ASSERT_NOT_NULL(tc, page);

	k_strcpy((char *)page, "/_ktopen_");
	page[0x100] = (uint8_t)'X';
	page[0x200] = (uint8_t)'Z';

	fd = syscall_handler(
	    SYS_OPEN, 0x10100000u, TEST_LINUX_O_CREAT, 0644u, 0, 0, 0);
	KTEST_ASSERT_TRUE(tc, fd < MAX_FDS);
	KTEST_EXPECT_EQ(tc, cur->files->open_files[fd].writable, 0u);
	KTEST_EXPECT_EQ(
	    tc, cur->files->open_files[fd].u.file.size, (uint32_t)sizeof(original));
	KTEST_EXPECT_EQ(tc,
	                syscall_handler(SYS_WRITE, fd, 0x10100100u, 1u, 0, 0, 0),
	                (uint32_t)-TEST_LINUX_EBADF);
	KTEST_EXPECT_EQ(tc, syscall_handler(SYS_CLOSE, fd, 0, 0, 0, 0, 0), 0u);

	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_open_file("_ktopen_", &ref, &size), 0u);
	KTEST_EXPECT_EQ(tc, size, (uint32_t)sizeof(original));
	n = vfs_read(ref, 0, buf, (uint32_t)sizeof(original));
	KTEST_ASSERT_EQ(tc, (uint32_t)n, (uint32_t)sizeof(original));
	KTEST_EXPECT_TRUE(tc, k_memcmp(buf, original, sizeof(original)) == 0);

	fd = syscall_handler(SYS_OPEN,
	                     0x10100000u,
	                     TEST_LINUX_O_WRONLY | TEST_LINUX_O_APPEND,
	                     0644u,
	                     0,
	                     0,
	                     0);
	KTEST_ASSERT_TRUE(tc, fd < MAX_FDS);
	KTEST_EXPECT_EQ(tc, syscall_handler(SYS_LSEEK, fd, 0, 0, 0, 0, 0), 0u);
	KTEST_EXPECT_EQ(
	    tc, syscall_handler(SYS_WRITE, fd, 0x10100200u, 1u, 0, 0, 0), 1u);
	KTEST_EXPECT_EQ(tc, syscall_handler(SYS_CLOSE, fd, 0, 0, 0, 0, 0), 0u);

	KTEST_ASSERT_EQ(tc, (uint32_t)vfs_open_file("_ktopen_", &ref, &size), 0u);
	KTEST_EXPECT_EQ(tc, size, (uint32_t)sizeof(appended));
	n = vfs_read(ref, 0, buf, (uint32_t)sizeof(appended));
	KTEST_ASSERT_EQ(tc, (uint32_t)n, (uint32_t)sizeof(appended));
	KTEST_EXPECT_TRUE(tc, k_memcmp(buf, appended, sizeof(appended)) == 0);

	stop_syscall_test_process(cur);
	KTEST_ASSERT_EQ(tc, (uint32_t)fs_unlink("_ktopen_"), 0u);
	vfs_reset();
}

static void
test_process_restore_user_tls_switches_global_gdt_slot(ktest_case_t *tc)
{
	static process_t first;
	static process_t second;
	uint32_t base = 0;
	uint32_t limit = 0;
	int limit_in_pages = 0;
	int present = 0;

	k_memset(&first, 0, sizeof(first));
	k_memset(&second, 0, sizeof(second));

	first.arch_state.user_tls_present = 1;
	first.arch_state.user_tls_base = 0x11111000u;
	first.arch_state.user_tls_limit = 0x00000FFFu;
	first.arch_state.user_tls_limit_in_pages = 0;

	second.arch_state.user_tls_present = 1;
	second.arch_state.user_tls_base = 0x22222000u;
	second.arch_state.user_tls_limit = 0x000FFFFFu;
	second.arch_state.user_tls_limit_in_pages = 1;

	process_restore_user_tls(&first);
	gdt_get_user_tls_for_test(&base, &limit, &limit_in_pages, &present);
	KTEST_EXPECT_EQ(tc, present, 1);
	KTEST_EXPECT_EQ(tc, base, first.arch_state.user_tls_base);
	KTEST_EXPECT_EQ(tc, limit, first.arch_state.user_tls_limit);
	KTEST_EXPECT_EQ(
	    tc, limit_in_pages, first.arch_state.user_tls_limit_in_pages);

	process_restore_user_tls(&second);
	gdt_get_user_tls_for_test(&base, &limit, &limit_in_pages, &present);
	KTEST_EXPECT_EQ(tc, present, 1);
	KTEST_EXPECT_EQ(tc, base, second.arch_state.user_tls_base);
	KTEST_EXPECT_EQ(tc, limit, second.arch_state.user_tls_limit);
	KTEST_EXPECT_EQ(
	    tc, limit_in_pages, second.arch_state.user_tls_limit_in_pages);

	second.arch_state.user_tls_present = 0;
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
	KTEST_ASSERT_EQ(tc, map_test_user_page(cur, 0x10101000u), 0u);
	page = mapped_alias(cur, 0x10101000u);
	KTEST_ASSERT_NOT_NULL(tc, page);

	desc = (uint32_t *)page;
	desc[0] = 0xFFFFFFFFu;
	desc[1] = 0x10101800u;
	desc[2] = 0x000FFFFFu;
	desc[3] = (1u << 0) | (1u << 4) | (1u << 6);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_SET_THREAD_AREA, 0x10101000u, 0, 0, 0, 0, 0),
	    0u);
	KTEST_EXPECT_EQ(tc, desc[0], GDT_USER_TLS_ENTRY);
	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_SET_TID_ADDRESS, 0x10101080u, 0, 0, 0, 0, 0),
	    cur->pid);

	addr = syscall_handler(SYS_MMAP2,
	                       0,
	                       PAGE_SIZE,
	                       PROT_READ | PROT_WRITE,
	                       MAP_PRIVATE | MAP_ANONYMOUS,
	                       (uint32_t)-1,
	                       0);
	KTEST_ASSERT_NE(tc, addr, (uint32_t)-1);
	KTEST_EXPECT_TRUE(tc, vma_find(cur, addr) != 0);

	KTEST_EXPECT_EQ(
	    tc,
	    syscall_handler(SYS_MMAP2,
	                    0,
	                    PAGE_SIZE,
	                    PROT_READ | PROT_WRITE,
	                    MAP_PRIVATE | MAP_ANONYMOUS | TEST_LINUX_MAP_FIXED,
	                    (uint32_t)-1,
	                    0),
	    (uint32_t)-1);

	stop_syscall_test_process(cur);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_process_build_initial_frame_layout),
    KTEST_CASE(test_process_build_exec_frame_layout),
    KTEST_CASE(test_elf_machine_validation_is_arch_owned),
    KTEST_CASE(test_elf_loader_rejects_direct_map_pt_load),
    KTEST_CASE(test_elf_loader_rejects_overlapping_pt_loads),
    KTEST_CASE(test_elf_loader_rejects_stack_pt_load),
    KTEST_CASE(test_elf_loader_rejects_kernel_pt_load),
    KTEST_CASE(test_elf_loader_rejects_overflowing_pt_load),
    KTEST_CASE(test_sched_add_builds_initial_frame_for_never_run_process),
    KTEST_CASE(test_process_builds_linux_i386_initial_stack_shape),
    KTEST_CASE(test_x86_user_layout_invariants),
    KTEST_CASE(test_brk_refuses_stack_collision),
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
    KTEST_CASE(test_mem_forensics_classifies_heap_lazy_miss),
    KTEST_CASE(test_mem_forensics_classifies_cow_write_fault),
    KTEST_CASE(test_mem_forensics_classifies_protection_fault),
    KTEST_CASE(test_mem_forensics_classifies_unknown_fault_vector),
    KTEST_CASE(test_mem_forensics_preserves_high_fault_addr_as_unknown),
    KTEST_CASE(test_mem_forensics_classifies_stack_limit_fault),
    KTEST_CASE(test_mem_forensics_counts_present_heap_pages),
    KTEST_CASE(test_process_resources_start_with_single_refs),
    KTEST_CASE(test_process_resource_get_put_tracks_refs),
    KTEST_CASE(test_proc_resource_put_exec_owner_releases_solo_owner),
    KTEST_CASE(test_repeated_exec_owner_put_preserves_heap),
    KTEST_CASE(test_syscall_fstat_reads_resource_fd_table),
    KTEST_CASE(test_syscall_getcwd_reads_resource_fs_state),
    KTEST_CASE(test_syscall_chdir_updates_resource_fs_state),
    KTEST_CASE(test_syscall_brk_reads_resource_address_space),
    KTEST_CASE(test_rt_sigaction_reads_resource_handlers),
    KTEST_CASE(test_clone_rejects_sighand_without_vm),
    KTEST_CASE(test_clone_rejects_thread_without_sighand),
    KTEST_CASE(test_clone_thread_shares_group_and_selected_resources),
    KTEST_CASE(test_clone_process_without_vm_gets_distinct_group_and_as),
    KTEST_CASE(test_linux_syscalls_fill_uname_time_and_fstat64),
    KTEST_CASE(test_linux_syscalls_cover_blockdev_fd_path),
    KTEST_CASE(test_linux_poll_and_select_wait_for_tty_input),
    KTEST_CASE(test_linux_termios_on_stdout_controls_foreground_tty),
    KTEST_CASE(test_linux_syscalls_support_busybox_identity_and_rt_sigmask),
    KTEST_CASE(test_linux_syscalls_support_busybox_stdio_helpers),
    KTEST_CASE(test_linux_open_create_append_preserves_flags_and_data),
    KTEST_CASE(test_process_restore_user_tls_switches_global_gdt_slot),
    KTEST_CASE(test_linux_syscalls_install_tls_and_map_mmap2),
};

static ktest_suite_t suite = KTEST_SUITE("process", cases);

ktest_suite_t *ktest_suite_process(void)
{
	return &suite;
}
