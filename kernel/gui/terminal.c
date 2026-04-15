#include "terminal.h"
#include "framebuffer.h"
#include "font8x16.h"
#include "kheap.h"
#include "kstring.h"
#include <limits.h>

static void terminal_clear_line(gui_terminal_t *term, int row);
static void terminal_push_history(gui_terminal_t *term, const gui_cell_t *row);
static void terminal_scroll_up(gui_terminal_t *term);
static void terminal_apply_ansi(gui_terminal_t *term, int code);
static void terminal_apply_char(gui_terminal_t *term, char c);
static int terminal_intersect_pixel_rect(gui_pixel_rect_t a,
                                         gui_pixel_rect_t b,
                                         gui_pixel_rect_t *out);
static void terminal_render_cell(const framebuffer_info_t *fb,
                                 const gui_pixel_rect_t *clip,
                                 int64_t cell_x,
                                 int64_t cell_y,
                                 const gui_cell_t *cell,
                                 uint32_t fg);

static int terminal_validate_dimensions(int cols,
                                        int rows,
                                        int history_rows,
                                        uint32_t *live_bytes,
                                        uint32_t *history_bytes,
                                        uint32_t *row_bytes)
{
    uint64_t live_cells;
    uint64_t history_cells;
    uint64_t bytes;

    if (cols <= 0 || rows <= 0 || history_rows < 0)
        return 0;
    if (history_rows > INT_MAX - rows)
        return 0;

    bytes = (uint64_t)(uint32_t)cols * (uint64_t)sizeof(gui_cell_t);
    if (bytes > UINT32_MAX)
        return 0;
    if (row_bytes)
        *row_bytes = (uint32_t)bytes;

    live_cells = (uint64_t)(uint32_t)cols * (uint64_t)(uint32_t)rows;
    bytes = live_cells * (uint64_t)sizeof(gui_cell_t);
    if (live_cells == 0 || bytes / (uint64_t)sizeof(gui_cell_t) != live_cells ||
        bytes > UINT32_MAX)
        return 0;
    if (live_bytes)
        *live_bytes = (uint32_t)bytes;

    if (history_rows > 0) {
        history_cells = (uint64_t)(uint32_t)cols *
                        (uint64_t)(uint32_t)history_rows;
        bytes = history_cells * (uint64_t)sizeof(gui_cell_t);
        if (history_cells == 0 ||
            bytes / (uint64_t)sizeof(gui_cell_t) != history_cells ||
            bytes > UINT32_MAX)
            return 0;
        if (history_bytes)
            *history_bytes = (uint32_t)bytes;
    } else if (history_bytes) {
        *history_bytes = 0;
    }

    return 1;
}

static int terminal_row_bytes_u32(const gui_terminal_t *term, uint32_t *bytes)
{
    uint64_t row_bytes;

    if (!term || !bytes || term->cols <= 0)
        return 0;

    row_bytes = (uint64_t)(uint32_t)term->cols * (uint64_t)sizeof(gui_cell_t);
    if (row_bytes > UINT32_MAX)
        return 0;
    *bytes = (uint32_t)row_bytes;
    return 1;
}

static int terminal_cell_count_u32(const gui_terminal_t *term, uint32_t *cells)
{
    uint64_t total_cells;

    if (!term || !cells || term->cols <= 0 || term->rows <= 0)
        return 0;

    total_cells = (uint64_t)(uint32_t)term->cols *
                  (uint64_t)(uint32_t)term->rows;
    if (total_cells > UINT32_MAX)
        return 0;

    *cells = (uint32_t)total_cells;
    return 1;
}

static const gui_cell_t *terminal_history_row_const(const gui_terminal_t *term,
                                                    int index)
{
    uint32_t row_index;
    uint32_t capacity;
    uint32_t cols;

    if (!term || !term->history || term->history_rows <= 0 || index < 0 ||
        index >= term->history_count)
        return 0;

    capacity = (uint32_t)term->history_rows;
    row_index = ((uint32_t)term->history_head + (uint32_t)index) % capacity;
    cols = (uint32_t)term->cols;
    return &term->history[row_index * cols];
}

