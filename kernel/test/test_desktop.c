#include "ktest.h"
#include "display.h"
#include "desktop.h"
#include "desktop_apps.h"
#include "font8x16.h"
#include "framebuffer.h"
#include "kheap.h"
#include "terminal.h"
#include "kstring.h"
#include "mouse.h"
#include "paging.h"
#include "process.h"
#include "sched.h"
#include "syscall.h"
#include "tty.h"
#include <limits.h>

#ifndef MOUSE_FRAMEBUFFER_PIXEL_SCALE
#define MOUSE_FRAMEBUFFER_PIXEL_SCALE 4
#endif

#define TEST_MOUSE_FRAMEBUFFER_SCALE_MIN 1
#define TEST_MOUSE_FRAMEBUFFER_SCALE_MAX 16

#if MOUSE_FRAMEBUFFER_PIXEL_SCALE < TEST_MOUSE_FRAMEBUFFER_SCALE_MIN
#define TEST_MOUSE_EFFECTIVE_FRAMEBUFFER_SCALE TEST_MOUSE_FRAMEBUFFER_SCALE_MIN
#elif MOUSE_FRAMEBUFFER_PIXEL_SCALE > TEST_MOUSE_FRAMEBUFFER_SCALE_MAX
#define TEST_MOUSE_EFFECTIVE_FRAMEBUFFER_SCALE TEST_MOUSE_FRAMEBUFFER_SCALE_MAX
#else
#define TEST_MOUSE_EFFECTIVE_FRAMEBUFFER_SCALE MOUSE_FRAMEBUFFER_PIXEL_SCALE
#endif

static gui_cell_t desktop_cells[80 * 25];
static gui_cell_t large_desktop_cells[128 * 48];
static gui_cell_t pointer_motion_cells[60 * 25];
static uint32_t pointer_motion_pixels[480 * 400];
static gui_cell_t terminal_cells[16 * 4];
static gui_cell_t terminal_history[16 * 8];
static gui_cell_t terminal_tiny_live[2];
static gui_cell_t terminal_tiny_history[2];
static uint32_t terminal_pixels[128 * 96];

extern int syscall_console_write_for_test(process_t *proc, const char *buf,
                                          uint32_t len);
extern int boot_framebuffer_grid_for_test(const framebuffer_info_t *fb,
                                          int *cols,
                                          int *rows);

static void desktop_test_destroy(desktop_state_t *desktop)
{
    if (!desktop)
        return;
    gui_terminal_destroy(&desktop->shell_terminal);
    if (desktop->shell_cells) {
        kfree(desktop->shell_cells);
        desktop->shell_cells = 0;
    }
}

static void test_desktop_files_app_lists_root_entries(ktest_case_t *tc)
{
    static desktop_app_state_t apps;
    static const char dents[] = "alpha\0beta/\0gamma";

    desktop_apps_init(&apps);
    desktop_app_refresh_files_for_test(&apps.files, dents, sizeof(dents));

    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.files, 0),
                                   "Files: /") == 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.files, 1),
                                   "alpha") == 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.files, 2),
                                   "beta/") == 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.files, 3),
                                   "gamma") == 0);
    KTEST_EXPECT_EQ(tc, apps.files.line_count, 4);
}

static void test_desktop_help_app_has_keyboard_page(ktest_case_t *tc)
{
    static desktop_app_state_t apps;

    desktop_apps_init(&apps);
    desktop_app_refresh_help_for_test(&apps.help);

    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.help, 0),
                                   "Drunix Help") == 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.help, 1),
                                   "Keyboard") == 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.help, 2),
                                   "Esc launcher") == 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.help, 7),
                                   "Shell keeps running") == 0);
    KTEST_EXPECT_GE(tc, apps.help.line_count, 8);
}

static void test_desktop_help_app_q_requests_close(ktest_case_t *tc)
{
    static desktop_app_state_t apps;

    desktop_apps_init(&apps);
    desktop_app_refresh(&apps);

    KTEST_EXPECT_EQ(tc,
                    desktop_app_handle_key(&apps, DESKTOP_APP_HELP, 'q'),
                    DESKTOP_APP_KEY_CLOSE);
}

static void test_desktop_app_render_clips_to_content_rect(ktest_case_t *tc)
{
    static uint32_t pixels[96 * 96];
    static desktop_app_state_t apps;
    framebuffer_info_t fb;
    gui_pixel_surface_t surface;
    gui_pixel_theme_t theme;
    gui_pixel_rect_t rect;
    uint32_t outside;
    uint32_t inside;

    desktop_apps_init(&apps);
    desktop_app_refresh_help_for_test(&apps.help);
    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 96u * sizeof(uint32_t);
    fb.width = 96u;
    fb.height = 96u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;
    surface.fb = &fb;
    surface.clip.x = 18;
    surface.clip.y = 18;
    surface.clip.w = 8;
    surface.clip.h = 8;
    k_memset(&theme, 0, sizeof(theme));
    theme.window_bg = 0x00A0B0C0u;
    rect.x = 10;
    rect.y = 10;
    rect.w = 40;
    rect.h = 40;

    desktop_app_render(&apps, DESKTOP_APP_HELP, &surface, &theme, &rect);

    outside = pixels[17 * 96 + 19];
    inside = pixels[19 * 96 + 19];
    KTEST_EXPECT_EQ(tc, outside, 0u);
    KTEST_EXPECT_NE(tc, inside, 0u);
}

static void test_desktop_help_app_render_is_visible_in_framebuffer(
    ktest_case_t *tc)
{
    static uint32_t pixels[480 * 400];
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    const desktop_window_t *win;
    uint32_t bg;
    int sample_x;
    int sample_y;
    int help_id;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);
    KTEST_ASSERT_TRUE(tc, help_id > 0);
    desktop_render(&desktop);

    win = desktop_window_for_test(&desktop, help_id);
    KTEST_ASSERT_NOT_NULL(tc, win);
    bg = framebuffer_pack_rgb(&fb, 0x2f, 0x49, 0x50);
    sample_x = win->content_rect.x + 2;
    sample_y = win->content_rect.y + 2;

    KTEST_EXPECT_EQ(tc, pixels[sample_y * 480 + sample_x], bg);
    desktop_test_destroy(&desktop);
}

static void test_desktop_help_app_key_input_is_ignored_while_launcher_open(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    int help_id;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);
    KTEST_ASSERT_TRUE(tc, help_id > 0);

    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 27), DESKTOP_KEY_CONSUMED);
    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'q'), DESKTOP_KEY_CONSUMED);
    KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 1);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_HELP);
    KTEST_EXPECT_TRUE(tc, desktop.launcher_open);
    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_LAUNCHER);

    desktop_test_destroy(&desktop);
}

static void test_desktop_open_invalid_app_kind_is_rejected(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);

    KTEST_EXPECT_EQ(tc,
                    desktop_open_app_window(&desktop, (desktop_app_kind_t)99),
                    -1);
    KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 0);

    desktop_test_destroy(&desktop);
}

static void test_desktop_processes_app_handles_empty_snapshot(ktest_case_t *tc)
{
    static desktop_app_state_t apps;

    sched_init();

    desktop_apps_init(&apps);
    desktop_app_refresh_processes_for_test(&apps.processes);

    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.processes, 0),
                                   "PID  STATE  NAME") == 0);
    KTEST_EXPECT_NULL(tc, desktop_app_line_for_test(&apps.processes, 1));
    KTEST_EXPECT_EQ(tc, apps.processes.line_count, 1);
}

static void test_desktop_open_processes_refreshes_after_late_process(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    static process_t proc;
    int pid;

    sched_init();
    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);

    k_memset(&proc, 0, sizeof(proc));
    proc.saved_esp = 1;
    k_strncpy(proc.name, "lateproc", sizeof(proc.name) - 1);
    pid = sched_add(&proc);
    KTEST_ASSERT_TRUE(tc, pid > 0);

    KTEST_ASSERT_TRUE(tc,
                      desktop_open_app_window(&desktop,
                                              DESKTOP_APP_PROCESSES) > 0);
    KTEST_EXPECT_TRUE(tc,
                      k_strcmp(desktop_app_line_for_test(
                                   &desktop.app_state.processes, 1),
                               "1  READY  lateproc") == 0);

    desktop_test_destroy(&desktop);
}

static void test_desktop_files_app_replaces_last_visible_line_when_truncated(
    ktest_case_t *tc)
{
    static desktop_app_state_t apps;
    struct {
        char dents[3];
        char guard;
    } input = { { 'a', 'b', 'c' }, 'Z' };

    desktop_apps_init(&apps);
    desktop_app_refresh_files_for_test(&apps.files, input.dents,
                                        sizeof(input.dents));

    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.files, 0),
                                   "Files: /") == 0);
    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop_app_line_for_test(&apps.files, 1),
                                   "... more entries") == 0);
    KTEST_EXPECT_EQ(tc, apps.files.line_count, 2);
    KTEST_EXPECT_EQ(tc, input.guard, 'Z');
}

static void test_terminal_write_wraps_and_retains_history(ktest_case_t *tc)
{
    gui_terminal_t term;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             4, 2, 4, 0x0f),
                    1);

    KTEST_EXPECT_EQ(tc, gui_terminal_write(&term, "abcdX", 5), 5);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).ch, 'a');
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 3, 0).ch, 'd');
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 1).ch, 'X');

    KTEST_EXPECT_EQ(tc, gui_terminal_write(&term, "\nzzzz\nq", 7), 7);
    KTEST_EXPECT_GE(tc, gui_terminal_history_count(&term), 1);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 1).ch, 'q');
}

static void terminal_test_fb(framebuffer_info_t *fb)
{
    k_memset(terminal_pixels, 0, sizeof(terminal_pixels));
    k_memset(fb, 0, sizeof(*fb));
    fb->address = (uintptr_t)terminal_pixels;
    fb->pitch = 128u * sizeof(uint32_t);
    fb->width = 128u;
    fb->height = 96u;
    fb->bpp = 32u;
    fb->red_pos = 16u;
    fb->red_size = 8u;
    fb->green_pos = 8u;
    fb->green_size = 8u;
    fb->blue_pos = 0u;
    fb->blue_size = 8u;
}

static void test_terminal_render_uses_pixel_padding(ktest_case_t *tc)
{
    gui_terminal_t term;
    framebuffer_info_t fb;
    gui_pixel_surface_t surface;
    gui_pixel_theme_t theme;

    terminal_test_fb(&fb);
    k_memset(&theme, 0, sizeof(theme));
    theme.terminal_bg = 0x00101010u;
    theme.terminal_fg = 0x00FFFFFFu;
    theme.terminal_cursor = 0x00FFCC44u;
    surface.fb = &fb;
    surface.clip.x = 0;
    surface.clip.y = 0;
    surface.clip.w = 128;
    surface.clip.h = 96;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 3, 4, 0x0f),
                    1);
    gui_terminal_set_pixel_rect(&term,
                                (gui_pixel_rect_t){ 10, 12, 80, 60 },
                                6, 5);
    KTEST_ASSERT_EQ(tc, gui_terminal_write(&term, "A", 1), 1);

    gui_terminal_render(&term, &surface, &theme, 1);

    KTEST_EXPECT_EQ(tc, terminal_pixels[12 * 128 + 10], 0x00101010u);
    KTEST_EXPECT_NE(tc, terminal_pixels[17 * 128 + 16], 0u);
}

static void test_terminal_render_draws_underline_cursor_in_scrollback_view(ktest_case_t *tc)
{
    gui_terminal_t term;
    framebuffer_info_t fb;
    gui_pixel_surface_t surface;
    gui_pixel_theme_t theme;
    int cursor_y;

    terminal_test_fb(&fb);
    k_memset(&theme, 0, sizeof(theme));
    theme.terminal_bg = 0x00000000u;
    theme.terminal_fg = 0x00FFFFFFu;
    theme.terminal_cursor = 0x00FFCC44u;
    surface.fb = &fb;
    surface.clip.x = 0;
    surface.clip.y = 0;
    surface.clip.w = 128;
    surface.clip.h = 96;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 3, 4, 0x0f),
                    1);
    gui_terminal_set_pixel_rect(&term,
                                (gui_pixel_rect_t){ 0, 0, 80, 60 },
                                4, 4);
    KTEST_ASSERT_EQ(tc, gui_terminal_write(&term, "A\n", 2), 2);
    term.view_top = 1;
    term.live_view = 0;
    term.history_count = 1;
    term.cursor_x = 0;
    term.cursor_y = 0;

    gui_terminal_render(&term, &surface, &theme, 1);

    cursor_y = 4 + (int)GUI_FONT_H + (int)GUI_FONT_H - 2;
    KTEST_EXPECT_EQ(tc, terminal_pixels[cursor_y * 128 + 4],
                    0x00FFCC44u);
}

static void test_terminal_render_clips_cursor_to_surface_clip(ktest_case_t *tc)
{
    gui_terminal_t term;
    framebuffer_info_t fb;
    gui_pixel_surface_t surface;
    gui_pixel_theme_t theme;
    int cursor_y;

    terminal_test_fb(&fb);
    k_memset(&theme, 0, sizeof(theme));
    theme.terminal_bg = 0x00000000u;
    theme.terminal_fg = 0x00FFFFFFu;
    theme.terminal_cursor = 0x00FFCC44u;
    surface.fb = &fb;
    surface.clip.x = 0;
    surface.clip.y = 0;
    surface.clip.w = 6;
    surface.clip.h = 32;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 3, 4, 0x0f),
                    1);
    gui_terminal_set_pixel_rect(&term,
                                (gui_pixel_rect_t){ 0, 0, 80, 60 },
                                4, 4);

    gui_terminal_render(&term, &surface, &theme, 1);

    cursor_y = 4 + (int)GUI_FONT_H - 2;
    KTEST_EXPECT_EQ(tc, terminal_pixels[cursor_y * 128 + 4],
                    0x00FFCC44u);
    KTEST_EXPECT_EQ(tc, terminal_pixels[cursor_y * 128 + 6], 0u);
}

