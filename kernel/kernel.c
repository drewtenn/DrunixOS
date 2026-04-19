/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kernel.c — top-level kernel bootstrap plus low-level console and port I/O helpers.
 */

#include "pmm.h" /* also defines multiboot_info_t */
#include "paging.h"
#include "kheap.h"
#include "ata.h"
#include "blkdev.h"
#include "bcache.h"
#include "gdt.h"
#include "idt.h"
#include "sse.h"
#include "irq.h"
#include "clock.h"
#include "process.h"
#include "sched.h"
#include "fs.h"
#include "ext3.h"
#include "vfs.h"
#include "desktop.h"
#include "framebuffer.h"
#include "klog.h"
#include "kstring.h"
#include "tty.h"
#ifdef KTEST_ENABLED
#include "ktest.h"
#endif

#define VGA_CTRL_REGISTER 0x3d4
#define VGA_DATA_REGISTER 0x3d5
#define VGA_OFFSET_LOW 0x0f
#define VGA_OFFSET_HIGH 0x0e

#define VIDEO_ADDRESS 0xb8000
#define MAX_ROWS 25
#define MAX_COLS 80
#define WHITE_ON_BLACK 0x0f
#define TAB_WIDTH 8
#define FB_REQUEST_WIDTH 1024
#define FB_REQUEST_HEIGHT 768
#define FB_CELL_COLS (FB_REQUEST_WIDTH / 8)
#define FB_CELL_ROWS (FB_REQUEST_HEIGHT / 16)

#ifndef DRUNIX_ROOT_FS
#define DRUNIX_ROOT_FS "ext3"
#endif

#define SCROLLBACK_ROWS 500
#define ROW_BYTES (MAX_COLS * 2)

static char scrollback[SCROLLBACK_ROWS][ROW_BYTES];
static char shadow_vga[MAX_ROWS][ROW_BYTES]; /* mirror of the live screen */
static gui_cell_t fb_scrollback[SCROLLBACK_ROWS][FB_CELL_COLS];
static int sb_head = 0;                      /* next write slot in the ring */
static int sb_count = 0;                     /* rows stored (0..SCROLLBACK_ROWS) */
static int sb_view = 0;                      /* 0 = live; N = scrolled N rows back */
static int shadow_cursor = 0;                /* "true" cursor byte-offset */
static int console_cols = MAX_COLS;
static int console_rows = MAX_ROWS;

static gui_cell_t boot_vga_cells[MAX_ROWS * MAX_COLS];
static gui_cell_t boot_fb_cells[FB_CELL_COLS * FB_CELL_ROWS];
static framebuffer_info_t boot_framebuffer;
/*
 * Off-screen back buffer used to double-buffer the framebuffer-mode
 * desktop. Sized for the requested boot resolution so we can composite the
 * desktop, windows, taskbar, and cursor overlay off-screen and flush only
 * dirty rects to video memory. Living in .bss costs no ELF bytes — just the
 * runtime .bss allocation — and is marked reserved by the PMM via the
 * _kernel_end bump, so other code won't step on it.
 */
static uint32_t boot_fb_back_buffer[FB_REQUEST_WIDTH * FB_REQUEST_HEIGHT]
    __attribute__((aligned(16)));
static gui_display_t boot_display;
static desktop_state_t boot_desktop;
static int boot_nodesktop;
static int boot_vgatext;
static framebuffer_info_t *legacy_console_fb;
static int legacy_console_cursor_offset = -1;

static unsigned char current_color = WHITE_ON_BLACK;
static int ansi_state = 0;
static int ansi_val = 0;
static int ansi_params[4];
static int ansi_param_count = 0;
static int ansi_private = 0;

void print_string(char *string);
void print_bytes(const char *buf, int n);
void set_cursor(int offset);
void clear_screen();
void set_char_at_video_memory(char character, int offset);
int get_offset(int col, int row);
void scrollback_init(void);
void scroll_up(int n);
void scroll_down(int n);

static int console_putc_at(int offset, char c);
static void legacy_console_set_framebuffer(framebuffer_info_t *fb);
static void legacy_console_disable_framebuffer(void);
static void boot_parse_cmdline(const char *cmdline,
                               int *nodesktop,
                               int *vgatext);
static uint8_t legacy_console_handoff_attr(char ch, uint8_t attr);
static void boot_register_block_devices(void);

extern void pit_init(void);
extern void keyboard_init(void);
extern int mouse_init(void);
#ifdef DOUBLE_FAULT_TEST
extern void trigger_double_fault(void);
#endif

static int boot_framebuffer_grid(const framebuffer_info_t *fb,
                                 int *cols,
                                 int *rows)
{
    if (!fb || !cols || !rows)
        return 0;
    if (fb->cell_cols == 0 || fb->cell_rows == 0)
        return 0;

    *cols = fb->cell_cols > FB_CELL_COLS
        ? FB_CELL_COLS
        : (int)fb->cell_cols;
    *rows = fb->cell_rows > FB_CELL_ROWS
        ? FB_CELL_ROWS
        : (int)fb->cell_rows;
    return 1;
}

static void boot_register_block_devices(void)
{
    int sda_idx;
    int sdb_idx;

    ata_register();
    sda_idx = blkdev_find_index("sda");
    sdb_idx = blkdev_find_index("sdb");
    if (sda_idx >= 0)
        blkdev_scan_mbr((uint32_t)sda_idx);
    if (sdb_idx >= 0)
        blkdev_scan_mbr((uint32_t)sdb_idx);
}

