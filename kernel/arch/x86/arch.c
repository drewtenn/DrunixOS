/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * arch.c — Phase 1/2 shared architecture boundary adapters for x86.
 */

#include "../arch.h"
#include "arch_layout.h"
#include "clock.h"
#include "idt.h"
#include "io.h"
#include "irq.h"
#include "mm/paging.h"
#include "pit.h"

#define QEMU_DEBUG_PORT 0xE9
#define COM1_PORT 0x3F8
#define COM1_LSR (COM1_PORT + 5)
#define COM1_FCR_ENABLE_CLEAR 0x07
#define COM1_LSR_DATA_READY 0x01
#define COM1_LSR_TX_EMPTY 0x20

extern void print_bytes(const char *buf, int n);

static arch_irq_handler_fn x86_periodic_timer_handler;

#ifdef DRUNIX_X86_SERIAL_CONSOLE
#include "tty.h"

static int x86_serial_console_ready;

static void x86_serial_console_init(void)
{
	if (x86_serial_console_ready)
		return;

	port_byte_out(COM1_PORT + 1, 0x00);
	port_byte_out(COM1_PORT + 3, 0x80);
	port_byte_out(COM1_PORT + 0, 0x03);
	port_byte_out(COM1_PORT + 1, 0x00);
	port_byte_out(COM1_PORT + 3, 0x03);
	port_byte_out(COM1_PORT + 2, COM1_FCR_ENABLE_CLEAR);
	port_byte_out(COM1_PORT + 4, 0x0B);
	x86_serial_console_ready = 1;
}

static void x86_serial_console_putc(char c)
{
	x86_serial_console_init();
	for (uint32_t spin = 0; spin < 100000u; spin++) {
		if (port_byte_in(COM1_LSR) & COM1_LSR_TX_EMPTY)
			break;
	}
	port_byte_out(COM1_PORT, (uint8_t)c);
}

static int x86_serial_console_getc(void)
{
	x86_serial_console_init();
	if ((port_byte_in(COM1_LSR) & COM1_LSR_DATA_READY) == 0)
		return -1;
	return port_byte_in(COM1_PORT);
}
#endif

static uint32_t arch_mm_to_paging_flags(uint32_t flags)
{
	uint32_t paging_flags = 0;

	if (flags & ARCH_MM_MAP_PRESENT)
		paging_flags |= PG_PRESENT;
	if ((flags & ARCH_MM_MAP_WRITE) && (flags & ARCH_MM_MAP_COW) == 0)
		paging_flags |= PG_WRITABLE;
	if (flags & ARCH_MM_MAP_USER)
		paging_flags |= PG_USER;
	if (flags & ARCH_MM_MAP_COW)
		paging_flags |= PG_COW;
	if (flags & ARCH_MM_MAP_IO)
		paging_flags |= PG_IO;

	return paging_flags;
}

static int arch_x86_user_page_allowed(uintptr_t virt)
{
	if (virt < ARCH_USER_VADDR_MIN)
		return 0;
	if (virt > ARCH_USER_VADDR_MAX - 0x1000u)
		return 0;
	return 1;
}

static void x86_timer_tick(void)
{
	arch_poll_input();
	if (x86_periodic_timer_handler)
		x86_periodic_timer_handler();
}

uint32_t arch_time_unix_seconds(void)
{
	return clock_unix_time();
}

uint32_t arch_time_uptime_ticks(void)
{
	return clock_uptime_ticks();
}

void arch_console_write(const char *buf, uint32_t len)
{
	if (!buf || len == 0)
		return;

	print_bytes(buf, (int)len);
#ifdef DRUNIX_X86_SERIAL_CONSOLE
	for (uint32_t i = 0; i < len; i++) {
		if (buf[i] == '\n')
			x86_serial_console_putc('\r');
		x86_serial_console_putc(buf[i]);
	}
#endif
}

void arch_debug_write(const char *buf, uint32_t len)
{
	if (!buf || len == 0)
		return;

#ifdef KLOG_TO_DEBUGCON
	for (uint32_t i = 0; i < len; i++) {
		if (buf[i] == '\n')
			port_byte_out(QEMU_DEBUG_PORT, '\r');
		port_byte_out(QEMU_DEBUG_PORT, (unsigned char)buf[i]);
	}
#else
	(void)buf;
	(void)len;
#endif
}

