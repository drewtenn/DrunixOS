#include "desktop.h"
#include "kstring.h"

static void desktop_layout(desktop_state_t *desktop)
{
    desktop->taskbar.x = 0;
    desktop->taskbar.y = desktop->display->rows - 1;
    desktop->taskbar.w = desktop->display->cols;
    desktop->taskbar.h = 1;

    desktop->launcher_rect.x = 1;
    desktop->launcher_rect.y = desktop->display->rows - 6;
    desktop->launcher_rect.w = 18;
    desktop->launcher_rect.h = 5;

    desktop->shell_rect.x = 6;
    desktop->shell_rect.y = 3;
    desktop->shell_rect.w = desktop->display->cols - 12;
    desktop->shell_rect.h = desktop->display->rows - 10;

    desktop->shell_content.x = desktop->shell_rect.x + 1;
    desktop->shell_content.y = desktop->shell_rect.y + 1;
    desktop->shell_content.w = desktop->shell_rect.w - 2;
    desktop->shell_content.h = desktop->shell_rect.h - 2;
}

void desktop_init(desktop_state_t *desktop, gui_display_t *display)
{
    k_memset(desktop, 0, sizeof(*desktop));
    desktop->display = display;
    desktop->video_address = 0xB8000u;
    desktop->active = 1;
    desktop->desktop_enabled = 1;
    desktop->focus = DESKTOP_FOCUS_TASKBAR;
    desktop_layout(desktop);
}

void desktop_open_shell_window(desktop_state_t *desktop)
{
    desktop->shell_window_open = 1;
    desktop->focus = DESKTOP_FOCUS_SHELL;
}

void desktop_render(desktop_state_t *desktop)
{
    gui_display_fill_rect(desktop->display, 0, 0,
                          desktop->display->cols, desktop->display->rows,
                          ' ', 0x1f);
    gui_display_fill_rect(desktop->display,
                          desktop->taskbar.x, desktop->taskbar.y,
                          desktop->taskbar.w, desktop->taskbar.h,
                          ' ', 0x70);
    gui_display_draw_text(desktop->display, 2, desktop->taskbar.y, 10,
                          "Menu", 0x70);

    if (desktop->shell_window_open) {
        gui_display_draw_frame(desktop->display,
                               desktop->shell_rect.x, desktop->shell_rect.y,
                               desktop->shell_rect.w, desktop->shell_rect.h,
                               0x1e);
        gui_display_draw_text(desktop->display,
                              desktop->shell_rect.x + 2,
                              desktop->shell_rect.y,
                              desktop->shell_rect.w - 4,
                              "Shell", 0x1e);
    }

    if (desktop->launcher_open) {
        gui_display_draw_frame(desktop->display,
                               desktop->launcher_rect.x,
                               desktop->launcher_rect.y,
                               desktop->launcher_rect.w,
                               desktop->launcher_rect.h,
                               0x70);
        gui_display_draw_text(desktop->display,
                              desktop->launcher_rect.x + 2,
                              desktop->launcher_rect.y + 1,
                              desktop->launcher_rect.w - 4,
                              "Shell", 0x70);
    }

    gui_display_present_to_vga(desktop->display, desktop->video_address);
}