static void terminal_clear_history(gui_terminal_t *term)
{
    int capacity;

    if (!term)
        return;

    capacity = term->history_rows;
    if (capacity > 0) {
        gui_cell_t blank = { ' ', 0 };
        uint32_t cols = (uint32_t)term->cols;

        blank.attr = term->default_attr;
        for (int row = 0; row < capacity; row++) {
            uint32_t base = (uint32_t)row * cols;

            for (int col = 0; col < term->cols; col++)
                term->history[base + (uint32_t)col] = blank;
        }
    }
    term->history_head = 0;
    term->history_count = 0;
}

static void terminal_write_cell(gui_terminal_t *term, char c)
{
    uint32_t cell_count;
    uint32_t cursor_index;
    gui_cell_t *cell;

    if (!term || !term->live || term->cols <= 0 || term->rows <= 0)
        return;

    if (term->wrap_pending) {
        term->cursor_x = 0;
        term->cursor_y++;
        term->wrap_pending = 0;
        if (term->cursor_y >= term->rows) {
            terminal_scroll_up(term);
            term->cursor_y = term->rows - 1;
        }
    }

    if (term->cursor_x < 0)
        term->cursor_x = 0;
    if (term->cursor_y < 0)
        term->cursor_y = 0;
    if (term->cursor_x >= term->cols)
        term->cursor_x = term->cols - 1;
    if (term->cursor_y >= term->rows)
        term->cursor_y = term->rows - 1;

    if (!terminal_cell_count_u32(term, &cell_count))
        return;
    cursor_index = (uint32_t)term->cursor_y * (uint32_t)term->cols +
                   (uint32_t)term->cursor_x;
    if (cursor_index >= cell_count)
        return;
    cell = &term->live[cursor_index];
    cell->ch = c;
    cell->attr = term->attr;
    if (term->cursor_x == term->cols - 1) {
        term->wrap_pending = 1;
    } else {
        term->cursor_x++;
        term->wrap_pending = 0;
    }
}

static void terminal_clear_line(gui_terminal_t *term, int row)
{
    gui_cell_t blank = { ' ', 0 };
    uint32_t base;
    uint32_t cols;

    if (!term || !term->live || row < 0 || row >= term->rows)
        return;

    blank.attr = term->default_attr;
    cols = (uint32_t)term->cols;
    base = (uint32_t)row * cols;
    for (int col = 0; col < term->cols; col++)
        term->live[base + (uint32_t)col] = blank;
}

static void terminal_push_history(gui_terminal_t *term, const gui_cell_t *row)
{
    uint32_t capacity;
    uint32_t tail;
    uint32_t bytes;

    if (!term || !row)
        return;

    capacity = (uint32_t)term->history_rows;
    if (capacity <= 0)
        return;
    if (!terminal_row_bytes_u32(term, &bytes))
        return;

    if (term->history_count < capacity) {
        tail = ((uint32_t)term->history_head +
                (uint32_t)term->history_count) % capacity;
        term->history_count++;
    } else {
        tail = (uint32_t)term->history_head;
        term->history_head = (term->history_head + 1) % term->history_rows;
    }

    k_memcpy(&term->history[tail * (uint32_t)term->cols], row, bytes);
}

static void terminal_scroll_up(gui_terminal_t *term)
{
    uint32_t bytes;
    uint32_t row;

    if (!term || !term->live || term->cols <= 0 || term->rows <= 0)
        return;
    terminal_push_history(term, term->live);
    if (!terminal_row_bytes_u32(term, &bytes))
        return;
    for (row = 1; row < (uint32_t)term->rows; row++) {
        k_memmove(&term->live[(row - 1u) * (uint32_t)term->cols],
                  &term->live[row * (uint32_t)term->cols],
                  bytes);
    }
    terminal_clear_line(term, term->rows - 1);
    if (term->cursor_y > 0)
        term->cursor_y--;
}

static void terminal_apply_ansi(gui_terminal_t *term, int code)
{
    if (!term)
        return;

    if (code == 0)
        term->attr = term->default_attr;
    if (code == 31)
        term->attr = 0x0c;
    if (code == 32)
        term->attr = 0x0a;
    if (code == 33)
        term->attr = 0x0e;
    if (code == 36)
        term->attr = 0x0b;
}