static void test_terminal_render_composes_scrollback_before_live_rows(ktest_case_t *tc)
{
    gui_terminal_t term;
    framebuffer_info_t fb;
    gui_pixel_surface_t surface;
    gui_pixel_theme_t theme;
    uint32_t fg;

    terminal_test_fb(&fb);
    k_memset(&theme, 0, sizeof(theme));
    theme.terminal_bg = 0x00000000u;
    theme.terminal_fg = 0x00112233u;
    theme.terminal_cursor = 0x00FFCC44u;
    surface.fb = &fb;
    surface.clip.x = 0;
    surface.clip.y = 0;
    surface.clip.w = 128;
    surface.clip.h = 96;
    fg = framebuffer_pack_rgb(&fb, 0x11, 0x22, 0x33);

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             4, 2, 4, 0x0f),
                    1);
    gui_terminal_set_pixel_rect(&term,
                                (gui_pixel_rect_t){ 0, 0, 64, 32 },
                                0, 0);
    KTEST_ASSERT_EQ(tc,
                    gui_terminal_write(&term, "Aaaa\nBbbb\nCccc", 14),
                    14);

    gui_terminal_scroll_view(&term, 1);
    gui_terminal_render(&term, &surface, &theme, 0);

    KTEST_EXPECT_EQ(tc, terminal_pixels[0 * 128 + 3], fg);
}

static void test_terminal_render_uses_ansi_foreground_color(ktest_case_t *tc)
{
    gui_terminal_t term;
    framebuffer_info_t fb;
    gui_pixel_surface_t surface;
    gui_pixel_theme_t theme;
    uint32_t green;
    static const char text[] = "\x1b[32mA";

    terminal_test_fb(&fb);
    k_memset(&theme, 0, sizeof(theme));
    theme.terminal_bg = 0x00000000u;
    theme.terminal_fg = 0x00FFFFFFu;
    theme.terminal_cursor = 0x00FFCC44u;
    surface.fb = &fb;
    surface.clip.x = 0;
    surface.clip.y = 0;
    surface.clip.w = 128;
    surface.clip.h = 96;
    green = framebuffer_pack_rgb(&fb, 0x67, 0xc5, 0x8f);

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 3, 4, 0x0f),
                    1);
    gui_terminal_set_pixel_rect(&term,
                                (gui_pixel_rect_t){ 0, 0, 80, 60 },
                                0, 0);
    KTEST_ASSERT_EQ(tc, gui_terminal_write(&term, text, sizeof(text) - 1),
                    (int)(sizeof(text) - 1));

    gui_terminal_render(&term, &surface, &theme, 0);

    KTEST_EXPECT_EQ(tc, terminal_pixels[0 * 128 + 3], green);
}

static void test_terminal_ansi_color_does_not_emit_escape_bytes(ktest_case_t *tc)
{
    gui_terminal_t term;
    static const char text[] = "\x1b[32mG\x1b[0mW";

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 2, 4, 0x0f),
                    1);

    KTEST_EXPECT_EQ(tc, gui_terminal_write(&term, text, sizeof(text) - 1),
                    sizeof(text) - 1);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).ch, 'G');
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).attr, 0x0a);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 1, 0).ch, 'W');
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 1, 0).attr, 0x0f);
}

static void test_terminal_clear_discards_history_and_resets_cursor(ktest_case_t *tc)
{
    gui_terminal_t term;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             4, 2, 4, 0x0f),
                    1);
    KTEST_ASSERT_EQ(tc, gui_terminal_write(&term, "abcd\nefgh\nijkl", 14), 14);

    gui_terminal_clear(&term);

    KTEST_EXPECT_EQ(tc, gui_terminal_history_count(&term), 0);
    KTEST_EXPECT_EQ(tc, gui_terminal_cursor_x(&term), 0);
    KTEST_EXPECT_EQ(tc, gui_terminal_cursor_y(&term), 0);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).ch, ' ');
}

static void test_terminal_writes_to_later_rows_after_hardening(ktest_case_t *tc)
{
    gui_terminal_t term;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             4, 4, 4, 0x0f),
                    1);

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_write(&term, "abcd\nefgh\nijkl\nmnop", 19),
                    19);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 3).ch, 'm');
}

static void test_terminal_init_rejects_overflow_dimensions_without_touching_buffers(ktest_case_t *tc)
{
    gui_terminal_t term;

    terminal_tiny_live[0].ch = 'L';
    terminal_tiny_live[0].attr = 0x11;
    terminal_tiny_live[1].ch = 'l';
    terminal_tiny_live[1].attr = 0x22;
    terminal_tiny_history[0].ch = 'H';
    terminal_tiny_history[0].attr = 0x33;
    terminal_tiny_history[1].ch = 'h';
    terminal_tiny_history[1].attr = 0x44;

    KTEST_EXPECT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_tiny_live,
                                             terminal_tiny_history,
                                             65536, 65536, 65536, 0x0f),
                    0);
    KTEST_EXPECT_EQ(tc, terminal_tiny_live[0].ch, 'L');
    KTEST_EXPECT_EQ(tc, terminal_tiny_live[0].attr, 0x11);
    KTEST_EXPECT_EQ(tc, terminal_tiny_live[1].ch, 'l');
    KTEST_EXPECT_EQ(tc, terminal_tiny_history[0].ch, 'H');
    KTEST_EXPECT_EQ(tc, terminal_tiny_history[1].attr, 0x44);

    KTEST_EXPECT_EQ(tc,
                    gui_terminal_init_alloc(&term, 65536, 65536, 65536, 0x0f),
                    0);
}

static void test_terminal_ansi_digit_sequence_caps_without_overflow(ktest_case_t *tc)
{
    gui_terminal_t term;
    static const char text[] = "\x1b[123456789012345678901234567890mG";

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 2, 4, 0x0f),
                    1);

    KTEST_EXPECT_EQ(tc, gui_terminal_write(&term, text, sizeof(text) - 1),
                    (int)(sizeof(text) - 1));
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).ch, 'G');
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 0, 0).attr, 0x0f);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&term, 1, 0).ch, ' ');
}

static void test_terminal_scroll_view_clamps_large_positive_and_negative_inputs(ktest_case_t *tc)
{
    gui_terminal_t term;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             4, 2, 4, 0x0f),
                    1);
    KTEST_ASSERT_EQ(tc, gui_terminal_write(&term, "abcd\nefgh\nijkl", 14), 14);

    gui_terminal_scroll_view(&term, INT_MAX);
    KTEST_EXPECT_EQ(tc, gui_terminal_visible_view_top(&term),
                    gui_terminal_history_count(&term));

    gui_terminal_scroll_view(&term, INT_MIN);
    KTEST_EXPECT_EQ(tc, gui_terminal_visible_view_top(&term), 0);
}

static void test_terminal_write_rejects_lengths_above_int_max(ktest_case_t *tc)
{
    gui_terminal_t term;
    uint32_t too_long = (uint32_t)INT_MAX + 1u;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_cells,
                                             terminal_history,
                                             8, 2, 4, 0x0f),
                    1);
    terminal_cells[0].ch = 'Q';
    terminal_cells[0].attr = 0x5a;

    KTEST_EXPECT_EQ(tc, gui_terminal_write(&term, "Z", too_long), 0);
    KTEST_EXPECT_EQ(tc, terminal_cells[0].ch, 'Q');
    KTEST_EXPECT_EQ(tc, terminal_cells[0].attr, 0x5a);
}

static void test_terminal_destroy_preserves_static_buffers_and_clears_owned_buffers(ktest_case_t *tc)
{
    gui_terminal_t term;

    KTEST_ASSERT_EQ(tc,
                    gui_terminal_init_static(&term,
                                             terminal_tiny_live,
                                             terminal_tiny_history,
                                             1, 1, 1, 0x0f),
                    1);
    terminal_tiny_live[0].ch = 'S';
    terminal_tiny_live[0].attr = 0x1f;
    terminal_tiny_history[0].ch = 'T';
    terminal_tiny_history[0].attr = 0x2f;
    gui_terminal_destroy(&term);
    KTEST_EXPECT_EQ(tc, terminal_tiny_live[0].ch, 'S');
    KTEST_EXPECT_EQ(tc, terminal_tiny_live[0].attr, 0x1f);
    KTEST_EXPECT_EQ(tc, terminal_tiny_history[0].ch, 'T');
    KTEST_EXPECT_EQ(tc, terminal_tiny_history[0].attr, 0x2f);

    KTEST_ASSERT_EQ(tc, gui_terminal_init_alloc(&term, 2, 2, 2, 0x0f), 1);
    KTEST_EXPECT_EQ(tc, term.owns_buffers, 1);
    gui_terminal_destroy(&term);
    KTEST_EXPECT_EQ(tc, term.live, 0);
    KTEST_EXPECT_EQ(tc, term.history, 0);
    KTEST_EXPECT_EQ(tc, term.owns_buffers, 0);
}

static void test_gui_display_fill_rect_clips_to_bounds(ktest_case_t *tc)
{
    gui_display_t display;
    gui_rect_t dirty;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    dirty = gui_display_fill_rect(&display, -2, 0, 4, 1, ' ', 0x1f);

    KTEST_EXPECT_EQ(tc, dirty.x, 0);
    KTEST_EXPECT_EQ(tc, dirty.y, 0);
    KTEST_EXPECT_EQ(tc, dirty.w, 2);
    KTEST_EXPECT_EQ(tc, dirty.h, 1);
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 0, 0).attr, 0x1f);
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 2, 0).attr, 0x0f);

    dirty = gui_display_fill_rect(&display, 200, 10, 4, 3, 'x', 0x1c);
    KTEST_EXPECT_EQ(tc, dirty.x, 0);
    KTEST_EXPECT_EQ(tc, dirty.y, 0);
    KTEST_EXPECT_EQ(tc, dirty.w, 0);
    KTEST_EXPECT_EQ(tc, dirty.h, 0);
}

static void test_gui_display_draw_text_stops_at_region_edge(ktest_case_t *tc)
{
    gui_display_t display;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    gui_display_draw_text(&display, 3, 2, 4, "desktop", 0x0e);

    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 3, 2).ch, 'd');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 6, 2).ch, 'k');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 7, 2).ch, ' ');
}

static void test_gui_display_presents_cells_to_framebuffer(ktest_case_t *tc)
{
    gui_cell_t cells[2];
    static uint32_t pixels[8 * 16 * 2];
    gui_display_t display;
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 16u * sizeof(uint32_t);
    fb.width = 16;
    fb.height = 16;
    fb.red_pos = 16;
    fb.red_size = 8;
    fb.green_pos = 8;
    fb.green_size = 8;
    fb.blue_pos = 0;
    fb.blue_size = 8;

    gui_display_init(&display, cells, 2, 1, 0x0f);
    gui_display_draw_text(&display, 0, 0, 1, "A", 0x1e);

    gui_display_present_to_framebuffer(&display, &fb);

    KTEST_EXPECT_EQ(tc, framebuffer_pack_rgb(&fb, 0xf2, 0xc9, 0x4c),
                    pixels[0 * 16 + 2]);
    KTEST_EXPECT_EQ(tc, framebuffer_pack_rgb(&fb, 0x16, 0x2a, 0x4f),
                    pixels[0 * 16 + 0]);
}

static void test_desktop_boot_layout_opens_shell_window(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop.video_address = 0xB8000u;
    desktop_open_shell_window(&desktop);

    KTEST_EXPECT_TRUE(tc, desktop.active);
    KTEST_EXPECT_TRUE(tc, desktop.shell_window_open);
    KTEST_EXPECT_FALSE(tc, desktop.launcher_open);
    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_SHELL);
    KTEST_EXPECT_EQ(tc, desktop.taskbar.y, 24);
    KTEST_EXPECT_GE(tc, desktop.shell_rect.w, 48);
    KTEST_EXPECT_GE(tc, desktop.launcher_rect.y,
                    desktop.shell_rect.y + desktop.shell_rect.h);
    desktop_test_destroy(&desktop);
}

static void test_desktop_layout_scales_to_framebuffer_grid(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, large_desktop_cells, 128, 48, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    KTEST_EXPECT_EQ(tc, desktop.taskbar.y, 47);
    KTEST_EXPECT_GE(tc, desktop.shell_rect.w, 100);
    KTEST_EXPECT_GE(tc, desktop.shell_rect.h, 30);
    KTEST_EXPECT_GE(tc, desktop.shell_content.w, 98);
    KTEST_EXPECT_GE(tc, desktop.shell_content.h, 28);
    desktop_test_destroy(&desktop);
}

static void test_desktop_render_draws_taskbar_and_launcher_label(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop.video_address = 0xB8000u;
    desktop.launcher_open = 1;
    desktop.shell_window_open = 1;
    desktop_render(&desktop);

    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 0, 24).ch, ' ');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 2, 24).ch, 'M');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 3, 24).ch, 'e');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 2,
                                            desktop.launcher_rect.y + 1).ch,
                    '1');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 3,
                                            desktop.launcher_rect.y + 1).ch,
                    ' ');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 4,
                                            desktop.launcher_rect.y + 1).ch,
                    'S');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 5,
                                            desktop.launcher_rect.y + 1).ch,
                    'h');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 6,
                                            desktop.launcher_rect.y + 1).ch,
                    'e');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.shell_rect.x + 2,
                                            desktop.shell_rect.y).ch,
                    'S');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.shell_rect.x + 3,
                                            desktop.shell_rect.y).ch,
                    'h');
    desktop_test_destroy(&desktop);
}

