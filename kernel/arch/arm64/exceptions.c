/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * exceptions.c — minimal AArch64 exception handlers for Milestone 1 bring-up.
 */

#include "../arch.h"
#include "../../platform/platform.h"
#include "fault.h"
#include "../../proc/sched.h"
#include "../../proc/syscall.h"
#include "kprintf.h"
#include <stdint.h>

static volatile uint32_t g_spurious_irq_count;

extern void sched_record_user_fault(const arch_trap_frame_t *frame,
                                    uint64_t fault_addr,
                                    int signum) __attribute__((weak));
extern void sched_mark_signaled(int sig, int dumped_core) __attribute__((weak));
extern void schedule(void) __attribute__((weak));
extern uint64_t syscall_dispatch_from_frame(arch_trap_frame_t *frame)
    __attribute__((weak));
extern uint64_t arm64_userspace_syscall_dispatch(arch_trap_frame_t *frame);

static void arm64_halt_forever(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

static void uart_put_hex64(const char *label, uint64_t value)
{
	char line[64];
	uint32_t hi = (uint32_t)(value >> 32);
	uint32_t lo = (uint32_t)value;

	k_snprintf(line, sizeof(line), "%s=0x%08X%08X\n", label, hi, lo);
	platform_uart_puts(line);
}

static void arm64_report_kernel_sync_exception(const arch_trap_frame_t *frame)
{
	platform_uart_puts("sync exception\n");
	uart_put_hex64("ESR_EL1", frame ? frame->esr_el1 : 0u);
	uart_put_hex64("ELR_EL1", frame ? frame->elr_el1 : 0u);
	uart_put_hex64("FAR_EL1", frame ? frame->far_el1 : 0u);
	arm64_halt_forever();
}

static int arm64_try_handle_user_fault(arch_trap_frame_t *frame)
{
	process_t *cur;
	uint64_t fault_addr;

	if (!frame || arch_trap_frame_fault_vector(frame) != 14u)
		return -1;

	fault_addr = arch_trap_frame_fault_addr(frame);
	if ((fault_addr >> 32) != 0)
		return -1;

	cur = sched_current();
	if (!cur)
		return -1;

	return paging_handle_fault(cur->pd_phys,
	                           (uint32_t)fault_addr,
	                           arch_trap_frame_fault_error_code(frame),
	                           arch_trap_frame_stack_pointer(frame),
	                           cur);
}

void arm64_sync_handler(arch_trap_frame_t *frame)
{
	if (frame && arch_irq_frame_is_user((uintptr_t)frame)) {
		if (arch_trap_frame_is_syscall(frame)) {
			if (syscall_dispatch_from_frame)
				(void)syscall_dispatch_from_frame(frame);
			else
				(void)arm64_userspace_syscall_dispatch(frame);
			(void)sched_signal_check((uint32_t)(uintptr_t)frame);
			return;
		}

		if (arm64_try_handle_user_fault(frame) == 0)
			return;

		if (!sched_record_user_fault || !sched_mark_signaled || !schedule)
			arm64_report_kernel_sync_exception(frame);
		sched_record_user_fault(
		    frame, arch_trap_frame_fault_addr(frame), SIGSEGV);
		sched_mark_signaled(SIGSEGV, 0);
		schedule();
		arm64_halt_forever();
	}

	arm64_report_kernel_sync_exception(frame);
}

void arm64_irq_handler(arch_trap_frame_t *frame)
{
	(void)frame;

	if (platform_irq_dispatch()) {
		schedule_if_needed();
		(void)sched_signal_check((uint32_t)(uintptr_t)frame);
		return;
	}

	g_spurious_irq_count++;
}

void arm64_fiq_handler(arch_trap_frame_t *frame)
{
	(void)frame;
	platform_uart_puts("fiq exception\n");
	arm64_halt_forever();
}

void arm64_serror_handler(arch_trap_frame_t *frame)
{
	(void)frame;
	platform_uart_puts("serror exception\n");
	arm64_halt_forever();
}