static int boot_map_framebuffer(const framebuffer_info_t *fb)
{
    uint64_t visible_row_bytes;
    uint64_t last_row_offset;
    uint64_t byte_len;

    if (!fb || fb->height == 0)
        return -1;

    visible_row_bytes = (uint64_t)fb->width * 4u;
    last_row_offset = (uint64_t)(fb->height - 1u) * fb->pitch;
    byte_len = last_row_offset + visible_row_bytes;
    if (byte_len == 0 || byte_len > UINT32_MAX)
        return -1;

    if (paging_identity_map_kernel_range((uint32_t)fb->address,
                                         (uint32_t)byte_len,
                                         PG_PRESENT | PG_WRITABLE) != 0)
        return -1;

    /*
     * Mark the visible framebuffer as write-combining so back→front
     * flushes coalesce stores into burst transactions. On CPUs without
     * PAT support we silently keep whatever cacheability the BIOS set —
     * drawing still works, just more slowly. We don't treat that as a
     * fatal error.
     */
    if (paging_mark_range_write_combining((uint32_t)fb->address,
                                          (uint32_t)byte_len) == 0)
        klog("BOOT", "framebuffer mapped write-combining");
    else
        klog("BOOT", "framebuffer WC mapping unavailable");
    return 0;
}

static void boot_parse_cmdline(const char *cmdline,
                               int *nodesktop,
                               int *vgatext)
{
    const char *p = cmdline;

    if (!p)
        return;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        uint32_t len = (uint32_t)(p - tok);
        if (nodesktop && len == 9 &&
            k_memcmp(tok, "nodesktop", 9) == 0)
            *nodesktop = 1;
        if (vgatext && len == 7 &&
            k_memcmp(tok, "vgatext", 7) == 0)
            *vgatext = 1;
    }
}

#ifdef KTEST_ENABLED
void boot_parse_cmdline_for_test(const char *cmdline,
                                 int *nodesktop,
                                 int *vgatext)
{
    if (nodesktop)
        *nodesktop = 0;
    if (vgatext)
        *vgatext = 0;
    boot_parse_cmdline(cmdline, nodesktop, vgatext);
}
#endif

static void legacy_console_vga_color(uint8_t color,
                                     uint8_t *r,
                                     uint8_t *g,
                                     uint8_t *b)
{
    static const uint8_t palette[16][3] = {
        {0x06,0x08,0x12}, {0x16,0x2a,0x4f},
        {0x1f,0x6f,0x54}, {0x27,0x8d,0x95},
        {0x84,0x2f,0x3a}, {0x7c,0x3f,0x8f},
        {0xb8,0x74,0x2a}, {0xc8,0xd1,0xd9},
        {0x4d,0x5b,0x6a}, {0x4a,0x78,0xc2},
        {0x67,0xc5,0x8f}, {0x6f,0xd6,0xd2},
        {0xe0,0x6c,0x75}, {0xc6,0x78,0xdd},
        {0xf2,0xc9,0x4c}, {0xf6,0xf1,0xde},
    };

    *r = palette[color & 0x0f][0];
    *g = palette[color & 0x0f][1];
    *b = palette[color & 0x0f][2];
}

static void legacy_console_render_cell(int col,
                                       int row,
                                       unsigned char ch,
                                       uint8_t attr)
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint32_t fg;
    uint32_t bg;

    if (!legacy_console_fb)
        return;
    if (col < 0 || row < 0 || col >= console_cols || row >= console_rows)
        return;

    legacy_console_vga_color(attr & 0x0f, &r, &g, &b);
    fg = framebuffer_pack_rgb(legacy_console_fb, r, g, b);
    legacy_console_vga_color((attr >> 4) & 0x0f, &r, &g, &b);
    bg = framebuffer_pack_rgb(legacy_console_fb, r, g, b);
    framebuffer_draw_glyph(legacy_console_fb,
                           col * (int)GUI_FONT_W,
                           row * (int)GUI_FONT_H,
                           ch, fg, bg);
}

static void legacy_console_render_cursor_at(int offset)
{
    int cell;
    int row;
    int col;
    uint32_t color;

    if (!legacy_console_fb || sb_view != 0 || offset < 0)
        return;

    cell = offset / 2;
    row = cell / console_cols;
    col = cell % console_cols;
    if (col < 0 || row < 0 || col >= console_cols || row >= console_rows)
        return;

    color = framebuffer_pack_rgb(legacy_console_fb, 0x67, 0xc5, 0x8f);
    framebuffer_fill_rect(legacy_console_fb,
                          col * (int)GUI_FONT_W,
                          row * (int)GUI_FONT_H + (int)GUI_FONT_H - 2,
                          (int)GUI_FONT_W, 2, color);
}

static void legacy_console_render_shadow_cell_at_offset(int offset)
{
    int cell;
    int row;
    int col;
    gui_cell_t *fb_cell;

    if (!legacy_console_fb || offset < 0)
        return;

    cell = offset / 2;
    row = cell / console_cols;
    col = cell % console_cols;
    if (col < 0 || row < 0 || col >= console_cols || row >= console_rows)
        return;

    fb_cell = &boot_fb_cells[row * console_cols + col];
    legacy_console_render_cell(col, row, (unsigned char)fb_cell->ch,
                               fb_cell->attr);
}

static void legacy_console_render_shadow(void)
{
    for (int row = 0; row < console_rows; row++) {
        for (int col = 0; col < console_cols; col++) {
            gui_cell_t *cell = &boot_fb_cells[row * console_cols + col];
            legacy_console_render_cell(col, row, (unsigned char)cell->ch,
                                       cell->attr);
        }
    }
    legacy_console_render_cursor_at(shadow_cursor);
}

static char legacy_console_handoff_char(char ch)
{
    unsigned char uch = (unsigned char)ch;

    if (uch < 0x20 || uch > 0x7e)
        return ' ';
    return ch;
}

static uint8_t legacy_console_handoff_attr(char ch, uint8_t attr)
{
    (void)ch;
    (void)attr;
    return WHITE_ON_BLACK;
}

