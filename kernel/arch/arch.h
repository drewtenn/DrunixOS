/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARCH_H
#define KERNEL_ARCH_ARCH_H

#include <stdint.h>

struct process;
typedef struct process process_t;

typedef void (*arch_irq_handler_fn)(void);
typedef uintptr_t arch_aspace_t;
typedef uintptr_t arch_context_t;

#include "proc/frame.h"
#include "proc/state.h"

typedef struct {
	uint64_t phys_addr;
	uint32_t flags;
} arch_mm_mapping_t;

#define ARCH_MM_MAP_PRESENT 0x0001u
#define ARCH_MM_MAP_READ 0x0002u
#define ARCH_MM_MAP_WRITE 0x0004u
#define ARCH_MM_MAP_EXEC 0x0008u
#define ARCH_MM_MAP_USER 0x0010u
#define ARCH_MM_MAP_COW 0x0020u

uint32_t arch_time_unix_seconds(void);
uint32_t arch_time_uptime_ticks(void);
void arch_console_write(const char *buf, uint32_t len);
void arch_debug_write(const char *buf, uint32_t len);
void arch_poll_input(void);
void arch_irq_init(void);
void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn);
void arch_irq_mask(uint32_t irq);
void arch_irq_unmask(uint32_t irq);
void arch_timer_set_periodic_handler(arch_irq_handler_fn fn);
void arch_timer_start(uint32_t hz);
void arch_interrupts_enable(void);
void arch_mm_init(void);
arch_aspace_t arch_aspace_kernel(void);
arch_aspace_t arch_aspace_create(void);
arch_aspace_t arch_aspace_clone(arch_aspace_t src);
void arch_aspace_switch(arch_aspace_t aspace);
void arch_aspace_destroy(arch_aspace_t aspace);
void arch_user_sync_from_active(void);
void arch_user_sync_to_active(void);
int arch_mm_map(arch_aspace_t aspace, uintptr_t virt, uint64_t phys, uint32_t flags);
int arch_mm_unmap(arch_aspace_t aspace, uintptr_t virt);
int arch_mm_query(arch_aspace_t aspace, uintptr_t virt, arch_mm_mapping_t *out);
/*
 * Scaffold limitation for Phase 3: arch_mm_update() only guarantees WRITE/COW
 * permission changes. PRESENT/USER transitions still require explicit
 * map/unmap paths on x86 until shared callers migrate behind the boundary.
 */
int arch_mm_update(arch_aspace_t aspace,
                   uintptr_t virt,
                   uint32_t clear_flags,
                   uint32_t set_flags);
void arch_mm_invalidate_page(arch_aspace_t aspace, uintptr_t virt);
void *arch_page_temp_map(uint64_t phys_addr);
void arch_page_temp_unmap(void *ptr);
uint32_t arch_mm_present_begin(void);
void arch_mm_present_end(uint32_t state);
int arch_process_build_user_stack(arch_aspace_t aspace,
                                  const char *const *argv,
                                  int argc,
                                  const char *const *envp,
                                  int envc,
                                  uintptr_t *stack_out);
void arch_process_build_initial_frame(process_t *proc);
void arch_process_build_exec_frame(process_t *proc,
                                   arch_aspace_t old_aspace,
                                   uintptr_t old_kstack_bottom);
int arch_process_clone_frame(process_t *child_out,
                             const process_t *parent,
                             uint32_t child_stack);
void arch_process_restore_tls(const process_t *proc);
void arch_process_launch(process_t *proc);
void arch_context_prepare(process_t *proc);
void arch_fpu_init_state(process_t *proc);
void arch_fpu_save(process_t *proc);
void arch_context_switch(arch_context_t *old_ctx,
                         arch_context_t new_ctx,
                         arch_aspace_t new_aspace);
void arch_idle_wait(void);
uintptr_t arch_current_irq_frame(void);
int arch_irq_frame_is_user(uintptr_t frame_ctx);
void arch_kstack_guard(uintptr_t addr);
void arch_kstack_unguard(uintptr_t addr);
uint32_t arch_trap_frame_ip(const arch_trap_frame_t *frame);
uint32_t arch_trap_frame_fault_vector(const arch_trap_frame_t *frame);
uint32_t arch_trap_frame_fault_error_code(const arch_trap_frame_t *frame);
uint32_t arch_trap_frame_stack_pointer(const arch_trap_frame_t *frame);
uint64_t arch_syscall_number(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg0(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg1(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg2(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg3(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg4(const arch_trap_frame_t *frame);
uint64_t arch_syscall_arg5(const arch_trap_frame_t *frame);
void arch_syscall_set_result(arch_trap_frame_t *frame, uint64_t value);
int arch_trap_frame_is_syscall(const arch_trap_frame_t *frame);
/*
 * Return the fault address associated with an active synchronous fault frame.
 * This is not guaranteed to be a replayable property of arbitrary saved trap
 * frames: some architectures, including x86, source it from fault-state
 * registers that must be queried during fault handling.
 */
uint64_t arch_trap_frame_fault_addr(const arch_trap_frame_t *frame);
void arch_core_fill_prstatus_regs(uint32_t *gregs,
                                  const arch_trap_frame_t *frame);
void arch_trap_frame_sanitize(process_t *proc, arch_trap_frame_t *frame);
int arch_signal_setup_frame(process_t *proc,
                            arch_trap_frame_t *frame,
                            int signum,
                            uint32_t handler_va);
uint32_t arch_user_tls_entry(void);

#endif
