#include "display.h"
#include "framebuffer.h"

static void gui_display_vga_color(uint8_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t palette[16][3] = {
        {0x00,0x00,0x00}, {0x00,0x00,0xaa}, {0x00,0xaa,0x00}, {0x00,0xaa,0xaa},
        {0xaa,0x00,0x00}, {0xaa,0x00,0xaa}, {0xaa,0x55,0x00}, {0xaa,0xaa,0xaa},
        {0x55,0x55,0x55}, {0x55,0x55,0xff}, {0x55,0xff,0x55}, {0x55,0xff,0xff},
        {0xff,0x55,0x55}, {0xff,0x55,0xff}, {0xff,0xff,0x55}, {0xff,0xff,0xff},
    };

    *r = palette[color & 0x0f][0];
    *g = palette[color & 0x0f][1];
    *b = palette[color & 0x0f][2];
}

static gui_rect_t gui_clip_rect(const gui_display_t *display,
                                int x, int y, int w, int h)
{
    gui_rect_t out = { 0, 0, 0, 0 };

    if (w <= 0 || h <= 0)
        return out;
    if (x >= display->cols || y >= display->rows)
        return out;
    if (x + w <= 0 || y + h <= 0)
        return out;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > display->cols)
        w = display->cols - x;
    if (y + h > display->rows)
        h = display->rows - y;
    if (w < 0)
        return out;
    if (h < 0)
        return out;

    out.x = x;
    out.y = y;
    out.w = w;
    out.h = h;
    return out;
}

void gui_display_init(gui_display_t *display, gui_cell_t *cells,
                      int cols, int rows, uint8_t default_attr)
{
    display->cells = cells;
    display->cols = cols;
    display->rows = rows;
    display->default_attr = default_attr;
    display->cursor_x = 0;
    display->cursor_y = 0;
    display->cursor_visible = 1;

    for (int i = 0; i < cols * rows; i++) {
        display->cells[i].ch = ' ';
        display->cells[i].attr = default_attr;
    }
}

gui_rect_t gui_display_fill_rect(gui_display_t *display,
                                 int x, int y, int w, int h,
                                 char ch, uint8_t attr)
{
    gui_rect_t dirty = gui_clip_rect(display, x, y, w, h);

    for (int row = dirty.y; row < dirty.y + dirty.h; row++) {
        for (int col = dirty.x; col < dirty.x + dirty.w; col++) {
            gui_cell_t *cell = &display->cells[row * display->cols + col];
            cell->ch = ch;
            cell->attr = attr;
        }
    }

    return dirty;
}

gui_rect_t gui_display_draw_text(gui_display_t *display,
                                 int x, int y, int max_w,
                                 const char *text, uint8_t attr)
{
    int written = 0;

    while (text[written] && written < max_w && x + written < display->cols) {
        if (x + written >= 0 && y >= 0 && y < display->rows) {
            gui_cell_t *cell = &display->cells[y * display->cols + x + written];
            cell->ch = text[written];
            cell->attr = attr;
        }
        written++;
    }

    return gui_clip_rect(display, x, y, written, 1);
}

gui_rect_t gui_display_draw_frame(gui_display_t *display,
                                  int x, int y, int w, int h,
                                  uint8_t attr)
{
    gui_display_fill_rect(display, x, y, w, 1, '-', attr);
    gui_display_fill_rect(display, x, y + h - 1, w, 1, '-', attr);
    gui_display_fill_rect(display, x, y, 1, h, '|', attr);
    gui_display_fill_rect(display, x + w - 1, y, 1, h, '|', attr);
    return gui_clip_rect(display, x, y, w, h);
}

gui_cell_t gui_display_cell_at(const gui_display_t *display, int x, int y)
{
    gui_cell_t blank = { ' ', display->default_attr };

    if (x < 0 || y < 0 || x >= display->cols || y >= display->rows)
        return blank;
    return display->cells[y * display->cols + x];
}

void gui_display_set_cursor(gui_display_t *display, int x, int y, int visible)
{
    display->cursor_x = x;
    display->cursor_y = y;
    display->cursor_visible = visible;
}

void gui_display_present_to_vga(const gui_display_t *display, uintptr_t video_address)
{
    volatile unsigned char *vidmem = (volatile unsigned char *)video_address;

    for (int row = 0; row < display->rows; row++) {
        for (int col = 0; col < display->cols; col++) {
            const gui_cell_t *cell = &display->cells[row * display->cols + col];
            int off = 2 * (row * display->cols + col);
            vidmem[off] = (unsigned char)cell->ch;
            vidmem[off + 1] = cell->attr;
        }
    }
}

void gui_display_present_to_framebuffer(const gui_display_t *display,
                                        const framebuffer_info_t *fb)
{
    uint8_t r;
    uint8_t g;
    uint8_t b;

    if (!display || !fb)
        return;
    if (!display->cells || display->cols <= 0 || display->rows <= 0)
        return;

    for (int row = 0; row < display->rows; row++) {
        for (int col = 0; col < display->cols; col++) {
            const gui_cell_t *cell = &display->cells[row * display->cols + col];
            uint32_t fg;
            uint32_t bg;

            gui_display_vga_color(cell->attr & 0x0f, &r, &g, &b);
            fg = framebuffer_pack_rgb(fb, r, g, b);
            gui_display_vga_color((cell->attr >> 4) & 0x0f, &r, &g, &b);
            bg = framebuffer_pack_rgb(fb, r, g, b);
            framebuffer_draw_glyph(fb, col * 8, row * 16,
                                   (unsigned char)cell->ch, fg, bg);
        }
    }
}
