#ifndef GUI_TERMINAL_H
#define GUI_TERMINAL_H

#include "display.h"
#include "framebuffer.h"
#include <stdint.h>

typedef struct {
    gui_cell_t *live;
    gui_cell_t *history;
    int cols;
    int rows;
    int history_rows;
    int owns_buffers;
    int history_head;
    int history_count;
    int cursor_x;
    int cursor_y;
    int wrap_pending;
    int ansi_state;
    int ansi_val;
    uint8_t attr;
    uint8_t default_attr;
    int view_top;
    int live_view;
    gui_pixel_rect_t pixel_rect;
    int padding_x;
    int padding_y;
} gui_terminal_t;

int gui_terminal_init_static(gui_terminal_t *term,
                             gui_cell_t *live,
                             gui_cell_t *history,
                             int cols,
                             int rows,
                             int history_rows,
                             uint8_t default_attr);
int gui_terminal_init_alloc(gui_terminal_t *term,
                            int cols,
                            int rows,
                            int history_rows,
                            uint8_t default_attr);
void gui_terminal_destroy(gui_terminal_t *term);
void gui_terminal_clear(gui_terminal_t *term);
int gui_terminal_write(gui_terminal_t *term, const char *buf, uint32_t len);
void gui_terminal_scroll_view(gui_terminal_t *term, int rows);
void gui_terminal_snap_live(gui_terminal_t *term);
gui_cell_t gui_terminal_cell_at(const gui_terminal_t *term, int x, int y);
int gui_terminal_history_count(const gui_terminal_t *term);
int gui_terminal_cursor_x(const gui_terminal_t *term);
int gui_terminal_cursor_y(const gui_terminal_t *term);
int gui_terminal_visible_view_top(const gui_terminal_t *term);
int gui_terminal_total_rows(const gui_terminal_t *term);
void gui_terminal_set_pixel_rect(gui_terminal_t *term,
                                 gui_pixel_rect_t rect,
                                 int padding_x,
                                 int padding_y);
void gui_terminal_render(const gui_terminal_t *term,
                         const gui_pixel_surface_t *surface,
                         const gui_pixel_theme_t *theme,
                         int draw_cursor);

#endif /* GUI_TERMINAL_H */
