/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_PLATFORM_PLATFORM_H
#define KERNEL_PLATFORM_PLATFORM_H

#include "framebuffer.h"
#include <stdint.h>

#if defined(DRUNIX_ARM64_PLATFORM_VIRT)
#include "virt/platform.h"
#else
#include "raspi3b/platform.h"
#endif

#define PLATFORM_IRQ_TIMER 0u
#define PLATFORM_IRQ_COUNT 1u

typedef void (*platform_irq_handler_fn)(void);

void platform_init(void);
void platform_uart_putc(char c);
void platform_uart_puts(const char *s);
char platform_uart_getc(void);
int platform_uart_try_getc(char *out);
void platform_irq_init(void);
void platform_irq_register(uint32_t irq, platform_irq_handler_fn fn);
int platform_irq_dispatch(void);
void platform_irq_enable(void);
int platform_framebuffer_acquire(framebuffer_info_t **out);
void platform_framebuffer_console_write(const char *buf, uint32_t len);
int platform_block_register(void);
int platform_usb_hci_register(void);
void platform_usb_hci_poll(void);

/*
 * Phase 1 M2.4b: per-platform memory classifier and RAM-layout
 * interface. mmu.c uses the classifier to decide block attributes
 * (Normal vs Device vs unmapped); arch.c / pmm.c / kheap.c use the
 * layout to drive PMM range setup and heap base/size.
 */
typedef enum {
	PLATFORM_MM_UNMAPPED = 0,
	PLATFORM_MM_NORMAL = 1,
	PLATFORM_MM_DEVICE = 2,
	/*
	 * M2.5a: software-framebuffer reservation. The classifier returns
	 * this for pages that must be mapped Normal-NC (Inner-NC Outer-NC)
	 * by both the kernel linear map and any user mmap aperture, so a
	 * host-side reader (QEMU ramfb) sees fresh pixel writes without
	 * explicit dcache cleans. Only the virt platform returns this
	 * attribute today; raspi3b uses framebuffer pages from VideoCore.
	 */
	PLATFORM_MM_FRAMEBUFFER = 3,
} platform_mm_attr_t;

platform_mm_attr_t platform_mm_classify(uint64_t phys);

typedef struct platform_ram_layout {
	uint64_t ram_base;
	uint64_t ram_size;
	uint64_t heap_base;
	uint64_t heap_size;
	uint64_t kernel_image_end;
	/*
	 * M2.5a: optional framebuffer carve-out at the top of RAM.
	 * framebuffer_size == 0 when the platform has no software FB
	 * reservation (raspi3b: 0; virt: 8 MiB unless FDT overrides).
	 */
	uint64_t framebuffer_base;
	uint64_t framebuffer_size;
} platform_ram_layout_t;

const platform_ram_layout_t *platform_ram_layout(void);

#endif
