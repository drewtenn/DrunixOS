/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch_proc.c — x86-owned process, trap, and context operations.
 */

#include "../../arch.h"
#include "../../../proc/process.h"
#include "../../../proc/elf.h"
#include "../../../proc/uaccess.h"
#include "../gdt.h"
#include "../mm/paging.h"
#include "../sse.h"
#include "kstring.h"
#include "pmm.h"
#include <stdint.h>

#define SYS_SIGRETURN 119u
#define LINUX_AT_NULL 0u
#define LINUX_AT_PAGESZ 6u

extern void process_enter_usermode(uint32_t entry,
                                   uint32_t user_esp,
                                   uint32_t user_cs,
                                   uint32_t user_ds);
extern void process_initial_launch(void);
extern void process_exec_launch(void);
extern void
switch_context(uint32_t *old_esp_ptr, uint32_t new_esp, uint32_t new_cr3);
extern uint32_t g_irq_frame_esp;

static uint32_t *arch_x86_build_launch_isr_frame(uint32_t *ksp,
                                                 const process_t *proc)
{
	/* CPU iret frame — ring-3 target. */
	*--ksp = GDT_USER_DS;
	*--ksp = proc->user_stack;
	*--ksp = 0x202;
	*--ksp = GDT_USER_CS;
	*--ksp = proc->entry;

	/* IRQ stub words. */
	*--ksp = 0;
	*--ksp = 0;

	/* pusha frame (high -> low: EAX first, EDI last). */
	*--ksp = 0; /* EAX */
	*--ksp = 0; /* ECX */
	*--ksp = 0; /* EDX */
	*--ksp = 0; /* EBX */
	*--ksp = 0; /* ESP_saved */
	*--ksp = 0; /* EBP */
	*--ksp = 0; /* ESI */
	*--ksp = 0; /* EDI */

	/* Segment registers (high -> low: DS first, GS last). */
	*--ksp = GDT_USER_DS;
	*--ksp = GDT_USER_DS;
	*--ksp = GDT_USER_DS;
	*--ksp = GDT_USER_DS;
	return ksp;
}

static uint32_t arch_x86_elf_segment_flags(uint32_t p_flags)
{
	uint32_t flags = ARCH_MM_MAP_PRESENT | ARCH_MM_MAP_USER |
	                 ARCH_MM_MAP_WRITE;

	if (p_flags & PF_R)
		flags |= ARCH_MM_MAP_READ;
	if (p_flags & PF_X)
		flags |= ARCH_MM_MAP_EXEC;
	if ((flags & (ARCH_MM_MAP_READ | ARCH_MM_MAP_EXEC)) == 0)
		flags |= ARCH_MM_MAP_READ;

	return flags;
}