static void legacy_console_set_framebuffer(framebuffer_info_t *fb)
{
    uint32_t bg;
    int cols;
    int rows;
    int old_cursor_cell;
    int old_cursor_row;
    int old_cursor_col;
    int copy_limit;

    legacy_console_fb = fb;
    if (!legacy_console_fb) {
        console_cols = MAX_COLS;
        console_rows = MAX_ROWS;
        legacy_console_cursor_offset = -1;
        return;
    }

    cols = fb->cell_cols ? (int)fb->cell_cols
                         : (int)(fb->width / GUI_FONT_W);
    rows = fb->cell_rows ? (int)fb->cell_rows
                         : (int)(fb->height / GUI_FONT_H);
    if (cols <= 0 || rows <= 0) {
        legacy_console_disable_framebuffer();
        return;
    }
    if (cols > FB_CELL_COLS)
        cols = FB_CELL_COLS;
    if (rows > FB_CELL_ROWS)
        rows = FB_CELL_ROWS;

    old_cursor_cell = shadow_cursor / 2;
    if (old_cursor_cell < 0)
        old_cursor_cell = 0;
    if (old_cursor_cell > MAX_COLS * MAX_ROWS)
        old_cursor_cell = MAX_COLS * MAX_ROWS;
    copy_limit = old_cursor_cell;
    old_cursor_row = old_cursor_cell / MAX_COLS;
    old_cursor_col = old_cursor_cell % MAX_COLS;
    if (old_cursor_row >= rows) {
        old_cursor_row = rows - 1;
        old_cursor_col = 0;
    } else if (old_cursor_col >= cols) {
        old_cursor_col = cols - 1;
    }

    console_cols = cols;
    console_rows = rows;
    shadow_cursor = get_offset(old_cursor_col, old_cursor_row);
    legacy_console_cursor_offset = shadow_cursor;
    sb_head = 0;
    sb_count = 0;
    sb_view = 0;

    for (int row = 0; row < console_rows; row++) {
        for (int col = 0; col < console_cols; col++) {
            gui_cell_t *cell = &boot_fb_cells[row * console_cols + col];
            int old_cell = row * MAX_COLS + col;
            if (row < MAX_ROWS && col < MAX_COLS &&
                old_cell < copy_limit) {
                cell->ch = legacy_console_handoff_char(
                    shadow_vga[row][col * 2]);
                cell->attr = legacy_console_handoff_attr(
                    cell->ch, (uint8_t)shadow_vga[row][col * 2 + 1]);
            } else {
                cell->ch = ' ';
                cell->attr = WHITE_ON_BLACK;
            }
        }
    }

    bg = framebuffer_pack_rgb(legacy_console_fb, 0x06, 0x08, 0x12);
    framebuffer_fill_rect(legacy_console_fb, 0, 0,
                          (int)legacy_console_fb->width,
                          (int)legacy_console_fb->height, bg);
    legacy_console_render_shadow();
}

static void legacy_console_disable_framebuffer(void)
{
    legacy_console_fb = 0;
    legacy_console_cursor_offset = -1;
    console_cols = MAX_COLS;
    console_rows = MAX_ROWS;
    if (shadow_cursor >= MAX_COLS * MAX_ROWS * 2)
        shadow_cursor = (MAX_COLS * MAX_ROWS - 1) * 2;
}

#ifdef KTEST_ENABLED
void legacy_console_set_framebuffer_for_test(framebuffer_info_t *fb)
{
    legacy_console_set_framebuffer(fb);
}

void legacy_console_disable_framebuffer_for_test(void)
{
    legacy_console_disable_framebuffer();
}

void legacy_console_seed_shadow_for_test(int row,
                                         int col,
                                         char ch,
                                         uint8_t attr)
{
    if (row < 0 || row >= MAX_ROWS || col < 0 || col >= MAX_COLS)
        return;
    shadow_vga[row][col * 2] = ch;
    shadow_vga[row][col * 2 + 1] = (char)attr;
}

char legacy_console_shadow_char_for_test(int row, int col)
{
    if (row < 0 || row >= console_rows || col < 0 || col >= console_cols)
        return '\0';
    if (legacy_console_fb)
        return boot_fb_cells[row * console_cols + col].ch;
    return shadow_vga[row][col * 2];
}

uint8_t legacy_console_shadow_attr_for_test(int row, int col)
{
    if (row < 0 || row >= console_rows || col < 0 || col >= console_cols)
        return 0;
    if (legacy_console_fb)
        return boot_fb_cells[row * console_cols + col].attr;
    return (uint8_t)shadow_vga[row][col * 2 + 1];
}
#endif

#ifdef KTEST_ENABLED
int boot_framebuffer_grid_for_test(const framebuffer_info_t *fb,
                                   int *cols,
                                   int *rows)
{
    return boot_framebuffer_grid(fb, cols, rows);
}
#endif

