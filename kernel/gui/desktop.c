#include "desktop.h"
#include "kheap.h"
#include "kstring.h"

static desktop_state_t *g_desktop = 0;

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

static void desktop_shell_clear_line(desktop_state_t *desktop, int row)
{
    int base;

    if (!desktop->shell_cells || row < 0 || row >= desktop->shell_cells_h)
        return;

    base = row * desktop->shell_cells_w;
    for (int col = 0; col < desktop->shell_cells_w; col++) {
        gui_cell_t *cell = &desktop->shell_cells[base + col];
        cell->ch = ' ';
        cell->attr = desktop->display->default_attr;
    }
}

static void desktop_shell_scroll_up(desktop_state_t *desktop)
{
    int bytes;

    if (!desktop->shell_cells || desktop->shell_cells_w <= 0 ||
        desktop->shell_cells_h <= 0)
        return;

    bytes = desktop->shell_cells_w * (int)sizeof(gui_cell_t);
    for (int row = 1; row < desktop->shell_cells_h; row++) {
        k_memmove(&desktop->shell_cells[(row - 1) * desktop->shell_cells_w],
                  &desktop->shell_cells[row * desktop->shell_cells_w],
                  (uint32_t)bytes);
    }
    desktop_shell_clear_line(desktop, desktop->shell_cells_h - 1);
    if (desktop->shell_cursor_y > 0)
        desktop->shell_cursor_y--;
    if (desktop->shell_cursor_y >= desktop->shell_cells_h)
        desktop->shell_cursor_y = desktop->shell_cells_h - 1;
}

static void desktop_shell_ensure_cursor(desktop_state_t *desktop)
{
    if (desktop->shell_cursor_x < 0)
        desktop->shell_cursor_x = 0;
    if (desktop->shell_cursor_y < 0)
        desktop->shell_cursor_y = 0;
    if (desktop->shell_cursor_x >= desktop->shell_cells_w)
        desktop->shell_cursor_x = desktop->shell_cells_w - 1;
    if (desktop->shell_cursor_y >= desktop->shell_cells_h)
        desktop->shell_cursor_y = desktop->shell_cells_h - 1;
}

static void desktop_shell_newline(desktop_state_t *desktop)
{
    desktop->shell_cursor_x = 0;
    desktop->shell_cursor_y++;
    if (desktop->shell_cursor_y >= desktop->shell_cells_h) {
        desktop_shell_scroll_up(desktop);
        desktop->shell_cursor_y = desktop->shell_cells_h - 1;
    }
}

static void desktop_shell_write_cell(desktop_state_t *desktop, char c)
{
    int x;
    int y;
    gui_cell_t *cell;

    if (!desktop->shell_cells || desktop->shell_cells_w <= 0 ||
        desktop->shell_cells_h <= 0)
        return;

    desktop_shell_ensure_cursor(desktop);
    if (desktop->shell_cursor_x >= desktop->shell_cells_w)
        desktop_shell_newline(desktop);

    x = desktop->shell_cursor_x;
    y = desktop->shell_cursor_y;
    cell = &desktop->shell_cells[y * desktop->shell_cells_w + x];
    cell->ch = c;
    cell->attr = desktop->display->default_attr;
    desktop->shell_cursor_x++;
    if (desktop->shell_cursor_x >= desktop->shell_cells_w)
        desktop_shell_newline(desktop);
}

static void desktop_shell_apply_char(desktop_state_t *desktop, char c)
{
    if (c == '\r') {
        desktop->shell_cursor_x = 0;
        return;
    }

    if (c == '\n') {
        desktop_shell_newline(desktop);
        return;
    }

    if (c == '\b') {
        if (desktop->shell_cursor_x > 0)
            desktop->shell_cursor_x--;
        return;
    }

    if (c == '\t') {
        int spaces = 4 - (desktop->shell_cursor_x % 4);

        if (spaces == 0)
            spaces = 4;
        while (spaces-- > 0)
            desktop_shell_apply_char(desktop, ' ');
        return;
    }

    desktop_shell_write_cell(desktop, c);
}

static void desktop_shell_redraw(desktop_state_t *desktop)
{
    if (!desktop->shell_cells || !desktop->display)
        return;

    for (int row = 0; row < desktop->shell_cells_h; row++) {
        for (int col = 0; col < desktop->shell_cells_w; col++) {
            int dx = desktop->shell_content.x + col;
            int dy = desktop->shell_content.y + row;

            if (dx < 0 || dy < 0 || dx >= desktop->display->cols ||
                dy >= desktop->display->rows)
                continue;
            desktop->display->cells[dy * desktop->display->cols + dx] =
                desktop->shell_cells[row * desktop->shell_cells_w + col];
        }
    }
}

desktop_state_t *desktop_global(void)
{
    return g_desktop;
}

int desktop_is_active(void)
{
    return g_desktop && g_desktop->active && g_desktop->desktop_enabled;
}

