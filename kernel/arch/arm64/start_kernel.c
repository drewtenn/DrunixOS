/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * start_kernel.c — Milestone 1 AArch64 kernel entry point.
 */

#include "../arch.h"
#include "../../blk/bcache.h"
#include "../../console/terminal.h"
#include "../../drivers/blkdev.h"
#include "../../drivers/tty.h"
#include "../../fs/ext3.h"
#include "../../fs/fs.h"
#include "../../fs/vfs.h"
#include "../../mm/kheap.h"
#include "platform/platform.h"
#if DRUNIX_ARM64_PLATFORM_VIRT
#include "platform/virt/virtio_mmio.h"
#include "platform/virt/virtio_blk.h"
#endif
#include "../../proc/init_launch.h"
#include "../../proc/sched.h"
#include "mm/pmm.h"
#include "timer.h"
#include "kprintf.h"
#if DRUNIX_ARM64_EMBED_ROOTFS
#include "rootfs.h"
#endif
#ifdef KTEST_ENABLED
#include "ktest.h"
#endif
#include <stdint.h>

extern char vectors_el1[];
extern int arm64_user_smoke_boot(void);

#ifndef DRUNIX_ARM64_SMOKE_FALLBACK
#define DRUNIX_ARM64_SMOKE_FALLBACK 0
#endif

#ifndef DRUNIX_ARM64_HALT_TEST
#define DRUNIX_ARM64_HALT_TEST 0
#endif

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

#if !DRUNIX_ARM64_PLATFORM_VIRT
static volatile uint64_t g_heartbeat_ticks;
static console_terminal_t g_console_terminal;

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
#endif /* !DRUNIX_ARM64_PLATFORM_VIRT */

#if DRUNIX_ARM64_PLATFORM_VIRT
/* Virt M0/M1 stub: smoke.c / syscall.c hold a static link reference to
 * arm64_console_loop for their error paths. Those paths are not reached
 * on virt without a scheduler, but the symbol must exist for the link
 * to succeed. M2 swaps to the real console_terminal-driven loop. */
void arm64_console_loop(void)
{
	for (;;)
		__asm__ volatile("wfi");
}

static volatile uint32_t g_virt_heartbeat_count;

void arm64_virt_heartbeat_handler(void)
{
	g_virt_heartbeat_count++;
	platform_uart_putc('.');
	if ((g_virt_heartbeat_count % 20u) == 0u) {
		char line[32];

		k_snprintf(line, sizeof(line), " [%u]\n",
		           (unsigned int)g_virt_heartbeat_count);
		platform_uart_puts(line);
	}
}
#else
void arm64_console_loop(void)
{
	for (;;) {
		char ch;

		__asm__ volatile("wfi");
		while (platform_uart_try_getc(&ch))
			console_terminal_handle_char(&g_console_terminal, ch);
	}
}

static void arm64_mount_root_namespace(void)
{
	int sda_idx;
	int sdb_idx;

#if DRUNIX_ARM64_EMBED_ROOTFS
	if (arm64_rootfs_register() != 0)
		platform_uart_puts("ARM64 rootfs register failed\n");
#else
	if (platform_block_register() != 0)
		platform_uart_puts("ARM64 block register failed\n");
#endif
	sda_idx = blkdev_find_index("sda");
	sdb_idx = blkdev_find_index("sdb");
	if (sda_idx >= 0)
		blkdev_scan_mbr((uint32_t)sda_idx);
	if (sdb_idx >= 0)
		blkdev_scan_mbr((uint32_t)sdb_idx);
	if (bcache_init() != 0)
		platform_uart_puts("ARM64 block cache init failed\n");
	vfs_reset();
	dufs_register();
	ext3_register();
	if (vfs_mount_with_source("/", DRUNIX_ROOT_FS, "/dev/sda1") != 0) {
		platform_uart_puts("ARM64 root mount failed\n");
		arm64_console_loop();
	}
	platform_uart_puts("ARM64 root mounted: ");
	platform_uart_puts(DRUNIX_ROOT_FS);
	platform_uart_puts("\n");
	if (sdb_idx >= 0 &&
	    vfs_mount_with_source("/dufs", "dufs", "/dev/sdb1") == 0)
		platform_uart_puts("ARM64 dufs mounted at /dufs\n");
}

static void arm64_mount_synthetic_filesystems(void)
{
	if (vfs_mount("/dev", "devfs") != 0)
		platform_uart_puts("ARM64 devfs mount failed\n");
	if (vfs_mount("/proc", "procfs") != 0)
		platform_uart_puts("ARM64 procfs mount failed\n");
	if (vfs_mount_with_source("/sys", "sysfs", "sysfs") != 0)
		platform_uart_puts("ARM64 sysfs mount failed\n");
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
		platform_uart_puts("ARM64 init launch failed: ");
		platform_uart_puts(DRUNIX_INIT_PROGRAM);
		platform_uart_puts("\n");
#if DRUNIX_ARM64_SMOKE_FALLBACK
		if (arm64_user_smoke_boot() != 0)
			platform_uart_puts("ARM64 user smoke: boot failed\n");
#endif
		arm64_console_loop();
	}

	init_proc = sched_bootstrap();
	if (!init_proc) {
		platform_uart_puts("ARM64 init bootstrap failed\n");
		arm64_console_loop();
	}
	arch_process_launch(init_proc);
}
#endif /* !DRUNIX_ARM64_PLATFORM_VIRT */

