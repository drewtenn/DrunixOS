/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * kernel.c — top-level kernel bootstrap plus low-level console and port I/O helpers.
 */

#include "pmm.h" /* also defines multiboot_info_t */
#include "paging.h"
#include "kheap.h"
#include "ata.h"
#include "blkdev.h"
#include "gdt.h"
#include "idt.h"
#include "sse.h"
#include "irq.h"
#include "clock.h"
#include "process.h"
#include "sched.h"
#include "fs.h"
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

#define SCROLLBACK_ROWS 500
#define ROW_BYTES (MAX_COLS * 2)

static char scrollback[SCROLLBACK_ROWS][ROW_BYTES];
static char shadow_vga[MAX_ROWS][ROW_BYTES]; /* mirror of the live screen */
static int sb_head = 0;                      /* next write slot in the ring */
static int sb_count = 0;                     /* rows stored (0..SCROLLBACK_ROWS) */
static int sb_view = 0;                      /* 0 = live; N = scrolled N rows back */
static int shadow_cursor = 0;                /* "true" cursor byte-offset */

static gui_cell_t boot_vga_cells[MAX_ROWS * MAX_COLS];
static gui_cell_t boot_fb_cells[FB_CELL_COLS * FB_CELL_ROWS];
static framebuffer_info_t boot_framebuffer;
static gui_display_t boot_display;
static desktop_state_t boot_desktop;

static unsigned char current_color = WHITE_ON_BLACK;
static int ansi_state = 0;
static int ansi_val = 0;

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

extern void pit_init(void);
extern void keyboard_init(void);
extern int mouse_init(void);
#ifdef DOUBLE_FAULT_TEST
extern void trigger_double_fault(void);
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
     * may overwrite mbi if GRUB placed it at the same address. */
    uint32_t mbi_flags = mbi ? mbi->flags : 0;
    int have_boot_framebuffer =
        framebuffer_info_from_multiboot(mbi, &boot_framebuffer) == 0;

    klog("BOOT", "initializing memory managers");
    pmm_init(mbi);
    paging_init();
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
    ata_register();
    klog("ATA", "disk initialized");

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
#endif

    klog("BOOT", "registering DUFS");
    dufs_register();

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
    if (vfs_mount("/", "dufs") != 0)
    {
        klog("FS", "mount failed");
        for (;;)
            __asm__ volatile("hlt");
    }
    klog("FS", "DUFS mounted at /");

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
    klog("BOOT", "namespace mounted");

    /* Initialise the process scheduler */
    klog("BOOT", "initializing scheduler");
    sched_init();
    klog("BOOT", "scheduler ready");

    klog("BOOT", "initializing desktop");
    gui_cell_t *cells = have_boot_framebuffer ? boot_fb_cells : boot_vga_cells;
    int cols = have_boot_framebuffer ? (int)boot_framebuffer.cell_cols : MAX_COLS;
    int rows = have_boot_framebuffer ? (int)boot_framebuffer.cell_rows : MAX_ROWS;

    gui_display_init(&boot_display, cells, cols, rows, WHITE_ON_BLACK);
    desktop_init(&boot_desktop, &boot_display);
    if (desktop_is_active()) {
        if (!have_boot_framebuffer) {
            desktop_set_presentation_target(&boot_desktop, VIDEO_ADDRESS);
        } else {
            klog_uint("BOOT", "framebuffer desktop cols",
                      boot_framebuffer.cell_cols);
            klog_uint("BOOT", "framebuffer desktop rows",
                      boot_framebuffer.cell_rows);
        }
        desktop_open_shell_window(&boot_desktop);
        desktop_render(&boot_desktop);
        klog("BOOT", "desktop enabled");
    } else {
        klog("BOOT", "desktop unavailable, using legacy console");
    }

    /* Look up the shell executable in the filesystem. */
    klog("BOOT", "locating initial shell");
    uint32_t shell_ino, elf_size;
    if (vfs_open("bin/shell", &shell_ino, &elf_size) != 0)
    {
        klog("FS", "shell not found");
        for (;;)
            __asm__ volatile("hlt");
    }
    klog_uint("FS", "shell inode", shell_ino);
    klog_uint("FS", "shell size", elf_size);

    /* Create the shell process and register it with the scheduler.
     * Pass a one-element argv so the boot shell sees argv[0] == "shell",
     * matching the argv[0] convention used by later exec'd programs. */
    static const char *shell_argv[] = {"shell"};
    static const char *shell_envp[] = {"PATH=/bin"};
    static process_t proc;
    klog_uint("HEAP", "before process_create", kheap_free_bytes());
    int rc = process_create(&proc, shell_ino, shell_argv, 1, shell_envp, 1, 0);
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
    klog_uint("PROC", "boot shell pid", proc.pid);
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
    for (int i = 0; i < MAX_COLS * MAX_ROWS; ++i)
    {
        set_char_at_video_memory(' ', i * 2);
    }
    set_cursor(get_offset(0, 0));
}


