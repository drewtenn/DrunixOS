#ifndef GUI_PIXEL_H
#define GUI_PIXEL_H

#include <stdint.h>

struct framebuffer_info;

typedef struct gui_pixel_rect {
    int x;
    int y;
    int w;
    int h;
} gui_pixel_rect_t;

typedef struct {
    const struct framebuffer_info *fb;
    gui_pixel_rect_t clip;
} gui_pixel_surface_t;

typedef struct {
    uint32_t desktop_bg;
    uint32_t taskbar_bg;
    uint32_t taskbar_fg;
    uint32_t window_bg;
    uint32_t window_border;
    uint32_t title_bg;
    uint32_t title_fg;
    uint32_t terminal_bg;
    uint32_t terminal_fg;
    uint32_t terminal_dim;
    uint32_t terminal_cursor;
    uint32_t scrollbar_track;
    uint32_t scrollbar_thumb;
} gui_pixel_theme_t;

#endif /* GUI_PIXEL_H */