void arm64_start_kernel(void)
{
	char line[64];
#if !DRUNIX_ARM64_PLATFORM_VIRT
	console_terminal_host_t host = {
	    .write = arm64_terminal_write,
	    .read_ticks = arm64_terminal_ticks,
	    .read_uptime_seconds = arm64_terminal_uptime_seconds,
	    .read_free_pages = arm64_terminal_free_pages,
	    .ctx = 0,
	};
#endif

	platform_init();
	__asm__ volatile("msr vbar_el1, %0" : : "r"(vectors_el1));
	__asm__ volatile("isb");

#if DRUNIX_ARM64_HALT_TEST
	platform_uart_puts("ARM64 halt test: triggering sync exception\n");
	__asm__ volatile(".inst 0x00000000");
#endif

	platform_uart_puts("Drunix AArch64 v0 - hello from EL1\n");

#if DRUNIX_ARM64_PLATFORM_VIRT
	/* Phase 1 M2.0 of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
	 * GICv3 + generic-timer are up. virtio-mmio enumeration (read-only)
	 * lands here; virtqueue mechanics + virtio-blk + DMA discipline
	 * arrive in M2.1+. MMU/heap/USB/rootfs still assume raspi3b's
	 * hardware backend so they remain gated. */
	k_snprintf(line,
	           sizeof(line),
	           "CurrentEL=0x%X (EL%u)\n",
	           (unsigned int)arm64_read_currentel(),
	           (unsigned int)(arm64_read_currentel() >> 2));
	platform_uart_puts(line);

	k_snprintf(line,
	           sizeof(line),
	           "CNTFRQ_EL0=%uHz\n",
	           (unsigned int)arm64_read_cntfrq());
	platform_uart_puts(line);

	(void)virtio_mmio_enumerate();

	/* Bring up the GICv3 distributor + redistributor + CPU interface,
	 * then unmask DAIF.I, before running virtio-blk so the device's
	 * SPI is delivered through to the registered handler. The timer
	 * stays dormant (CNTP_CTL_EL0 = 0) until arch_timer_start runs,
	 * so no spurious heartbeat ticks during the blk smoke. */
	arch_irq_init();
	arch_interrupts_enable();

	(void)virtio_blk_smoke();

	arch_timer_set_periodic_handler(arm64_virt_heartbeat_handler);
	arch_timer_start(2u);

	platform_uart_puts(
	    "Drunix virt M2.2: GICv3 + virtio-mmio + virtio-blk (IRQ-driven). "
	    "Heartbeat at 2 Hz.\n");

	for (;;)
		__asm__ volatile("wfi");
#else
	arch_mm_init();
	kheap_init();
	pmm_mark_used(HEAP_START, HEAP_END - HEAP_START);
#if DRUNIX_ARM64_VGA
	framebuffer_info_t *fb = 0;

	if (platform_framebuffer_acquire(&fb) == 0)
		platform_uart_puts("ARM64 framebuffer console enabled\n");
	else
		platform_uart_puts("ARM64 framebuffer console unavailable\n");
#endif

	k_snprintf(line,
	           sizeof(line),
	           "CurrentEL=0x%X (EL%u)\n",
	           (unsigned int)arm64_read_currentel(),
	           (unsigned int)(arm64_read_currentel() >> 2));
	platform_uart_puts(line);

	k_snprintf(line,
	           sizeof(line),
	           "CNTFRQ_EL0=%uHz\n",
	           (unsigned int)arm64_read_cntfrq());
	platform_uart_puts(line);

	arch_irq_init();
	arch_timer_set_periodic_handler(arm64_timer_tick);
	arch_timer_start(SCHED_HZ);
	arch_interrupts_enable();
	tty_init();
#ifdef KTEST_ENABLED
	sched_init();
	arm64_mount_root_namespace();
	ktest_run_all();
	for (;;)
		__asm__ volatile("wfi");
#endif
#if DRUNIX_ARM64_VGA
	if (platform_usb_hci_register() != 0)
		platform_uart_puts("ARM64 USB keyboard unavailable\n");
#endif
	console_terminal_init(&g_console_terminal, &host);
	console_terminal_start(&g_console_terminal);
	arm64_launch_init_or_fallback();
#endif /* !DRUNIX_ARM64_PLATFORM_VIRT */
}
