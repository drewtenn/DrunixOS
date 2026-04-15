#include "terminal.h"
#include "kheap.h"
#include "kstring.h"

static void terminal_clear_line(gui_terminal_t *term, int row);
static void terminal_push_history(gui_terminal_t *term, const gui_cell_t *row);
static void terminal_scroll_up(gui_terminal_t *term);
static void terminal_apply_ansi(gui_terminal_t *term, int code);
static void terminal_apply_char(gui_terminal_t *term, char c);

static int terminal_row_bytes(const gui_terminal_t *term)
{
    if (!term || term->cols <= 0)
        return 0;
    return term->cols * (int)sizeof(gui_cell_t);
}

static int terminal_history_capacity(const gui_terminal_t *term)
{
    if (!term || term->history_rows <= 0 || !term->history)
        return 0;
    return term->history_rows;
}

static gui_cell_t *terminal_history_row(gui_terminal_t *term, int index)
{
    int capacity;

    if (!term || !term->history || index < 0)
        return 0;

    capacity = terminal_history_capacity(term);
    if (capacity <= 0 || index >= term->history_count)
        return 0;
    return &term->history[((term->history_head + index) % capacity) *
                          term->cols];
}

static const gui_cell_t *terminal_history_row_const(const gui_terminal_t *term,
                                                    int index)
{
    return terminal_history_row((gui_terminal_t *)term, index);
}

static gui_cell_t *terminal_live_row(gui_terminal_t *term, int row)
{
    if (!term || !term->live || row < 0 || row >= term->rows)
        return 0;
    return &term->live[row * term->cols];
}

static void terminal_clear_history(gui_terminal_t *term)
{
    int capacity;

    if (!term)
        return;

    capacity = terminal_history_capacity(term);
    if (capacity > 0) {
        gui_cell_t blank = { ' ', 0 };

        blank.attr = term->default_attr;
        for (int row = 0; row < capacity; row++) {
            int base = row * term->cols;

            for (int col = 0; col < term->cols; col++)
                term->history[base + col] = blank;
        }
    }
    term->history_head = 0;
    term->history_count = 0;
}

static void terminal_write_cell(gui_terminal_t *term, char c)
{
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

    cell = &term->live[term->cursor_y * term->cols + term->cursor_x];
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
    int base;

    if (!term || !term->live || row < 0 || row >= term->rows)
        return;

    blank.attr = term->default_attr;
    base = row * term->cols;
    for (int col = 0; col < term->cols; col++)
        term->live[base + col] = blank;
}

static void terminal_push_history(gui_terminal_t *term, const gui_cell_t *row)
{
    int capacity;
    int tail;

    if (!term || !row)
        return;

    capacity = terminal_history_capacity(term);
    if (capacity <= 0)
        return;

    if (term->history_count < capacity) {
        tail = (term->history_head + term->history_count) % capacity;
        term->history_count++;
    } else {
        tail = term->history_head;
        term->history_head = (term->history_head + 1) % capacity;
    }

    k_memcpy(&term->history[tail * term->cols], row,
             (uint32_t)terminal_row_bytes(term));
}

static void terminal_scroll_up(gui_terminal_t *term)
{
    int bytes;

    if (!term || !term->live || term->cols <= 0 || term->rows <= 0)
        return;
    terminal_push_history(term, term->live);
    bytes = term->cols * (int)sizeof(gui_cell_t);
    for (int row = 1; row < term->rows; row++) {
        k_memmove(&term->live[(row - 1) * term->cols],
                  &term->live[row * term->cols],
                  (uint32_t)bytes);
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
            term->ansi_val = (term->ansi_val * 10) + (c - '0');
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
    if (!term || !live || cols <= 0 || rows <= 0 || history_rows < 0 ||
        (history_rows > 0 && !history))
        return 0;

    k_memset(term, 0, sizeof(*term));
    term->live = live;
    term->history = history;
    term->cols = cols;
    term->rows = rows;
    term->history_rows = history_rows;
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

    if (!term || cols <= 0 || rows <= 0 || history_rows < 0)
        return 0;

    live_bytes = (uint32_t)(cols * rows * (int)sizeof(gui_cell_t));
    history_bytes = (uint32_t)(cols * history_rows * (int)sizeof(gui_cell_t));

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

    return 1;
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

    for (i = 0; i < len; i++)
        terminal_apply_char(term, buf[i]);

    term->view_top = 0;
    term->live_view = 1;
    return (int)len;
}

void gui_terminal_scroll_view(gui_terminal_t *term, int rows)
{
    if (!term)
        return;

    term->view_top += rows;
    if (term->view_top < 0)
        term->view_top = 0;
    if (term->view_top > term->history_count)
        term->view_top = term->history_count;
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

void gui_terminal_render(const gui_terminal_t *term,
                         const gui_pixel_surface_t *surface,
                         const gui_pixel_theme_t *theme,
                         int draw_cursor)
{
    (void)term;
    (void)surface;
    (void)theme;
    (void)draw_cursor;
}
