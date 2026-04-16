/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * pmm.c — physical frame allocator and per-frame reference counting.
 */

#include "pmm.h"
#include <stdint.h>

/* Linker-provided symbols: first and last byte of the kernel image */
extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

/*
 * Bitmap lives in kernel .bss — physically above 0x100000, safely above
 * anything GRUB places in low memory (MBI struct, mmap entries, modules).
 * Linux takes the same approach: mem_map sits right after the kernel image.
 */
static uint8_t bitmap[PMM_MAX_PAGES / 8];
static uint8_t refcount[PMM_MAX_PAGES];

static int pmm_multiboot_framebuffer_range(const multiboot_info_t *mbi,
                                           uint32_t *base_out,
                                           uint32_t *length_out)
{
    uint32_t bytes_per_pixel;
    uint64_t row_bytes;
    uint64_t last_row_offset;
    uint64_t length;
    uint64_t base;

    if (!mbi || !base_out || !length_out)
        return -1;
    if ((mbi->flags & MULTIBOOT_FLAG_FRAMEBUFFER) == 0)
        return -1;
    base = mbi->framebuffer_addr;
    if (base > UINT32_MAX)
        return -1;
    if (mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0 ||
        mbi->framebuffer_pitch == 0 || mbi->framebuffer_bpp == 0)
        return -1;

    bytes_per_pixel = ((uint32_t)mbi->framebuffer_bpp + 7u) / 8u;
    row_bytes = (uint64_t)mbi->framebuffer_width * bytes_per_pixel;
    last_row_offset =
        (uint64_t)(mbi->framebuffer_height - 1u) * mbi->framebuffer_pitch;
    length = last_row_offset + row_bytes;
    if (length == 0 || length > UINT32_MAX ||
        base + length > ((uint64_t)UINT32_MAX + 1u))
        return -1;

    *base_out = (uint32_t)base;
    *length_out = (uint32_t)length;
    return 0;
}

static void bitmap_set(uint32_t page) {
    bitmap[page / 8] |= (1u << (page % 8));
}

static void bitmap_clear(uint32_t page) {
    bitmap[page / 8] &= ~(1u << (page % 8));
}

static int bitmap_test(uint32_t page) {
    return bitmap[page / 8] & (1u << (page % 8));
}

void pmm_mark_used(uint32_t base, uint32_t length) {
    uint32_t page = base / PAGE_SIZE;
    uint32_t end  = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (; page < end && page < PMM_MAX_PAGES; page++)
        bitmap_set(page);
}

void pmm_mark_free(uint32_t base, uint32_t length) {
    uint32_t page = base / PAGE_SIZE;
    uint32_t end  = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (; page < end && page < PMM_MAX_PAGES; page++) {
        bitmap_clear(page);
        refcount[page] = 0;
    }
}