static void test_desktop_launcher_enter_opens_files_window(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop.launcher_open = 1;
    desktop.launcher_selection = DESKTOP_APP_FILES;

    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, '\n'),
                    DESKTOP_KEY_CONSUMED);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_FILES);

    desktop_test_destroy(&desktop);
}

static void test_desktop_launcher_enter_does_not_refresh_files_view(
    ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    const char *sentinel = "sentinel files snapshot";

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    k_memset(desktop.app_state.files.lines[0], 0, DESKTOP_APP_LINE_MAX);
    k_strncpy(desktop.app_state.files.lines[0], sentinel,
              DESKTOP_APP_LINE_MAX - 1);
    desktop.app_state.files.line_count = 1;
    desktop.launcher_open = 1;
    desktop.launcher_selection = DESKTOP_APP_FILES;

    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, '\n'),
                    DESKTOP_KEY_CONSUMED);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_FILES);
    KTEST_EXPECT_TRUE(tc, k_strcmp(desktop.app_state.files.lines[0],
                                   sentinel) == 0);

    desktop_test_destroy(&desktop);
}

static void test_desktop_framebuffer_launcher_click_opens_files_window(
    ktest_case_t *tc)
{
    static uint32_t pixels[480 * 400];
    desktop_state_t desktop;
    gui_display_t display;
    framebuffer_info_t fb;
    desktop_pointer_event_t ev;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop.launcher_open = 1;
    desktop.focus = DESKTOP_FOCUS_LAUNCHER;
    desktop_render(&desktop);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = desktop.launcher_pixel_rect.x + 24;
    ev.pixel_y = desktop.launcher_pixel_rect.y + 30;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_FALSE(tc, desktop.launcher_open);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_FILES);
    KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 1);

    desktop_test_destroy(&desktop);
}

static void test_desktop_framebuffer_launcher_click_uses_visible_item_rows(
    ktest_case_t *tc)
{
    static uint32_t pixels[480 * 400];
    desktop_state_t desktop;
    gui_display_t display;
    framebuffer_info_t fb;
    desktop_pointer_event_t ev;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = desktop.launcher_pixel_rect.x + 24;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;

    desktop.launcher_open = 1;
    desktop.focus = DESKTOP_FOCUS_LAUNCHER;
    ev.pixel_y = desktop.launcher_pixel_rect.y + 20;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_SHELL);

    desktop.launcher_open = 1;
    desktop.focus = DESKTOP_FOCUS_LAUNCHER;
    ev.pixel_y = desktop.launcher_pixel_rect.y + 36;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_FILES);

    desktop.launcher_open = 1;
    desktop.focus = DESKTOP_FOCUS_LAUNCHER;
    ev.pixel_y = desktop.launcher_pixel_rect.y + 52;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_PROCESSES);

    desktop.launcher_open = 1;
    desktop.focus = DESKTOP_FOCUS_LAUNCHER;
    ev.pixel_y = desktop.launcher_pixel_rect.y + 68;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_HELP);

    desktop_test_destroy(&desktop);
}

static void test_desktop_text_launcher_keeps_bottom_border_visible(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop.launcher_open = 1;
    desktop.launcher_selection = DESKTOP_APP_HELP;
    desktop_render(&desktop);

    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 2,
                                            desktop.launcher_rect.y + 4).ch,
                    '4');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 2,
                                            desktop.launcher_rect.y + 5).ch,
                    '-');

    desktop_test_destroy(&desktop);
}

static void test_desktop_taskbar_click_focuses_processes_window(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    desktop_pointer_event_t ev;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
    desktop_open_app_window(&desktop, DESKTOP_APP_PROCESSES);
    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.x = 20;
    ev.y = desktop.taskbar.y;
    ev.pixel_x = ev.x * (int)GUI_FONT_W;
    ev.pixel_y = ev.y * (int)GUI_FONT_H;

    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_PROCESSES);

    desktop_test_destroy(&desktop);
}

static void test_desktop_taskbar_shell_refocus_forwards_keys(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    desktop_pointer_event_t ev;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 27),
                    DESKTOP_KEY_CONSUMED);
    desktop.launcher_selection = DESKTOP_APP_FILES;
    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, '\n'),
                    DESKTOP_KEY_CONSUMED);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_FILES);
    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'a'),
                    DESKTOP_KEY_CONSUMED);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.x = 8;
    ev.y = desktop.taskbar.y;
    ev.pixel_x = ev.x * (int)GUI_FONT_W;
    ev.pixel_y = ev.y * (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_SHELL);
    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'a'),
                    DESKTOP_KEY_FORWARD);

    desktop_test_destroy(&desktop);
}

static void test_desktop_text_taskbar_renders_open_window_labels(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
    desktop_open_app_window(&desktop, DESKTOP_APP_PROCESSES);
    desktop_render(&desktop);

    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 8, desktop.taskbar.y).ch,
                    'F');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 9, desktop.taskbar.y).ch,
                    'i');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 18, desktop.taskbar.y).ch,
                    'P');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 19, desktop.taskbar.y).ch,
                    'r');

    desktop_test_destroy(&desktop);
}

static void test_desktop_shell_open_matches_rendered_window_rect(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    int shell_id;
    const desktop_window_t *win;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    shell_id = desktop_open_app_window(&desktop, DESKTOP_APP_SHELL);
    win = desktop_window_for_test(&desktop, shell_id);
    KTEST_ASSERT_NOT_NULL(tc, win);

    KTEST_EXPECT_EQ(tc, win->rect.x, desktop.window_pixel_rect.x);
    KTEST_EXPECT_EQ(tc, win->rect.y, desktop.window_pixel_rect.y);
    KTEST_EXPECT_EQ(tc, win->rect.w, desktop.window_pixel_rect.w);
    KTEST_EXPECT_EQ(tc, win->rect.h, desktop.window_pixel_rect.h);
    KTEST_EXPECT_EQ(tc, win->content_rect.x, desktop.window_pixel_rect.x + 8);
    KTEST_EXPECT_EQ(tc, win->content_rect.y, desktop.window_pixel_rect.y + 24);
    KTEST_EXPECT_EQ(tc, desktop.shell_terminal.pixel_rect.x,
                    desktop.shell_pixel_rect.x);
    KTEST_EXPECT_EQ(tc, desktop.shell_terminal.pixel_rect.y,
                    desktop.shell_pixel_rect.y);

    desktop_test_destroy(&desktop);
}

static void test_desktop_shell_output_still_routes_after_mini_apps_open(
    ktest_case_t *tc)
{
    static uint32_t pixels[480 * 400];
    desktop_state_t desktop;
    gui_display_t display;
    framebuffer_info_t fb;
    int files_id;
    int help_id;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;
    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
    help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);

    KTEST_ASSERT_TRUE(tc, desktop.shell_window_open);
    KTEST_ASSERT_TRUE(tc, files_id > 0);
    KTEST_ASSERT_TRUE(tc, help_id > 0);
    KTEST_ASSERT_NOT_NULL(tc, desktop_window_for_test(&desktop, files_id));
    KTEST_ASSERT_NOT_NULL(tc, desktop_window_for_test(&desktop, help_id));
    KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 3);
    desktop_render(&desktop);

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, "A", 1), 1);
    KTEST_EXPECT_EQ(tc, gui_terminal_cell_at(&desktop.shell_terminal, 0, 0).ch,
                    'A');

    desktop_test_destroy(&desktop);
}

static void test_desktop_close_button_closes_files_window(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    desktop_pointer_event_t ev;
    int files_id;
    const desktop_window_t *win;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
    win = desktop_window_for_test(&desktop, files_id);
    KTEST_ASSERT_NOT_NULL(tc, win);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = win->rect.x + win->rect.w - 10;
    ev.pixel_y = win->rect.y + 8;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;

    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 0);

    desktop_test_destroy(&desktop);
}

static void test_desktop_shell_close_button_closes_visible_shell_window(
    ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    desktop_pointer_event_t ev;
    int shell_id;
    const desktop_window_t *win;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    shell_id = desktop_open_app_window(&desktop, DESKTOP_APP_SHELL);
    win = desktop_window_for_test(&desktop, shell_id);
    KTEST_ASSERT_NOT_NULL(tc, win);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = desktop.window_pixel_rect.x +
                 desktop.window_pixel_rect.w - 10;
    ev.pixel_y = desktop.window_pixel_rect.y + 8;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_FALSE(tc, desktop.shell_window_open);
    KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 0);
    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'a'),
                    DESKTOP_KEY_CONSUMED);

    desktop_test_destroy(&desktop);
}

static void test_desktop_title_drag_moves_window_and_clamps_top_left(
    ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    desktop_pointer_event_t ev;
    int files_id;
    const desktop_window_t *win;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
    win = desktop_window_for_test(&desktop, files_id);
    KTEST_ASSERT_NOT_NULL(tc, win);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = win->rect.x + 20;
    ev.pixel_y = win->rect.y + 8;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    ev.pixel_x = -80;
    ev.pixel_y = -80;
    ev.x = 0;
    ev.y = 0;
    desktop_handle_pointer(&desktop, &ev);

    win = desktop_window_for_test(&desktop, files_id);
    KTEST_ASSERT_NOT_NULL(tc, win);
    KTEST_EXPECT_EQ(tc, win->rect.y, 0);
    KTEST_EXPECT_TRUE(tc, win->rect.x >= 0);

    ev.left_down = 0;
    desktop_handle_pointer(&desktop, &ev);
    KTEST_EXPECT_EQ(tc, desktop_dragging_window_for_test(&desktop), 0);

    desktop_test_destroy(&desktop);
}

static void test_desktop_shell_drag_preserves_window_size(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    desktop_pointer_event_t ev;
    int shell_id;
    const desktop_window_t *win;
    int original_x;
    int original_y;
    int original_shell_w;
    int original_shell_h;
    int original_content_w;
    int original_content_h;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    shell_id = desktop_open_app_window(&desktop, DESKTOP_APP_SHELL);
    win = desktop_window_for_test(&desktop, shell_id);
    KTEST_ASSERT_NOT_NULL(tc, win);
    original_x = desktop.shell_rect.x;
    original_y = desktop.shell_rect.y;
    original_shell_w = desktop.shell_rect.w;
    original_shell_h = desktop.shell_rect.h;
    original_content_w = desktop.shell_content.w;
    original_content_h = desktop.shell_content.h;

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = win->rect.x + 24;
    ev.pixel_y = win->rect.y + 8;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    ev.pixel_x = -64;
    ev.pixel_y = -48;
    ev.x = 0;
    ev.y = 0;
    desktop_handle_pointer(&desktop, &ev);

    ev.left_down = 0;
    desktop_handle_pointer(&desktop, &ev);

    win = desktop_window_for_test(&desktop, shell_id);
    KTEST_ASSERT_NOT_NULL(tc, win);
    KTEST_EXPECT_EQ(tc, desktop.shell_rect.w, original_shell_w);
    KTEST_EXPECT_EQ(tc, desktop.shell_rect.h, original_shell_h);
    KTEST_EXPECT_EQ(tc, desktop.shell_content.w, original_content_w);
    KTEST_EXPECT_EQ(tc, desktop.shell_content.h, original_content_h);
    KTEST_EXPECT_TRUE(tc, desktop.shell_rect.x != original_x ||
                           desktop.shell_rect.y != original_y);
    KTEST_EXPECT_EQ(tc, desktop_dragging_window_for_test(&desktop), 0);

    desktop_test_destroy(&desktop);
}

static void test_desktop_shell_drag_syncs_terminal_pixel_rect_in_framebuffer_mode(
    ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    framebuffer_info_t fb;
    desktop_pointer_event_t ev;
    int shell_id;
    const desktop_window_t *win;
    gui_pixel_rect_t original_terminal_rect;
    gui_pixel_rect_t original_shell_pixel_rect;
    int original_shell_w;
    int original_shell_h;
    int original_content_w;
    int original_content_h;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;
    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    shell_id = desktop_open_app_window(&desktop, DESKTOP_APP_SHELL);
    desktop_render(&desktop);
    win = desktop_window_for_test(&desktop, shell_id);
    KTEST_ASSERT_NOT_NULL(tc, win);
    original_terminal_rect = desktop.shell_terminal.pixel_rect;
    original_shell_pixel_rect = desktop.shell_pixel_rect;
    original_shell_w = desktop.shell_rect.w;
    original_shell_h = desktop.shell_rect.h;
    original_content_w = desktop.shell_content.w;
    original_content_h = desktop.shell_content.h;

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = win->rect.x + 24;
    ev.pixel_y = win->rect.y + 8;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    ev.pixel_x = -64;
    ev.pixel_y = -48;
    ev.x = 0;
    ev.y = 0;
    desktop_handle_pointer(&desktop, &ev);

    ev.left_down = 0;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop.shell_terminal.pixel_rect.x,
                    desktop.shell_pixel_rect.x);
    KTEST_EXPECT_EQ(tc, desktop.shell_terminal.pixel_rect.y,
                    desktop.shell_pixel_rect.y);
    KTEST_EXPECT_NE(tc, desktop.shell_terminal.pixel_rect.x,
                    original_terminal_rect.x);
    KTEST_EXPECT_NE(tc, desktop.shell_terminal.pixel_rect.y,
                    original_terminal_rect.y);
    KTEST_EXPECT_EQ(tc, desktop.shell_terminal.pixel_rect.w,
                    original_terminal_rect.w);
    KTEST_EXPECT_EQ(tc, desktop.shell_terminal.pixel_rect.h,
                    original_terminal_rect.h);
    KTEST_EXPECT_EQ(tc, desktop.shell_rect.w, original_shell_w);
    KTEST_EXPECT_EQ(tc, desktop.shell_rect.h, original_shell_h);
    KTEST_EXPECT_EQ(tc, desktop.shell_content.w, original_content_w);
    KTEST_EXPECT_EQ(tc, desktop.shell_content.h, original_content_h);
    KTEST_EXPECT_EQ(tc, desktop.shell_pixel_rect.w,
                    original_shell_pixel_rect.w);
    KTEST_EXPECT_EQ(tc, desktop.shell_pixel_rect.h,
                    original_shell_pixel_rect.h);

    desktop_test_destroy(&desktop);
}