void start_kernel(uint32_t magic, multiboot_info_t *mbi)
{
    (void)magic; /* could validate == 0x2BADB002 for extra safety */

    klog("BOOT", "kernel entry");
    klog_hex("BOOT", "multiboot magic", magic);
    klog_hex("BOOT", "multiboot info", (uint32_t)mbi);

    sse_init();
    klog("BOOT", "sse state initialized");

    /* Save flags before pmm_init, which writes the bitmap at 0x10000 and
     * may overwrite mbi if GRUB placed it at the same address. The cmdline
     * string also lives in bootloader memory and must be scanned here. */
    uint32_t mbi_flags = mbi ? mbi->flags : 0;
#ifdef DRUNIX_VGA_TEXT
    boot_nodesktop = 1;
    boot_vgatext = 1;
#endif
    if (mbi && (mbi_flags & MULTIBOOT_FLAG_CMDLINE) && mbi->cmdline)
        boot_parse_cmdline((const char *)mbi->cmdline,
                           &boot_nodesktop,
                           &boot_vgatext);
    int have_boot_framebuffer =
        !boot_vgatext &&
        framebuffer_info_from_multiboot(mbi, &boot_framebuffer) == 0;
    if (boot_vgatext)
        klog("BOOT", "VGA text console requested");

    klog("BOOT", "initializing memory managers");
    pmm_init(mbi);
    paging_init();
    if (have_boot_framebuffer && boot_map_framebuffer(&boot_framebuffer) != 0) {
        klog("BOOT", "framebuffer map failed, using VGA fallback");
        have_boot_framebuffer = 0;
    }
    kheap_init();
    klog_uint("HEAP", "after kheap_init", kheap_free_bytes());
    scrollback_init();
    klog_uint("HEAP", "after scrollback_init", kheap_free_bytes());

    klog_hex("PMM", "mbi flags", mbi_flags);
    klog_uint("PMM", "free pages", pmm_free_page_count());
    klog("BOOT", "memory managers ready");

    /*
     * Rebuild the GDT with ring-3 user segments and a TSS.
     * Must happen before idt_init_early() so that the kernel code and data
     * selectors (0x08, 0x10) remain at the same indices as before —
     * the ISR trampolines in isr.asm hard-code 0x10 for the data reload.
     */
    gdt_init();
    klog("GDT", "user segments + TSS installed");
    idt_init_early();
    klog("IDT", "interrupt descriptor table loaded");
    klog("BOOT", "descriptor tables initialized");

    klog("BOOT", "bringing up interrupt, timer, and clock subsystems");
    irq_dispatch_init();
    pit_init();
    clock_init();
    klog("IRQ", "PIT handler registered");

    ata_init();
    boot_register_block_devices();
    klog("ATA", "disk initialized");

    bcache_init();

    tty_init();
    klog("TTY", "tty0 initialized");
    keyboard_init();
    klog("IRQ", "keyboard handler registered");
    int mouse_rc = mouse_init();
    if (mouse_rc == 0)
        klog("IRQ", "mouse handler registered");
    else
        klog_uint("IRQ", "mouse init failed rc", (uint32_t)(-mouse_rc));
    klog("BOOT", "console and input devices ready");

#ifdef KTEST_ENABLED
    klog("BOOT", "running kernel unit tests");
    ktest_run_all();
    klog("BOOT", "kernel unit tests complete");
    blkdev_reset();
    boot_register_block_devices();
    bcache_init();
#endif

    klog("BOOT", "registering DUFS");
    dufs_register();
    klog("BOOT", "registering EXT3");
    ext3_register();

    /*
     * Enable hardware interrupts only after the IRQ dispatch table and early
     * device handlers are registered. The IDT itself was already loaded above
     * so early breakpoints and traps have a valid destination.
     */
    interrupts_enable();
    klog("IDT", "interrupts enabled");
    klog("BOOT", "interrupt descriptor table live");

#ifdef DOUBLE_FAULT_TEST
    klog("TEST", "triggering double fault");
    trigger_double_fault();
#endif

    klog("BOOT", "mounting root namespace");
    if (vfs_mount_with_source("/", DRUNIX_ROOT_FS, "/dev/sda1") != 0)
    {
        klog("FS", "mount failed");
        for (;;)
            __asm__ volatile("hlt");
    }
    klog("FS", "root mounted");

    if (k_strcmp(DRUNIX_ROOT_FS, "ext3") == 0) {
        if (vfs_mount_with_source("/dufs", "dufs", "/dev/sdb1") != 0)
            klog("FS", "dufs mount at /dufs failed");
        else
            klog("FS", "dufs mounted at /dufs");
    }

    if (vfs_mount("/dev", "devfs") != 0)
    {
        klog("FS", "devfs mount failed");
        for (;;)
            __asm__ volatile("hlt");
    }
    klog("FS", "devfs mounted at /dev");

    if (vfs_mount("/proc", "procfs") != 0)
    {
        klog("FS", "procfs mount failed");
        for (;;)
            __asm__ volatile("hlt");
    }
    klog("FS", "procfs mounted at /proc");

    if (vfs_mount_with_source("/sys", "sysfs", "sysfs") != 0)
    {
        klog("FS", "sysfs mount failed");
        for (;;)
            __asm__ volatile("hlt");
    }
    klog("FS", "sysfs mounted at /sys");
    klog("BOOT", "namespace mounted");

    /* Initialise the process scheduler */
    klog("BOOT", "initializing scheduler");
    sched_init();
    klog("BOOT", "scheduler ready");

    klog("BOOT", "initializing desktop");
    gui_cell_t *cells = boot_vga_cells;
    int cols = MAX_COLS;
    int rows = MAX_ROWS;

    if (have_boot_framebuffer &&
        boot_framebuffer_grid(&boot_framebuffer, &cols, &rows)) {
        cells = boot_fb_cells;
    } else {
        have_boot_framebuffer = 0;
    }

    gui_display_init(&boot_display, cells, cols, rows, WHITE_ON_BLACK);
#ifdef DRUNIX_NO_DESKTOP
    int desktop_requested = 0;
#else
    int desktop_requested = !boot_nodesktop;
#endif

    if (!desktop_requested && have_boot_framebuffer) {
        legacy_console_set_framebuffer(&boot_framebuffer);
        klog("BOOT", "legacy console framebuffer enabled");
        klog_uint("BOOT", "framebuffer console cols",
                  (uint32_t)console_cols);
        klog_uint("BOOT", "framebuffer console rows",
                  (uint32_t)console_rows);
    }

#ifdef DRUNIX_NO_DESKTOP
    klog("BOOT", "desktop disabled at build time (DRUNIX_NO_DESKTOP)");
#else
    if (boot_nodesktop)
        klog("BOOT", "desktop disabled via cmdline (nodesktop)");
#endif
#ifdef DRUNIX_VGA_TEXT
    klog("BOOT", "VGA text console forced at build time (DRUNIX_VGA_TEXT)");
#else
    if (boot_vgatext)
        klog("BOOT", "VGA text console forced via cmdline (vgatext)");
#endif
    if (desktop_requested)
        desktop_init(&boot_desktop, &boot_display);
    if (desktop_requested && desktop_is_active()) {
        if (!have_boot_framebuffer) {
            desktop_set_presentation_target(&boot_desktop, VIDEO_ADDRESS);
            klog("BOOT", "desktop VGA fallback enabled");
        } else {
            desktop_set_framebuffer_target(&boot_desktop, &boot_framebuffer);
            /*
             * Attach the off-screen back buffer. The framebuffer primitives
             * will transparently draw into it and we'll flush dirty rects
             * to the visible framebuffer via framebuffer_present_rect().
             * If the firmware gave us a framebuffer larger than we reserved
             * space for, attach fails and we fall back to direct drawing —
             * which still works but without the flicker-free guarantees.
             */
            if (framebuffer_attach_back_buffer(&boot_framebuffer,
                                               boot_fb_back_buffer,
                                               FB_REQUEST_WIDTH * 4u,
                                               sizeof(boot_fb_back_buffer))
                == 0) {
                klog("BOOT", "desktop framebuffer back buffer attached");
            } else {
                klog("BOOT",
                     "desktop framebuffer back buffer unavailable, "
                     "drawing direct");
            }
            klog("BOOT", "desktop framebuffer enabled");
            klog_uint("BOOT", "framebuffer desktop cols",
                      (uint32_t)cols);
            klog_uint("BOOT", "framebuffer desktop rows",
                      (uint32_t)rows);
        }
        desktop_open_shell_window(&boot_desktop);
        desktop_render(&boot_desktop);
        klog("BOOT", "desktop enabled");
    } else {
        klog("BOOT", "desktop unavailable, using legacy console");
    }

#ifndef DRUNIX_INIT_PROGRAM
#define DRUNIX_INIT_PROGRAM "bin/shell"
#endif
#ifndef DRUNIX_INIT_ARG0
#define DRUNIX_INIT_ARG0 "shell"
#endif
#ifndef DRUNIX_INIT_ENV0
#define DRUNIX_INIT_ENV0 "PATH=/bin"
#endif

    /* Look up the initial user executable in the filesystem. */
    klog("BOOT", "locating initial program");
    vfs_file_ref_t shell_ref;
    uint32_t elf_size;
    if (vfs_open_file(DRUNIX_INIT_PROGRAM, &shell_ref, &elf_size) != 0)
    {
        klog("FS", "initial program not found");
        for (;;)
            __asm__ volatile("hlt");
    }
    klog_uint("FS", "initial program inode", shell_ref.inode_num);
    klog_uint("FS", "initial program size", elf_size);

    /* Create the initial process and register it with the scheduler.
     * Pass a one-element argv so argv[0] follows the later exec convention. */
    static const char *shell_argv[] = {DRUNIX_INIT_ARG0};
    static const char *shell_envp[] = {DRUNIX_INIT_ENV0};
    static process_t proc;
    klog_uint("HEAP", "before process_create", kheap_free_bytes());
    int rc = process_create_file(&proc, shell_ref,
                                 shell_argv, 1, shell_envp, 1, 0);
    klog_uint("HEAP", "after process_create", kheap_free_bytes());
    if (rc != 0)
    {
        klog_uint("PROC", "process_create failed, code", (uint32_t)(-rc));
        for (;;)
            __asm__ volatile("hlt");
    }
    if (sched_add(&proc) < 0)
    {
        klog("PROC", "sched_add failed");
        for (;;)
            __asm__ volatile("hlt");
    }
    klog_uint("PROC", "initial process pid", proc.pid);
    if (desktop_is_active()) {
        desktop_attach_shell_process(&boot_desktop, proc.pid, proc.pgid);
        desktop_render(&boot_desktop);
    }

    /*
     * Bootstrap: promote the shell to RUNNING and launch it.
     * sched_bootstrap() finds the first READY process in the table,
     * marks it RUNNING, and returns a pointer to it.  We pass that
     * pointer to process_launch(), which sets TSS.ESP0, switches CR3,
     * restores FPU, and irets to ring 3.  It does not return.
     * After the iret the scheduler takes over on each timer IRQ0.
     */
    process_t *shell = sched_bootstrap();
    if (!shell)
    {
        klog("PROC", "sched_bootstrap failed");
        for (;;)
            __asm__ volatile("hlt");
    }

    klog("PROC", "entering ring 3");
    klog_uint("PROC", "bootstrap pid", shell->pid);
    process_launch(shell); /* does not return */
}