void arch_poll_input(void)
{
#ifdef DRUNIX_X86_SERIAL_CONSOLE
	for (;;) {
		int ch = x86_serial_console_getc();
		if (ch < 0)
			break;
		if (ch == 0)
			continue;
		tty_input_char(0, (char)ch);
	}
#endif
}

void arch_irq_init(void)
{
	irq_dispatch_init();
	pit_init();
	irq_register(0, pit_handle_irq);
}

void arch_irq_register(uint32_t irq, arch_irq_handler_fn fn)
{
	if (irq < IRQ_COUNT)
		irq_register((uint8_t)irq, fn);
}

void arch_irq_mask(uint32_t irq)
{
	if (irq < IRQ_COUNT)
		irq_mask((uint8_t)irq);
}

void arch_irq_unmask(uint32_t irq)
{
	if (irq < IRQ_COUNT)
		irq_unmask((uint8_t)irq);
}

void arch_timer_set_periodic_handler(arch_irq_handler_fn fn)
{
	x86_periodic_timer_handler = fn;
	pit_set_periodic_handler(x86_timer_tick);
}

void arch_timer_start(uint32_t hz)
{
	pit_start(hz);
}

void arch_interrupts_enable(void)
{
	interrupts_enable();
}

void arch_mm_init(void)
{
	paging_init();
}

arch_aspace_t arch_aspace_kernel(void)
{
	return (arch_aspace_t)PAGE_DIR_ADDR;
}

arch_aspace_t arch_aspace_create(void)
{
	return (arch_aspace_t)paging_create_user_space();
}

arch_aspace_t arch_aspace_clone(arch_aspace_t src)
{
	return (arch_aspace_t)paging_clone_user_space((uint32_t)src);
}

void arch_aspace_switch(arch_aspace_t aspace)
{
	paging_switch_directory((uint32_t)aspace);
}

void arch_aspace_destroy(arch_aspace_t aspace)
{
	paging_destroy_user_space((uint32_t)aspace);
}

void arch_user_sync_from_active(void)
{
}

void arch_user_sync_to_active(void)
{
}

int arch_mm_map(arch_aspace_t aspace,
                uintptr_t virt,
                uint64_t phys,
                uint32_t flags)
{
	if (phys > UINT32_MAX)
		return -1;
	if ((flags & ARCH_MM_MAP_USER) && !arch_x86_user_page_allowed(virt))
		return -1;

	return paging_map_page((uint32_t)aspace,
	                       (uint32_t)virt,
	                       (uint32_t)phys,
	                       arch_mm_to_paging_flags(flags));
}

int arch_mm_unmap(arch_aspace_t aspace, uintptr_t virt)
{
	return paging_unmap_page((uint32_t)aspace, (uint32_t)virt);
}

int arch_mm_query(arch_aspace_t aspace, uintptr_t virt, arch_mm_mapping_t *out)
{
	return paging_query_page((uint32_t)aspace, (uint32_t)virt, out);
}

int arch_mm_update(arch_aspace_t aspace,
                   uintptr_t virt,
                   uint32_t clear_flags,
                   uint32_t set_flags)
{
	return paging_update_page(
	    (uint32_t)aspace, (uint32_t)virt, clear_flags, set_flags);
}

void arch_mm_invalidate_page(arch_aspace_t aspace, uintptr_t virt)
{
	(void)aspace;
	paging_invalidate_page((uint32_t)virt);
}

void *arch_page_temp_map(uint64_t phys_addr)
{
	if (phys_addr > UINT32_MAX)
		return 0;

	return paging_temp_map((uint32_t)phys_addr);
}

void arch_page_temp_unmap(void *ptr)
{
	paging_temp_unmap(ptr);
}

uint32_t arch_mm_present_begin(void)
{
	return paging_present_begin();
}

void arch_mm_present_end(uint32_t state)
{
	paging_present_end(state);
}