static void test_desktop_open_clamps_help_window_to_framebuffer(
    ktest_case_t *tc)
{
    static uint32_t pixels[480 * 400];
    desktop_state_t desktop;
    gui_display_t display;
    framebuffer_info_t fb;
    const desktop_window_t *win;
    int help_id;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);
    KTEST_ASSERT_TRUE(tc, help_id > 0);
    win = desktop_window_for_test(&desktop, help_id);
    KTEST_ASSERT_NOT_NULL(tc, win);

    KTEST_EXPECT_GE(tc, win->rect.x, 0);
    KTEST_EXPECT_GE(tc, win->rect.y, 0);
    KTEST_EXPECT_LE(tc, win->rect.x + win->rect.w, (int)fb.width);
    KTEST_EXPECT_LE(tc, win->rect.y + win->rect.h, (int)fb.height);

    desktop_test_destroy(&desktop);
}

static void test_framebuffer_windows_render_in_z_order(ktest_case_t *tc)
{
    char shell_text[40];
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    int shell_id;
    int help_id;
    uint32_t bg;
    uint32_t top_pixel;
    int sample_x;
    int sample_y;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);

    shell_id = desktop_open_app_window(&desktop, DESKTOP_APP_SHELL);
    help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);
    KTEST_ASSERT_TRUE(tc, shell_id > 0);
    KTEST_ASSERT_TRUE(tc, help_id > 0);

    k_memset(shell_text, ' ', sizeof(shell_text));
    shell_text[0] = '\n';
    shell_text[1] = '\n';
    shell_text[2] = '\n';
    shell_text[3] = '\n';
    shell_text[4] = '\n';
    shell_text[39] = 'A';

    KTEST_ASSERT_EQ(tc,
                    desktop_write_console_output(&desktop, shell_text,
                                                 sizeof(shell_text)),
                    (int)sizeof(shell_text));
    desktop_render(&desktop);

    bg = framebuffer_pack_rgb(&fb, 0x2f, 0x49, 0x50);
    sample_x = desktop.shell_pixel_rect.x + desktop.shell_terminal.padding_x +
               34 * (int)GUI_FONT_W + 3;
    sample_y = desktop.shell_pixel_rect.y + desktop.shell_terminal.padding_y +
               5 * (int)GUI_FONT_H + 6;

    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[sample_y * 480 + sample_x], bg);

    desktop_focus_window_for_test(&desktop, shell_id);
    desktop_render(&desktop);

    top_pixel = pointer_motion_pixels[sample_y * 480 + sample_x];
    KTEST_EXPECT_NE(tc, top_pixel, bg);

    desktop_test_destroy(&desktop);
}

static void test_framebuffer_grid_desktop_renders_taskbar_and_shell_title(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, large_desktop_cells, 128, 48, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 2, 47).ch, 'M');
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_rect.x + 2,
                                        desktop.shell_rect.y).ch,
                    'S');
    desktop_test_destroy(&desktop);
}

static void test_desktop_init_binds_global_keyboard_target(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);

    KTEST_EXPECT_EQ(tc, (uint32_t)desktop_global(), (uint32_t)&desktop);
    KTEST_EXPECT_TRUE(tc, desktop_is_active());
    desktop_test_destroy(&desktop);
}

static void test_desktop_escape_opens_launcher_and_consumes_input(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 27), DESKTOP_KEY_CONSUMED);
    KTEST_EXPECT_TRUE(tc, desktop.launcher_open);
    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_LAUNCHER);
    desktop_test_destroy(&desktop);
}

static void test_desktop_plain_text_forwards_to_shell_when_focused(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'a'), DESKTOP_KEY_FORWARD);
    KTEST_EXPECT_FALSE(tc, desktop.launcher_open);
    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_SHELL);
    desktop_test_destroy(&desktop);
}

static void test_desktop_write_process_output_targets_shell_surface(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_pid(&desktop, 7);

    KTEST_EXPECT_EQ(tc,
                    desktop_write_process_output(&desktop, 7, 7, "help", 4),
                    4);
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    'h');
    desktop_test_destroy(&desktop);
}

static void test_desktop_child_process_group_output_targets_shell_surface(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_process(&desktop, 7, 7);

    KTEST_EXPECT_EQ(tc,
                    desktop_write_process_output(&desktop, 8, 7, "child", 5),
                    5);
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    'c');
    desktop_test_destroy(&desktop);
}

static void test_desktop_unrelated_process_group_output_is_rejected(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_process(&desktop, 7, 7);

    KTEST_EXPECT_EQ(tc,
                    desktop_write_process_output(&desktop, 8, 42, "x", 1),
                    0);
    desktop_test_destroy(&desktop);
}

static void test_syscall_console_write_routes_session_output_to_desktop(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    static process_t proc;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_process(&desktop, 7, 7);
    k_memset(&proc, 0, sizeof(proc));
    proc.pid = 8;
    proc.parent_pid = 7;
    proc.pgid = 8;

    KTEST_EXPECT_EQ(tc,
                    syscall_console_write_for_test(&proc, "sys", 3),
                    3);
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    's');
    desktop_test_destroy(&desktop);
}

static void test_desktop_ansi_color_escape_updates_attr_without_printing(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    static const char ansi_text[] = "\x1b[31mR\x1b[0mW";

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_process(&desktop, 7, 7);

    KTEST_EXPECT_EQ(tc,
                    desktop_write_process_output(&desktop, 7, 7, ansi_text,
                                                 sizeof(ansi_text) - 1),
                    sizeof(ansi_text) - 1);
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    'R');
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).attr,
                    0x0c);
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x + 1,
                                        desktop.shell_content.y).ch,
                    'W');
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x + 1,
                                        desktop.shell_content.y).attr,
                    display.default_attr);
    desktop_test_destroy(&desktop);
}

static void test_desktop_full_screen_write_does_not_scroll_until_next_char(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    char row_char;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_process(&desktop, 7, 7);

    for (int row = 0; row < desktop.shell_cells_h; row++) {
        row_char = (char)('A' + row);
        for (int col = 0; col < desktop.shell_cells_w; col++)
            KTEST_ASSERT_EQ(tc,
                            desktop_write_process_output(&desktop, 7, 7,
                                                         &row_char, 1),
                            1);
    }

    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    'A');
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y +
                                            desktop.shell_cells_h - 1).ch,
                    (char)('A' + desktop.shell_cells_h - 1));
    desktop_test_destroy(&desktop);
}

static void desktop_write_history_for_scroll_test(ktest_case_t *tc,
                                                  desktop_state_t *desktop)
{
    char row_char;

    for (int row = 0; row < desktop->shell_terminal.rows + 2; row++) {
        row_char = (char)('A' + (row % 26));
        for (int col = 0; col < desktop->shell_terminal.cols; col++)
            KTEST_ASSERT_EQ(tc,
                            desktop_write_console_output(desktop,
                                                         &row_char, 1),
                            1);
        KTEST_ASSERT_EQ(tc, desktop_write_console_output(desktop, "\n", 1),
                        1);
    }
    KTEST_EXPECT_GE(tc,
                    gui_terminal_history_count(&desktop->shell_terminal),
                    1);
}

static void test_desktop_scroll_syscall_moves_terminal_view(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_process(&desktop, 7, 7);
    desktop_write_history_for_scroll_test(tc, &desktop);

    KTEST_EXPECT_TRUE(tc, desktop.shell_terminal.live_view);
    KTEST_EXPECT_EQ(tc, syscall_handler(SYS_DRUNIX_SCROLL_UP, 1, 0, 0, 0, 0, 0), 0);
    KTEST_EXPECT_EQ(tc,
                    gui_terminal_visible_view_top(&desktop.shell_terminal),
                    1);
    KTEST_EXPECT_FALSE(tc, desktop.shell_terminal.live_view);

    KTEST_EXPECT_EQ(tc, syscall_handler(SYS_DRUNIX_SCROLL_DOWN, 1, 0, 0, 0, 0, 0), 0);
    KTEST_EXPECT_EQ(tc,
                    gui_terminal_visible_view_top(&desktop.shell_terminal),
                    0);
    KTEST_EXPECT_TRUE(tc, desktop.shell_terminal.live_view);
    desktop_test_destroy(&desktop);
}

static void test_desktop_new_output_snaps_scrollback_to_live(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_write_history_for_scroll_test(tc, &desktop);

    KTEST_ASSERT_EQ(tc, desktop_scroll_console(&desktop, 1), 1);
    KTEST_ASSERT_TRUE(tc, !desktop.shell_terminal.live_view);

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, "x", 1), 1);
    KTEST_EXPECT_EQ(tc,
                    gui_terminal_visible_view_top(&desktop.shell_terminal),
                    0);
    KTEST_EXPECT_TRUE(tc, desktop.shell_terminal.live_view);
    desktop_test_destroy(&desktop);
}

static void test_syscall_clear_clears_desktop_shell_buffer(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_process(&desktop, 7, 7);

    KTEST_ASSERT_EQ(tc,
                    desktop_write_process_output(&desktop, 7, 7, "abc", 3),
                    3);
    KTEST_ASSERT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    'a');

    KTEST_EXPECT_EQ(tc, syscall_handler(SYS_DRUNIX_CLEAR, 0, 0, 0, 0, 0, 0), 0);
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    ' ');
    KTEST_EXPECT_EQ(tc, desktop.shell_cursor_x, 0);
    KTEST_EXPECT_EQ(tc, desktop.shell_cursor_y, 0);
    desktop_test_destroy(&desktop);
}

static void test_tty_ctrl_c_echo_routes_to_desktop_shell_buffer(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    tty_t *tty;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_attach_shell_process(&desktop, 7, 7);
    tty_init();
    tty = tty_get(0);
    KTEST_ASSERT_NOT_NULL(tc, tty);
    tty->fg_pgid = 7;

    tty_ctrl_c(0);

    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    '^');
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x + 1,
                                        desktop.shell_content.y).ch,
                    'C');
    desktop_test_destroy(&desktop);
}

static void test_mouse_packet_decode_preserves_motion_and_buttons(ktest_case_t *tc)
{
    desktop_pointer_event_t ev;
    mouse_packet_t packet = { .buttons = 0x09, .dx = -120, .dy = 60 };

    KTEST_EXPECT_EQ(tc, mouse_decode_packet(&packet, &ev), 0);
    KTEST_EXPECT_TRUE(tc, ev.left_down);
    KTEST_EXPECT_EQ(tc, ev.dx, -120);
    KTEST_EXPECT_EQ(tc, ev.dy, 60);
}

static void test_mouse_packet_decode_saturates_overflow_motion(ktest_case_t *tc)
{
    desktop_pointer_event_t ev;
    mouse_packet_t packet = { .buttons = 0x48, .dx = 1, .dy = 0 };

    KTEST_EXPECT_EQ(tc, mouse_decode_packet(&packet, &ev), 0);
    KTEST_EXPECT_EQ(tc, ev.dx, 127);
    KTEST_EXPECT_EQ(tc, ev.dy, 0);

    packet.buttons = 0x58;
    KTEST_EXPECT_EQ(tc, mouse_decode_packet(&packet, &ev), 0);
    KTEST_EXPECT_EQ(tc, ev.dx, (uint32_t)-127);
}

static void test_mouse_stream_resyncs_after_noise(ktest_case_t *tc)
{
    mouse_packet_stream_t stream;
    mouse_packet_t packet;

    mouse_stream_reset(&stream);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x00, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 0);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0xC0, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 0);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x0B, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 1);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x01, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 2);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0xFF, &packet), 1);
    KTEST_EXPECT_EQ(tc, packet.buttons, 0x0B);
    KTEST_EXPECT_EQ(tc, packet.dx, 0x01);
    KTEST_EXPECT_EQ(tc, packet.dy, -1);
}

static void test_mouse_stream_delivers_response_like_packet_headers(
    ktest_case_t *tc)
{
    mouse_packet_stream_t stream;
    mouse_packet_t packet;

    mouse_stream_reset(&stream);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0xAA, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 1);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x01, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 2);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x02, &packet), 1);
    KTEST_EXPECT_EQ(tc, packet.buttons, 0xAA);
    KTEST_EXPECT_EQ(tc, packet.dx, 1);
    KTEST_EXPECT_EQ(tc, packet.dy, 2);
}

static void test_mouse_stream_delivers_overflow_packets(ktest_case_t *tc)
{
    mouse_packet_stream_t stream;
    mouse_packet_t packet;

    mouse_stream_reset(&stream);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x48, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 1);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x01, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 2);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x00, &packet), 1);
    KTEST_EXPECT_EQ(tc, packet.buttons, 0x48);
    KTEST_EXPECT_EQ(tc, packet.dx, 1);
    KTEST_EXPECT_EQ(tc, packet.dy, 0);
}

static void test_mouse_stream_keeps_response_like_bytes_inside_packet(ktest_case_t *tc)
{
    mouse_packet_stream_t stream;
    mouse_packet_t packet;

    mouse_stream_reset(&stream);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x0B, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 1);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0xFA, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 2);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0xAA, &packet), 1);
    KTEST_EXPECT_EQ(tc, packet.buttons, 0x0B);
    KTEST_EXPECT_EQ(tc, packet.dx, -6);
    KTEST_EXPECT_EQ(tc, packet.dy, -86);
}