void clear_screen()
{
    sb_view = 0;
    for (int i = 0; i < console_cols * console_rows; ++i)
    {
        set_char_at_video_memory(' ', i * 2);
    }
    set_cursor(get_offset(0, 0));
}


int scroll_ln(int offset)
{
    if (legacy_console_fb) {
        k_memcpy(fb_scrollback[sb_head], boot_fb_cells,
                 (uint32_t)console_cols * sizeof(gui_cell_t));
        sb_head = (sb_head + 1) % SCROLLBACK_ROWS;
        if (sb_count < SCROLLBACK_ROWS)
            sb_count++;

        k_memmove(boot_fb_cells, boot_fb_cells + console_cols,
                  (uint32_t)console_cols * (console_rows - 1) *
                      sizeof(gui_cell_t));
        for (int col = 0; col < console_cols; col++) {
            gui_cell_t *cell =
                &boot_fb_cells[(console_rows - 1) * console_cols + col];
            cell->ch = ' ';
            cell->attr = WHITE_ON_BLACK;
        }
        legacy_console_render_shadow();
    } else {
        k_memcpy(scrollback[sb_head], shadow_vga[0], ROW_BYTES);
        sb_head = (sb_head + 1) % SCROLLBACK_ROWS;
        if (sb_count < SCROLLBACK_ROWS)
            sb_count++;

        k_memcpy(shadow_vga[0], shadow_vga[1], (MAX_ROWS - 1) * ROW_BYTES);
        for (int col = 0; col < MAX_COLS; col++)
        {
            shadow_vga[MAX_ROWS - 1][col * 2] = ' ';
            shadow_vga[MAX_ROWS - 1][col * 2 + 1] = WHITE_ON_BLACK;
        }

        k_memcpy(
            (char *)(get_offset(0, 0) + VIDEO_ADDRESS),
            (char *)(get_offset(0, 1) + VIDEO_ADDRESS),
            MAX_COLS * (MAX_ROWS - 1) * 2);

        for (int col = 0; col < MAX_COLS; col++)
        {
            unsigned char *vidmem = (unsigned char *)VIDEO_ADDRESS;
            vidmem[get_offset(col, MAX_ROWS - 1)] = ' ';
            vidmem[get_offset(col, MAX_ROWS - 1) + 1] = WHITE_ON_BLACK;
        }
    }

    return offset - 2 * console_cols;
}

