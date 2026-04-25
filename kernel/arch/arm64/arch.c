/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch.c — Phase 1/2 shared architecture boundary adapters for arm64.
 */

#include "../arch.h"
#include "../../drivers/tty.h"
#include "irq.h"
#include "mm/mmu.h"
#include "mm/pmm.h"
#include "proc/init_layout.h"
#include "timer.h"
#include "uart.h"
#include "usb_keyboard.h"
#include "video.h"

extern char _kernel_start[];
extern char _kernel_end[];

static int g_arm64_mm_ready;

uint32_t arch_time_unix_seconds(void)
{
	return 0;
}

uint32_t arch_time_uptime_ticks(void)
{
	return (uint32_t)arm64_timer_ticks();
}

void arch_console_write(const char *buf, uint32_t len)
{
	if (!buf || len == 0)
		return;

	for (uint32_t i = 0; i < len; i++) {
		if (buf[i] == '\n')
			uart_putc('\r');
		uart_putc(buf[i]);
	}
#if DRUNIX_ARM64_VGA
	arm64_video_console_write(buf, len);
#endif
}

void arch_debug_write(const char *buf, uint32_t len)
{
	arch_console_write(buf, len);
}

void arch_poll_input(void)
{
	char ch;

	while (uart_try_getc(&ch))
		tty_input_char(0, ch);
	arm64_usb_keyboard_poll();
}

void arch_irq_init(void)
{
	arm64_irq_init();
}

void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn)
{
	arm64_irq_register(irq, fn);
}

void arch_irq_mask(uint32_t irq)
{
	(void)irq;
}

void arch_irq_unmask(uint32_t irq)
{
	(void)irq;
}

void arch_timer_set_periodic_handler(arch_irq_handler_fn fn)
{
	arm64_irq_register(ARM64_IRQ_LOCAL_TIMER, fn);
}

void arch_timer_start(uint32_t hz)
{
	arm64_timer_start(hz);
}

void arch_interrupts_enable(void)
{
	arm64_irq_enable();
}

void arch_mm_init(void)
{
	uintptr_t kernel_start;
	uintptr_t kernel_end;

	if (g_arm64_mm_ready)
		return;

	pmm_init();
	kernel_start = (uintptr_t)_kernel_start & ~(uintptr_t)(PAGE_SIZE - 1u);
	kernel_end = ((uintptr_t)_kernel_end + PAGE_SIZE - 1u) &
	             ~(uintptr_t)(PAGE_SIZE - 1u);
	if (kernel_end > kernel_start) {
		pmm_mark_used((uint32_t)kernel_start,
		              (uint32_t)(kernel_end - kernel_start));
	}
	pmm_mark_used((uint32_t)ARM64_INIT_IMAGE_BASE,
	              (uint32_t)(ARM64_INIT_IMAGE_LIMIT - ARM64_INIT_IMAGE_BASE));
	pmm_mark_used((uint32_t)ARM64_INIT_STACK_BASE,
	              (uint32_t)(ARM64_INIT_STACK_TOP - ARM64_INIT_STACK_BASE));

	arm64_mmu_init();
	if (arm64_mmu_enabled())
		uart_puts("ARM64 MMU enabled\n");
	g_arm64_mm_ready = 1;
}

arch_aspace_t arch_aspace_kernel(void)
{
	return arm64_mmu_kernel_aspace();
}

arch_aspace_t arch_aspace_create(void)
{
	return arm64_mmu_aspace_create();
}

arch_aspace_t arch_aspace_clone(arch_aspace_t src)
{
	return arm64_mmu_aspace_clone(src);
}

void arch_aspace_switch(arch_aspace_t aspace)
{
	arm64_mmu_aspace_switch(aspace);
}

void arch_aspace_destroy(arch_aspace_t aspace)
{
	arm64_mmu_aspace_destroy(aspace);
}

void arch_user_sync_from_active(void)
{
	arm64_mmu_sync_current_from_identity();
}

void arch_user_sync_to_active(void)
{
	arm64_mmu_sync_current_to_identity();
}

int arch_mm_map(arch_aspace_t aspace,
                uintptr_t virt,
                uint64_t phys,
                uint32_t flags)
{
	return arm64_mmu_map(aspace, virt, phys, flags);
}

int arch_mm_unmap(arch_aspace_t aspace, uintptr_t virt)
{
	return arm64_mmu_unmap(aspace, virt);
}

int arch_mm_query(arch_aspace_t aspace, uintptr_t virt, arch_mm_mapping_t *out)
{
	return arm64_mmu_query(aspace, virt, out);
}

int arch_mm_update(arch_aspace_t aspace,
                   uintptr_t virt,
                   uint32_t clear_flags,
                   uint32_t set_flags)
{
	return arm64_mmu_update(aspace, virt, clear_flags, set_flags);
}

void arch_mm_invalidate_page(arch_aspace_t aspace, uintptr_t virt)
{
	arm64_mmu_invalidate_page(aspace, virt);
}

void *arch_page_temp_map(uint64_t phys_addr)
{
	return arm64_temp_map(phys_addr);
}

void arch_page_temp_unmap(void *ptr)
{
	arm64_temp_unmap(ptr);
}

uint32_t arch_mm_present_begin(void)
{
	uint64_t daif;

	__asm__ volatile("mrs %0, daif" : "=r"(daif));
	__asm__ volatile("msr daifset, #2" ::: "memory");
	return (uint32_t)daif;
}

void arch_mm_present_end(uint32_t state)
{
	uint64_t daif = state;

	__asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
}