static void terminal_apply_char(gui_terminal_t *term, char c)
{
    if (!term)
        return;

    if (c == '\x1b') {
        term->ansi_state = 1;
        term->ansi_val = 0;
        return;
    }

    if (term->ansi_state == 1) {
        term->ansi_state = (c == '[') ? 2 : 0;
        return;
    }

    if (term->ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            if (term->ansi_val >= 100) {
                term->ansi_val = 999;
            } else {
                term->ansi_val = (term->ansi_val * 10) + (c - '0');
                if (term->ansi_val > 999)
                    term->ansi_val = 999;
            }
            return;
        }
        if (c == 'm') {
            terminal_apply_ansi(term, term->ansi_val);
            term->ansi_state = 0;
            return;
        }
        term->ansi_state = 0;
        return;
    }

    if (c == '\r') {
        term->cursor_x = 0;
        term->wrap_pending = 0;
        return;
    }

    if (c == '\n') {
        term->cursor_x = 0;
        term->wrap_pending = 0;
        term->cursor_y++;
        if (term->cursor_y >= term->rows) {
            terminal_scroll_up(term);
            term->cursor_y = term->rows - 1;
        }
        return;
    }

    if (c == '\b') {
        if (term->wrap_pending) {
            term->wrap_pending = 0;
            return;
        }
        if (term->cursor_x > 0)
            term->cursor_x--;
        return;
    }

    if (c == '\t') {
        int spaces = 4 - (term->cursor_x % 4);

        if (spaces == 0)
            spaces = 4;
        while (spaces-- > 0)
            terminal_apply_char(term, ' ');
        return;
    }

    terminal_write_cell(term, c);
}

int gui_terminal_init_static(gui_terminal_t *term,
                             gui_cell_t *live,
                             gui_cell_t *history,
                             int cols,
                             int rows,
                             int history_rows,
                             uint8_t default_attr)
{
    if (!term || !live)
        return 0;

    if (!terminal_validate_dimensions(cols, rows, history_rows, 0, 0, 0))
        return 0;
    if (history_rows > 0 && !history)
        return 0;

    k_memset(term, 0, sizeof(*term));
    term->live = live;
    term->history = history;
    term->cols = cols;
    term->rows = rows;
    term->history_rows = history_rows;
    term->owns_buffers = 0;
    term->attr = default_attr;
    term->default_attr = default_attr;
    term->view_top = 0;
    term->live_view = 1;

    for (int row = 0; row < rows; row++)
        terminal_clear_line(term, row);
    terminal_clear_history(term);
    return 1;
}

int gui_terminal_init_alloc(gui_terminal_t *term,
                            int cols,
                            int rows,
                            int history_rows,
                            uint8_t default_attr)
{
    gui_cell_t *live;
    gui_cell_t *history;
    uint32_t live_bytes;
    uint32_t history_bytes;

    if (!term)
        return 0;

    if (!terminal_validate_dimensions(cols, rows, history_rows,
                                      &live_bytes, &history_bytes, 0))
        return 0;

    live = (gui_cell_t *)kmalloc(live_bytes);
    if (!live)
        return 0;

    history = 0;
    if (history_rows > 0) {
        history = (gui_cell_t *)kmalloc(history_bytes);
        if (!history) {
            kfree(live);
            return 0;
        }
    }

    if (!gui_terminal_init_static(term, live, history,
                                  cols, rows, history_rows,
                                  default_attr)) {
        if (history)
            kfree(history);
        kfree(live);
        return 0;
    }

    term->owns_buffers = 1;
    return 1;
}

void gui_terminal_destroy(gui_terminal_t *term)
{
    if (!term)
        return;

    if (term->owns_buffers) {
        if (term->history)
            kfree(term->history);
        if (term->live)
            kfree(term->live);
    }

    k_memset(term, 0, sizeof(*term));
}

