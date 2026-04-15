#include "desktop.h"
#include "kheap.h"
#include "kstring.h"

#define DESKTOP_ATTR_BACKGROUND 0x1f
#define DESKTOP_ATTR_TASKBAR    0x70
#define DESKTOP_ATTR_WINDOW     0x1e
#define DESKTOP_ATTR_TITLE      0x70
#define DESKTOP_ATTR_LAUNCHER   0x70
#define DESKTOP_CURSOR_W        8
#define DESKTOP_CURSOR_H        12

static desktop_state_t *g_desktop = 0;

static void desktop_layout(desktop_state_t *desktop)
{
    int margin_x = desktop->display->cols >= 100 ? 10 : 6;
    int margin_top = desktop->display->rows >= 40 ? 4 : 3;
    int taskbar_gap = desktop->display->rows >= 40 ? 8 : 7;
    int shell_w;
    int shell_h;

    desktop->taskbar.x = 0;
    desktop->taskbar.y = desktop->display->rows - 1;
    desktop->taskbar.w = desktop->display->cols;
    desktop->taskbar.h = 1;

    desktop->launcher_rect.x = 1;
    desktop->launcher_rect.y = desktop->display->rows - 6;
    desktop->launcher_rect.w = 18;
    desktop->launcher_rect.h = 5;

    shell_w = desktop->display->cols - margin_x * 2;
    shell_h = desktop->display->rows - margin_top - taskbar_gap;
    if (shell_w < 48 && desktop->display->cols >= 60)
        shell_w = 48;
    if (shell_h < 15 && desktop->display->rows >= 25)
        shell_h = 15;

    desktop->shell_rect.x = margin_x;
    desktop->shell_rect.y = margin_top;
    desktop->shell_rect.w = shell_w;
    desktop->shell_rect.h = shell_h;

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
    desktop->shell_wrap_pending = 0;
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

    if (desktop->shell_wrap_pending)
        desktop_shell_newline(desktop);

    desktop_shell_ensure_cursor(desktop);

    x = desktop->shell_cursor_x;
    y = desktop->shell_cursor_y;
    cell = &desktop->shell_cells[y * desktop->shell_cells_w + x];
    cell->ch = c;
    cell->attr = desktop->shell_attr;
    if (desktop->shell_cursor_x == desktop->shell_cells_w - 1) {
        desktop->shell_wrap_pending = 1;
    } else {
        desktop->shell_cursor_x++;
        desktop->shell_wrap_pending = 0;
    }
}

static void desktop_shell_apply_ansi(desktop_state_t *desktop, int code)
{
    if (code == 0)
        desktop->shell_attr = desktop->display->default_attr;
    if (code == 31)
        desktop->shell_attr = 0x0c;
    if (code == 32)
        desktop->shell_attr = 0x0a;
    if (code == 33)
        desktop->shell_attr = 0x0e;
    if (code == 36)
        desktop->shell_attr = 0x0b;
}

static void desktop_shell_apply_char(desktop_state_t *desktop, char c)
{
    if (c == '\x1b') {
        desktop->shell_ansi_state = 1;
        desktop->shell_ansi_val = 0;
        return;
    }

    if (desktop->shell_ansi_state == 1) {
        desktop->shell_ansi_state = (c == '[') ? 2 : 0;
        return;
    }

    if (desktop->shell_ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            desktop->shell_ansi_val =
                (desktop->shell_ansi_val * 10) + (c - '0');
            return;
        }
        if (c == 'm') {
            desktop_shell_apply_ansi(desktop, desktop->shell_ansi_val);
            desktop->shell_ansi_state = 0;
            return;
        }
        desktop->shell_ansi_state = 0;
        return;
    }

    if (c == '\r') {
        desktop->shell_cursor_x = 0;
        desktop->shell_wrap_pending = 0;
        return;
    }

    if (c == '\n') {
        desktop_shell_newline(desktop);
        return;
    }

    if (c == '\b') {
        if (desktop->shell_wrap_pending) {
            desktop->shell_wrap_pending = 0;
            return;
        }
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

static void desktop_set_pointer(desktop_state_t *desktop, int x, int y)
{
    if (!desktop || !desktop->display)
        return;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= desktop->display->cols)
        x = desktop->display->cols - 1;
    if (y >= desktop->display->rows)
        y = desktop->display->rows - 1;

    desktop->pointer_x = x;
    desktop->pointer_y = y;
    desktop->pointer_pixel_x = x * (int)GUI_FONT_W;
    desktop->pointer_pixel_y = y * (int)GUI_FONT_H;
    desktop->pointer_visible = 1;
}

