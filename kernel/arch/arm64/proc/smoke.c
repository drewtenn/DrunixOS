/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../../arch.h"
#include "../../../proc/elf.h"
#include "../../../proc/process.h"
#include "../../../proc/sched.h"
#include "../mm/pmm.h"
#include "elf64.h"
#include "kprintf.h"
#include "kstring.h"
#include <stdint.h>

#define ARM64_SMOKE_LOAD_BASE 0x00200000u
#define ARM64_SMOKE_STACK_BASE 0x002fc000u
#define ARM64_SMOKE_STACK_TOP 0x00300000u
#define ARM64_SMOKE_STACK_SIZE 0x4000u
#define ARM64_SMOKE_RAM_LIMIT 0x3f000000u
#define ARM64_LINUX_SYS_WRITE 64u
#define ARM64_LINUX_SYS_EXIT 93u
#define ARM64_LINUX_SYS_EXIT_GROUP 94u

extern const uint8_t arm64_smoke_elf_start[];
extern const uint8_t arm64_smoke_elf_end[];
extern void arm64_console_loop(void);
extern uint32_t __real_syscall_case_exit_exit_group(uint32_t exit_group,
                                                    uint32_t status);

static process_t g_arm64_smoke_proc;
static uint8_t g_arm64_smoke_kstack[KSTACK_SIZE] __attribute__((aligned(16)));

static int
arm64_smoke_copy_to_ram(uintptr_t dst, const uint8_t *src, uint32_t len)
{
	if (!src)
		return -1;
	k_memcpy((void *)dst, src, len);
	return 0;
}

static int arm64_smoke_load_image(uintptr_t *entry_out,
                                  uintptr_t *image_end_out)
{
	const uint8_t *image = arm64_smoke_elf_start;
	uint32_t image_size = (uint32_t)((uintptr_t)arm64_smoke_elf_end -
	                                 (uintptr_t)arm64_smoke_elf_start);
	const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)image;
	uint64_t high_water = 0u;

	if (image_size < sizeof(*ehdr))
		return -1;
	if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
	    ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F')
		return -1;
	if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_AARCH64 ||
	    ehdr->e_ident[EI_CLASS] != ELF_CLASS_64)
		return -1;

	for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
		const Elf64_Phdr *phdr;
		uint64_t seg_end;
		uint64_t seg_page_end;

		if (ehdr->e_phoff + (uint64_t)(i + 1) * ehdr->e_phentsize > image_size)
			return -1;
		phdr = (const Elf64_Phdr *)(image + ehdr->e_phoff +
		                            (uint64_t)i * ehdr->e_phentsize);
		if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
			continue;
		if (phdr->p_filesz > phdr->p_memsz)
			return -1;
		seg_end = phdr->p_vaddr + phdr->p_memsz;
		if (phdr->p_vaddr < ARM64_SMOKE_LOAD_BASE || seg_end < phdr->p_vaddr ||
		    seg_end > ARM64_SMOKE_STACK_BASE || seg_end > ARM64_SMOKE_RAM_LIMIT)
			return -1;
		if (phdr->p_offset + phdr->p_filesz > image_size)
			return -1;

		k_memset((void *)(uintptr_t)phdr->p_vaddr, 0, (uint32_t)phdr->p_memsz);
		if (arm64_smoke_copy_to_ram((uintptr_t)phdr->p_vaddr,
		                            image + phdr->p_offset,
		                            (uint32_t)phdr->p_filesz) != 0)
			return -1;

		seg_page_end = (seg_end + 0xfffull) & ~0xfffull;
		if (seg_page_end > high_water)
			high_water = seg_page_end;
	}

	if (!entry_out || !image_end_out || high_water <= ARM64_SMOKE_LOAD_BASE ||
	    ehdr->e_entry < ARM64_SMOKE_LOAD_BASE || ehdr->e_entry >= high_water)
		return -1;
	k_memset(
	    (void *)(uintptr_t)ARM64_SMOKE_STACK_BASE, 0, ARM64_SMOKE_STACK_SIZE);
	pmm_mark_used(ARM64_SMOKE_LOAD_BASE,
	              (uint32_t)(high_water - ARM64_SMOKE_LOAD_BASE));
	pmm_mark_used(ARM64_SMOKE_STACK_BASE, ARM64_SMOKE_STACK_SIZE);
	*entry_out = (uintptr_t)ehdr->e_entry;
	*image_end_out = (uintptr_t)high_water;
	return 0;
}