void gui_terminal_clear(gui_terminal_t *term)
{
    if (!term)
        return;

    for (int row = 0; row < term->rows; row++)
        terminal_clear_line(term, row);
    terminal_clear_history(term);
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->wrap_pending = 0;
    term->ansi_state = 0;
    term->ansi_val = 0;
    term->attr = term->default_attr;
    term->view_top = 0;
    term->live_view = 1;
}

int gui_terminal_write(gui_terminal_t *term, const char *buf, uint32_t len)
{
    uint32_t i;

    if (!term || !buf)
        return 0;
    if (len > (uint32_t)INT_MAX)
        return 0;

    for (i = 0; i < len; i++)
        terminal_apply_char(term, buf[i]);

    term->view_top = 0;
    term->live_view = 1;
    return (int)len;
}

void gui_terminal_scroll_view(gui_terminal_t *term, int rows)
{
    int64_t view_top;

    if (!term)
        return;

    view_top = (int64_t)term->view_top + (int64_t)rows;
    if (view_top < 0)
        view_top = 0;
    if (view_top > (int64_t)term->history_count)
        view_top = term->history_count;
    if (view_top > (int64_t)INT_MAX)
        view_top = INT_MAX;
    term->view_top = (int)view_top;
    term->live_view = (term->view_top == 0);
}

void gui_terminal_snap_live(gui_terminal_t *term)
{
    if (!term)
        return;

    term->view_top = 0;
    term->live_view = 1;
}

gui_cell_t gui_terminal_cell_at(const gui_terminal_t *term, int x, int y)
{
    int top;
    int index;
    gui_cell_t blank = { ' ', 0 };
    const gui_cell_t *row;

    if (!term || x < 0 || y < 0 || x >= term->cols || y >= term->rows)
        return blank;

    blank.attr = term->default_attr;
    top = term->history_count - term->view_top;
    if (top < 0)
        top = 0;
    index = top + y;
    if (index < term->history_count) {
        row = terminal_history_row_const(term, index);
        if (row)
            return row[x];
        return blank;
    }

    index -= term->history_count;
    if (index < term->rows)
        return term->live[index * term->cols + x];
    return blank;
}

int gui_terminal_history_count(const gui_terminal_t *term)
{
    return term ? term->history_count : 0;
}

int gui_terminal_cursor_x(const gui_terminal_t *term)
{
    return term ? term->cursor_x : 0;
}

int gui_terminal_cursor_y(const gui_terminal_t *term)
{
    return term ? term->cursor_y : 0;
}

int gui_terminal_visible_view_top(const gui_terminal_t *term)
{
    return term ? term->view_top : 0;
}

int gui_terminal_total_rows(const gui_terminal_t *term)
{
    if (!term)
        return 0;
    if (term->history_count > INT_MAX - term->rows)
        return INT_MAX;
    return term->history_count + term->rows;
}

void gui_terminal_set_pixel_rect(gui_terminal_t *term,
                                 gui_pixel_rect_t rect,
                                 int padding_x,
                                 int padding_y)
{
    if (!term)
        return;
    term->pixel_rect = rect;
    term->padding_x = padding_x < 0 ? 0 : padding_x;
    term->padding_y = padding_y < 0 ? 0 : padding_y;
}

static int terminal_intersect_pixel_rect(gui_pixel_rect_t a,
                                         gui_pixel_rect_t b,
                                         gui_pixel_rect_t *out)
{
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    int64_t a_right;
    int64_t a_bottom;
    int64_t b_right;
    int64_t b_bottom;

    if (!out)
        return 0;

    a_right = (int64_t)a.x + (int64_t)a.w;
    a_bottom = (int64_t)a.y + (int64_t)a.h;
    b_right = (int64_t)b.x + (int64_t)b.w;
    b_bottom = (int64_t)b.y + (int64_t)b.h;
    left = a.x > b.x ? a.x : b.x;
    top = a.y > b.y ? a.y : b.y;
    right = a_right < b_right ? a_right : b_right;
    bottom = a_bottom < b_bottom ? a_bottom : b_bottom;
    if (right <= left || bottom <= top)
        return 0;
    if (left < INT_MIN || top < INT_MIN || right > INT_MAX ||
        bottom > INT_MAX)
        return 0;

    out->x = (int)left;
    out->y = (int)top;
    out->w = (int)(right - left);
    out->h = (int)(bottom - top);
    return 1;
}

