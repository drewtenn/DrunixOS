/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * start_kernel.c — Milestone 1 AArch64 kernel entry point.
 */

#include "../arch.h"
#include "../../blk/bcache.h"
#include "../../console/terminal.h"
#include "../../drivers/tty.h"
#include "../../fs/fs.h"
#include "../../fs/vfs.h"
#include "../../mm/kheap.h"
#include "../../proc/init_launch.h"
#include "../../proc/sched.h"
#include "rootfs.h"
#include "mm/pmm.h"
#include "timer.h"
#include "uart.h"
#include "usb_keyboard.h"
#include "video.h"
#include "kprintf.h"
#include <stdint.h>

extern char vectors_el1[];
extern int arm64_user_smoke_boot(void);

#ifndef DRUNIX_ARM64_SMOKE_FALLBACK
#define DRUNIX_ARM64_SMOKE_FALLBACK 0
#endif

static volatile uint64_t g_heartbeat_ticks;
static console_terminal_t g_console_terminal;

static uint64_t arm64_read_currentel(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
	return value;
}

static uint64_t arm64_read_cntfrq(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

static void arm64_heartbeat_tick(void)
{
	g_heartbeat_ticks++;
}

static void arm64_timer_tick(void)
{
	if (sched_current())
		arch_poll_input();
	arm64_heartbeat_tick();
	sched_tick();
}

static void arm64_terminal_write(const char *buf, uint32_t len, void *ctx)
{
	(void)ctx;
	arch_console_write(buf, len);
}

static uint32_t arm64_terminal_ticks(void *ctx)
{
	(void)ctx;
	return (uint32_t)g_heartbeat_ticks;
}

static uint32_t arm64_terminal_uptime_seconds(void *ctx)
{
	(void)ctx;
	return (uint32_t)(g_heartbeat_ticks / 10u);
}

static uint32_t arm64_terminal_free_pages(void *ctx)
{
	(void)ctx;
	return pmm_free_page_count();
}

void arm64_console_loop(void)
{
	for (;;) {
		char ch;

		__asm__ volatile("wfi");
		while (uart_try_getc(&ch))
			console_terminal_handle_char(&g_console_terminal, ch);
	}
}

static void arm64_mount_root_namespace(void)
{
	if (arm64_rootfs_register() != 0)
		uart_puts("ARM64 rootfs register failed\n");
	if (bcache_init() != 0)
		uart_puts("ARM64 block cache init failed\n");
	vfs_reset();
	dufs_register();
	if (vfs_mount_with_source("/", "dufs", "/dev/sda1") != 0) {
		uart_puts("ARM64 root mount failed\n");
		arm64_console_loop();
	}
}

static void arm64_mount_synthetic_filesystems(void)
{
	if (vfs_mount("/dev", "devfs") != 0)
		uart_puts("ARM64 devfs mount failed\n");
	if (vfs_mount("/proc", "procfs") != 0)
		uart_puts("ARM64 procfs mount failed\n");
	if (vfs_mount_with_source("/sys", "sysfs", "sysfs") != 0)
		uart_puts("ARM64 sysfs mount failed\n");
}

static void arm64_launch_init_or_fallback(void)
{
	process_t *init_proc;
	int init_pid;

	sched_init();
	arm64_mount_root_namespace();
	arm64_mount_synthetic_filesystems();
	init_pid = boot_launch_init_process(DRUNIX_INIT_PROGRAM,
	                                    DRUNIX_INIT_ARG0,
	                                    DRUNIX_INIT_ENV0,
	                                    BOOT_LAUNCH_INIT_STANDALONE);
	if (init_pid < 0) {
		uart_puts("ARM64 init launch failed: ");
		uart_puts(DRUNIX_INIT_PROGRAM);
		uart_puts("\n");
#if DRUNIX_ARM64_SMOKE_FALLBACK
		if (arm64_user_smoke_boot() != 0)
			uart_puts("ARM64 user smoke: boot failed\n");
#endif
		arm64_console_loop();
	}

	init_proc = sched_bootstrap();
	if (!init_proc) {
		uart_puts("ARM64 init bootstrap failed\n");
		arm64_console_loop();
	}
	arch_process_launch(init_proc);
}

void arm64_start_kernel(void)
{
	char line[64];
	console_terminal_host_t host = {
	    .write = arm64_terminal_write,
	    .read_ticks = arm64_terminal_ticks,
	    .read_uptime_seconds = arm64_terminal_uptime_seconds,
	    .read_free_pages = arm64_terminal_free_pages,
	    .ctx = 0,
	};

	uart_init();
	__asm__ volatile("msr vbar_el1, %0" : : "r"(vectors_el1));
	__asm__ volatile("isb");

	uart_puts("Drunix AArch64 v0 - hello from EL1\n");
	arch_mm_init();
	kheap_init();
	pmm_mark_used(HEAP_START, HEAP_END - HEAP_START);
#if DRUNIX_ARM64_VGA
	if (arm64_video_init() == 0)
		uart_puts("ARM64 framebuffer console enabled\n");
	else
		uart_puts("ARM64 framebuffer console unavailable\n");
#endif

	k_snprintf(line,
	           sizeof(line),
	           "CurrentEL=0x%X (EL%u)\n",
	           (unsigned int)arm64_read_currentel(),
	           (unsigned int)(arm64_read_currentel() >> 2));
	uart_puts(line);

	k_snprintf(line,
	           sizeof(line),
	           "CNTFRQ_EL0=%uHz\n",
	           (unsigned int)arm64_read_cntfrq());
	uart_puts(line);

	arch_irq_init();
	arch_timer_set_periodic_handler(arm64_timer_tick);
	arch_timer_start(SCHED_HZ);
	arch_interrupts_enable();
	tty_init();
#if DRUNIX_ARM64_VGA
	if (arm64_usb_keyboard_init() != 0)
		uart_puts("ARM64 USB keyboard unavailable\n");
#endif
	console_terminal_init(&g_console_terminal, &host);
	console_terminal_start(&g_console_terminal);
	arm64_launch_init_or_fallback();
}