static int arm64_smoke_map_range(arch_aspace_t aspace,
                                 uintptr_t start,
                                 uintptr_t end,
                                 uint32_t flags)
{
	for (uintptr_t page = start; page < end; page += PAGE_SIZE) {
		if (arch_mm_map(aspace, page, page, flags) != 0)
			return -1;
	}
	return 0;
}

static void arm64_smoke_write_bytes(const char *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++)
		arch_console_write(buf + i, 1u);
}

void arm64_report_init_exit(uint32_t status)
{
	char line[64];

	k_snprintf(
	    line, sizeof(line), "ARM64 init exited with status %u\n", status);
	arm64_smoke_write_bytes(line, (uint32_t)k_strlen(line));
}

uint32_t __wrap_syscall_case_exit_exit_group(uint32_t exit_group,
                                             uint32_t status)
{
	if (sched_current_pid() == 1u) {
		if (exit_group)
			sched_mark_group_exit(status);
		else {
			sched_set_exit_status(status);
			sched_mark_exit();
		}
		arm64_report_init_exit(status);
		arm64_console_loop();
	}
	return __real_syscall_case_exit_exit_group(exit_group, status);
}

uint64_t arm64_userspace_syscall_dispatch(arch_trap_frame_t *frame)
{
	uint64_t nr = arch_syscall_number(frame);

	if (!frame)
		return (uint64_t)-1;

	if (nr == ARM64_LINUX_SYS_WRITE) {
		uint64_t fd = frame->x[0];
		const char *buf = (const char *)(uintptr_t)frame->x[1];
		uint32_t len = (uint32_t)frame->x[2];

		if (fd != 1u && fd != 2u) {
			frame->x[0] = (uint64_t)-1;
			return frame->x[0];
		}
		arm64_smoke_write_bytes(buf, len);
		frame->x[0] = len;
		return len;
	}

	if (nr == ARM64_LINUX_SYS_EXIT || nr == ARM64_LINUX_SYS_EXIT_GROUP) {
		frame->elr_el1 = (uintptr_t)arm64_console_loop;
		frame->spsr_el1 = 0x5u;
		frame->x[0] = 0u;
		return 0u;
	}

	frame->x[0] = (uint64_t)-38;
	return frame->x[0];
}

int arm64_user_smoke_boot(void)
{
	uintptr_t entry = 0u;
	uintptr_t image_end = 0u;
	arch_aspace_t aspace;

	k_memset(&g_arm64_smoke_proc, 0, sizeof(g_arm64_smoke_proc));
	k_memset(g_arm64_smoke_kstack, 0, sizeof(g_arm64_smoke_kstack));
	if (arm64_smoke_load_image(&entry, &image_end) != 0)
		return -1;

	aspace = arch_aspace_create();
	if (!aspace)
		return -1;
	if (arm64_smoke_map_range(aspace,
	                          ARM64_SMOKE_LOAD_BASE,
	                          image_end,
	                          ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
	                              ARCH_MM_MAP_WRITE | ARCH_MM_MAP_EXEC |
	                              ARCH_MM_MAP_USER) != 0) {
		arch_aspace_destroy(aspace);
		return -1;
	}
	if (arm64_smoke_map_range(aspace,
	                          ARM64_SMOKE_STACK_BASE,
	                          ARM64_SMOKE_STACK_TOP,
	                          ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_READ |
	                              ARCH_MM_MAP_WRITE | ARCH_MM_MAP_USER) != 0) {
		arch_aspace_destroy(aspace);
		return -1;
	}

	g_arm64_smoke_proc.pd_phys = (uint32_t)aspace;
	g_arm64_smoke_proc.entry = (uint32_t)entry;
	g_arm64_smoke_proc.user_stack = ARM64_SMOKE_STACK_TOP;
	g_arm64_smoke_proc.kstack_bottom =
	    (uint32_t)(uintptr_t)g_arm64_smoke_kstack;
	g_arm64_smoke_proc.kstack_top =
	    (uint32_t)(uintptr_t)(g_arm64_smoke_kstack +
	                          sizeof(g_arm64_smoke_kstack));
	arch_fpu_init_state(&g_arm64_smoke_proc);
	arch_process_build_initial_frame(&g_arm64_smoke_proc);
	arch_process_launch(&g_arm64_smoke_proc);
	return 0;
}