int arch_process_build_user_stack(arch_aspace_t aspace,
                                  const char *const *argv,
                                  int argc,
                                  const char *const *envp,
                                  int envc,
                                  uintptr_t *stack_out)
{
	uint32_t argv_strbytes = 0;
	uint32_t env_strbytes = 0;
	uint32_t strings_off;
	uint32_t pad;
	uint32_t aux_words = 4u;
	uint32_t stack_words;
	uint32_t frame_off;
	uint32_t top_vpage = USER_STACK_TOP - 0x1000u;
	arch_mm_mapping_t mapping;
	uint8_t *page;
	uint8_t *page_end;
	uint8_t *strbase_k;
	uint32_t strbase_u;
	uint32_t uargv_ptrs[PROCESS_ARGV_MAX_COUNT];
	uint32_t uenv_ptrs[PROCESS_ENV_MAX_COUNT];
	uint32_t write_cursor = 0;
	uint32_t *tail_k;
	uint32_t idx = 0;

	if (!stack_out)
		return -1;
	if (argc < 0 || argv == 0)
		argc = 0;
	if (envc < 0 || envp == 0)
		envc = 0;
	if ((uint32_t)argc > PROCESS_ARGV_MAX_COUNT)
		return -1;
	if ((uint32_t)envc > PROCESS_ENV_MAX_COUNT)
		return -1;

	for (int i = 0; i < argc; i++) {
		const char *s = argv[i];
		uint32_t n = 0;

		if (!s)
			return -1;
		while (s[n])
			n++;
		argv_strbytes += n + 1u;
		if (argv_strbytes > PROCESS_ARGV_MAX_BYTES)
			return -1;
	}
	for (int i = 0; i < envc; i++) {
		const char *s = envp[i];
		uint32_t n = 0;

		if (!s)
			return -1;
		while (s[n])
			n++;
		env_strbytes += n + 1u;
		if (env_strbytes > PROCESS_ENV_MAX_BYTES)
			return -1;
	}

	strings_off = argv_strbytes + env_strbytes;
	pad = (4u - (strings_off & 3u)) & 3u;
	stack_words =
	    1u + (uint32_t)argc + 1u + (uint32_t)envc + 1u + aux_words;
	frame_off = strings_off + pad + stack_words * 4u;
	if (frame_off > PAGE_SIZE)
		return -1;

	if (arch_mm_query(aspace, top_vpage, &mapping) != 0)
		return -1;
	page = (uint8_t *)arch_page_temp_map(mapping.phys_addr);
	if (!page)
		return -1;
	page_end = page + PAGE_SIZE;
	strbase_k = page_end - strings_off;
	strbase_u = USER_STACK_TOP - strings_off;

	for (int i = 0; i < argc; i++) {
		const char *s = argv[i];
		uint32_t j = 0;

		while (s[j]) {
			strbase_k[write_cursor + j] = (uint8_t)s[j];
			j++;
		}
		strbase_k[write_cursor + j] = 0;
		uargv_ptrs[i] = strbase_u + write_cursor;
		write_cursor += j + 1u;
	}
	for (int i = 0; i < envc; i++) {
		const char *s = envp[i];
		uint32_t j = 0;

		while (s[j]) {
			strbase_k[write_cursor + j] = (uint8_t)s[j];
			j++;
		}
		strbase_k[write_cursor + j] = 0;
		uenv_ptrs[i] = strbase_u + write_cursor;
		write_cursor += j + 1u;
	}

	tail_k = (uint32_t *)(page_end - frame_off);
	tail_k[idx++] = (uint32_t)argc;
	for (int i = 0; i < argc; i++)
		tail_k[idx++] = uargv_ptrs[i];
	tail_k[idx++] = 0;
	for (int i = 0; i < envc; i++)
		tail_k[idx++] = uenv_ptrs[i];
	tail_k[idx++] = 0;
	tail_k[idx++] = LINUX_AT_PAGESZ;
	tail_k[idx++] = PAGE_SIZE;
	tail_k[idx++] = LINUX_AT_NULL;
	tail_k[idx++] = 0;

	*stack_out = USER_STACK_TOP - frame_off;
	arch_page_temp_unmap(page);
	return 0;
}

int arch_elf_machine_supported(elf_class_t elf_class, uint16_t machine)
{
	return elf_class == ELF_CLASS_32 && machine == EM_386;
}

