/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * start_kernel.c — AArch64 kernel entry point.
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
#include "platform/virt/dma.h"
#include "platform/virt/virtio_mmio.h"
#include "platform/virt/virtio_blk.h"
#endif
#include "../../proc/init_launch.h"
#include "../../proc/sched.h"
#include "mm/pmm.h"
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

/* Emergency rollback for M2.4c bring-up. When set, the virt path stops
 * after the M2.4b bring-up (MMU + GICv3 + virtio-mmio + virtio-blk) and
 * halts in WFI without mounting a root filesystem or launching init.
 * Build with `make ... DRUNIX_ARM64_VIRT_NO_INIT=1` to opt in. */
#ifndef DRUNIX_ARM64_VIRT_NO_INIT
#define DRUNIX_ARM64_VIRT_NO_INIT 0
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
	/* Idempotent: virt's bring-up may have already registered sda via
	 * platform_block_register; raspi3b only registers here. */
	if (blkdev_find_index("sda") < 0 && platform_block_register() != 0)
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

#if DRUNIX_ARM64_PLATFORM_VIRT && DRUNIX_ARM64_VIRT_NO_INIT
/* Heartbeat handler retained for the emergency-rollback path. The
 * production virt boot uses arm64_timer_tick (shared with raspi3b). */
static volatile uint32_t g_virt_heartbeat_count;

static void arm64_virt_heartbeat_handler(void)
{
	g_virt_heartbeat_count++;
	platform_uart_putc('.');
	if ((g_virt_heartbeat_count % 20u) == 0u) {
		char line[32];

		k_snprintf(line,
		           sizeof(line),
		           " [%u]\n",
		           (unsigned int)g_virt_heartbeat_count);
		platform_uart_puts(line);
	}
}
#endif

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

	platform_init();
	__asm__ volatile("msr vbar_el1, %0" : : "r"(vectors_el1));
	__asm__ volatile("isb");

#if DRUNIX_ARM64_HALT_TEST
	platform_uart_puts("ARM64 halt test: triggering sync exception\n");
	__asm__ volatile(".inst 0x00000000");
#endif

	platform_uart_puts("Drunix AArch64 v0 - hello from EL1\n");

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

#if DRUNIX_ARM64_PLATFORM_VIRT
	/* Phase 1 M2.4c of docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md.
	 * GICv3 + generic-timer (M1), virtio-mmio (M2.0-M2.2), MMU + heap
	 * (M2.4b) are all up. M2.4c lifts the gates around the shared
	 * mount/init helpers so virt reuses them, and runs ktest_run_all
	 * under KTEST builds. */
	(void)virtio_mmio_enumerate();

	platform_uart_puts("ARM64: before MMU enable\n");
	arch_mm_init();
	platform_uart_puts("ARM64: MMU enable returned\n");
	kheap_init();

	arch_irq_init();
	arch_interrupts_enable();

	virt_dma_init();

	if (virtio_blk_smoke() != 0)
		platform_uart_puts("virtio-blk: smoke test failed; continuing\n");
#if !DRUNIX_ARM64_EMBED_ROOTFS
	/* When the rootfs is embedded (KTEST builds with ROOT_FS=dufs),
	 * arm64_rootfs_register() owns sda/sdb registration and would
	 * collide with this call. Skip and let the mount path register. */
	else if (platform_block_register() != 0)
		platform_uart_puts(
		    "virtio-blk: blkdev registration failed; continuing\n");
#endif

#if DRUNIX_M2_4B_CACHE_SMOKE
	if (virtio_blk_cache_smoke() != 0)
		platform_uart_puts("virtio-blk: cache torture failed; continuing\n");
#endif

#if DRUNIX_ARM64_VIRT_NO_INIT
	/* Emergency rollback: stop at the M2.4b boot envelope. */
	arch_timer_set_periodic_handler(arm64_virt_heartbeat_handler);
	arch_timer_start(2u);
	platform_uart_puts(
	    "Drunix virt M2.4b (NO_INIT): MMU + GICv3 + virtio-mmio + virtio-blk. "
	    "Mount path skipped. Heartbeat at 2 Hz.\n");
	for (;;)
		__asm__ volatile("wfi");
#else
	tty_init();
	console_terminal_init(&g_console_terminal, &host);
	console_terminal_start(&g_console_terminal);

	arch_timer_set_periodic_handler(arm64_timer_tick);
	arch_timer_start(SCHED_HZ);

	platform_uart_puts(
	    "Drunix virt M2.4c: MMU + GICv3 + virtio-mmio + virtio-blk. "
	    "Mounting root and launching init.\n");

#ifdef KTEST_ENABLED
	sched_init();
	arm64_mount_root_namespace();
	ktest_run_all();
	for (;;)
		__asm__ volatile("wfi");
#endif
	arm64_launch_init_or_fallback();
#endif /* DRUNIX_ARM64_VIRT_NO_INIT */
#else  /* !DRUNIX_ARM64_PLATFORM_VIRT (raspi3b) */
	arch_mm_init();
	kheap_init();
	/* arch_mm_init now reserves the heap range from
	 * platform_ram_layout(); the explicit mark used to live here. */
#if DRUNIX_ARM64_VGA
	framebuffer_info_t *fb = 0;

	if (platform_framebuffer_acquire(&fb) == 0)
		platform_uart_puts("ARM64 framebuffer console enabled\n");
	else
		platform_uart_puts("ARM64 framebuffer console unavailable\n");
#endif

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