unsigned char port_byte_in(unsigned short port)
{
    unsigned char result;
    __asm__("in %%dx, %%al" : "=a"(result) : "d"(port));
    return result;
}

void port_byte_out(unsigned short port, unsigned char data)
{
    __asm__("out %%al, %%dx" : : "a"(data), "d"(port));
}

int get_row_from_offset(int offset)
{
    return offset / (2 * console_cols);
}

int get_offset(int col, int row)
{
    return 2 * (row * console_cols + col);
}

int move_offset_to_new_line(int offset)
{
    return get_offset(0, get_row_from_offset(offset) + 1);
}

static void set_hw_cursor(int offset)
{
    offset /= 2;
    port_byte_out(VGA_CTRL_REGISTER, VGA_OFFSET_HIGH);
    port_byte_out(VGA_DATA_REGISTER, (unsigned char)(offset >> 8));
    port_byte_out(VGA_CTRL_REGISTER, VGA_OFFSET_LOW);
    port_byte_out(VGA_DATA_REGISTER, (unsigned char)(offset & 0xff));
}

void set_cursor(int offset)
{
    if (legacy_console_fb && legacy_console_cursor_offset >= 0)
        legacy_console_render_shadow_cell_at_offset(
            legacy_console_cursor_offset);

    shadow_cursor = offset;
    legacy_console_cursor_offset = offset;
    if (sb_view == 0)
    {
        if (legacy_console_fb)
            legacy_console_render_cursor_at(offset);
        else
            set_hw_cursor(offset);
    }
}

int get_cursor()
{
    if (legacy_console_fb)
        return shadow_cursor;

    port_byte_out(VGA_CTRL_REGISTER, VGA_OFFSET_HIGH);
    int offset = port_byte_in(VGA_DATA_REGISTER) << 8;
    port_byte_out(VGA_CTRL_REGISTER, VGA_OFFSET_LOW);
    offset += port_byte_in(VGA_DATA_REGISTER);
    return offset * 2;
}

/* ── scrollback ─────────────────────────────────────────────────────────── */

void scrollback_init(void)
{
    /* Capture whatever is already on screen (pre-init klog output). */
    unsigned char *vidmem = (unsigned char *)VIDEO_ADDRESS;
    for (int r = 0; r < MAX_ROWS; r++)
    {
        for (int b = 0; b < ROW_BYTES; b++)
        {
            shadow_vga[r][b] = (char)vidmem[get_offset(0, r) + b];
        }
    }
    shadow_cursor = get_cursor();
}

/*
 * Repaint the hardware VGA buffer from history + shadow_vga according to
 * sb_view.  When sb_view == 0 the display is identical to shadow_vga.
 */
static void redraw_screen(void)
{
    unsigned char *vidmem = (unsigned char *)VIDEO_ADDRESS;
    for (int r = 0; r < console_rows; r++)
    {
        int logical = sb_count - sb_view + r;

        if (logical < 0)
        {
            /* Before recorded history — write blank row. */
            for (int c = 0; c < console_cols; c++)
            {
                if (legacy_console_fb) {
                    legacy_console_render_cell(c, r, ' ', WHITE_ON_BLACK);
                } else {
                    vidmem[get_offset(c, r)] = ' ';
                    vidmem[get_offset(c, r) + 1] = WHITE_ON_BLACK;
                }
            }
            continue;
        }

        if (legacy_console_fb) {
            gui_cell_t *src;
            if (logical < sb_count) {
                int idx = (sb_head - sb_count + logical + SCROLLBACK_ROWS) %
                          SCROLLBACK_ROWS;
                src = fb_scrollback[idx];
            } else {
                int shadow_row = logical - sb_count;
                src = &boot_fb_cells[shadow_row * console_cols];
            }
            for (int c = 0; c < console_cols; c++)
                legacy_console_render_cell(c, r, (unsigned char)src[c].ch,
                                           src[c].attr);
        } else {
            char *src;
            if (logical < sb_count) {
                int idx = (sb_head - sb_count + logical + SCROLLBACK_ROWS) %
                          SCROLLBACK_ROWS;
                src = scrollback[idx];
            } else {
                int shadow_row = logical - sb_count;
                src = shadow_vga[shadow_row];
            }
            k_memcpy((char *)(vidmem + get_offset(0, r)), src, ROW_BYTES);
        }
    }

    if (sb_view > 0)
    {
        /* Push cursor off-screen to hide it during scrollback. */
        if (!legacy_console_fb)
            set_hw_cursor(get_offset(0, MAX_ROWS));
    }
    else
    {
        if (legacy_console_fb)
            legacy_console_render_cursor_at(shadow_cursor);
        else
            set_hw_cursor(shadow_cursor);
    }
}

void scroll_up(int n)
{
    int max_view = sb_count;
    sb_view += n;
    if (sb_view > max_view)
        sb_view = max_view;
    redraw_screen();
}