int arch_elf_load_user_image(vfs_file_ref_t file_ref,
                             arch_aspace_t aspace,
                             uintptr_t *entry_out,
                             uintptr_t *image_start_out,
                             uintptr_t *heap_start_out)
{
	Elf32_Ehdr ehdr;
	uint32_t min_vaddr = 0xFFFFFFFFu;
	uint32_t max_vend = 0u;
	int loaded_segment = 0;

	if (vfs_read(file_ref, 0, (uint8_t *)&ehdr, sizeof(ehdr)) !=
	    (int)sizeof(ehdr))
		return -1;

	if (*(uint32_t *)ehdr.e_ident != ELF_MAGIC)
		return -2;
	if (ehdr.e_type != ET_EXEC)
		return -3;
	if (!arch_elf_machine_supported((elf_class_t)ehdr.e_ident[EI_CLASS],
	                                ehdr.e_machine))
		return -4;
	if (ehdr.e_phnum == 0)
		return -5;

	*entry_out = ehdr.e_entry;

	for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
		Elf32_Phdr phdr;
		uint32_t phdr_off = ehdr.e_phoff + (uint32_t)i * ehdr.e_phentsize;
		uint32_t seg_flags;

		if (vfs_read(file_ref, phdr_off, (uint8_t *)&phdr, sizeof(phdr)) !=
		    (int)sizeof(phdr))
			return -6;
		if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0)
			continue;
		if (phdr.p_filesz > phdr.p_memsz)
			return -6;

		{
			uint32_t vaddr = phdr.p_vaddr & ~0xFFFu;
			uint32_t vend =
			    (phdr.p_vaddr + phdr.p_memsz + 0xFFFu) & ~0xFFFu;
			uint32_t npages = (vend - vaddr) / 0x1000u;

			if (vaddr < min_vaddr)
				min_vaddr = vaddr;
			if (vend > max_vend)
				max_vend = vend;
			loaded_segment = 1;

			seg_flags = arch_x86_elf_segment_flags(phdr.p_flags);
			for (uint32_t p = 0; p < npages; p++) {
				uint32_t phys = pmm_alloc_page();
				uint32_t vpage = vaddr + p * 0x1000u;
				void *page;

				if (!phys)
					return -7;
				if (arch_mm_map(aspace, vpage, phys, seg_flags) != 0) {
					pmm_free_page(phys);
					return -8;
				}

				page = arch_page_temp_map(phys);
				if (!page) {
					(void)arch_mm_unmap(aspace, vpage);
					pmm_free_page(phys);
					return -8;
				}
				k_memset(page, 0, 0x1000u);
				arch_page_temp_unmap(page);
			}

			{
				uint32_t file_remain = phdr.p_filesz;
				uint32_t file_done = 0u;
				uint32_t first_off = phdr.p_vaddr & 0xFFFu;

				for (uint32_t p = 0; p < npages && file_remain > 0; p++) {
					arch_mm_mapping_t mapping;
					uint32_t vpage = vaddr + p * 0x1000u;
					uint32_t write_off = (p == 0) ? first_off : 0u;
					uint32_t space = 0x1000u - write_off;
					uint32_t to_copy =
					    (file_remain < space) ? file_remain : space;
					void *page;

					if (arch_mm_query(aspace, vpage, &mapping) != 0)
						return -9;
					page = arch_page_temp_map(mapping.phys_addr);
					if (!page)
						return -9;
					if (vfs_read(file_ref,
					             phdr.p_offset + file_done,
					             (uint8_t *)page + write_off,
					             to_copy) != (int)to_copy) {
						arch_page_temp_unmap(page);
						return -9;
					}
					arch_page_temp_unmap(page);
					file_done += to_copy;
					file_remain -= to_copy;
				}
			}
		}
	}

	if (!loaded_segment)
		return -5;
	*image_start_out = (min_vaddr == 0xFFFFFFFFu) ? 0u : min_vaddr;
	*heap_start_out = max_vend;
	return 0;
}

void arch_process_build_initial_frame(process_t *proc)
{
	uint32_t *ksp;

	if (!proc)
		return;

	ksp = arch_x86_build_launch_isr_frame((uint32_t *)proc->kstack_top, proc);
	*--ksp = (uint32_t)process_initial_launch;
	*--ksp = 0;
	*--ksp = 0;
	*--ksp = 0;
	*--ksp = 0;
	proc->arch_state.context = (uintptr_t)ksp;
}

void arch_process_build_exec_frame(process_t *proc,
                                   arch_aspace_t old_aspace,
                                   uintptr_t old_kstack_bottom)
{
	uint32_t *ksp;

	if (!proc)
		return;

	ksp = arch_x86_build_launch_isr_frame((uint32_t *)proc->kstack_top, proc);
	*--ksp = (uint32_t)old_kstack_bottom;
	*--ksp = (uint32_t)old_aspace;
	*--ksp = (uint32_t)process_exec_launch;
	*--ksp = 0;
	*--ksp = 0;
	*--ksp = 0;
	*--ksp = 0;
	proc->arch_state.context = (uintptr_t)ksp;
}