static void test_mouse_irq_drains_initial_packet_without_aux_status(
    ktest_case_t *tc)
{
    KTEST_EXPECT_EQ(tc, mouse_irq_should_read_byte_for_test(0x01, 0), 1);
    KTEST_EXPECT_EQ(tc, mouse_irq_should_read_byte_for_test(0x01, 1), 1);
    KTEST_EXPECT_EQ(tc, mouse_irq_should_read_byte_for_test(0x01, 2), 1);
    KTEST_EXPECT_EQ(tc, mouse_irq_should_read_byte_for_test(0x01, 3), 0);
    KTEST_EXPECT_EQ(tc, mouse_irq_should_read_byte_for_test(0x21, 3), 1);
    KTEST_EXPECT_EQ(tc, mouse_irq_should_read_byte_for_test(0x00, 0), 0);
}

static void test_mouse_framebuffer_motion_uses_build_configured_scale(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    desktop_pointer_event_t ev;
    int expected_x;

    k_memset(&fb, 0, sizeof(fb));
    fb.width = 480u;
    fb.height = 400u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);

    mouse_pointer_reset_for_test(240, 192);
    k_memset(&ev, 0, sizeof(ev));
    ev.dx = 2;
    expected_x = 240 + 2 * TEST_MOUSE_EFFECTIVE_FRAMEBUFFER_SCALE;

    mouse_update_pointer_for_test(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, mouse_motion_scale_for_test(&desktop),
                    TEST_MOUSE_EFFECTIVE_FRAMEBUFFER_SCALE);
    KTEST_EXPECT_EQ(tc, ev.pixel_x, expected_x);
    KTEST_EXPECT_EQ(tc, ev.pixel_y, 192);
    KTEST_EXPECT_EQ(tc, ev.x, expected_x / (int)GUI_FONT_W);
    KTEST_EXPECT_EQ(tc, ev.y, 12);
    desktop_test_destroy(&desktop);
}

static void test_mouse_text_motion_ignores_framebuffer_speed(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    desktop_pointer_event_t ev;
    int start_x;
    int start_y;
    int raw_dx;
    int expected_x;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);

    start_x = 240;
    start_y = 192;
    raw_dx = 2;
    expected_x = start_x + raw_dx;

    mouse_pointer_reset_for_test(start_x, start_y);
    k_memset(&ev, 0, sizeof(ev));
    ev.dx = raw_dx;

    mouse_update_pointer_for_test(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, mouse_motion_scale_for_test(&desktop), 1);
    KTEST_EXPECT_EQ(tc, ev.pixel_x, expected_x);
    KTEST_EXPECT_EQ(tc, ev.pixel_y, start_y);
    KTEST_EXPECT_EQ(tc, ev.x, 30);
    KTEST_EXPECT_EQ(tc, ev.y, 12);
    desktop_test_destroy(&desktop);
}

static void test_mouse_overflow_packet_keeps_framebuffer_cursor_visible(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    mouse_packet_stream_t stream;
    mouse_packet_t packet;
    desktop_pointer_event_t ev;
    int start_x;
    int start_y;
    int expected_x;
    int max_cursor_x;
    uint32_t white;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;
    start_x = 240;
    start_y = 192;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_render(&desktop);

    mouse_pointer_reset_for_test(start_x, start_y);
    mouse_stream_reset(&stream);
    k_memset(&packet, 0, sizeof(packet));
    k_memset(&ev, 0, sizeof(ev));

    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x48, &packet), 0);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x01, &packet), 0);
    KTEST_ASSERT_EQ(tc, mouse_stream_consume(&stream, 0x00, &packet), 1);
    KTEST_ASSERT_EQ(tc, mouse_decode_packet(&packet, &ev), 0);

    max_cursor_x = (int)fb.width - 8;
    expected_x = start_x + 127 * TEST_MOUSE_EFFECTIVE_FRAMEBUFFER_SCALE;
    if (expected_x > max_cursor_x)
        expected_x = max_cursor_x;

    mouse_update_pointer_for_test(&desktop, &ev);
    desktop_handle_pointer(&desktop, &ev);

    white = framebuffer_pack_rgb(&fb, 255, 255, 255);
    KTEST_EXPECT_EQ(tc, desktop.pointer_pixel_x, expected_x);
    KTEST_EXPECT_EQ(tc, desktop.pointer_pixel_y, start_y);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[start_y * 480 + expected_x],
                    white);
    desktop_test_destroy(&desktop);
}

static void test_desktop_pointer_click_focuses_shell_window(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    desktop_pointer_event_t ev = { .x = 10, .y = 5, .left_down = 1 };

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop.focus = DESKTOP_FOCUS_TASKBAR;

    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_SHELL);
    desktop_test_destroy(&desktop);
}

static void test_desktop_pointer_click_ignores_hidden_shell_window(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    desktop_pointer_event_t ev = { .x = 10, .y = 5, .left_down = 1 };

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop.focus = DESKTOP_FOCUS_TASKBAR;

    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_FALSE(tc, desktop.shell_window_open);
    KTEST_EXPECT_EQ(tc, desktop.focus, DESKTOP_FOCUS_TASKBAR);
    desktop_test_destroy(&desktop);
}

static void test_desktop_open_builtin_window_focuses_and_reuses_instance(
    ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    int first_id;
    int second_id;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);

    first_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
    second_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);

    KTEST_EXPECT_TRUE(tc, first_id >= 0);
    KTEST_EXPECT_EQ(tc, second_id, first_id);
    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_FILES);
    KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 1);

    desktop_test_destroy(&desktop);
}

static void test_desktop_window_focus_raise_updates_z_order(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    int files_id;
    int help_id;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
    help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);

    desktop_focus_window_for_test(&desktop, files_id);

    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_FILES);
    KTEST_EXPECT_TRUE(tc,
                      desktop_window_z_for_test(&desktop, files_id) >
                      desktop_window_z_for_test(&desktop, help_id));

    desktop_test_destroy(&desktop);
}

static void test_desktop_focused_builtin_window_consumes_keys(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_app_window(&desktop, DESKTOP_APP_FILES);

    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'a'),
                    DESKTOP_KEY_CONSUMED);

    desktop_test_destroy(&desktop);
}

static void test_desktop_close_focused_non_shell_window_returns_shell_focus(
    ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    int files_id;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);

    KTEST_EXPECT_EQ(tc, desktop_close_window(&desktop, files_id), 1);
    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'a'),
                    DESKTOP_KEY_FORWARD);

    desktop_test_destroy(&desktop);
}

static void test_desktop_close_shell_window_stops_key_forwarding(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    int shell_id;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    shell_id = desktop_open_app_window(&desktop, DESKTOP_APP_SHELL);

    KTEST_EXPECT_TRUE(tc, shell_id >= 0);
    KTEST_EXPECT_EQ(tc, desktop_close_window(&desktop, shell_id), 1);
    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'a'),
                    DESKTOP_KEY_CONSUMED);

    desktop_test_destroy(&desktop);
}

static void test_desktop_shell_click_focuses_shell_window_table_entry(
    ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    desktop_pointer_event_t ev;
    int files_id;
    int shell_id;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    shell_id = desktop_open_app_window(&desktop, DESKTOP_APP_SHELL);
    files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
    KTEST_ASSERT_TRUE(tc, shell_id >= 0);
    KTEST_ASSERT_TRUE(tc, files_id >= 0);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.x = desktop.shell_rect.x + 1;
    ev.y = desktop.shell_rect.y + 1;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_SHELL);
    KTEST_EXPECT_TRUE(tc,
                      desktop_window_z_for_test(&desktop, shell_id) >
                      desktop_window_z_for_test(&desktop, files_id));
    KTEST_EXPECT_EQ(tc, desktop_close_window(&desktop, shell_id), 1);
    KTEST_EXPECT_EQ(tc, desktop_handle_key(&desktop, 'a'),
                    DESKTOP_KEY_CONSUMED);

    desktop_test_destroy(&desktop);
}

static void test_desktop_overlapped_pointer_hits_visible_top_window(ktest_case_t *tc)
{
    desktop_state_t desktop;
    gui_display_t display;
    desktop_pointer_event_t ev;
    const desktop_window_t *help_win;
    int shell_id;
    int help_id;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    shell_id = desktop_open_app_window(&desktop, DESKTOP_APP_SHELL);
    help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);
    KTEST_ASSERT_TRUE(tc, shell_id > 0);
    KTEST_ASSERT_TRUE(tc, help_id > 0);

    help_win = desktop_window_for_test(&desktop, help_id);
    KTEST_ASSERT_NOT_NULL(tc, help_win);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = help_win->rect.x + 24;
    ev.pixel_y = help_win->rect.y + 8;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_HELP);
    KTEST_EXPECT_EQ(tc, desktop_dragging_window_for_test(&desktop), help_id);

    ev.pixel_x = -64;
    ev.pixel_y = help_win->rect.y + 8;
    ev.x = 0;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    ev.left_down = 0;
    desktop_handle_pointer(&desktop, &ev);

    help_win = desktop_window_for_test(&desktop, help_id);
    KTEST_ASSERT_NOT_NULL(tc, help_win);
    KTEST_EXPECT_EQ(tc, help_win->rect.x, 0);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = help_win->rect.x + help_win->rect.w - 8;
    ev.pixel_y = help_win->rect.y + 8;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop_focused_app_for_test(&desktop),
                    DESKTOP_APP_SHELL);
    KTEST_EXPECT_EQ(tc, desktop_window_count_for_test(&desktop), 1);
    KTEST_EXPECT_NULL(tc, desktop_window_for_test(&desktop, help_id));
    KTEST_ASSERT_NOT_NULL(tc, desktop_window_for_test(&desktop, shell_id));

    desktop_test_destroy(&desktop);
}

static void test_desktop_render_draws_visible_mouse_pointer(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        display.cols / 2,
                                        display.rows / 2).ch,
                    '^');
    desktop_test_destroy(&desktop);
}

static void test_desktop_pointer_event_moves_visible_mouse_pointer(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    desktop_pointer_event_t ev = { .x = 12, .y = 8 };

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_open_shell_window(&desktop);

    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display, 12, 8).ch, '^');
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_pointer_motion_does_not_repaint_unrelated_pixels(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    desktop_pointer_event_t ev;
    uint32_t sentinel = 0x0BADCAFEu;
    uint32_t white;
    int sentinel_index = 300 * 480 + 400;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_render(&desktop);

    pointer_motion_pixels[sentinel_index] = sentinel;
    white = framebuffer_pack_rgb(&fb, 255, 255, 255);

    k_memset(&ev, 0, sizeof(ev));
    ev.x = 31;
    ev.y = 12;
    ev.pixel_x = 248;
    ev.pixel_y = 192;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[sentinel_index], sentinel);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[192 * 480 + 248], white);
    KTEST_EXPECT_NE(tc, pointer_motion_pixels[192 * 480 + 240], white);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_fast_pointer_motion_keeps_cursor_visible_at_edge(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    desktop_pointer_event_t ev;
    uint32_t white;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_render(&desktop);

    white = framebuffer_pack_rgb(&fb, 255, 255, 255);
    k_memset(&ev, 0, sizeof(ev));
    ev.x = 59;
    ev.y = 24;
    ev.pixel_x = 479;
    ev.pixel_y = 399;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, desktop.pointer_pixel_x, 472);
    KTEST_EXPECT_EQ(tc, desktop.pointer_pixel_y, 388);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[388 * 480 + 472], white);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_fast_pointer_motion_repaints_only_cursor_regions(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    desktop_pointer_event_t ev;
    uint32_t sentinel = 0x0BADCAFEu;
    uint32_t white;
    int sentinel_index = 240 * 480 + 300;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_render(&desktop);

    pointer_motion_pixels[sentinel_index] = sentinel;
    white = framebuffer_pack_rgb(&fb, 255, 255, 255);
    k_memset(&ev, 0, sizeof(ev));
    ev.x = 50;
    ev.y = 18;
    ev.pixel_x = 400;
    ev.pixel_y = 288;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[sentinel_index], sentinel);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[288 * 480 + 400], white);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_window_drag_repaints_only_drag_regions(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    desktop_pointer_event_t ev;
    uint32_t sentinel = 0x0BADCAFEu;
    int sentinel_index = 370 * 480 + 470;
    int files_id;
    const desktop_window_t *win;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    files_id = desktop_open_app_window(&desktop, DESKTOP_APP_FILES);
    desktop_render(&desktop);

    win = desktop_window_for_test(&desktop, files_id);
    KTEST_ASSERT_NOT_NULL(tc, win);

    k_memset(&ev, 0, sizeof(ev));
    ev.left_down = 1;
    ev.pixel_x = win->rect.x + 20;
    ev.pixel_y = win->rect.y + 8;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    pointer_motion_pixels[sentinel_index] = sentinel;

    ev.pixel_x = win->rect.x + 48;
    ev.pixel_y = win->rect.y + 32;
    ev.x = ev.pixel_x / (int)GUI_FONT_W;
    ev.y = ev.pixel_y / (int)GUI_FONT_H;
    desktop_handle_pointer(&desktop, &ev);

    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[sentinel_index], sentinel);

    desktop_test_destroy(&desktop);
}