void desktop_init(desktop_state_t *desktop, gui_display_t *display)
{
    k_memset(desktop, 0, sizeof(*desktop));
    g_desktop = desktop;
    desktop->display = display;
    desktop->active = 1;
    desktop->desktop_enabled = 1;
    desktop->focus = DESKTOP_FOCUS_TASKBAR;
    desktop_layout(desktop);
    desktop->shell_cells_w = desktop->shell_content.w;
    desktop->shell_cells_h = desktop->shell_content.h;

    if (desktop->shell_cells_w <= 0 || desktop->shell_cells_h <= 0) {
        desktop->active = 0;
        desktop->desktop_enabled = 0;
        return;
    }

    desktop->shell_cells = (gui_cell_t *)kmalloc(
        (uint32_t)(desktop->shell_cells_w *
                   desktop->shell_cells_h *
                   (int)sizeof(gui_cell_t)));
    if (!desktop->shell_cells) {
        desktop->active = 0;
        desktop->desktop_enabled = 0;
        return;
    }

    desktop->shell_cursor_x = 0;
    desktop->shell_cursor_y = 0;
    for (int row = 0; row < desktop->shell_cells_h; row++)
        desktop_shell_clear_line(desktop, row);
}

void desktop_set_presentation_target(desktop_state_t *desktop,
                                     uintptr_t video_address)
{
    if (!desktop)
        return;

    desktop->video_address = video_address;
}

void desktop_attach_shell_pid(desktop_state_t *desktop, uint32_t pid)
{
    if (!desktop)
        return;

    desktop->shell_pid = pid;
}

int desktop_console_mirror_enabled(void)
{
    return !desktop_is_active();
}

int desktop_write_process_output(desktop_state_t *desktop,
                                 uint32_t pid,
                                 const char *buf,
                                 uint32_t len)
{
    uint32_t i;

    if (!desktop || !desktop->active || !desktop->desktop_enabled)
        return 0;
    if (!desktop->shell_window_open || desktop->shell_pid != pid)
        return 0;
    if (!buf || len == 0 || !desktop->shell_cells)
        return 0;

    for (i = 0; i < len; i++)
        desktop_shell_apply_char(desktop, buf[i]);

    desktop_render(desktop);
    return (int)len;
}

desktop_key_result_t desktop_handle_key(desktop_state_t *desktop, char c)
{
    if (!desktop || !desktop->active)
        return DESKTOP_KEY_FORWARD;

    if (c == 27) {
        desktop->launcher_open = !desktop->launcher_open;
        desktop->focus = desktop->launcher_open
            ? DESKTOP_FOCUS_LAUNCHER
            : DESKTOP_FOCUS_SHELL;
        desktop_render(desktop);
        return DESKTOP_KEY_CONSUMED;
    }

    if (desktop->launcher_open && c == '\n') {
        desktop->launcher_open = 0;
        desktop_open_shell_window(desktop);
        desktop_render(desktop);
        return DESKTOP_KEY_CONSUMED;
    }

    if (desktop->launcher_open && c == '\t') {
        desktop->focus = DESKTOP_FOCUS_TASKBAR;
        desktop_render(desktop);
        return DESKTOP_KEY_CONSUMED;
    }

    return (desktop->focus == DESKTOP_FOCUS_SHELL)
        ? DESKTOP_KEY_FORWARD
        : DESKTOP_KEY_CONSUMED;
}

void desktop_open_shell_window(desktop_state_t *desktop)
{
    desktop->shell_window_open = 1;
    desktop->focus = DESKTOP_FOCUS_SHELL;
}

void desktop_handle_pointer(desktop_state_t *desktop,
                            const desktop_pointer_event_t *ev)
{
    if (!desktop || !ev)
        return;

    if (desktop->shell_window_open &&
        ev->left_down &&
        ev->x >= desktop->shell_rect.x &&
        ev->x < desktop->shell_rect.x + desktop->shell_rect.w &&
        ev->y >= desktop->shell_rect.y &&
        ev->y < desktop->shell_rect.y + desktop->shell_rect.h) {
        desktop->focus = DESKTOP_FOCUS_SHELL;
    }

    if (ev->left_down &&
        ev->x >= 1 && ev->x < 6 &&
        ev->y == desktop->taskbar.y) {
        desktop->launcher_open = !desktop->launcher_open;
        desktop->focus = desktop->launcher_open
            ? DESKTOP_FOCUS_LAUNCHER
            : DESKTOP_FOCUS_SHELL;
    }

    desktop_render(desktop);
}

void desktop_render(desktop_state_t *desktop)
{
    if (!desktop || !desktop->display)
        return;

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

    if (desktop->shell_window_open)
        desktop_shell_redraw(desktop);

    if (desktop->video_address)
        gui_display_present_to_vga(desktop->display, desktop->video_address);
}