int arch_process_clone_frame(process_t *child_out,
                             const process_t *parent,
                             uint32_t child_stack)
{
	uint32_t parent_frame;
	uint32_t child_frame;
	uint32_t *ksp;

	if (!child_out || !parent)
		return -1;

	if (!parent->kstack_top) {
		if (child_stack)
			child_out->user_stack = child_stack;
		arch_process_build_initial_frame(child_out);
		return 0;
	}

	parent_frame = parent->kstack_top - 76u;
	child_frame = child_out->kstack_top - 76u;
	k_memcpy((void *)child_frame, (void *)parent_frame, 76);
	((uint32_t *)child_frame)[11] = 0;
	if (child_stack)
		((uint32_t *)child_frame)[17] = child_stack;

	ksp = (uint32_t *)child_frame;
	*--ksp = (uint32_t)process_initial_launch;
	*--ksp = 0;
	*--ksp = 0;
	*--ksp = 0;
	*--ksp = 0;
	child_out->arch_state.context = (uintptr_t)ksp;
	return 0;
}

void arch_process_restore_tls(const process_t *proc)
{
	if (proc && proc->arch_state.user_tls_present) {
		gdt_set_user_tls(proc->arch_state.user_tls_base,
		                 proc->arch_state.user_tls_limit,
		                 (int)proc->arch_state.user_tls_limit_in_pages);
	} else {
		gdt_clear_user_tls();
	}
}

void arch_process_launch(process_t *proc)
{
	gdt_set_tss_esp0(proc->kstack_top);
	arch_process_restore_tls(proc);
	arch_aspace_switch((arch_aspace_t)proc->pd_phys);
	__asm__ volatile("fxrstor %0" ::"m"(*proc->arch_state.fpu_state));
	process_enter_usermode(
	    proc->entry, proc->user_stack, GDT_USER_CS, GDT_USER_DS);
	for (;;)
		__asm__ volatile("hlt");
}

void arch_context_prepare(process_t *proc)
{
	if (!proc)
		return;

	gdt_set_tss_esp0(proc->kstack_top);
	arch_process_restore_tls(proc);
	__asm__ volatile("fxrstor %0" ::"m"(*proc->arch_state.fpu_state));
}

void arch_fpu_init_state(process_t *proc)
{
	if (!proc)
		return;
	k_memcpy(proc->arch_state.fpu_state, sse_initial_fpu_state, 512u);
}

void arch_fpu_save(process_t *proc)
{
	if (!proc)
		return;
	__asm__ volatile("fxsave %0" : "=m"(*proc->arch_state.fpu_state));
}

void arch_context_switch(arch_context_t *old_ctx,
                         arch_context_t new_ctx,
                         arch_aspace_t new_aspace)
{
	switch_context((uint32_t *)old_ctx, (uint32_t)new_ctx, (uint32_t)new_aspace);
}

void arch_idle_wait(void)
{
	__asm__ volatile("sti; hlt; cli");
}

uintptr_t arch_current_irq_frame(void)
{
	return (uintptr_t)g_irq_frame_esp;
}

int arch_irq_frame_is_user(uintptr_t frame_ctx)
{
	if (frame_ctx == 0)
		return 0;
	return ((*(uint32_t *)(frame_ctx + 60u)) & 3u) == 3u;
}

void arch_kstack_guard(uintptr_t addr)
{
	paging_guard_page((uint32_t)addr);
}

void arch_kstack_unguard(uintptr_t addr)
{
	paging_unguard_page((uint32_t)addr);
}

uint32_t arch_trap_frame_ip(const arch_trap_frame_t *frame)
{
	return frame ? frame->eip : 0u;
}

uint64_t arch_syscall_number(const arch_trap_frame_t *frame)
{
	return frame ? frame->eax : 0u;
}

uint64_t arch_syscall_arg0(const arch_trap_frame_t *frame)
{
	return frame ? frame->ebx : 0u;
}

uint64_t arch_syscall_arg1(const arch_trap_frame_t *frame)
{
	return frame ? frame->ecx : 0u;
}

uint64_t arch_syscall_arg2(const arch_trap_frame_t *frame)
{
	return frame ? frame->edx : 0u;
}