static void test_framebuffer_shell_write_repaints_only_dirty_terminal_cells(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t sentinel = 0x0BADCAFEu;
    int sentinel_index = 300 * 480 + 400;
    int shell_sentinel_index;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    pointer_motion_pixels[sentinel_index] = sentinel;
    shell_sentinel_index = (desktop.shell_content.y + 5) * 16 * 480 +
                           (desktop.shell_content.x + 20) * 8;
    pointer_motion_pixels[shell_sentinel_index] = sentinel;

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, "x", 1), 1);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[sentinel_index], sentinel);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[shell_sentinel_index],
                    sentinel);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_backspace_repaints_only_dirty_terminal_cells(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t sentinel = 0x0BADCAFEu;
    int shell_sentinel_index;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);
    KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "ab", 2), 2);

    shell_sentinel_index = (desktop.shell_content.y + 5) * 16 * 480 +
                           (desktop.shell_content.x + 20) * 8;
    pointer_motion_pixels[shell_sentinel_index] = sentinel;

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, "\b", 1), 1);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[shell_sentinel_index],
                    sentinel);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_newline_repaints_only_dirty_terminal_cells(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t sentinel = 0x0BADCAFEu;
    int shell_sentinel_index;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    shell_sentinel_index = (desktop.shell_content.y + 5) * 16 * 480 +
                           (desktop.shell_content.x + 20) * 8;
    pointer_motion_pixels[shell_sentinel_index] = sentinel;

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, "\n", 1), 1);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[shell_sentinel_index],
                    sentinel);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_shell_backspace_prompt_redraw_keeps_unrelated_pixels(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    const char redraw[] =
        "\r\x1b[36mdrunix:\x1b[32m/\x1b[36m> \x1b[0ma ";
    uint32_t sentinel = 0x0BADCAFEu;
    int shell_sentinel_index;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);
    KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "drunix:/> ab", 11), 11);

    shell_sentinel_index = (desktop.shell_content.y + 5) * 16 * 480 +
                           (desktop.shell_content.x + 20) * 8;
    pointer_motion_pixels[shell_sentinel_index] = sentinel;

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, redraw,
                                                     sizeof(redraw) - 1),
                    sizeof(redraw) - 1);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[shell_sentinel_index],
                    sentinel);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_shell_return_prompt_keeps_unrelated_pixels(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    const char prompt[] =
        "\n\x1b[36mdrunix:\x1b[32m/\x1b[36m> \x1b[0m";
    uint32_t sentinel = 0x0BADCAFEu;
    int shell_sentinel_index;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);
    KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "ls", 2), 2);

    shell_sentinel_index = (desktop.shell_content.y + 5) * 16 * 480 +
                           (desktop.shell_content.x + 20) * 8;
    pointer_motion_pixels[shell_sentinel_index] = sentinel;

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, prompt,
                                                     sizeof(prompt) - 1),
                    sizeof(prompt) - 1);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[shell_sentinel_index],
                    sentinel);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_terminal_scroll_keeps_padding_pixels(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t sentinel = 0x0BADCAFEu;
    int sentinel_index;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    for (int row = 0; row < desktop.shell_terminal.rows - 1; row++)
        KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "\n", 1),
                        1);

    sentinel_index = (desktop.shell_pixel_rect.y + 2) * 480 +
                     desktop.shell_pixel_rect.x + 2;
    pointer_motion_pixels[sentinel_index] = sentinel;

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, "\n", 1), 1);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[sentinel_index], sentinel);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_terminal_scroll_does_not_copy_mouse_pointer(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    desktop_pointer_event_t ev;
    uint32_t white;
    int pointer_x;
    int pointer_y;
    int copied_pointer_index;
    int current_pointer_index;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    for (int row = 0; row < desktop.shell_terminal.rows - 1; row++)
        KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "\n", 1),
                        1);

    pointer_x = desktop.shell_pixel_rect.x +
                desktop.shell_terminal.padding_x + 24;
    pointer_y = desktop.shell_pixel_rect.y +
                desktop.shell_terminal.padding_y +
                (int)GUI_FONT_H * 2 + 3;
    k_memset(&ev, 0, sizeof(ev));
    ev.x = pointer_x / (int)GUI_FONT_W;
    ev.y = pointer_y / (int)GUI_FONT_H;
    ev.pixel_x = pointer_x;
    ev.pixel_y = pointer_y;
    desktop_handle_pointer(&desktop, &ev);

    white = framebuffer_pack_rgb(&fb, 255, 255, 255);
    current_pointer_index = pointer_y * 480 + pointer_x;
    copied_pointer_index = (pointer_y - (int)GUI_FONT_H) * 480 + pointer_x;
    KTEST_ASSERT_EQ(tc, pointer_motion_pixels[current_pointer_index], white);

    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, "\n", 1), 1);
    KTEST_EXPECT_NE(tc, pointer_motion_pixels[copied_pointer_index], white);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[current_pointer_index], white);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_terminal_scroll_paints_line_written_before_newline(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t terminal_fg;
    int found_fg;
    int glyph_base_x;
    int glyph_base_y;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    for (int row = 0; row < desktop.shell_terminal.rows - 1; row++)
        KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "\n", 1),
                        1);

    KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "A\n", 2), 2);
    KTEST_ASSERT_EQ(tc,
                    gui_terminal_cell_at(&desktop.shell_terminal,
                                         0,
                                         desktop.shell_terminal.rows - 2).ch,
                    'A');

    terminal_fg = framebuffer_pack_rgb(&fb, 0xf6, 0xf1, 0xde);
    glyph_base_x = desktop.shell_pixel_rect.x +
                   desktop.shell_terminal.padding_x;
    glyph_base_y = desktop.shell_pixel_rect.y +
                   desktop.shell_terminal.padding_y +
                   (desktop.shell_terminal.rows - 2) * (int)GUI_FONT_H;
    found_fg = 0;
    for (int py = 0; py < (int)GUI_FONT_H; py++) {
        for (int px = 0; px < (int)GUI_FONT_W; px++) {
            int index = (glyph_base_y + py) * 480 + glyph_base_x + px;

            if (pointer_motion_pixels[index] == terminal_fg)
                found_fg = 1;
        }
    }

    KTEST_EXPECT_TRUE(tc, found_fg);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_shell_scroll_keeps_overlapped_window_pixels(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    const desktop_window_t *help_win;
    uint32_t help_fg;
    uint32_t before;
    uint32_t after;
    char scroll_buf[64];
    int scroll_len;
    int help_id;
    int sample_x;
    int sample_y;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    help_id = desktop_open_app_window(&desktop, DESKTOP_APP_HELP);
    KTEST_ASSERT_TRUE(tc, help_id > 0);
    desktop_render(&desktop);

    help_win = desktop_window_for_test(&desktop, help_id);
    KTEST_ASSERT_NOT_NULL(tc, help_win);

    help_fg = framebuffer_pack_rgb(&fb, 0xff, 0xf7, 0xe8);
    sample_x = help_win->content_rect.x + 6 + 3;
    sample_y = help_win->content_rect.y + 6 + 2 * (int)GUI_FONT_H + 6;
    before = pointer_motion_pixels[sample_y * 480 + sample_x];
    KTEST_EXPECT_EQ(tc, before, help_fg);

    scroll_len = desktop.shell_terminal.rows + 1;
    if (scroll_len > (int)sizeof(scroll_buf))
        scroll_len = (int)sizeof(scroll_buf);
    k_memset(scroll_buf, '\n', (uint32_t)scroll_len);

    KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, scroll_buf,
                                                     (uint32_t)scroll_len),
                    scroll_len);
    after = pointer_motion_pixels[sample_y * 480 + sample_x];
    KTEST_EXPECT_EQ(tc, after, help_fg);

    desktop_test_destroy(&desktop);
}

static void scroll_interleave_moves_pointer(desktop_state_t *desktop)
{
    desktop_pointer_event_t ev;
    int pointer_x;
    int pointer_y;

    if (!desktop)
        return;

    pointer_x = desktop->shell_pixel_rect.x +
                desktop->shell_terminal.padding_x + 32;
    pointer_y = desktop->shell_pixel_rect.y +
                desktop->shell_terminal.padding_y +
                (int)GUI_FONT_H * 3 + 4;
    k_memset(&ev, 0, sizeof(ev));
    ev.x = pointer_x / (int)GUI_FONT_W;
    ev.y = pointer_y / (int)GUI_FONT_H;
    ev.pixel_x = pointer_x;
    ev.pixel_y = pointer_y;
    desktop_handle_pointer(desktop, &ev);
}

static void test_framebuffer_terminal_scroll_blocks_mouse_pointer_interleave(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t white;
    int pointer_x;
    int pointer_y;
    int copied_pointer_index;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    for (int row = 0; row < desktop.shell_terminal.rows - 1; row++)
        KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "\n", 1),
                        1);

    pointer_x = desktop.shell_pixel_rect.x +
                desktop.shell_terminal.padding_x + 32;
    pointer_y = desktop.shell_pixel_rect.y +
                desktop.shell_terminal.padding_y +
                (int)GUI_FONT_H * 3 + 4;
    copied_pointer_index = (pointer_y - (int)GUI_FONT_H) * 480 + pointer_x;
    white = framebuffer_pack_rgb(&fb, 255, 255, 255);

    desktop_set_scroll_interleave_hook_for_test(scroll_interleave_moves_pointer);
    KTEST_EXPECT_EQ(tc, desktop_write_console_output(&desktop, "\n", 1), 1);
    desktop_set_scroll_interleave_hook_for_test(0);

    KTEST_EXPECT_NE(tc, pointer_motion_pixels[copied_pointer_index], white);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_desktop_renders_shell_terminal_background(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t terminal_bg;
    int bg_x;
    int bg_y;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    terminal_bg = framebuffer_pack_rgb(&fb, 0x08, 0x10, 0x18);
    bg_x = desktop.shell_pixel_rect.x + 2;
    bg_y = desktop.shell_pixel_rect.y + 2;

    KTEST_ASSERT_TRUE(tc, desktop.shell_pixel_rect.w >= 4);
    KTEST_ASSERT_TRUE(tc, desktop.shell_pixel_rect.h >= 4);
    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[bg_y * 480 + bg_x],
                    terminal_bg);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_shell_terminal_rect_includes_padding_and_cells(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    KTEST_EXPECT_EQ(tc, desktop.shell_terminal.pixel_rect.w,
                    desktop.shell_terminal.padding_x +
                        desktop.shell_terminal.cols * (int)GUI_FONT_W);
    KTEST_EXPECT_EQ(tc, desktop.shell_terminal.pixel_rect.h,
                    desktop.shell_terminal.padding_y +
                        desktop.shell_terminal.rows * (int)GUI_FONT_H);

    desktop_test_destroy(&desktop);
}

static void test_framebuffer_shell_window_keeps_right_border_visible(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t border;
    int right_x;
    int border_y;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    border = framebuffer_pack_rgb(&fb, 0xf2, 0xc9, 0x4c);
    right_x = desktop.window_pixel_rect.x + desktop.window_pixel_rect.w - 1;
    border_y = desktop.shell_pixel_rect.y + 4;

    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[border_y * 480 + right_x],
                    border);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_desktop_console_output_renders_terminal_glyph(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t terminal_fg;
    int glyph_x;
    int glyph_y;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);

    KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "A", 1), 1);

    terminal_fg = framebuffer_pack_rgb(&fb, 0xf6, 0xf1, 0xde);
    glyph_x = desktop.shell_pixel_rect.x + 8 + 3;
    glyph_y = desktop.shell_pixel_rect.y + 6;

    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[glyph_y * 480 + glyph_x],
                    terminal_fg);
    desktop_test_destroy(&desktop);
}

static void test_framebuffer_desktop_terminal_edges_render_inside_window(
    ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;
    uint32_t cursor;
    int cursor_x;
    int cursor_y;

    k_memset(pointer_motion_pixels, 0, sizeof(pointer_motion_pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pointer_motion_pixels;
    fb.pitch = 480u * sizeof(uint32_t);
    fb.width = 480u;
    fb.height = 400u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, pointer_motion_cells, 60, 25, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_open_shell_window(&desktop);
    desktop_render(&desktop);

    for (int row = 0; row < desktop.shell_terminal.rows - 1; row++)
        KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, "\n", 1),
                        1);
    for (int col = 0; col < desktop.shell_terminal.cols - 1; col++)
        KTEST_ASSERT_EQ(tc, desktop_write_console_output(&desktop, " ", 1),
                        1);

    cursor = framebuffer_pack_rgb(&fb, 0x67, 0xc5, 0x8f);
    cursor_x = desktop.shell_pixel_rect.x + desktop.shell_terminal.padding_x +
               (desktop.shell_terminal.cols - 1) * (int)GUI_FONT_W;
    cursor_y = desktop.shell_pixel_rect.y + desktop.shell_terminal.padding_y +
               (desktop.shell_terminal.rows - 1) * (int)GUI_FONT_H +
               ((int)GUI_FONT_H - 2);

    KTEST_EXPECT_EQ(tc, pointer_motion_pixels[cursor_y * 480 + cursor_x],
                    cursor);
    KTEST_EXPECT_TRUE(tc,
                      cursor_x < desktop.window_pixel_rect.x +
                          desktop.window_pixel_rect.w);
    KTEST_EXPECT_TRUE(tc,
                      cursor_y < desktop.window_pixel_rect.y +
                          desktop.window_pixel_rect.h);
    desktop_test_destroy(&desktop);
}

static void test_desktop_can_use_framebuffer_presentation_target(ktest_case_t *tc)
{
    static uint32_t pixels[16 * 16];
    gui_display_t display;
    desktop_state_t desktop;
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 16u * sizeof(uint32_t);
    fb.width = 16u;
    fb.height = 16u;
    fb.bpp = 32u;
    fb.red_pos = 16u;
    fb.red_size = 8u;
    fb.green_pos = 8u;
    fb.green_size = 8u;
    fb.blue_pos = 0u;
    fb.blue_size = 8u;

    gui_display_init(&display, desktop_cells, 2, 1, 0x0f);
    desktop_init(&desktop, &display);
    desktop_set_framebuffer_target(&desktop, &fb);
    desktop_render(&desktop);

    KTEST_EXPECT_NE(tc, pixels[0], 0u);
    desktop_test_destroy(&desktop);
}