void scroll_down(int n)
{
    sb_view -= n;
    if (sb_view < 0)
        sb_view = 0;
    redraw_screen();
}

/* ─────────────────────────────────────────────────────────────────────────── */

void set_char_at_video_memory(char character, int offset)
{
    int cell = offset / 2;
    int row = cell / console_cols;
    int col = cell % console_cols;

    if (col < 0 || row < 0 || col >= console_cols || row >= console_rows)
        return;

    if (legacy_console_fb) {
        gui_cell_t *fb_cell = &boot_fb_cells[row * console_cols + col];
        fb_cell->ch = character;
        fb_cell->attr = current_color;
        legacy_console_render_cell(col, row, (unsigned char)character,
                                   current_color);
        return;
    }

    shadow_vga[row][col * 2] = character;
    shadow_vga[row][col * 2 + 1] = current_color;

    unsigned char *vidmem = (unsigned char *)VIDEO_ADDRESS;
    vidmem[offset] = character;
    vidmem[offset + 1] = current_color;
}

static int console_putc_at(int offset, char c)
{
    if (offset >= console_rows * console_cols * 2)
        offset = scroll_ln(offset);

    if (c == '\n')
        return move_offset_to_new_line(offset);

    if (c == '\r')
        return get_offset(0, (offset / 2) / console_cols);

    if (c == '\b')
    {
        offset = (offset >= 2) ? offset - 2 : 0;
        set_char_at_video_memory(' ', offset);
        return offset;
    }

    if (c == '\t')
    {
        int col = (offset / 2) % console_cols;
        int spaces = TAB_WIDTH - (col % TAB_WIDTH);
        for (int i = 0; i < spaces; i++)
        {
            if (offset >= console_rows * console_cols * 2)
                offset = scroll_ln(offset);
            set_char_at_video_memory(' ', offset);
            offset += 2;
        }
        return offset;
    }

    set_char_at_video_memory(c, offset);
    return offset + 2;
}

static int console_csi_param(int index, int fallback)
{
    if (index < 0 || index >= ansi_param_count)
        return fallback;
    if (ansi_params[index] == 0)
        return fallback;
    return ansi_params[index];
}

static int console_move_cursor_to(int col, int row)
{
    if (col < 0)
        col = 0;
    if (row < 0)
        row = 0;
    if (col >= console_cols)
        col = console_cols - 1;
    if (row >= console_rows)
        row = console_rows - 1;
    return get_offset(col, row);
}

static void console_clear_line_from(int row, int col)
{
    unsigned char old_color = current_color;

    if (row < 0 || row >= console_rows)
        return;
    if (col < 0)
        col = 0;
    if (col >= console_cols)
        return;

    current_color = WHITE_ON_BLACK;
    for (int x = col; x < console_cols; x++)
        set_char_at_video_memory(' ', get_offset(x, row));
    current_color = old_color;
}

static void console_clear_screen_from(int offset)
{
    int cell = offset / 2;
    int row = cell / console_cols;
    int col = cell % console_cols;

    console_clear_line_from(row, col);
    for (int y = row + 1; y < console_rows; y++)
        console_clear_line_from(y, 0);
}

static void console_erase_chars(int offset, int count)
{
    int cell = offset / 2;
    int row = cell / console_cols;
    int col = cell % console_cols;
    unsigned char old_color = current_color;

    if (row < 0 || row >= console_rows || col < 0 || col >= console_cols)
        return;
    if (count <= 0)
        count = 1;
    if (count > console_cols - col)
        count = console_cols - col;

    current_color = WHITE_ON_BLACK;
    for (int i = 0; i < count; i++)
        set_char_at_video_memory(' ', get_offset(col + i, row));
    current_color = old_color;
}

static void console_delete_chars(int offset, int count)
{
    int cell = offset / 2;
    int row = cell / console_cols;
    int col = cell % console_cols;
    unsigned char old_color = current_color;

    if (row < 0 || row >= console_rows || col < 0 || col >= console_cols)
        return;
    if (count <= 0)
        count = 1;
    if (count > console_cols - col)
        count = console_cols - col;

    for (int x = col; x < console_cols - count; x++) {
        int from = get_offset(x + count, row);
        unsigned char ch;
        unsigned char attr;

        if (legacy_console_fb) {
            gui_cell_t *src = &boot_fb_cells[row * console_cols + x + count];
            ch = (unsigned char)src->ch;
            attr = src->attr;
        } else {
            ch = (unsigned char)shadow_vga[row][(x + count) * 2];
            attr = (unsigned char)shadow_vga[row][(x + count) * 2 + 1];
        }
        current_color = attr;
        set_char_at_video_memory((char)ch, get_offset(x, row));
        (void)from;
    }
    current_color = WHITE_ON_BLACK;
    for (int x = console_cols - count; x < console_cols; x++)
        set_char_at_video_memory(' ', get_offset(x, row));
    current_color = old_color;
}

static void console_insert_chars(int offset, int count)
{
    int cell = offset / 2;
    int row = cell / console_cols;
    int col = cell % console_cols;
    unsigned char old_color = current_color;

    if (row < 0 || row >= console_rows || col < 0 || col >= console_cols)
        return;
    if (count <= 0)
        count = 1;
    if (count > console_cols - col)
        count = console_cols - col;

    for (int x = console_cols - count - 1; x >= col; x--) {
        unsigned char ch;
        unsigned char attr;

        if (legacy_console_fb) {
            gui_cell_t *src = &boot_fb_cells[row * console_cols + x];
            ch = (unsigned char)src->ch;
            attr = src->attr;
        } else {
            ch = (unsigned char)shadow_vga[row][x * 2];
            attr = (unsigned char)shadow_vga[row][x * 2 + 1];
        }
        current_color = attr;
        set_char_at_video_memory((char)ch, get_offset(x + count, row));
    }
    current_color = WHITE_ON_BLACK;
    for (int x = col; x < col + count; x++)
        set_char_at_video_memory(' ', get_offset(x, row));
    current_color = old_color;
}

