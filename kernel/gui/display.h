#ifndef GUI_DISPLAY_H
#define GUI_DISPLAY_H

#include <stdint.h>

typedef struct {
    int x;
    int y;
    int w;
    int h;
} gui_rect_t;

typedef struct {
    char ch;
    uint8_t attr;
} gui_cell_t;

typedef struct {
    gui_cell_t *cells;
    int cols;
    int rows;
    uint8_t default_attr;
    int cursor_x;
    int cursor_y;
    int cursor_visible;
} gui_display_t;

struct framebuffer_info;

void gui_display_init(gui_display_t *display, gui_cell_t *cells,
                      int cols, int rows, uint8_t default_attr);
gui_rect_t gui_display_fill_rect(gui_display_t *display,
                                 int x, int y, int w, int h,
                                 char ch, uint8_t attr);
gui_rect_t gui_display_draw_text(gui_display_t *display,
                                 int x, int y, int max_w,
                                 const char *text, uint8_t attr);
gui_rect_t gui_display_draw_frame(gui_display_t *display,
                                  int x, int y, int w, int h,
                                  uint8_t attr);
gui_cell_t gui_display_cell_at(const gui_display_t *display, int x, int y);
void gui_display_set_cursor(gui_display_t *display, int x, int y, int visible);
void gui_display_present_to_vga(const gui_display_t *display, uintptr_t video_address);
void gui_display_present_to_framebuffer(const gui_display_t *display,
                                        const struct framebuffer_info *fb);
void gui_display_present_rect_to_framebuffer(const gui_display_t *display,
                                             const struct framebuffer_info *fb,
                                             int x, int y, int w, int h);

#endif /* GUI_DISPLAY_H */