static void framebuffer_test_init_valid_record(multiboot_info_t *mbi)
{
    k_memset(mbi, 0, sizeof(*mbi));
    mbi->flags = MULTIBOOT_FLAG_FRAMEBUFFER;
    mbi->framebuffer_addr = 0xE0000000ull;
    mbi->framebuffer_pitch = 1024u * 4u;
    mbi->framebuffer_width = 1024u;
    mbi->framebuffer_height = 768u;
    mbi->framebuffer_bpp = 32u;
    mbi->framebuffer_type = MULTIBOOT_FRAMEBUFFER_TYPE_RGB;
    mbi->framebuffer_red_field_position = 16u;
    mbi->framebuffer_red_mask_size = 8u;
    mbi->framebuffer_green_field_position = 8u;
    mbi->framebuffer_green_mask_size = 8u;
    mbi->framebuffer_blue_field_position = 0u;
    mbi->framebuffer_blue_mask_size = 8u;
}

static void test_framebuffer_info_accepts_1024_768_32_rgb(ktest_case_t *tc)
{
    multiboot_info_t mbi;
    framebuffer_info_t info;

    framebuffer_test_init_valid_record(&mbi);
    k_memset(&info, 0, sizeof(info));

    KTEST_EXPECT_EQ(tc, framebuffer_info_from_multiboot(&mbi, &info), 0);
    KTEST_EXPECT_EQ(tc, info.width, 1024u);
    KTEST_EXPECT_EQ(tc, info.height, 768u);
    KTEST_EXPECT_EQ(tc, info.bpp, 32u);
    KTEST_EXPECT_EQ(tc, info.cell_cols, 128u);
    KTEST_EXPECT_EQ(tc, info.cell_rows, 48u);
}

static void test_multiboot_framebuffer_color_info_uses_grub_layout(ktest_case_t *tc)
{
    KTEST_EXPECT_EQ(tc,
                    __builtin_offsetof(multiboot_info_t,
                                       framebuffer_red_field_position),
                    112u);
    KTEST_EXPECT_EQ(tc,
                    __builtin_offsetof(multiboot_info_t,
                                       framebuffer_blue_mask_size),
                    117u);
}

static void
test_boot_framebuffer_grid_clamps_to_static_cell_buffer(ktest_case_t *tc)
{
    framebuffer_info_t info;
    int cols = 0;
    int rows = 0;

    k_memset(&info, 0, sizeof(info));
    info.cell_cols = 256u;
    info.cell_rows = 96u;

    KTEST_EXPECT_EQ(tc, boot_framebuffer_grid_for_test(&info, &cols, &rows), 1);
    KTEST_EXPECT_EQ(tc, cols, 128);
    KTEST_EXPECT_EQ(tc, rows, 48);
}

static void test_framebuffer_mapping_reaches_high_physical_lfb(ktest_case_t *tc)
{
    uint32_t high_fb = 0xE0000000u;
    uint32_t *kernel_pte;
    uint32_t *user_pte;
    uint32_t user_pd;

    KTEST_ASSERT_EQ(tc,
                    paging_identity_map_kernel_range(high_fb, 0x2000u,
                                                     PG_PRESENT | PG_WRITABLE),
                    0);
    KTEST_ASSERT_EQ(tc, paging_walk(PAGE_DIR_ADDR, high_fb, &kernel_pte), 0);
    KTEST_EXPECT_EQ(tc, paging_entry_addr(*kernel_pte), high_fb);
    KTEST_EXPECT_EQ(tc, *kernel_pte & (PG_PRESENT | PG_WRITABLE),
                    PG_PRESENT | PG_WRITABLE);
    KTEST_EXPECT_EQ(tc, *kernel_pte & PG_USER, 0u);

    user_pd = paging_create_user_space();
    KTEST_ASSERT_NE(tc, user_pd, 0u);
    KTEST_ASSERT_EQ(tc, paging_walk(user_pd, high_fb, &user_pte), 0);
    KTEST_EXPECT_EQ(tc, paging_entry_addr(*user_pte), high_fb);
    KTEST_EXPECT_EQ(tc, *user_pte & PG_USER, 0u);
}

static void test_framebuffer_info_rejects_address_above_uintptr(ktest_case_t *tc)
{
    multiboot_info_t mbi;
    framebuffer_info_t info;

    framebuffer_test_init_valid_record(&mbi);
    mbi.framebuffer_addr = (uint64_t)UINTPTR_MAX + 1u;

    KTEST_EXPECT_NE(tc, framebuffer_info_from_multiboot(&mbi, &info), 0);
}

static void test_framebuffer_info_rejects_extent_above_uintptr(ktest_case_t *tc)
{
    multiboot_info_t mbi;
    framebuffer_info_t info;

    framebuffer_test_init_valid_record(&mbi);
    mbi.framebuffer_addr = (uint64_t)UINTPTR_MAX - 4095u;

    KTEST_EXPECT_NE(tc, framebuffer_info_from_multiboot(&mbi, &info), 0);
}

static void test_framebuffer_info_rejects_pitch_overflow_width(ktest_case_t *tc)
{
    multiboot_info_t mbi;
    framebuffer_info_t info;

    framebuffer_test_init_valid_record(&mbi);
    mbi.framebuffer_width = (UINT32_MAX / 4u) + 1u;
    mbi.framebuffer_pitch = UINT32_MAX;

    KTEST_EXPECT_NE(tc, framebuffer_info_from_multiboot(&mbi, &info), 0);
}

static void test_framebuffer_info_rejects_rgb_mask_past_32_bits(ktest_case_t *tc)
{
    multiboot_info_t mbi;
    framebuffer_info_t info;

    framebuffer_test_init_valid_record(&mbi);
    mbi.framebuffer_red_field_position = 28u;
    mbi.framebuffer_red_mask_size = 8u;

    KTEST_EXPECT_NE(tc, framebuffer_info_from_multiboot(&mbi, &info), 0);
}

static void test_framebuffer_info_rejects_overlapping_rgb_masks(ktest_case_t *tc)
{
    multiboot_info_t mbi;
    framebuffer_info_t info;

    framebuffer_test_init_valid_record(&mbi);
    mbi.framebuffer_green_field_position = 12u;
    mbi.framebuffer_green_mask_size = 8u;

    KTEST_EXPECT_NE(tc, framebuffer_info_from_multiboot(&mbi, &info), 0);
}

static void test_framebuffer_pack_rgb_uses_mask_positions(ktest_case_t *tc)
{
    framebuffer_info_t info;

    k_memset(&info, 0, sizeof(info));
    info.red_pos = 16;
    info.red_size = 8;
    info.green_pos = 8;
    info.green_size = 8;
    info.blue_pos = 0;
    info.blue_size = 8;

    KTEST_EXPECT_EQ(tc, framebuffer_pack_rgb(&info, 0x12, 0x34, 0x56),
                    0x00123456u);
}

static void test_framebuffer_draw_rect_outline_handles_large_dimensions(ktest_case_t *tc)
{
    uint32_t pixels[4 * 4];
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 4u * sizeof(uint32_t);
    fb.width = 4;
    fb.height = 4;

    framebuffer_draw_rect_outline(&fb, -2, -2147483646, 2147483647, 2147483647,
                                  0x00ABCDEFu);

    KTEST_EXPECT_EQ(tc, pixels[0 * 4 + 0], 0x00ABCDEFu);
    KTEST_EXPECT_EQ(tc, pixels[0 * 4 + 3], 0x00ABCDEFu);
    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 0], 0u);
    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 1], 0u);
}

static void test_framebuffer_draw_rect_outline_clips_to_bounds(ktest_case_t *tc)
{
    uint32_t pixels[6 * 6];
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 6u * sizeof(uint32_t);
    fb.width = 6;
    fb.height = 6;

    framebuffer_draw_rect_outline(&fb, -1, 1, 4, 4, 0x00ABCDEFu);

    KTEST_EXPECT_EQ(tc, pixels[1 * 6 + 0], 0x00ABCDEFu);
    KTEST_EXPECT_EQ(tc, pixels[1 * 6 + 2], 0x00ABCDEFu);
    KTEST_EXPECT_EQ(tc, pixels[3 * 6 + 0], 0u);
    KTEST_EXPECT_EQ(tc, pixels[4 * 6 + 0], 0x00ABCDEFu);
}

static void test_framebuffer_draw_text_clipped_honors_pixel_clip(ktest_case_t *tc)
{
    static uint32_t pixels[32 * 20];
    framebuffer_info_t fb;
    gui_pixel_rect_t clip = { 8, 0, 8, 16 };

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 32u * sizeof(uint32_t);
    fb.width = 32;
    fb.height = 20;

    framebuffer_draw_text_clipped(&fb, &clip, 0, 0, "AB",
                                  0x00FFFFFFu, 0x00112233u);

    KTEST_EXPECT_EQ(tc, pixels[0 * 32 + 0], 0u);
    KTEST_EXPECT_EQ(tc, pixels[0 * 32 + 8], 0x00FFFFFFu);
    KTEST_EXPECT_EQ(tc, pixels[0 * 32 + 16], 0u);
}

static void test_framebuffer_draw_text_clipped_huge_origin_is_noop(ktest_case_t *tc)
{
    uint32_t pixels[16 * 16];
    framebuffer_info_t fb;
    gui_pixel_rect_t clip = { 0, 0, 8, 16 };

    for (uint32_t i = 0; i < 16u * 16u; i++)
        pixels[i] = 0xDEADBEEFu;
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 16u * sizeof(uint32_t);
    fb.width = 16;
    fb.height = 16;

    framebuffer_draw_text_clipped(&fb, &clip, 2147483644, 0, "AB",
                                  0x00FFFFFFu, 0x00112233u);

    KTEST_EXPECT_EQ(tc, pixels[0], 0xDEADBEEFu);
    KTEST_EXPECT_EQ(tc, pixels[15], 0xDEADBEEFu);
}

static void test_framebuffer_draw_scrollbar_places_thumb(ktest_case_t *tc)
{
    uint32_t pixels[8 * 40];
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 8u * sizeof(uint32_t);
    fb.width = 8;
    fb.height = 40;

    framebuffer_draw_scrollbar(&fb, 2, 4, 4, 32, 100, 25, 50,
                               0x00010101u, 0x00EEEEEEu);

    KTEST_EXPECT_EQ(tc, pixels[4 * 8 + 2], 0x00010101u);
    KTEST_EXPECT_EQ(tc, pixels[20 * 8 + 2], 0x00EEEEEEu);
    KTEST_EXPECT_EQ(tc, pixels[35 * 8 + 5], 0x00010101u);
}

static void test_framebuffer_draw_scrollbar_handles_large_row_counts(ktest_case_t *tc)
{
    uint32_t pixels[8 * 40];
    framebuffer_info_t fb;

    for (uint32_t i = 0; i < 8u * 40u; i++)
        pixels[i] = 0xDEADBEEFu;
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 8u * sizeof(uint32_t);
    fb.width = 8;
    fb.height = 40;

    framebuffer_draw_scrollbar(&fb, 2, 4, 4, 32, 2147483647, 1073741824,
                               2147483646, 0x00010101u, 0x00EEEEEEu);

    KTEST_EXPECT_EQ(tc, pixels[4 * 8 + 2], 0x00010101u);
    KTEST_EXPECT_EQ(tc, pixels[20 * 8 + 2], 0x00EEEEEEu);
    KTEST_EXPECT_EQ(tc, pixels[36 * 8 + 2], 0xDEADBEEFu);
}

static void test_framebuffer_fill_rect_clips_to_bounds(ktest_case_t *tc)
{
    uint32_t pixels[4 * 4];
    framebuffer_info_t info;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&info, 0, sizeof(info));
    info.address = (uintptr_t)pixels;
    info.pitch = 4u * sizeof(uint32_t);
    info.width = 4;
    info.height = 4;

    framebuffer_fill_rect(&info, -1, 1, 3, 2, 0xAABBCCDDu);

    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 0], 0xAABBCCDDu);
    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 1], 0xAABBCCDDu);
    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 2], 0u);
    KTEST_EXPECT_EQ(tc, pixels[0], 0u);
    KTEST_EXPECT_EQ(tc, pixels[2 * 4 + 0], 0xAABBCCDDu);
    KTEST_EXPECT_EQ(tc, pixels[2 * 4 + 1], 0xAABBCCDDu);
    KTEST_EXPECT_EQ(tc, pixels[2 * 4 + 2], 0u);
    KTEST_EXPECT_EQ(tc, pixels[3 * 4 + 0], 0u);
}

static void test_framebuffer_fill_rect_handles_large_dimensions(ktest_case_t *tc)
{
    uint32_t pixels[4 * 4];
    framebuffer_info_t info;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&info, 0, sizeof(info));
    info.address = (uintptr_t)pixels;
    info.pitch = 4u * sizeof(uint32_t);
    info.width = 4;
    info.height = 4;

    framebuffer_fill_rect(&info, 2, 1, 2147483647, 1, 0x11223344u);

    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 0], 0u);
    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 1], 0u);
    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 2], 0x11223344u);
    KTEST_EXPECT_EQ(tc, pixels[1 * 4 + 3], 0x11223344u);
}

static void test_framebuffer_draws_pixel_arrow_cursor(ktest_case_t *tc)
{
    static uint32_t pixels[16 * 16];
    framebuffer_info_t fb;

    k_memset(pixels, 0, sizeof(pixels));
    k_memset(&fb, 0, sizeof(fb));
    fb.address = (uintptr_t)pixels;
    fb.pitch = 16u * sizeof(uint32_t);
    fb.width = 16;
    fb.height = 16;

    framebuffer_draw_cursor(&fb, 2, 2, 0x00FFFFFFu, 0x00000000u);

    KTEST_EXPECT_EQ(tc, pixels[2 * 16 + 2], 0x00FFFFFFu);
    KTEST_EXPECT_EQ(tc, pixels[3 * 16 + 2], 0x00FFFFFFu);
    KTEST_EXPECT_EQ(tc, pixels[3 * 16 + 3], 0x00FFFFFFu);
}