static void console_apply_ansi_color(int code)
{
    if (code == 0)
        current_color = WHITE_ON_BLACK;
    if (code == 31)
        current_color = 0x0c;
    if (code == 32)
        current_color = 0x0a;
    if (code == 33)
        current_color = 0x0e;
    if (code == 36)
        current_color = 0x0b;
    if (code == 7)
        current_color = 0x70;
    if (code == 27)
        current_color = WHITE_ON_BLACK;
}

static void console_apply_csi(int *offset, char final)
{
    int n;

    if (ansi_private) {
        ansi_private = 0;
        return;
    }

    switch (final) {
    case 'm':
        if (ansi_param_count == 0) {
            console_apply_ansi_color(0);
            break;
        }
        for (int i = 0; i < ansi_param_count; i++)
            console_apply_ansi_color(ansi_params[i]);
        break;
    case 'H':
    case 'f': {
        int row = console_csi_param(0, 1) - 1;
        int col = console_csi_param(1, 1) - 1;

        *offset = console_move_cursor_to(col, row);
        break;
    }
    case 'J':
        n = console_csi_param(0, 0);
        if (n == 0)
            console_clear_screen_from(*offset);
        else if (n == 2) {
            for (int row = 0; row < console_rows; row++)
                console_clear_line_from(row, 0);
        }
        break;
    case 'K':
        n = console_csi_param(0, 0);
        if (n == 0) {
            int cell = *offset / 2;
            console_clear_line_from(cell / console_cols, cell % console_cols);
        } else if (n == 2) {
            console_clear_line_from((*offset / 2) / console_cols, 0);
        }
        break;
    case 'A':
        n = console_csi_param(0, 1);
        *offset = console_move_cursor_to((*offset / 2) % console_cols,
                                         ((*offset / 2) / console_cols) - n);
        break;
    case 'B':
        n = console_csi_param(0, 1);
        *offset = console_move_cursor_to((*offset / 2) % console_cols,
                                         ((*offset / 2) / console_cols) + n);
        break;
    case 'C':
        n = console_csi_param(0, 1);
        *offset = console_move_cursor_to(((*offset / 2) % console_cols) + n,
                                         (*offset / 2) / console_cols);
        break;
    case 'D':
        n = console_csi_param(0, 1);
        *offset = console_move_cursor_to(((*offset / 2) % console_cols) - n,
                                         (*offset / 2) / console_cols);
        break;
    case 'X':
        n = console_csi_param(0, 1);
        console_erase_chars(*offset, n);
        break;
    case 'P':
        n = console_csi_param(0, 1);
        console_delete_chars(*offset, n);
        break;
    case '@':
        n = console_csi_param(0, 1);
        console_insert_chars(*offset, n);
        break;
    case 'r':
    case 'h':
    case 'l':
        break;
    default:
        break;
    }
}

static int console_consume_ansi(int *offset, char c)
{
    if (c == '\x0e' || c == '\x0f')
        return 1;

    if (c == '\x1b') {
        ansi_state = 1;
        ansi_val = 0;
        ansi_param_count = 0;
        ansi_private = 0;
        return 1;
    }

    if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
            ansi_param_count = 0;
            ansi_private = 0;
        } else if (c == '(' || c == ')') {
            ansi_state = 3;
        } else {
            ansi_state = 0;
        }
        return 1;
    }

    if (ansi_state == 2) {
        if (c == '?') {
            ansi_private = 1;
            return 1;
        }
        if (c >= '0' && c <= '9') {
            if (ansi_param_count == 0)
                ansi_param_count = 1;
            if (ansi_val >= 100) {
                ansi_val = 999;
            } else {
                ansi_val = (ansi_val * 10) + (c - '0');
                if (ansi_val > 999)
                    ansi_val = 999;
            }
            ansi_params[ansi_param_count - 1] = ansi_val;
            return 1;
        }
        if (c == ';') {
            if (ansi_param_count == 0)
                ansi_param_count = 1;
            if (ansi_param_count < 4) {
                ansi_param_count++;
                ansi_params[ansi_param_count - 1] = 0;
            }
            ansi_val = 0;
            return 1;
        }
        if (c >= '@' && c <= '~')
            console_apply_csi(offset, c);
        ansi_state = 0;
        ansi_val = 0;
        ansi_param_count = 0;
        ansi_private = 0;
        return 1;
    }

    if (ansi_state == 3) {
        ansi_state = 0;
        return 1;
    }

    return 0;
}

void print_string(char *string)
{
    if (sb_view > 0)
        scroll_down(sb_view); /* snap back to live view */
    int offset = get_cursor();
    int i = 0;
    while (string[i] != 0)
    {
        char c = string[i];
        if (console_consume_ansi(&offset, c)) {
            i++;
            continue;
        }

        offset = console_putc_at(offset, c);
        i++;
    }
    set_cursor(offset);
}

/*
 * print_bytes — print exactly `n` bytes starting at `buf` to the VGA console.
 *
 * Unlike print_string, this does not stop on a NUL byte, so a caller can feed
 * it raw file data (ELF images, binary blobs) without fear that an embedded
 * 0x00 will truncate the output.  \n and \b are still handled specially; every
 * other byte — including NUL — draws whatever glyph the VGA font maps to its
 * value and advances the cursor by one cell.
 */
void print_bytes(const char *buf, int n)
{
    if (n <= 0)
        return;
    if (sb_view > 0)
        scroll_down(sb_view); /* snap back to live view */
    int offset = get_cursor();
    for (int i = 0; i < n; i++)
    {
        char c = buf[i];
        if (console_consume_ansi(&offset, c))
            continue;

        offset = console_putc_at(offset, c);
    }
    set_cursor(offset);
}
