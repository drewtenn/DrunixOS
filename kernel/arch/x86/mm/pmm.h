/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PMM_H
#define PMM_H

#include "pmm_core.h"
#include <stdint.h>

/* ── Multiboot1 types ─────────────────────────────────────────────────────── */

/* Multiboot memory-map entry (Multiboot1 spec §3.3) */
typedef struct {
	uint32_t size; /* size of this entry, NOT counting this field */
	uint64_t addr;
	uint64_t len;
	uint32_t type; /* 1 = usable RAM */
} __attribute__((packed)) multiboot_mmap_entry_t;

typedef struct {
	uint32_t flags;
	uint32_t mem_lower;
	uint32_t mem_upper;
	uint32_t boot_device;
	uint32_t cmdline;
	uint32_t mods_count;
	uint32_t mods_addr;
	uint8_t syms[16];
	uint32_t mmap_length;
	uint32_t mmap_addr;
	uint32_t drives_length;
	uint32_t drives_addr;
	uint32_t config_table;
	uint32_t boot_loader_name;
	uint32_t apm_table;
	uint32_t vbe_control_info;
	uint32_t vbe_mode_info;
	uint16_t vbe_mode;
	uint16_t vbe_interface_seg;
	uint16_t vbe_interface_off;
	uint16_t vbe_interface_len;
	uint64_t framebuffer_addr;
	uint32_t framebuffer_pitch;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint8_t framebuffer_bpp;
	uint8_t framebuffer_type;
	/* GRUB's Multiboot1 color-info union is 32-bit aligned. */
	uint8_t framebuffer_color_info_pad[2];
	uint8_t framebuffer_red_field_position;
	uint8_t framebuffer_red_mask_size;
	uint8_t framebuffer_green_field_position;
	uint8_t framebuffer_green_mask_size;
	uint8_t framebuffer_blue_field_position;
	uint8_t framebuffer_blue_mask_size;
} __attribute__((packed)) multiboot_info_t;

#define MULTIBOOT_FLAG_CMDLINE (1u << 2)
#define MULTIBOOT_FLAG_MMAP (1u << 6)
#define MULTIBOOT_FLAG_FRAMEBUFFER (1u << 12)
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB 1u

/* ── PMM API ──────────────────────────────────────────────────────────────── */

void pmm_init(multiboot_info_t *mbi);
void pmm_mark_used(uint32_t base, uint32_t length);
void pmm_mark_free(uint32_t base, uint32_t length);
uint32_t pmm_alloc_page(void); /* returns physical address, 0 on failure */
void pmm_incref(uint32_t phys_addr);
void pmm_decref(uint32_t phys_addr);
void pmm_free_page(uint32_t phys_addr);
uint8_t pmm_refcount(uint32_t phys_addr);
uint32_t pmm_free_page_count(void);

#ifdef KTEST_ENABLED
int pmm_multiboot_framebuffer_range_for_test(const multiboot_info_t *mbi,
                                             uint32_t *base_out,
                                             uint32_t *length_out);
void pmm_multiboot_apply_usable_ranges_for_test(pmm_core_state_t *state,
                                                const multiboot_info_t *mbi);
#endif

#endif
