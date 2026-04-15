#include "ktest.h"
#include "display.h"
#include "desktop.h"
#include "font8x16.h"
#include "framebuffer.h"
#include "kstring.h"
#include "mouse.h"
#include "process.h"
#include "syscall.h"
#include "tty.h"

static gui_cell_t desktop_cells[80 * 25];
static gui_cell_t large_desktop_cells[128 * 48];

extern int syscall_console_write_for_test(process_t *proc, const char *buf,
                                          uint32_t len);
extern int boot_framebuffer_grid_for_test(const framebuffer_info_t *fb,
                                          int *cols,
                                          int *rows);

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

    KTEST_EXPECT_EQ(tc, framebuffer_pack_rgb(&fb, 0xff, 0xff, 0x55),
                    pixels[0 * 16 + 2]);
    KTEST_EXPECT_EQ(tc, framebuffer_pack_rgb(&fb, 0x00, 0x00, 0xaa),
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
                    'S');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 3,
                                            desktop.launcher_rect.y + 1).ch,
                    'h');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 4,
                                            desktop.launcher_rect.y + 1).ch,
                    'e');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 5,
                                            desktop.launcher_rect.y + 1).ch,
                    'l');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.launcher_rect.x + 6,
                                            desktop.launcher_rect.y + 1).ch,
                    'l');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.shell_rect.x + 2,
                                            desktop.shell_rect.y).ch,
                    'S');
    KTEST_EXPECT_EQ(tc, gui_display_cell_at(&display,
                                            desktop.shell_rect.x + 3,
                                            desktop.shell_rect.y).ch,
                    'h');
}

static void test_desktop_init_binds_global_keyboard_target(ktest_case_t *tc)
{
    gui_display_t display;
    desktop_state_t desktop;

    gui_display_init(&display, desktop_cells, 80, 25, 0x0f);
    desktop_init(&desktop, &display);

    KTEST_EXPECT_EQ(tc, (uint32_t)desktop_global(), (uint32_t)&desktop);
    KTEST_EXPECT_TRUE(tc, desktop_is_active());
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

    KTEST_EXPECT_EQ(tc, syscall_handler(SYS_CLEAR, 0, 0, 0, 0, 0), 0);
    KTEST_EXPECT_EQ(tc,
                    gui_display_cell_at(&display,
                                        desktop.shell_content.x,
                                        desktop.shell_content.y).ch,
                    ' ');
    KTEST_EXPECT_EQ(tc, desktop.shell_cursor_x, 0);
    KTEST_EXPECT_EQ(tc, desktop.shell_cursor_y, 0);
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

static void test_mouse_stream_resyncs_after_noise_and_ack(ktest_case_t *tc)
{
    mouse_packet_stream_t stream;
    mouse_packet_t packet;

    mouse_stream_reset(&stream);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0xFA, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 0);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0x00, &packet), 0);
    KTEST_EXPECT_EQ(tc, stream.index, 0);
    KTEST_EXPECT_EQ(tc, mouse_stream_consume(&stream, 0xC8, &packet), 0);
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
    KTEST_CASE(test_gui_display_fill_rect_clips_to_bounds),
    KTEST_CASE(test_gui_display_draw_text_stops_at_region_edge),
    KTEST_CASE(test_gui_display_presents_cells_to_framebuffer),
    KTEST_CASE(test_desktop_boot_layout_opens_shell_window),
    KTEST_CASE(test_desktop_layout_scales_to_framebuffer_grid),
    KTEST_CASE(test_desktop_render_draws_taskbar_and_launcher_label),
    KTEST_CASE(test_desktop_init_binds_global_keyboard_target),
    KTEST_CASE(test_desktop_escape_opens_launcher_and_consumes_input),
    KTEST_CASE(test_desktop_plain_text_forwards_to_shell_when_focused),
    KTEST_CASE(test_desktop_write_process_output_targets_shell_surface),
    KTEST_CASE(test_desktop_child_process_group_output_targets_shell_surface),
    KTEST_CASE(test_desktop_unrelated_process_group_output_is_rejected),
    KTEST_CASE(test_syscall_console_write_routes_session_output_to_desktop),
    KTEST_CASE(test_desktop_ansi_color_escape_updates_attr_without_printing),
    KTEST_CASE(test_desktop_full_screen_write_does_not_scroll_until_next_char),
    KTEST_CASE(test_syscall_clear_clears_desktop_shell_buffer),
    KTEST_CASE(test_tty_ctrl_c_echo_routes_to_desktop_shell_buffer),
    KTEST_CASE(test_mouse_packet_decode_preserves_motion_and_buttons),
    KTEST_CASE(test_mouse_stream_resyncs_after_noise_and_ack),
    KTEST_CASE(test_mouse_stream_keeps_response_like_bytes_inside_packet),
    KTEST_CASE(test_desktop_pointer_click_focuses_shell_window),
    KTEST_CASE(test_desktop_pointer_click_ignores_hidden_shell_window),
    KTEST_CASE(test_desktop_render_draws_visible_mouse_pointer),
    KTEST_CASE(test_desktop_pointer_event_moves_visible_mouse_pointer),
    KTEST_CASE(test_framebuffer_info_accepts_1024_768_32_rgb),
    KTEST_CASE(test_boot_framebuffer_grid_clamps_to_static_cell_buffer),
    KTEST_CASE(test_framebuffer_info_rejects_address_above_uintptr),
    KTEST_CASE(test_framebuffer_info_rejects_extent_above_uintptr),
    KTEST_CASE(test_framebuffer_info_rejects_pitch_overflow_width),
    KTEST_CASE(test_framebuffer_info_rejects_rgb_mask_past_32_bits),
    KTEST_CASE(test_framebuffer_info_rejects_overlapping_rgb_masks),
    KTEST_CASE(test_framebuffer_pack_rgb_uses_mask_positions),
    KTEST_CASE(test_framebuffer_fill_rect_clips_to_bounds),
    KTEST_CASE(test_framebuffer_fill_rect_handles_large_dimensions),
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