static void terminal_render_cell(const framebuffer_info_t *fb,
                                 const gui_pixel_rect_t *clip,
                                 int64_t cell_x,
                                 int64_t cell_y,
                                 const gui_cell_t *cell,
                                 uint32_t fg)
{
    const uint8_t *glyph;
    int64_t clip_right;
    int64_t clip_bottom;

    if (!fb || !clip || !cell)
        return;
    if (cell_x >= (int64_t)clip->x + (int64_t)clip->w ||
        cell_y >= (int64_t)clip->y + (int64_t)clip->h ||
        cell_x + (int64_t)GUI_FONT_W <= clip->x ||
        cell_y + (int64_t)GUI_FONT_H <= clip->y)
        return;

    glyph = font8x16_glyph((unsigned char)cell->ch);
    clip_right = (int64_t)clip->x + (int64_t)clip->w;
    clip_bottom = (int64_t)clip->y + (int64_t)clip->h;
    for (int row = 0; row < 16; row++) {
        int64_t py = cell_y + row;
        uint8_t bits;

        if (py < clip->y || py >= clip_bottom)
            continue;
        bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int64_t px = cell_x + col;

            if ((bits & (1u << col)) == 0)
                continue;
            if (px < clip->x || px >= clip_right)
                continue;
            framebuffer_fill_rect(fb, (int)px, (int)py, 1, 1, fg);
        }
    }
}

void gui_terminal_render(const gui_terminal_t *term,
                         const gui_pixel_surface_t *surface,
                         const gui_pixel_theme_t *theme,
                         int draw_cursor)
{
    gui_pixel_rect_t clip;
    int64_t content_x;
    int64_t content_y;
    int start_row;

    if (!term || !surface || !surface->fb || !theme)
        return;
    if (!terminal_intersect_pixel_rect(term->pixel_rect, surface->clip,
                                       &clip))
        return;

    framebuffer_fill_rect(surface->fb, clip.x, clip.y, clip.w, clip.h,
                          theme->terminal_bg);

    if (term->cols <= 0 || term->rows <= 0)
        return;

    content_x = (int64_t)term->pixel_rect.x + (int64_t)term->padding_x;
    content_y = (int64_t)term->pixel_rect.y + (int64_t)term->padding_y;
    start_row = term->history_count - term->view_top;
    if (start_row < 0)
        start_row = 0;

    for (int row = 0; row < term->rows; row++) {
        int global_row = start_row + row;
        const gui_cell_t *cells;
        int64_t cell_y = content_y + (int64_t)row * (int64_t)GUI_FONT_H;

        if (global_row < term->history_count) {
            cells = terminal_history_row_const(term, global_row);
        } else {
            int live_row = global_row - term->history_count;

            if (live_row < 0 || live_row >= term->rows)
                break;
            cells = &term->live[(uint32_t)live_row * (uint32_t)term->cols];
        }

        if (!cells)
            continue;
        if (cell_y >= (int64_t)clip.y + (int64_t)clip.h)
            break;

        for (int col = 0; col < term->cols; col++) {
            int64_t cell_x = content_x + (int64_t)col * (int64_t)GUI_FONT_W;

            terminal_render_cell(surface->fb, &clip, cell_x, cell_y,
                                 &cells[col], theme->terminal_fg);
        }
    }

    if (draw_cursor && term->live_view) {
        int64_t cursor_x = content_x +
                           (int64_t)term->cursor_x * (int64_t)GUI_FONT_W;
        int64_t cursor_y = content_y +
                           (int64_t)term->cursor_y * (int64_t)GUI_FONT_H +
                           ((int64_t)GUI_FONT_H - 2);

        if (cursor_x >= INT_MIN && cursor_x <= INT_MAX &&
            cursor_y >= INT_MIN && cursor_y <= INT_MAX) {
            framebuffer_fill_rect(surface->fb, (int)cursor_x, (int)cursor_y,
                                  (int)GUI_FONT_W, 2,
                                  theme->terminal_cursor);
        }
    }
}
