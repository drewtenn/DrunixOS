/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch_proc.c — arm64-owned process and context stubs for the split boundary.
 */

#include "../../arch.h"
#include "../../../proc/process.h"
#include "kstring.h"
#include <stdint.h>

typedef struct arm64_kernel_context {
	uint64_t x19;
	uint64_t x20;
	uint64_t x21;
	uint64_t x22;
	uint64_t x23;
	uint64_t x24;
	uint64_t x25;
	uint64_t x26;
	uint64_t x27;
	uint64_t x28;
	uint64_t x29;
	uint64_t x30;
} arm64_kernel_context_t;

extern void arm64_enter_user_frame(arch_trap_frame_t *frame);
extern void
arm64_switch_context(arch_context_t *old_ctx, arch_context_t new_ctx);
extern void arm64_process_first_resume(void);

uintptr_t g_arm64_exception_frame;

static arch_trap_frame_t *arm64_process_user_frame(process_t *proc)
{
	uintptr_t top;

	if (!proc || !proc->kstack_top)
		return 0;

	top = (uintptr_t)proc->kstack_top - sizeof(arch_trap_frame_t);
	return (arch_trap_frame_t *)top;
}

static arm64_kernel_context_t *arm64_process_kernel_context(process_t *proc)
{
	uintptr_t top;

	if (!proc || !proc->kstack_top)
		return 0;

	top = (uintptr_t)proc->kstack_top - sizeof(arch_trap_frame_t) -
	      sizeof(arm64_kernel_context_t);
	return (arm64_kernel_context_t *)top;
}

static void arm64_build_launch_context(process_t *proc)
{
	arch_trap_frame_t *frame;
	arm64_kernel_context_t *ctx;

	if (!proc)
		return;

	frame = arm64_process_user_frame(proc);
	ctx = arm64_process_kernel_context(proc);
	if (!frame || !ctx)
		return;

	k_memset(frame, 0, sizeof(*frame));
	frame->sp_el0 = (uint64_t)proc->user_stack;
	frame->elr_el1 = (uint64_t)proc->entry;
	frame->spsr_el1 = 0u;

	k_memset(ctx, 0, sizeof(*ctx));
	ctx->x19 = (uint64_t)(uintptr_t)frame;
	ctx->x30 = (uint64_t)(uintptr_t)arm64_process_first_resume;
	proc->arch_state.context = (uintptr_t)ctx;
}

void arch_process_build_initial_frame(process_t *proc)
{
	arm64_build_launch_context(proc);
}

void arch_process_build_exec_frame(process_t *proc,
                                   arch_aspace_t old_aspace,
                                   uintptr_t old_kstack_bottom)
{
	(void)old_aspace;
	(void)old_kstack_bottom;
	arm64_build_launch_context(proc);
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
	if (!proc)
		return;

	arch_context_prepare(proc);
	arch_context_switch((arch_context_t *)0,
	                    (arch_context_t)proc->arch_state.context,
	                    (arch_aspace_t)proc->pd_phys);
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
	arch_aspace_switch(new_aspace);
	arm64_switch_context(old_ctx, new_ctx);
}

void arch_idle_wait(void)
{
	__asm__ volatile("wfi");
}

uintptr_t arch_current_irq_frame(void)
{
	return g_arm64_exception_frame;
}

int arch_irq_frame_is_user(uintptr_t frame_ctx)
{
	const arch_trap_frame_t *frame = (const arch_trap_frame_t *)frame_ctx;

	if (!frame)
		return 0;
	return (frame->spsr_el1 & 0xFu) == 0u;
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
	return frame ? (uint32_t)frame->elr_el1 : 0u;
}

uint64_t arch_syscall_number(const arch_trap_frame_t *frame)
{
	return frame ? frame->x[8] : 0u;
}

uint64_t arch_syscall_arg0(const arch_trap_frame_t *frame)
{
	return frame ? frame->x[0] : 0u;
}

uint64_t arch_syscall_arg1(const arch_trap_frame_t *frame)
{
	return frame ? frame->x[1] : 0u;
}

uint64_t arch_syscall_arg2(const arch_trap_frame_t *frame)
{
	return frame ? frame->x[2] : 0u;
}

uint64_t arch_syscall_arg3(const arch_trap_frame_t *frame)
{
	return frame ? frame->x[3] : 0u;
}

uint64_t arch_syscall_arg4(const arch_trap_frame_t *frame)
{
	return frame ? frame->x[4] : 0u;
}

uint64_t arch_syscall_arg5(const arch_trap_frame_t *frame)
{
	return frame ? frame->x[5] : 0u;
}

void arch_syscall_set_result(arch_trap_frame_t *frame, uint64_t value)
{
	if (frame)
		frame->x[0] = value;
}

int arch_trap_frame_is_syscall(const arch_trap_frame_t *frame)
{
	uint64_t esr_el1;

	if (!frame)
		return 0;

	esr_el1 = frame->esr_el1;
	return ((esr_el1 >> 26) & 0x3Fu) == 0x15u;
}

uint64_t arch_trap_frame_fault_addr(const arch_trap_frame_t *frame)
{
	return frame ? frame->far_el1 : 0u;
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