uint64_t arch_syscall_arg3(const arch_trap_frame_t *frame)
{
	return frame ? frame->esi : 0u;
}

uint64_t arch_syscall_arg4(const arch_trap_frame_t *frame)
{
	return frame ? frame->edi : 0u;
}

uint64_t arch_syscall_arg5(const arch_trap_frame_t *frame)
{
	return frame ? frame->ebp : 0u;
}

void arch_syscall_set_result(arch_trap_frame_t *frame, uint64_t value)
{
	if (frame)
		frame->eax = (uint32_t)value;
}

int arch_trap_frame_is_syscall(const arch_trap_frame_t *frame)
{
	return frame && frame->vector == 0x80u;
}

uint64_t arch_trap_frame_fault_addr(const arch_trap_frame_t *frame)
{
	uint32_t cr2 = 0u;

	if (!frame || frame->vector != 14u)
		return 0u;

	/* x86 keeps the page-fault address in CR2 rather than in the trap frame. */
	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
	return cr2;
}

void arch_core_fill_prstatus_regs(uint32_t *gregs, const arch_trap_frame_t *frame)
{
	if (!gregs || !frame)
		return;

	gregs[0] = frame->ebx;
	gregs[1] = frame->ecx;
	gregs[2] = frame->edx;
	gregs[3] = frame->esi;
	gregs[4] = frame->edi;
	gregs[5] = frame->ebp;
	gregs[6] = frame->eax;
	gregs[7] = frame->ds;
	gregs[8] = frame->es;
	gregs[9] = frame->fs;
	gregs[10] = frame->gs;
	gregs[11] = 0xFFFFFFFFu;
	gregs[12] = frame->eip;
	gregs[13] = frame->cs;
	gregs[14] = frame->eflags;
	gregs[15] = frame->user_esp;
	gregs[16] = frame->user_ss;
}

void arch_trap_frame_sanitize(process_t *proc, arch_trap_frame_t *frame)
{
	if (!proc || !frame || (frame->cs & 3u) != 3u)
		return;

	if (!proc->arch_state.user_tls_present &&
	    (frame->gs & 0xFFFFu) == GDT_USER_TLS_SEG)
		frame->gs = 0;
}

int arch_signal_setup_frame(process_t *proc,
                            arch_trap_frame_t *frame,
                            int signum,
                            uint32_t handler_va)
{
	struct __attribute__((packed)) {
		uint32_t ret_addr;
		uint32_t signum;
		uint32_t saved_eip;
		uint32_t saved_eflags;
		uint32_t saved_eax;
		uint32_t saved_esp;
		uint8_t tramp[8];
	} sf;
	uint32_t new_esp;
	uint32_t trampoline_va;

	if (!proc || !frame)
		return -1;

	new_esp = frame->user_esp - 32u;
	trampoline_va = new_esp + 24u;
	sf.ret_addr = trampoline_va;
	sf.signum = (uint32_t)signum;
	sf.saved_eip = frame->eip;
	sf.saved_eflags = frame->eflags;
	sf.saved_eax = frame->eax;
	sf.saved_esp = frame->user_esp;
	sf.tramp[0] = 0xB8;
	sf.tramp[1] = (uint8_t)(SYS_SIGRETURN & 0xFFu);
	sf.tramp[2] = (uint8_t)((SYS_SIGRETURN >> 8) & 0xFFu);
	sf.tramp[3] = (uint8_t)((SYS_SIGRETURN >> 16) & 0xFFu);
	sf.tramp[4] = (uint8_t)((SYS_SIGRETURN >> 24) & 0xFFu);
	sf.tramp[5] = 0xCD;
	sf.tramp[6] = 0x80;
	sf.tramp[7] = 0x90;

	if (uaccess_copy_to_user(proc, new_esp, &sf, sizeof(sf)) != 0)
		return -1;

	frame->eip = handler_va;
	frame->user_esp = new_esp;
	return 0;
}

uint32_t arch_user_tls_entry(void)
{
	return GDT_USER_TLS_ENTRY;
}