static void test_font8x16_glyph_returns_stable_storage(ktest_case_t *tc)
{
    const uint8_t *glyph_a = font8x16_glyph('A');
    uint8_t first_row = glyph_a[0];
    const uint8_t *glyph_b = font8x16_glyph('B');

    KTEST_EXPECT_NE(tc, (uint32_t)glyph_a, (uint32_t)glyph_b);
    KTEST_EXPECT_EQ(tc, glyph_a[0], first_row);
}

static void test_framebuffer_draw_glyph_writes_foreground_pixels(ktest_case_t *tc)
{
    static uint32_t pixels[16 * 16];
    framebuffer_info_t info;

    for (uint32_t i = 0; i < 16u * 16u; i++)
        pixels[i] = 0xDEADBEEFu;
    k_memset(&info, 0, sizeof(info));
    info.address = (uintptr_t)pixels;
    info.pitch = 16u * sizeof(uint32_t);
    info.width = 16;
    info.height = 16;

    framebuffer_draw_glyph(&info, 0, 0, 'A', 0x00FFFFFFu, 0x00112233u);

    KTEST_EXPECT_EQ(tc, pixels[0 * 16 + 3], 0x00FFFFFFu);
    KTEST_EXPECT_EQ(tc, pixels[1 * 16 + 3], 0x00FFFFFFu);
    KTEST_EXPECT_EQ(tc, pixels[0 * 16 + 0], 0x00112233u);
}

static void test_framebuffer_draw_glyph_rejects_overflowing_position(ktest_case_t *tc)
{
    uint32_t pixels[4 * 4];
    framebuffer_info_t info;

    for (uint32_t i = 0; i < 4u * 4u; i++)
        pixels[i] = 0xDEADBEEFu;
    k_memset(&info, 0, sizeof(info));
    info.address = (uintptr_t)pixels;
    info.pitch = 4u * sizeof(uint32_t);
    info.width = 4;
    info.height = 4;

    framebuffer_draw_glyph(&info, 2147483647, 0, 'A',
                           0x00FFFFFFu, 0x00112233u);

    for (uint32_t i = 0; i < 4u * 4u; i++)
        KTEST_EXPECT_EQ(tc, pixels[i], 0xDEADBEEFu);
}

static void test_framebuffer_pack_rgb_scales_to_mask_size(ktest_case_t *tc)
{
    framebuffer_info_t info;

    k_memset(&info, 0, sizeof(info));
    info.red_pos = 20;
    info.red_size = 10;
    info.green_pos = 10;
    info.green_size = 10;
    info.blue_pos = 0;
    info.blue_size = 10;

    KTEST_EXPECT_EQ(tc, framebuffer_pack_rgb(&info, 0xff, 0xff, 0xff),
                    0x3fffffffu);
}

static ktest_case_t desktop_cases[] = {
    KTEST_CASE(test_terminal_write_wraps_and_retains_history),
    KTEST_CASE(test_terminal_render_uses_pixel_padding),
    KTEST_CASE(test_terminal_render_draws_underline_cursor_in_scrollback_view),
    KTEST_CASE(test_terminal_render_clips_cursor_to_surface_clip),
    KTEST_CASE(test_terminal_render_composes_scrollback_before_live_rows),
    KTEST_CASE(test_terminal_render_uses_ansi_foreground_color),
    KTEST_CASE(test_terminal_ansi_color_does_not_emit_escape_bytes),
    KTEST_CASE(test_terminal_clear_discards_history_and_resets_cursor),
    KTEST_CASE(test_terminal_writes_to_later_rows_after_hardening),
    KTEST_CASE(test_terminal_init_rejects_overflow_dimensions_without_touching_buffers),
    KTEST_CASE(test_terminal_ansi_digit_sequence_caps_without_overflow),
    KTEST_CASE(test_terminal_scroll_view_clamps_large_positive_and_negative_inputs),
    KTEST_CASE(test_terminal_write_rejects_lengths_above_int_max),
    KTEST_CASE(test_terminal_destroy_preserves_static_buffers_and_clears_owned_buffers),
    KTEST_CASE(test_gui_display_fill_rect_clips_to_bounds),
    KTEST_CASE(test_gui_display_draw_text_stops_at_region_edge),
    KTEST_CASE(test_gui_display_presents_cells_to_framebuffer),
    KTEST_CASE(test_desktop_files_app_lists_root_entries),
    KTEST_CASE(test_desktop_files_app_replaces_last_visible_line_when_truncated),
    KTEST_CASE(test_desktop_help_app_has_keyboard_page),
    KTEST_CASE(test_desktop_help_app_q_requests_close),
    KTEST_CASE(test_desktop_app_render_clips_to_content_rect),
    KTEST_CASE(test_desktop_help_app_render_is_visible_in_framebuffer),
    KTEST_CASE(test_desktop_help_app_key_input_is_ignored_while_launcher_open),
    KTEST_CASE(test_desktop_open_invalid_app_kind_is_rejected),
    KTEST_CASE(test_desktop_processes_app_handles_empty_snapshot),
    KTEST_CASE(test_desktop_open_processes_refreshes_after_late_process),
    KTEST_CASE(test_desktop_boot_layout_opens_shell_window),
    KTEST_CASE(test_desktop_layout_scales_to_framebuffer_grid),
    KTEST_CASE(test_desktop_render_draws_taskbar_and_launcher_label),
    KTEST_CASE(test_desktop_launcher_enter_opens_files_window),
    KTEST_CASE(test_desktop_launcher_enter_does_not_refresh_files_view),
    KTEST_CASE(test_desktop_framebuffer_launcher_click_opens_files_window),
    KTEST_CASE(test_desktop_framebuffer_launcher_click_uses_visible_item_rows),
    KTEST_CASE(test_desktop_text_launcher_keeps_bottom_border_visible),
    KTEST_CASE(test_desktop_taskbar_click_focuses_processes_window),
    KTEST_CASE(test_desktop_taskbar_shell_refocus_forwards_keys),
    KTEST_CASE(test_desktop_text_taskbar_renders_open_window_labels),
    KTEST_CASE(test_desktop_shell_open_matches_rendered_window_rect),
    KTEST_CASE(test_desktop_shell_output_still_routes_after_mini_apps_open),
    KTEST_CASE(test_desktop_close_button_closes_files_window),
    KTEST_CASE(test_desktop_shell_close_button_closes_visible_shell_window),
    KTEST_CASE(test_desktop_title_drag_moves_window_and_clamps_top_left),
    KTEST_CASE(test_desktop_shell_drag_preserves_window_size),
    KTEST_CASE(test_desktop_shell_drag_syncs_terminal_pixel_rect_in_framebuffer_mode),
    KTEST_CASE(test_desktop_open_clamps_help_window_to_framebuffer),
    KTEST_CASE(test_framebuffer_windows_render_in_z_order),
    KTEST_CASE(test_framebuffer_grid_desktop_renders_taskbar_and_shell_title),
    KTEST_CASE(test_desktop_init_binds_global_keyboard_target),
    KTEST_CASE(test_desktop_escape_opens_launcher_and_consumes_input),
    KTEST_CASE(test_desktop_plain_text_forwards_to_shell_when_focused),
    KTEST_CASE(test_desktop_write_process_output_targets_shell_surface),
    KTEST_CASE(test_desktop_child_process_group_output_targets_shell_surface),
    KTEST_CASE(test_desktop_unrelated_process_group_output_is_rejected),
    KTEST_CASE(test_syscall_console_write_routes_session_output_to_desktop),
    KTEST_CASE(test_desktop_ansi_color_escape_updates_attr_without_printing),
    KTEST_CASE(test_desktop_full_screen_write_does_not_scroll_until_next_char),
    KTEST_CASE(test_desktop_scroll_syscall_moves_terminal_view),
    KTEST_CASE(test_desktop_new_output_snaps_scrollback_to_live),
    KTEST_CASE(test_syscall_clear_clears_desktop_shell_buffer),
    KTEST_CASE(test_tty_ctrl_c_echo_routes_to_desktop_shell_buffer),
    KTEST_CASE(test_mouse_packet_decode_preserves_motion_and_buttons),
    KTEST_CASE(test_mouse_packet_decode_saturates_overflow_motion),
    KTEST_CASE(test_mouse_stream_resyncs_after_noise),
    KTEST_CASE(test_mouse_stream_delivers_response_like_packet_headers),
    KTEST_CASE(test_mouse_stream_delivers_overflow_packets),
    KTEST_CASE(test_mouse_stream_keeps_response_like_bytes_inside_packet),
    KTEST_CASE(test_mouse_irq_drains_initial_packet_without_aux_status),
    KTEST_CASE(test_mouse_framebuffer_motion_uses_build_configured_scale),
    KTEST_CASE(test_mouse_text_motion_ignores_framebuffer_speed),
    KTEST_CASE(test_mouse_overflow_packet_keeps_framebuffer_cursor_visible),
    KTEST_CASE(test_desktop_pointer_click_focuses_shell_window),
    KTEST_CASE(test_desktop_pointer_click_ignores_hidden_shell_window),
    KTEST_CASE(test_desktop_open_builtin_window_focuses_and_reuses_instance),
    KTEST_CASE(test_desktop_window_focus_raise_updates_z_order),
    KTEST_CASE(test_desktop_focused_builtin_window_consumes_keys),
    KTEST_CASE(test_desktop_close_focused_non_shell_window_returns_shell_focus),
    KTEST_CASE(test_desktop_close_shell_window_stops_key_forwarding),
    KTEST_CASE(test_desktop_shell_click_focuses_shell_window_table_entry),
    KTEST_CASE(test_desktop_overlapped_pointer_hits_visible_top_window),
    KTEST_CASE(test_desktop_render_draws_visible_mouse_pointer),
    KTEST_CASE(test_desktop_pointer_event_moves_visible_mouse_pointer),
    KTEST_CASE(test_framebuffer_pointer_motion_does_not_repaint_unrelated_pixels),
    KTEST_CASE(test_framebuffer_fast_pointer_motion_keeps_cursor_visible_at_edge),
    KTEST_CASE(test_framebuffer_fast_pointer_motion_repaints_only_cursor_regions),
    KTEST_CASE(test_framebuffer_window_drag_repaints_only_drag_regions),
    KTEST_CASE(test_framebuffer_shell_write_repaints_only_dirty_terminal_cells),
    KTEST_CASE(test_framebuffer_backspace_repaints_only_dirty_terminal_cells),
    KTEST_CASE(test_framebuffer_newline_repaints_only_dirty_terminal_cells),
    KTEST_CASE(test_framebuffer_shell_backspace_prompt_redraw_keeps_unrelated_pixels),
    KTEST_CASE(test_framebuffer_shell_return_prompt_keeps_unrelated_pixels),
    KTEST_CASE(test_framebuffer_terminal_scroll_keeps_padding_pixels),
    KTEST_CASE(test_framebuffer_terminal_scroll_does_not_copy_mouse_pointer),
    KTEST_CASE(test_framebuffer_terminal_scroll_paints_line_written_before_newline),
    KTEST_CASE(test_framebuffer_shell_scroll_keeps_overlapped_window_pixels),
    KTEST_CASE(test_framebuffer_terminal_scroll_blocks_mouse_pointer_interleave),
    KTEST_CASE(test_framebuffer_desktop_renders_shell_terminal_background),
    KTEST_CASE(test_framebuffer_shell_terminal_rect_includes_padding_and_cells),
    KTEST_CASE(test_framebuffer_shell_window_keeps_right_border_visible),
    KTEST_CASE(test_framebuffer_desktop_console_output_renders_terminal_glyph),
    KTEST_CASE(test_framebuffer_desktop_terminal_edges_render_inside_window),
    KTEST_CASE(test_desktop_can_use_framebuffer_presentation_target),
    KTEST_CASE(test_framebuffer_info_accepts_1024_768_32_rgb),
    KTEST_CASE(test_multiboot_framebuffer_color_info_uses_grub_layout),
    KTEST_CASE(test_boot_framebuffer_grid_clamps_to_static_cell_buffer),
    KTEST_CASE(test_framebuffer_mapping_reaches_high_physical_lfb),
    KTEST_CASE(test_framebuffer_info_rejects_address_above_uintptr),
    KTEST_CASE(test_framebuffer_info_rejects_extent_above_uintptr),
    KTEST_CASE(test_framebuffer_info_rejects_pitch_overflow_width),
    KTEST_CASE(test_framebuffer_info_rejects_rgb_mask_past_32_bits),
    KTEST_CASE(test_framebuffer_info_rejects_overlapping_rgb_masks),
    KTEST_CASE(test_framebuffer_pack_rgb_uses_mask_positions),
    KTEST_CASE(test_framebuffer_draw_rect_outline_handles_large_dimensions),
    KTEST_CASE(test_framebuffer_draw_rect_outline_clips_to_bounds),
    KTEST_CASE(test_framebuffer_draw_text_clipped_honors_pixel_clip),
    KTEST_CASE(test_framebuffer_draw_text_clipped_huge_origin_is_noop),
    KTEST_CASE(test_framebuffer_draw_scrollbar_places_thumb),
    KTEST_CASE(test_framebuffer_draw_scrollbar_handles_large_row_counts),
    KTEST_CASE(test_framebuffer_fill_rect_clips_to_bounds),
    KTEST_CASE(test_framebuffer_fill_rect_handles_large_dimensions),
    KTEST_CASE(test_framebuffer_draws_pixel_arrow_cursor),
    KTEST_CASE(test_font8x16_glyph_returns_stable_storage),
    KTEST_CASE(test_framebuffer_draw_glyph_writes_foreground_pixels),
    KTEST_CASE(test_framebuffer_draw_glyph_rejects_overflowing_position),
    KTEST_CASE(test_framebuffer_pack_rgb_scales_to_mask_size),
};

ktest_suite_t *ktest_suite_desktop(void)
{
    static ktest_suite_t suite = KTEST_SUITE("desktop", desktop_cases);
    return &suite;
}