int scroll_ln(int offset)
{
    /* Save the row being evicted from the top to the scrollback ring. */
    if (scrollback)
    {
        k_memcpy(scrollback[sb_head], shadow_vga[0], ROW_BYTES);
        sb_head = (sb_head + 1) % SCROLLBACK_ROWS;
        if (sb_count < SCROLLBACK_ROWS)
            sb_count++;
    }

    /* Scroll shadow_vga up one row. */
    k_memcpy(shadow_vga[0], shadow_vga[1], (MAX_ROWS - 1) * ROW_BYTES);
    for (int col = 0; col < MAX_COLS; col++)
    {
        shadow_vga[MAX_ROWS - 1][col * 2] = ' ';
        shadow_vga[MAX_ROWS - 1][col * 2 + 1] = WHITE_ON_BLACK;
    }

    /* Scroll VGA up one row. */
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

    return offset - 2 * MAX_COLS;
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
    return offset / (2 * MAX_COLS);
}

int get_offset(int col, int row)
{
    return 2 * (row * MAX_COLS + col);
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
    shadow_cursor = offset;
    if (sb_view == 0)
    {
        set_hw_cursor(offset);
    }
}

int get_cursor()
{
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
    for (int r = 0; r < MAX_ROWS; r++)
    {
        int logical = sb_count - sb_view + r;
        char *src = (char *)0;

        if (logical < 0)
        {
            /* Before recorded history — write blank row. */
            for (int c = 0; c < MAX_COLS; c++)
            {
                vidmem[get_offset(c, r)] = ' ';
                vidmem[get_offset(c, r) + 1] = WHITE_ON_BLACK;
            }
            continue;
        }
        else if (logical < sb_count)
        {
            /* From scrollback ring. */
            int idx = (sb_head - sb_count + logical + SCROLLBACK_ROWS) % SCROLLBACK_ROWS;
            src = scrollback[idx];
        }
        else
        {
            /* From shadow_vga. */
            int shadow_row = logical - sb_count; /* == r - sb_view */
            src = shadow_vga[shadow_row];
        }

        k_memcpy((char *)(vidmem + get_offset(0, r)), src, ROW_BYTES);
    }

    if (sb_view > 0)
    {
        /* Push cursor off-screen to hide it during scrollback. */
        set_hw_cursor(get_offset(0, MAX_ROWS));
    }
    else
    {
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
    int row = cell / MAX_COLS;
    int col = cell % MAX_COLS;
    shadow_vga[row][col * 2] = character;
    shadow_vga[row][col * 2 + 1] = current_color;

    unsigned char *vidmem = (unsigned char *)VIDEO_ADDRESS;
    vidmem[offset] = character;
    vidmem[offset + 1] = current_color;
}

static int console_putc_at(int offset, char c)
{
    if (offset >= MAX_ROWS * MAX_COLS * 2)
        offset = scroll_ln(offset);

    if (c == '\n')
        return move_offset_to_new_line(offset);

    if (c == '\r')
        return get_offset(0, (offset / 2) / MAX_COLS);

    if (c == '\b')
    {
        offset = (offset >= 2) ? offset - 2 : 0;
        set_char_at_video_memory(' ', offset);
        return offset;
    }

    if (c == '\t')
    {
        int col = (offset / 2) % MAX_COLS;
        int spaces = TAB_WIDTH - (col % TAB_WIDTH);
        for (int i = 0; i < spaces; i++)
        {
            if (offset >= MAX_ROWS * MAX_COLS * 2)
                offset = scroll_ln(offset);
            set_char_at_video_memory(' ', offset);
            offset += 2;
        }
        return offset;
    }

    set_char_at_video_memory(c, offset);
    return offset + 2;
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
        if (c == '\x1b')
        {
            ansi_state = 1;
            ansi_val = 0;
            i++;
            continue;
        }
        if (ansi_state == 1)
        {
            ansi_state = (c == '[') ? 2 : 0;
            i++;
            continue;
        }
        if (ansi_state == 2)
        {
            if (c >= '0' && c <= '9')
            {
                ansi_val = (ansi_val * 10) + (c - '0');
                i++;
                continue;
            }
            if (c == 'm')
            {
                if (ansi_val == 0)
                    current_color = WHITE_ON_BLACK;
                if (ansi_val == 31)
                    current_color = 0x0c; /* L. Red */
                if (ansi_val == 32)
                    current_color = 0x0a; /* L. Green */
                if (ansi_val == 33)
                    current_color = 0x0e; /* Yellow */
                if (ansi_val == 36)
                    current_color = 0x0b; /* L. Cyan */
                ansi_state = 0;
                i++;
                continue;
            }
            ansi_state = 0;
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
        if (c == '\x1b')
        {
            ansi_state = 1;
            ansi_val = 0;
            continue;
        }
        if (ansi_state == 1)
        {
            ansi_state = (c == '[') ? 2 : 0;
            continue;
        }
        if (ansi_state == 2)
        {
            if (c >= '0' && c <= '9')
            {
                ansi_val = (ansi_val * 10) + (c - '0');
                continue;
            }
            if (c == 'm')
            {
                if (ansi_val == 0)
                    current_color = WHITE_ON_BLACK;
                if (ansi_val == 31)
                    current_color = 0x0c;
                if (ansi_val == 32)
                    current_color = 0x0a;
                if (ansi_val == 33)
                    current_color = 0x0e;
                if (ansi_val == 36)
                    current_color = 0x0b;
                ansi_state = 0;
                continue;
            }
            ansi_state = 0;
            continue;
        }

        offset = console_putc_at(offset, c);
    }
    set_cursor(offset);
}