void pmm_init(multiboot_info_t *mbi) {
    uint32_t have_mmap = mbi && (mbi->flags & MULTIBOOT_FLAG_MMAP);
    uint32_t mmap_base = have_mmap ? mbi->mmap_addr   : 0;
    uint32_t mmap_len  = have_mmap ? mbi->mmap_length : 0;

    /* Start with all pages marked used — safe default */
    for (uint32_t i = 0; i < PMM_MAX_PAGES / 8; i++)
        bitmap[i] = 0xFF;

    /* Mark free regions from the Multiboot memory map (flags bit 6) */
    if (have_mmap) {
        multiboot_mmap_entry_t *entry =
            (multiboot_mmap_entry_t *)mmap_base;
        uint32_t map_end = mmap_base + mmap_len;

        while ((uint32_t)entry < map_end) {
            if (entry->type == 1) {   /* usable RAM */
                /* Truncate 64-bit fields to 32-bit; we manage only 4 GB */
                uint32_t base = (uint32_t)entry->addr;
                uint32_t len  = (uint32_t)entry->len;
                pmm_mark_free(base, len);
            }
            /* Advance by entry->size bytes (size field does not count itself) */
            entry = (multiboot_mmap_entry_t *)((uint32_t)entry + entry->size + 4);
        }
    } else {
        /* Fallback: assume 1 MB – 128 MB is usable */
        pmm_mark_free(0x00100000, 0x07F00000);
    }

    /* ── Re-mark all reserved regions ─────────────────────────────────────── */

    /*
     * Reserve the whole first MiB.  GRUB's memory map may describe parts of
     * conventional memory as usable, but this range also contains firmware
     * scratch areas and Multiboot data that can still be touched after boot.
     * User pages must never be allocated here; otherwise normal stack writes
     * can corrupt executable text placed in low physical memory.
     */
    pmm_mark_used(0x00000000, 0x00100000);

    /* Kernel image: from _kernel_start (0x100000) to _kernel_end */
    uint32_t kstart = (uint32_t)&_kernel_start;
    uint32_t kend   = (uint32_t)&_kernel_end;
    uint32_t kend_page = (kend + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_mark_used(kstart, kend_page * PAGE_SIZE - kstart);

    /* Page directory + 32 page tables (1 + 32 = 33 pages) */
    pmm_mark_used(0x00011000, 0x21000);

    /* Kernel heap (0x32000–0x8FFFF); 0x90000–0xFFFFF is unused low RAM.
     * The boot kernel stack is a 16 KB BSS region inside the kernel image
     * and is already covered by the kernel-image reservation below.  Kept
     * explicit for documentation even though the first-MiB reservation
     * above already pins these pages. */
    pmm_mark_used(0x00032000, 0x000CE000);  /* 0x32000 – 0xFFFFF */

    /* VGA/ROM hole */
    pmm_mark_used(0x000A0000, 0x20000);

    {
        uint32_t fb_base;
        uint32_t fb_len;

        if (pmm_multiboot_framebuffer_range(mbi, &fb_base, &fb_len) == 0)
            pmm_mark_used(fb_base, fb_len);
    }

    for (uint32_t page = 0; page < PMM_MAX_PAGES; page++) {
        if (bitmap_test(page))
            refcount[page] = 0xFF;
    }
}

#ifdef KTEST_ENABLED
int pmm_multiboot_framebuffer_range_for_test(const multiboot_info_t *mbi,
                                             uint32_t *base_out,
                                             uint32_t *length_out)
{
    return pmm_multiboot_framebuffer_range(mbi, base_out, length_out);
}
#endif

uint32_t pmm_alloc_page(void) {
    for (uint32_t i = 0; i < PMM_MAX_PAGES / 8; i++) {
        if (bitmap[i] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(bitmap[i] & (1u << bit))) {
                    uint32_t page = i * 8 + (uint32_t)bit;
                    bitmap[i] |= (1u << bit);
                    refcount[page] = 1;
                    return page * PAGE_SIZE;
                }
            }
        }
    }
    return 0;   /* out of memory */
}

void pmm_free_page(uint32_t phys_addr) {
    pmm_decref(phys_addr);
}

void pmm_incref(uint32_t phys_addr)
{
    uint32_t page = phys_addr / PAGE_SIZE;
    if (page >= PMM_MAX_PAGES || refcount[page] == 0xFF)
        return;
    if (refcount[page] == 0)
        bitmap_set(page);
    if (refcount[page] < 0xFF)
        refcount[page]++;
}

void pmm_decref(uint32_t phys_addr)
{
    uint32_t page = phys_addr / PAGE_SIZE;
    if (page >= PMM_MAX_PAGES || refcount[page] == 0xFF || refcount[page] == 0)
        return;

    refcount[page]--;
    if (refcount[page] == 0)
        bitmap_clear(page);
}

uint8_t pmm_refcount(uint32_t phys_addr)
{
    uint32_t page = phys_addr / PAGE_SIZE;
    if (page >= PMM_MAX_PAGES)
        return 0;
    return refcount[page];
}

uint32_t pmm_free_page_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < PMM_MAX_PAGES; i++) {
        if (!bitmap_test(i))
            count++;
    }
    return count;
}
