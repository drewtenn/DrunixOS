#include "ktest.h"
#include "display.h"
#include "desktop.h"

static gui_cell_t desktop_cells[80 * 25];

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

static ktest_case_t desktop_cases[] = {
    KTEST_CASE(test_gui_display_fill_rect_clips_to_bounds),
    KTEST_CASE(test_gui_display_draw_text_stops_at_region_edge),
    KTEST_CASE(test_desktop_boot_layout_opens_shell_window),
    KTEST_CASE(test_desktop_render_draws_taskbar_and_launcher_label),
    KTEST_CASE(test_desktop_init_binds_global_keyboard_target),
    KTEST_CASE(test_desktop_escape_opens_launcher_and_consumes_input),
    KTEST_CASE(test_desktop_plain_text_forwards_to_shell_when_focused),
};

ktest_suite_t *ktest_suite_desktop(void)
{
    static ktest_suite_t suite = KTEST_SUITE("desktop", desktop_cases);
    return &suite;
}