static void desktop_draw_pointer(desktop_state_t *desktop)
{
    if (!desktop || !desktop->display || !desktop->pointer_visible)
        return;

    gui_display_draw_text(desktop->display,
                          desktop->pointer_x,
                          desktop->pointer_y,
                          1,
                          "^",
                          0x0f);
}

static void desktop_present_cursor_region(desktop_state_t *desktop,
                                          int pixel_x,
                                          int pixel_y)
{
    int cell_x;
    int cell_y;
    int cell_right;
    int cell_bottom;

    if (!desktop || !desktop->display || !desktop->framebuffer)
        return;

    cell_x = pixel_x / (int)GUI_FONT_W;
    cell_y = pixel_y / (int)GUI_FONT_H;
    cell_right = (pixel_x + DESKTOP_CURSOR_W - 1) / (int)GUI_FONT_W;
    cell_bottom = (pixel_y + DESKTOP_CURSOR_H - 1) / (int)GUI_FONT_H;

    gui_display_present_rect_to_framebuffer(desktop->display,
                                            desktop->framebuffer,
                                            cell_x,
                                            cell_y,
                                            cell_right - cell_x + 1,
                                            cell_bottom - cell_y + 1);
}

static void desktop_draw_framebuffer_pointer(desktop_state_t *desktop)
{
    uint32_t fg;
    uint32_t shadow;

    if (!desktop || !desktop->framebuffer || !desktop->pointer_visible)
        return;

    fg = framebuffer_pack_rgb(desktop->framebuffer, 255, 255, 255);
    shadow = framebuffer_pack_rgb(desktop->framebuffer, 0, 0, 0);
    framebuffer_draw_cursor(desktop->framebuffer,
                            desktop->pointer_pixel_x,
                            desktop->pointer_pixel_y,
                            fg,
                            shadow);
}

static void desktop_present_framebuffer_pointer_motion(desktop_state_t *desktop,
                                                       int old_pixel_x,
                                                       int old_pixel_y)
{
    if (!desktop || !desktop->framebuffer_enabled || !desktop->framebuffer)
        return;

    desktop_present_cursor_region(desktop, old_pixel_x, old_pixel_y);
    if (old_pixel_x != desktop->pointer_pixel_x ||
        old_pixel_y != desktop->pointer_pixel_y)
        desktop_present_cursor_region(desktop,
                                      desktop->pointer_pixel_x,
                                      desktop->pointer_pixel_y);
    desktop_draw_framebuffer_pointer(desktop);
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
    desktop_set_pointer(desktop, display->cols / 2, display->rows / 2);
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
    desktop->shell_wrap_pending = 0;
    desktop->shell_ansi_state = 0;
    desktop->shell_ansi_val = 0;
    desktop->shell_attr = display->default_attr;
    for (int row = 0; row < desktop->shell_cells_h; row++)
        desktop_shell_clear_line(desktop, row);
}

void desktop_set_presentation_target(desktop_state_t *desktop,
                                     uintptr_t video_address)
{
    if (!desktop)
        return;

    desktop->video_address = video_address;
    desktop->framebuffer_enabled = 0;
    desktop->framebuffer = 0;
}

void desktop_set_framebuffer_target(desktop_state_t *desktop,
                                    const framebuffer_info_t *framebuffer)
{
    if (!desktop)
        return;

    desktop->framebuffer = framebuffer;
    desktop->framebuffer_enabled = framebuffer != 0;
}

void desktop_attach_shell_pid(desktop_state_t *desktop, uint32_t pid)
{
    desktop_attach_shell_process(desktop, pid, pid);
}

void desktop_attach_shell_process(desktop_state_t *desktop, uint32_t pid,
                                  uint32_t pgid)
{
    if (!desktop)
        return;

    desktop->shell_pid = pid;
    desktop->shell_pgid = pgid ? pgid : pid;
}

uint32_t desktop_shell_pid(const desktop_state_t *desktop)
{
    return desktop ? desktop->shell_pid : 0;
}

int desktop_process_owns_shell(desktop_state_t *desktop,
                               uint32_t pid,
                               uint32_t pgid)
{
    if (!desktop || desktop->shell_pid == 0)
        return 0;
    if (pid == desktop->shell_pid)
        return 1;
    if (desktop->shell_pgid != 0 && pgid == desktop->shell_pgid)
        return 1;
    return 0;
}

int desktop_console_mirror_enabled(void)
{
    return !desktop_is_active();
}

