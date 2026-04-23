/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch_proc.c — arm64-owned process and context stubs for the split boundary.
 */

#include "../../arch.h"
#include "../../../proc/process.h"
#include "kstring.h"
#include <stdint.h>

void arch_process_build_initial_frame(process_t *proc)
{
	(void)proc;
}

void arch_process_build_exec_frame(process_t *proc,
                                   arch_aspace_t old_aspace,
                                   uintptr_t old_kstack_bottom)
{
	(void)proc;
	(void)old_aspace;
	(void)old_kstack_bottom;
}

int arch_process_clone_frame(process_t *child_out,
                             const process_t *parent,
                             uint32_t child_stack)
{
	(void)child_out;
	(void)parent;
	(void)child_stack;
	return -1;
}

void arch_process_restore_tls(const process_t *proc)
{
	(void)proc;
}

void arch_process_launch(process_t *proc)
{
	(void)proc;
	for (;;)
		__asm__ volatile("wfi");
}

void arch_context_prepare(process_t *proc)
{
	(void)proc;
}

void arch_fpu_init_state(process_t *proc)
{
	if (!proc)
		return;
	k_memset(proc->arch_state.fpu_state, 0, sizeof(proc->arch_state.fpu_state));
}

void arch_fpu_save(process_t *proc)
{
	(void)proc;
}

void arch_context_switch(arch_context_t *old_ctx,
                         arch_context_t new_ctx,
                         arch_aspace_t new_aspace)
{
	(void)old_ctx;
	(void)new_ctx;
	arch_aspace_switch(new_aspace);
}

void arch_idle_wait(void)
{
	__asm__ volatile("wfi");
}

uintptr_t arch_current_irq_frame(void)
{
	return 0;
}

int arch_irq_frame_is_user(uintptr_t frame_ctx)
{
	(void)frame_ctx;
	return 0;
}

void arch_kstack_guard(uintptr_t addr)
{
	(void)addr;
}

void arch_kstack_unguard(uintptr_t addr)
{
	(void)addr;
}

uint32_t arch_trap_frame_ip(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

uint64_t arch_syscall_number(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

uint64_t arch_syscall_arg0(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

uint64_t arch_syscall_arg1(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

uint64_t arch_syscall_arg2(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

uint64_t arch_syscall_arg3(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

uint64_t arch_syscall_arg4(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

uint64_t arch_syscall_arg5(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

void arch_syscall_set_result(arch_trap_frame_t *frame, uint64_t value)
{
	(void)frame;
	(void)value;
}

int arch_trap_frame_is_syscall(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

uint64_t arch_trap_frame_fault_addr(const arch_trap_frame_t *frame)
{
	(void)frame;
	return 0;
}

void arch_core_fill_prstatus_regs(uint32_t *gregs, const arch_trap_frame_t *frame)
{
	(void)frame;
	if (gregs)
		k_memset(gregs, 0, sizeof(uint32_t) * 17u);
}

void arch_trap_frame_sanitize(process_t *proc, arch_trap_frame_t *frame)
{
	(void)proc;
	(void)frame;
}

int arch_signal_setup_frame(process_t *proc,
                            arch_trap_frame_t *frame,
                            int signum,
                            uint32_t handler_va)
{
	(void)proc;
	(void)frame;
	(void)signum;
	(void)handler_va;
	return -1;
}

uint32_t arch_user_tls_entry(void)
{
	return 0;
}