int desktop_write_console_output(desktop_state_t *desktop,
                                 const char *buf,
                                 uint32_t len)
{
    uint32_t i;

    if (!desktop || !desktop->active || !desktop->desktop_enabled)
        return 0;
    if (!desktop->shell_window_open)
        return 0;
    if (!buf || len == 0 || !desktop->shell_cells)
        return 0;

    for (i = 0; i < len; i++)
        desktop_shell_apply_char(desktop, buf[i]);

    desktop_render(desktop);
    return (int)len;
}

int desktop_clear_console(desktop_state_t *desktop)
{
    if (!desktop || !desktop->active || !desktop->desktop_enabled)
        return 0;
    if (!desktop->shell_window_open || !desktop->shell_cells)
        return 0;

    for (int row = 0; row < desktop->shell_cells_h; row++)
        desktop_shell_clear_line(desktop, row);

    desktop->shell_cursor_x = 0;
    desktop->shell_cursor_y = 0;
    desktop->shell_wrap_pending = 0;
    desktop->shell_ansi_state = 0;
    desktop->shell_ansi_val = 0;
    desktop->shell_attr = desktop->display->default_attr;
    desktop_render(desktop);
    return 1;
}

int desktop_write_process_output(desktop_state_t *desktop,
                                 uint32_t pid,
                                 uint32_t pgid,
                                 const char *buf,
                                 uint32_t len)
{
    if (!desktop_process_owns_shell(desktop, pid, pgid))
        return 0;
    return desktop_write_console_output(desktop, buf, len);
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
    int old_pixel_x;
    int old_pixel_y;
    int launcher_open;

    if (!desktop || !ev)
        return;

    old_pixel_x = desktop->pointer_pixel_x;
    old_pixel_y = desktop->pointer_pixel_y;
    launcher_open = desktop->launcher_open;

    desktop_set_pointer(desktop, ev->x, ev->y);
    if (desktop->framebuffer_enabled && desktop->framebuffer) {
        desktop->pointer_pixel_x = ev->pixel_x;
        desktop->pointer_pixel_y = ev->pixel_y;
    }

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

    if (desktop->framebuffer_enabled && desktop->framebuffer &&
        launcher_open == desktop->launcher_open) {
        desktop_present_framebuffer_pointer_motion(desktop,
                                                   old_pixel_x,
                                                   old_pixel_y);
    } else {
        desktop_render(desktop);
    }
}

void desktop_render(desktop_state_t *desktop)
{
    if (!desktop || !desktop->display)
        return;

    gui_display_fill_rect(desktop->display, 0, 0,
                          desktop->display->cols, desktop->display->rows,
                          ' ', DESKTOP_ATTR_BACKGROUND);
    gui_display_fill_rect(desktop->display,
                          desktop->taskbar.x, desktop->taskbar.y,
                          desktop->taskbar.w, desktop->taskbar.h,
                          ' ', DESKTOP_ATTR_TASKBAR);
    gui_display_draw_text(desktop->display, 2, desktop->taskbar.y, 10,
                          "Menu", DESKTOP_ATTR_TITLE);

    if (desktop->shell_window_open) {
        gui_display_draw_frame(desktop->display,
                               desktop->shell_rect.x, desktop->shell_rect.y,
                               desktop->shell_rect.w, desktop->shell_rect.h,
                               DESKTOP_ATTR_WINDOW);
        gui_display_draw_text(desktop->display,
                              desktop->shell_rect.x + 2,
                              desktop->shell_rect.y,
                              desktop->shell_rect.w - 4,
                              "Shell", DESKTOP_ATTR_WINDOW);
    }

    if (desktop->launcher_open) {
        gui_display_draw_frame(desktop->display,
                               desktop->launcher_rect.x,
                               desktop->launcher_rect.y,
                               desktop->launcher_rect.w,
                               desktop->launcher_rect.h,
                               DESKTOP_ATTR_LAUNCHER);
        gui_display_draw_text(desktop->display,
                              desktop->launcher_rect.x + 2,
                              desktop->launcher_rect.y + 1,
                              desktop->launcher_rect.w - 4,
                              "Shell", DESKTOP_ATTR_LAUNCHER);
    }

    if (desktop->shell_window_open)
        desktop_shell_redraw(desktop);

    if (desktop->framebuffer_enabled && desktop->framebuffer) {
        gui_display_present_to_framebuffer(desktop->display,
                                           desktop->framebuffer);
        desktop_draw_framebuffer_pointer(desktop);
    } else if (desktop->video_address) {
        desktop_draw_pointer(desktop);
        gui_display_present_to_vga(desktop->display, desktop->video_address);
    } else {
        desktop_draw_pointer(desktop);
    }
}
